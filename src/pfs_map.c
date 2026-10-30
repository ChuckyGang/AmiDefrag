/*
 * pfs_map.c - block map of a Professional File System 3 volume, read-only.
 * Layouts from pfs3aio (blocks.h, allocation.c, anodes.c).
 *
 * PFS3 counts in 512-byte sectors.  Partition layout:
 *   0, 1          boot sectors
 *   2             rootblock (512 bytes) followed, inside the same
 *                 rblkcluster-sector cluster, by the RESERVED bitmap
 *   firstreserved .. lastreserved   reserved area: reserved_blksize-byte
 *                 blocks (1/2/4 KB) holding every structure: bitmap index
 *                 'MI', bitmap 'BM', anode index 'IB', anode 'AB', super
 *                 index 'SB', directory 'DB', deldir 'DD', extension 'EX'
 *   lastreserved+1 .. disksize-1    user data, one bitmap bit per sector
 *
 * Rootblock fields: disktype @0, options @4, diskname @20 (BSTR),
 * lastreserved @52, firstreserved @56, reserved_blksize @64 (UW),
 * rblkcluster @66 (UW), blocksfree @68, disksize @84, extension @88,
 * bitmapindex[] @96, small-mode anode indexblocks[] @116.
 *
 * Bitmaps: header 12 bytes {id UW, pad, datestamp, seqnr}, then longs,
 * bit (1 << (31 - n)) set = free.  Data bitmap bit n = sector
 * bitmapstart + n with bitmapstart = lastreserved + 1, longsperbmb =
 * reserved_blksize/4 - 3 longs per bitmap block, bitmap block seq found
 * through bitmapindex[seq / indexperblock] -> index[seq % indexperblock].
 *
 * Anodes {clustersize, blocknr, next} (12 bytes) from offset 16 of an
 * 'AB' block.  anodenr -> (seqnr, offset): split mode (MODE_SPLITTED_
 * ANODES) seqnr = anodenr >> 16, offset = anodenr & 0xffff; otherwise
 * divide by anodesperblock.  'AB' block for seqnr through the anode
 * index: index block seqnr / indexperblock, slot seqnr % indexperblock;
 * index blocks from the small-mode table in the rootblock or, with
 * MODE_SUPERINDEX, from 'SB' blocks listed in the rootblock extension at
 * @64.  A file is its anode chain: one anode per contiguous cluster.
 *
 * Directories: anode 5 is the root; a directory's anode chain lists its
 * 'DB' blocks; entries from @20: {next UB (entry length, 0 = end), type
 * B, anode UL, fsize UL, date 3xUW, protection UB, nlength UB, name}.
 * type: 2 dir, -3 file, -16 rollover file, 3 softlink, 4 / -4 links.
 */
#include "fsmap_int.h"

#define PFS_ID_DB 0x4442
#define PFS_ID_AB 0x4142
#define PFS_ID_IB 0x4942
#define PFS_ID_BM 0x424D
#define PFS_ID_MI 0x4D49
#define PFS_ID_EX 0x4558
#define PFS_ID_SB 0x5342

#define MODE_SPLITTED_ANODES 2
#define MODE_SUPERINDEX      128

#define ANODE_ROOTDIR 5

struct Pfs {
    struct FsCtx *c;
    ULONG options, firstres, lastres, resblksize, rescluster, rblkcluster;
    ULONG disksize, extension, bitmapstart, longsperbmb, indexperblock, anodesperblock;
    UBYTE *root;       /* rootblock cluster (rblkcluster sectors) */
    UBYTE *rext;       /* rootblock extension or NULL */
    UBYTE *ibuf;       /* index / super block cache */
    ULONG  ibuf_nr;    /* which index block is cached, ~0 none */
    UBYTE *abuf;       /* anode block cache */
    ULONG  abuf_seq;
    UBYTE *sbuf;       /* super block cache */
    ULONG  sbuf_nr;
    UBYTE *dbuf;       /* directory block */
    UBYTE *tbuf;       /* scratch (bitmaps) */
    ULONG  visited, used_blocks;
};

/* Read one reserved block (resblksize bytes) at sector nr. */
static BOOL res_read(struct Pfs *p, ULONG nr, UBYTE *buf)
{
    if (nr < p->firstres || nr > p->lastres) return FALSE;
    return fs_read_sectors(p->c, nr, p->rescluster, buf);
}

static void res_mark(struct Pfs *p, ULONG nr, UBYTE cls)
{
    FsMap_SetRange(p->c->m, nr, p->rescluster, cls);
}

/* --- bitmaps ------------------------------------------------------- */

static BOOL read_reserved_bitmap(struct Pfs *p)
{
    ULONG numres = (p->lastres - p->firstres + 1) / p->rescluster;
    const UBYTE *bm = p->root + 512;
    ULONG avail_bits = (p->rblkcluster * 512 - 512 - 12) * 8;
    ULONG i;
    if (be16(bm, 0) != PFS_ID_BM) { fs_note(p->c, "reserved bitmap not found"); return FALSE; }
    for (i = 0; i < numres && i < avail_bits; i++) {
        ULONG word = be32(bm, 12 + 4 * (i / 32));
        BOOL  fr   = (word & (0x80000000UL >> (i % 32))) != 0;
        res_mark(p, p->firstres + i * p->rescluster, fr ? BLK_RESV : BLK_META);
    }
    return TRUE;
}

static BOOL read_data_bitmap(struct Pfs *p)
{
    struct FsCtx *c = p->c;
    struct FsMap *m = c->m;
    ULONG nbits = (p->disksize > p->bitmapstart) ? p->disksize - p->bitmapstart : 0;
    ULONG longs = (nbits + 31) / 32;
    ULONG nbmb  = (longs + p->longsperbmb - 1) / p->longsperbmb;
    ULONG seq, used = 0;

    for (seq = 0; seq < nbmb; seq++) {
        ULONG minr = seq / p->indexperblock, slot = seq % p->indexperblock;
        ULONG mi_blk, bm_blk, j;
        if (minr >= 104) break;
        mi_blk = be32(p->root, 96 + 4 * minr);
        if (p->ibuf_nr != mi_blk) {
            if (!res_read(p, mi_blk, p->ibuf) || be16(p->ibuf, 0) != PFS_ID_MI) {
                fs_note(c, "bitmap index block missing");
                return FALSE;
            }
            p->ibuf_nr = mi_blk;
        }
        bm_blk = be32(p->ibuf, 12 + 4 * slot);
        if (!res_read(p, bm_blk, p->tbuf) || be16(p->tbuf, 0) != PFS_ID_BM) {
            fs_note(c, "bitmap block missing");
            return FALSE;
        }
        for (j = 0; j < p->longsperbmb; j++) {
            ULONG word = be32(p->tbuf, 12 + 4 * j);
            ULONG base = p->bitmapstart + (seq * p->longsperbmb + j) * 32, bit;
            if (base >= p->disksize) break;
            if (word == 0xFFFFFFFFUL) continue;
            for (bit = 0; bit < 32 && base + bit < p->disksize; bit++)
                if (!(word & (0x80000000UL >> bit))) { FsMap_Set(m, base + bit, BLK_UNKN); used++; }
        }
        if (!fs_progress(c, seq + 1, nbmb, "Reading bitmap")) return FALSE;
    }
    p->ibuf_nr = 0xFFFFFFFFUL;        /* ibuf is reused for anode indexes */
    p->used_blocks = used;
    m->bitmap_free = nbits - used;
    return TRUE;
}

/* --- anodes -------------------------------------------------------- */

static BOOL get_index_block(struct Pfs *p, ULONG nr)
{
    ULONG blk;
    if (p->ibuf_nr == nr) return TRUE;
    if (p->options & MODE_SUPERINDEX) {
        ULONG snr = nr / p->indexperblock;
        if (!p->rext || snr > 15) return FALSE;
        if (p->sbuf_nr != snr) {
            ULONG sblk = be32(p->rext, 64 + 4 * snr);
            if (!res_read(p, sblk, p->sbuf) || be16(p->sbuf, 0) != PFS_ID_SB) return FALSE;
            p->sbuf_nr = snr;
        }
        blk = be32(p->sbuf, 12 + 4 * (nr % p->indexperblock));
    } else {
        if (nr > 98) return FALSE;
        blk = be32(p->root, 116 + 4 * nr);
    }
    if (!res_read(p, blk, p->ibuf) || be16(p->ibuf, 0) != PFS_ID_IB) return FALSE;
    p->ibuf_nr = nr;
    return TRUE;
}

/* Fetch anode anodenr into cs / blk / next.  FALSE when unreachable. */
static BOOL get_anode(struct Pfs *p, ULONG anodenr, ULONG *cs, ULONG *blk, ULONG *next)
{
    ULONG seq, off;
    if (p->options & MODE_SPLITTED_ANODES) { seq = anodenr >> 16; off = anodenr & 0xFFFF; }
    else { seq = anodenr / p->anodesperblock; off = anodenr % p->anodesperblock; }
    if (off >= p->anodesperblock) return FALSE;
    if (p->abuf_seq != seq) {
        ULONG ablk;
        if (!get_index_block(p, seq / p->indexperblock)) return FALSE;
        ablk = be32(p->ibuf, 12 + 4 * (seq % p->indexperblock));
        if (!res_read(p, ablk, p->abuf) || be16(p->abuf, 0) != PFS_ID_AB) return FALSE;
        p->abuf_seq = seq;
    }
    *cs   = be32(p->abuf, 16 + 12 * off);
    *blk  = be32(p->abuf, 16 + 12 * off + 4);
    *next = be32(p->abuf, 16 + 12 * off + 8);
    return TRUE;
}

/* Walk a file's anode chain: pass 1 counts clusters, pass 2 marks. */
static void account_file(struct Pfs *p, ULONG anodenr)
{
    struct FsMap *m = p->c->m;
    ULONG runs = 0, a = anodenr, guard = 0, cs, blk, next;
    UBYTE cls;

    while (a && guard++ < 1000000UL) {
        if (!get_anode(p, a, &cs, &blk, &next)) { fs_note(p->c, "file with unreachable anode"); break; }
        if (cs) runs++;
        a = next;
    }
    m->files++;
    if (runs > 1) { m->frag_files++; m->extra_runs += runs - 1; if (runs > m->worst_runs) m->worst_runs = runs; }
    cls = runs > 1 ? BLK_FRAG : BLK_DATA;

    a = anodenr; guard = 0;
    while (a && guard++ < 1000000UL) {
        if (!get_anode(p, a, &cs, &blk, &next)) break;
        if (cs) {
            if (blk < p->bitmapstart || blk + cs > p->disksize) { fs_note(p->c, "file data outside the data area"); break; }
            FsMap_SetRange(m, blk, cs, cls);
            p->visited += cs;
        }
        a = next;
    }
}

/* --- directories --------------------------------------------------- */

static BOOL walk_directories(struct Pfs *p)
{
    struct FsCtx *c = p->c;
    struct FsMap *m = c->m;
    struct UStack st;
    BOOL ok = TRUE;

    memset(&st, 0, sizeof(st));
    if (!ustack_push(&st, ANODE_ROOTDIR)) return FALSE;

    while (st.n > 0 && ok) {
        ULONG a = st.v[--st.n], guard = 0;
        m->dirs++;
        while (a && guard++ < 100000UL && ok) {
            ULONG cs, blk, next, off;
            if (!get_anode(p, a, &cs, &blk, &next)) { fs_note(c, "directory with unreachable anode"); break; }
            if (cs == 0 || blk == 0) { a = next; continue; }
            if (!res_read(p, blk, p->dbuf) || be16(p->dbuf, 0) != PFS_ID_DB) { fs_note(c, "directory block missing"); break; }
            for (off = 20; off + 22 <= p->resblksize; ) {
                UBYTE len  = p->dbuf[off];
                BYTE  type = (BYTE)p->dbuf[off + 1];
                ULONG an   = be32(p->dbuf, off + 2);
                if (len == 0) break;
                if (type == 2) { if (!ustack_push(&st, an)) { ok = FALSE; break; } }
                else if (type == -3 || type == -16) account_file(p, an);
                /* 3 / 4 / -4: links - nothing allocated in the data area */
                off += len;
            }
            if (!fs_progress(c, p->visited, p->used_blocks, "Reading directories")) ok = FALSE;
            a = next;
        }
    }
    ustack_free(&st);
    return ok;
}

/* --- entry --------------------------------------------------------- */

BOOL PfsMap_Build(struct FsCtx *c)
{
    struct FsMap *m = c->m;
    struct Pfs p;
    UBYTE rb[512];
    BOOL ok = FALSE;

    memset(&p, 0, sizeof(p));
    p.c = c;
    p.ibuf_nr = p.abuf_seq = p.sbuf_nr = 0xFFFFFFFFUL;

    if (!fs_read_sectors(c, 2, 1, rb)) { fs_note(c, "cannot read rootblock"); return FALSE; }
    if ((be32(rb, 0) & 0xFFFFFF00UL) != 0x50465300UL) { fs_note(c, "no PFS rootblock at sector 2"); return FALSE; }
    p.options     = be32(rb, 4);
    p.lastres     = be32(rb, 52);
    p.firstres    = be32(rb, 56);
    p.resblksize  = be16(rb, 64);
    p.rblkcluster = be16(rb, 66);
    p.disksize    = be32(rb, 84);
    p.extension   = be32(rb, 88);
    if (p.resblksize < 1024 || p.resblksize > 4096 || p.rblkcluster == 0 || p.rblkcluster > 64 ||
        p.firstres != 2 || p.lastres <= p.firstres) {
        fs_note(c, "rootblock geometry not understood");
        return FALSE;
    }
    p.rescluster     = p.resblksize / 512;
    p.bitmapstart    = p.lastres + 1;
    p.longsperbmb    = p.resblksize / 4 - 3;
    p.indexperblock  = (p.resblksize - 12) / 4;
    p.anodesperblock = (p.resblksize - 16) / 12;
    if (p.disksize == 0 || p.disksize > m->nblocks) p.disksize = m->nblocks;
    {
        ULONG n = rb[20];
        if (n > 31) n = 31;
        memcpy(m->volname, rb + 21, n);
        m->volname[n] = '\0';
    }
    m->bitmap_valid = TRUE;

    p.root = (UBYTE *)AllocVec(p.rblkcluster * 512 + p.resblksize * 6, MEMF_PUBLIC);
    if (!p.root) { fs_note(c, "out of memory"); return FALSE; }
    p.rext = p.root + p.rblkcluster * 512;
    p.ibuf = p.rext + p.resblksize;
    p.abuf = p.ibuf + p.resblksize;
    p.sbuf = p.abuf + p.resblksize;
    p.dbuf = p.sbuf + p.resblksize;
    p.tbuf = p.dbuf + p.resblksize;

    if (!fs_read_sectors(c, 2, p.rblkcluster, p.root)) { fs_note(c, "cannot read rootblock cluster"); goto done; }
    if (p.extension && res_read(&p, p.extension, p.rext) && be16(p.rext, 0) == PFS_ID_EX) {
        /* keep */
    } else {
        p.rext = NULL;
        if (p.options & MODE_SUPERINDEX) { fs_note(c, "rootblock extension missing (needed for superindex)"); }
    }

    FsMap_SetRange(m, 0, 2, BLK_META);                                /* boot */
    FsMap_SetRange(m, 2, p.rblkcluster, BLK_META);                    /* rootblock cluster */
    if (p.disksize < m->nblocks) FsMap_SetRange(m, p.disksize, m->nblocks - p.disksize, BLK_RESV);
    if (!read_reserved_bitmap(&p)) goto done;
    FsMap_SetRange(m, 2, p.rblkcluster, BLK_META);                    /* (bitmap covers it too) */
    if (!read_data_bitmap(&p)) goto done;
    if (!walk_directories(&p)) goto done;
    if (c->aborted) goto done;
    ok = TRUE;

done:
    FreeVec(p.root);
    return ok;
}
