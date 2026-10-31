/*
 * pfs_core.c - PFS3 volume access shared by pfs_map.c and pfs_defrag.c.
 * See pfs_core.h for the on-disk layout summary.
 *
 * Rootblock fields: disktype @0, options @4, datestamp @8, diskname @20
 * (BSTR), lastreserved @52, firstreserved @56, reserved_free @60,
 * reserved_blksize @64 (UW), rblkcluster @66 (UW), blocksfree @68,
 * roving_ptr @76, deldir @80 (pre-17.9 deldir, 0 today), disksize @84,
 * extension @88, bitmapindex[] @96, small-mode anode indexblocks[] @116.
 * Rootblock extension: deldirsize @54 (UW), superindex[16] @64,
 * deldir[32] @144.  Deldir block: 31 entries of 32 bytes from @32,
 * anodenr first, name as a length-prefixed string at @14.
 */
#include "fsmap_int.h"
#include "pfs_core.h"

#define SEC 512UL

/* ------------------------------------------------------------------ */
/* raw access                                                          */
/* ------------------------------------------------------------------ */

BOOL pfs_read_sectors(struct PfsVol *v, ULONG sector, ULONG n, void *buf)
{
    if (sector >= v->nsectors || n > v->nsectors - sector) return FALSE;
    return BlockDev_ReadBlocks(v->bd, v->part_start + sector, n, buf);
}

BOOL pfs_write_sectors(struct PfsVol *v, ULONG sector, ULONG n, const void *buf)
{
    if (sector >= v->nsectors || n > v->nsectors - sector) return FALSE;
    return BlockDev_WriteBlocks(v->bd, v->part_start + sector, n, buf);
}

BOOL pfs_res_read(struct PfsVol *v, ULONG nr, UBYTE *buf)
{
    if (nr < v->firstres || nr > v->lastres) return FALSE;
    return pfs_read_sectors(v, nr, v->rescluster, buf);
}

static void put32(UBYTE *b, ULONG o, ULONG x)
{
    b[o] = (UBYTE)(x >> 24); b[o + 1] = (UBYTE)(x >> 16); b[o + 2] = (UBYTE)(x >> 8); b[o + 3] = (UBYTE)x;
}

static BOOL progress(struct PfsVol *v, ULONG done, ULONG total, const char *what)
{
    if (v->aborted) return FALSE;
    if (v->cb && !v->cb(v->cb_ud, done, total, what)) { v->aborted = TRUE; return FALSE; }
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* open / close                                                        */
/* ------------------------------------------------------------------ */

BOOL pfs_open(struct PfsVol *v, struct BlockDev *bd, const struct VolumeInfo *vi)
{
    UBYTE rb[SEC];
    FsMapProgress cb = v->cb;
    void *cb_ud = v->cb_ud;

    memset(v, 0, sizeof(*v));
    v->cb = cb; v->cb_ud = cb_ud;
    v->bd = bd;
    v->part_start = vi->start_block;
    v->nsectors   = vi->num_blocks;
    v->ibuf_nr = v->abuf_seq = v->sbuf_nr = 0xFFFFFFFFUL;

    if (!pfs_read_sectors(v, 2, 1, rb)) { strncpy(v->err, "cannot read rootblock", sizeof(v->err) - 1); return FALSE; }
    if ((be32(rb, 0) & 0xFFFFFF00UL) != 0x50465300UL) { strncpy(v->err, "no PFS rootblock at sector 2", sizeof(v->err) - 1); return FALSE; }
    v->options       = be32(rb, 4);
    v->datestamp     = be32(rb, 8);
    v->lastres       = be32(rb, 52);
    v->firstres      = be32(rb, 56);
    v->reserved_free = be32(rb, 60);
    v->resblksize    = be16(rb, 64);
    v->rblkcluster   = be16(rb, 66);
    v->blocksfree    = be32(rb, 68);
    v->deldir_old    = be32(rb, 80);
    v->disksize      = be32(rb, 84);
    v->extension     = be32(rb, 88);
    if (v->resblksize < 1024 || v->resblksize > 4096 || v->rblkcluster == 0 || v->rblkcluster > 64 ||
        v->firstres != 2 || v->lastres <= v->firstres) {
        strncpy(v->err, "rootblock geometry not understood", sizeof(v->err) - 1);
        return FALSE;
    }
    v->rescluster     = v->resblksize / SEC;
    v->bitmapstart    = v->lastres + 1;
    v->longsperbmb    = v->resblksize / 4 - 3;
    v->indexperblock  = (v->resblksize - 12) / 4;
    v->anodesperblock = (v->resblksize - 16) / 12;
    if (v->disksize == 0 || v->disksize > v->nsectors) v->disksize = v->nsectors;
    if (v->bitmapstart >= v->disksize) { strncpy(v->err, "no data area", sizeof(v->err) - 1); return FALSE; }

    v->root = (UBYTE *)AllocVec(v->rblkcluster * SEC + v->resblksize * 6, MEMF_PUBLIC);
    if (!v->root) { strncpy(v->err, "out of memory", sizeof(v->err) - 1); return FALSE; }
    v->rext = v->root + v->rblkcluster * SEC;
    v->ibuf = v->rext + v->resblksize;
    v->abuf = v->ibuf + v->resblksize;
    v->sbuf = v->abuf + v->resblksize;
    v->dbuf = v->sbuf + v->resblksize;
    v->tbuf = v->dbuf + v->resblksize;

    if (!pfs_read_sectors(v, 2, v->rblkcluster, v->root)) {
        strncpy(v->err, "cannot read rootblock cluster", sizeof(v->err) - 1);
        pfs_close(v);
        return FALSE;
    }
    if (!(v->extension && pfs_res_read(v, v->extension, v->rext) && be16(v->rext, 0) == PFS_ID_EX))
        v->rext = NULL;
    return TRUE;
}

void pfs_close(struct PfsVol *v)
{
    if (v->root) FreeVec(v->root);
    if (v->bm) FreeVec(v->bm);
    if (v->bmb_key) FreeVec(v->bmb_key);
    if (v->bmb_dirty) FreeVec(v->bmb_dirty);
    v->root = NULL; v->bm = NULL; v->bmb_key = NULL; v->bmb_dirty = NULL;
    v->bm_loaded = FALSE;
}

/* ------------------------------------------------------------------ */
/* anodes                                                              */
/* ------------------------------------------------------------------ */

static BOOL get_index_block(struct PfsVol *v, ULONG nr)
{
    ULONG blk;
    if (v->ibuf_nr == nr) return TRUE;
    if (v->options & PFS_MODE_SUPERINDEX) {
        ULONG snr = nr / v->indexperblock;
        if (!v->rext || snr > 15) return FALSE;
        if (v->sbuf_nr != snr) {
            ULONG sblk = be32(v->rext, 64 + 4 * snr);
            if (!pfs_res_read(v, sblk, v->sbuf) || be16(v->sbuf, 0) != PFS_ID_SB) return FALSE;
            v->sbuf_nr = snr;
        }
        blk = be32(v->sbuf, 12 + 4 * (nr % v->indexperblock));
    } else {
        if (nr > 98) return FALSE;
        blk = be32(v->root, 116 + 4 * nr);
    }
    if (!pfs_res_read(v, blk, v->ibuf) || be16(v->ibuf, 0) != PFS_ID_IB) { v->ibuf_nr = 0xFFFFFFFFUL; return FALSE; }
    v->ibuf_nr = nr;
    return TRUE;
}

/* Load the anode block holding anodenr; returns the byte offset of the
 * anode inside v->abuf or ~0. */
static ULONG locate_anode(struct PfsVol *v, ULONG anodenr)
{
    ULONG seq, off;
    if (v->options & PFS_MODE_SPLITTED_ANODES) { seq = anodenr >> 16; off = anodenr & 0xFFFF; }
    else { seq = anodenr / v->anodesperblock; off = anodenr % v->anodesperblock; }
    if (off >= v->anodesperblock) return 0xFFFFFFFFUL;
    if (v->abuf_seq != seq) {
        ULONG ablk;
        if (!get_index_block(v, seq / v->indexperblock)) return 0xFFFFFFFFUL;
        ablk = be32(v->ibuf, 12 + 4 * (seq % v->indexperblock));
        if (!pfs_res_read(v, ablk, v->abuf) || be16(v->abuf, 0) != PFS_ID_AB) { v->abuf_seq = 0xFFFFFFFFUL; return 0xFFFFFFFFUL; }
        v->abuf_seq = seq;
        v->abuf_blk = ablk;
    }
    return 16 + 12 * off;
}

BOOL pfs_get_anode(struct PfsVol *v, ULONG anodenr, ULONG *cs, ULONG *blk, ULONG *next)
{
    ULONG o = locate_anode(v, anodenr);
    if (o == 0xFFFFFFFFUL) return FALSE;
    *cs   = be32(v->abuf, o);
    *blk  = be32(v->abuf, o + 4);
    *next = be32(v->abuf, o + 8);
    return TRUE;
}

BOOL pfs_set_anode(struct PfsVol *v, ULONG anodenr, ULONG cs, ULONG blk, ULONG next)
{
    ULONG o = locate_anode(v, anodenr), k1, k2, kfirst, ksecond;
    if (o == 0xFFFFFFFFUL) return FALSE;
    /* An anode sits at 16 + 12 * slot, so one anode per 512 bytes of an
     * anode block straddles a sector boundary (slot 41 at 508..519, slot
     * 126 at 1528..1539, ...); its fields never do (offsets are multiples
     * of 4).  Refresh the sector(s) from disk first so that the other
     * anodes in them are written back exactly as they are now. */
    k1 = o / SEC;
    k2 = (o + 11) / SEC;
    if (!pfs_read_sectors(v, v->abuf_blk + k1, 1, v->abuf + k1 * SEC)) return FALSE;
    if (k2 != k1 && !pfs_read_sectors(v, v->abuf_blk + k2, 1, v->abuf + k2 * SEC)) return FALSE;
    put32(v->abuf, o, cs);
    put32(v->abuf, o + 4, blk);
    put32(v->abuf, o + 8, next);
    if (k2 == k1) return pfs_write_sectors(v, v->abuf_blk + k1, 1, v->abuf + k1 * SEC);
    /* Two writes: the sector holding blocknr goes first, so that a crash
     * in between leaves the anode pointing at either the old or the new
     * data, and the engine's journal recovery decides by blocknr which
     * one and completes the other fields. */
    kfirst  = (o + 4) / SEC;
    ksecond = (kfirst == k1) ? k2 : k1;
    if (!pfs_write_sectors(v, v->abuf_blk + kfirst, 1, v->abuf + kfirst * SEC)) return FALSE;
    return pfs_write_sectors(v, v->abuf_blk + ksecond, 1, v->abuf + ksecond * SEC);
}

/* ------------------------------------------------------------------ */
/* data bitmap                                                         */
/* ------------------------------------------------------------------ */

BOOL pfs_bm_load(struct PfsVol *v)
{
    ULONG nbits = v->disksize - v->bitmapstart;
    ULONG seq, mi_cached = 0xFFFFFFFFUL;

    v->bm_longs = (nbits + 31) / 32;
    v->nbmb     = (v->bm_longs + v->longsperbmb - 1) / v->longsperbmb;
    v->bm        = (ULONG *)AllocVec(v->nbmb * v->longsperbmb * 4, MEMF_PUBLIC | MEMF_CLEAR);
    v->bmb_key   = (ULONG *)AllocVec(v->nbmb * 4, MEMF_PUBLIC | MEMF_CLEAR);
    v->bmb_dirty = (UBYTE *)AllocVec(v->nbmb, MEMF_PUBLIC | MEMF_CLEAR);
    if (!v->bm || !v->bmb_key || !v->bmb_dirty) { strncpy(v->err, "out of memory for the bitmap", sizeof(v->err) - 1); return FALSE; }
    v->free_blocks = 0;

    for (seq = 0; seq < v->nbmb; seq++) {
        ULONG minr = seq / v->indexperblock, slot = seq % v->indexperblock;
        ULONG mi_blk, bm_blk, j;
        if (minr >= 104) { strncpy(v->err, "bitmap index out of range", sizeof(v->err) - 1); return FALSE; }
        mi_blk = be32(v->root, 96 + 4 * minr);
        if (mi_cached != mi_blk) {
            if (!pfs_res_read(v, mi_blk, v->ibuf) || be16(v->ibuf, 0) != PFS_ID_MI) {
                strncpy(v->err, "bitmap index block missing", sizeof(v->err) - 1);
                v->ibuf_nr = 0xFFFFFFFFUL;
                return FALSE;
            }
            mi_cached = mi_blk;
        }
        bm_blk = be32(v->ibuf, 12 + 4 * slot);
        if (!pfs_res_read(v, bm_blk, v->tbuf) || be16(v->tbuf, 0) != PFS_ID_BM) {
            strncpy(v->err, "bitmap block missing", sizeof(v->err) - 1);
            v->ibuf_nr = 0xFFFFFFFFUL;
            return FALSE;
        }
        v->bmb_key[seq] = bm_blk;
        for (j = 0; j < v->longsperbmb; j++) {
            ULONG idx = seq * v->longsperbmb + j, w, base, bit;
            if (idx >= v->bm_longs) break;
            w = be32(v->tbuf, 12 + 4 * j);
            v->bm[idx] = w;
            base = idx * 32;
            if (w == 0xFFFFFFFFUL && base + 32 <= nbits) { v->free_blocks += 32; continue; }
            if (w == 0) continue;
            for (bit = 0; bit < 32 && base + bit < nbits; bit++)
                if (w & (0x80000000UL >> bit)) v->free_blocks++;
        }
        if (!progress(v, seq + 1, v->nbmb, "Reading bitmap")) return FALSE;
    }
    v->ibuf_nr = 0xFFFFFFFFUL;       /* ibuf is shared with the anode index */
    v->bm_loaded = TRUE;
    return TRUE;
}

BOOL pfs_bm_isfree(const struct PfsVol *v, ULONG sector)
{
    ULONG idx;
    if (sector < v->bitmapstart || sector >= v->disksize) return FALSE;
    idx = sector - v->bitmapstart;
    return (v->bm[idx >> 5] & (0x80000000UL >> (idx & 31))) != 0;
}

void pfs_bm_set(struct PfsVol *v, ULONG sector, BOOL isfree)
{
    ULONG idx, li, mask, bmb, j, k;
    if (sector < v->bitmapstart || sector >= v->disksize) return;
    idx  = sector - v->bitmapstart;
    li   = idx >> 5;
    mask = 0x80000000UL >> (idx & 31);
    if (isfree) { if (!(v->bm[li] & mask)) v->free_blocks++; v->bm[li] |= mask; }
    else        { if (v->bm[li] & mask) v->free_blocks--;  v->bm[li] &= ~mask; }
    bmb = li / v->longsperbmb;
    j   = li % v->longsperbmb;
    k   = (12 + 4 * j) / SEC;
    v->bmb_dirty[bmb] |= (UBYTE)(1 << k);
}

BOOL pfs_bm_flush(struct PfsVol *v)
{
    ULONG bmb;
    for (bmb = 0; bmb < v->nbmb; bmb++) {
        ULONG k;
        if (!v->bmb_dirty[bmb]) continue;
        for (k = 0; k < v->rescluster; k++) {
            ULONG b;
            if (!(v->bmb_dirty[bmb] & (1 << k))) continue;
            if (!pfs_read_sectors(v, v->bmb_key[bmb] + k, 1, v->tbuf)) { strncpy(v->err, "bitmap sector read failed", sizeof(v->err) - 1); return FALSE; }
            for (b = (k == 0) ? 12 : 0; b + 4 <= SEC; b += 4) {
                ULONG j = (k * SEC + b - 12) / 4, idx;
                if (j >= v->longsperbmb) break;
                idx = bmb * v->longsperbmb + j;
                if (idx >= v->bm_longs) break;
                put32(v->tbuf, b, v->bm[idx]);
            }
            if (!pfs_write_sectors(v, v->bmb_key[bmb] + k, 1, v->tbuf)) { strncpy(v->err, "bitmap sector write failed", sizeof(v->err) - 1); return FALSE; }
        }
        v->bmb_dirty[bmb] = 0;
    }
    return TRUE;
}

/* Free-run scanner shared by the finders below.  Bits inside the
 * exclusion zone [ex_lo, ex_hi) (absolute sectors) count as used.
 * mode 0: best fit, the smallest run of at least `need` sectors.
 * mode 1: the largest run whose start is not in skip[]; *len = its length. */
static ULONG bm_scan(const struct PfsVol *v, int mode, ULONG need, ULONG ex_lo, ULONG ex_hi,
                     const ULONG *skip, ULONG nskip, ULONG *len)
{
    ULONG span = v->disksize - v->bitmapstart;
    ULONG li, run_start = 0, run_len = 0, best = 0, best_len = mode ? 0 : 0xFFFFFFFFUL;
    ULONG rlo = 0, rhi = 0;
    if (!v->bm_loaded || (mode == 0 && need == 0)) return 0;
    if (ex_hi > ex_lo && ex_hi > v->bitmapstart) {
        rlo = ex_lo > v->bitmapstart ? ex_lo - v->bitmapstart : 0;
        rhi = ex_hi - v->bitmapstart;
        if (rhi > span) rhi = span;
    }
#define END_RUN() do { \
        if (mode == 0) { if (run_len >= need && run_len < best_len) { best = run_start; best_len = run_len; } } \
        else if (run_len > best_len) { ULONG k; BOOL sk = FALSE; \
            for (k = 0; k < nskip; k++) if (skip[k] == run_start + v->bitmapstart) sk = TRUE; \
            if (!sk) { best = run_start; best_len = run_len; } } \
        run_len = 0; } while (0)
    for (li = 0; li < v->bm_longs && !(mode == 0 && best_len == need); li++) {
        ULONG w = v->bm[li], base = li * 32;
        if (rhi > rlo && base < rhi && base + 32 > rlo) {           /* long touches the zone */
            ULONG b0 = rlo > base ? rlo - base : 0, b1 = rhi < base + 32 ? rhi - base : 32, bit;
            for (bit = b0; bit < b1; bit++) w &= ~(0x80000000UL >> bit);
        }
        if (w == 0) { if (run_len) END_RUN(); continue; }
        if (w == 0xFFFFFFFFUL && base + 32 <= span) {
            if (run_len == 0) run_start = base;
            run_len += 32;
            continue;
        }
        {
            ULONG bit;
            for (bit = 0; bit < 32; bit++) {
                if (base + bit >= span) { if (run_len) END_RUN(); break; }
                if (w & (0x80000000UL >> bit)) { if (run_len == 0) run_start = base + bit; run_len++; }
                else if (run_len) END_RUN();
            }
        }
    }
    if (run_len) END_RUN();
#undef END_RUN
    if (mode == 0) return best_len != 0xFFFFFFFFUL ? best + v->bitmapstart : 0;
    if (len) *len = best_len;
    return best_len ? best + v->bitmapstart : 0;
}

ULONG pfs_bm_find_run(const struct PfsVol *v, ULONG need)
{
    return bm_scan(v, 0, need, 0, 0, NULL, 0, NULL);
}

ULONG pfs_bm_find_run_ex(const struct PfsVol *v, ULONG need, ULONG ex_lo, ULONG ex_hi)
{
    return bm_scan(v, 0, need, ex_lo, ex_hi, NULL, 0, NULL);
}

ULONG pfs_bm_largest_run(const struct PfsVol *v, const ULONG *skip, ULONG nskip, ULONG *len)
{
    return bm_scan(v, 1, 0, 0, 0, skip, nskip, len);
}

/* ------------------------------------------------------------------ */
/* directories                                                         */
/* ------------------------------------------------------------------ */

static void copy_name(char *out, ULONG outlen, const UBYTE *p, ULONG n)
{
    if (n >= outlen) n = outlen - 1;
    memcpy(out, p, n);
    out[n] = '\0';
}

/* Call cb for every entry of the directory whose chain starts at anodenr.
 * *sub collects the anodenrs of subdirectories. Returns FALSE to stop. */
static BOOL walk_dir(struct PfsVol *v, ULONG dirnr, PfsWalkCb cb, void *ud, struct UStack *sub, ULONG *blocks)
{
    ULONG a = dirnr, guard = 0;
    char name[112];
    while (a && guard++ < 100000UL) {
        ULONG cs, blk, next, off;
        if (!pfs_get_anode(v, a, &cs, &blk, &next)) { strncpy(v->err, "directory with unreachable anode", sizeof(v->err) - 1); return TRUE; }
        if (cs == 0 || blk == 0) { a = next; continue; }
        if (!pfs_res_read(v, blk, v->dbuf) || be16(v->dbuf, 0) != PFS_ID_DB) { strncpy(v->err, "directory block missing", sizeof(v->err) - 1); return TRUE; }
        (*blocks)++;
        for (off = 20; off + 22 <= v->resblksize; ) {
            UBYTE len  = v->dbuf[off];
            BYTE  type = (BYTE)v->dbuf[off + 1];
            ULONG an   = be32(v->dbuf, off + 2);
            ULONG nl   = v->dbuf[off + 17];
            if (len == 0) break;
            if (off + 18 + nl > v->resblksize) nl = 0;
            copy_name(name, sizeof(name), v->dbuf + off + 18, nl);
            if (!cb(ud, an, (LONG)type, name, dirnr)) return FALSE;
            if (type == PFS_ST_USERDIR && sub) { if (!ustack_push(sub, an)) return FALSE; }
            off += len;
        }
        if (!progress(v, *blocks, 0, "Reading directories")) return FALSE;
        a = next;
    }
    return TRUE;
}

BOOL pfs_walk(struct PfsVol *v, PfsWalkCb cb, void *ud, ULONG *dirs)
{
    struct UStack st;
    ULONG blocks = 0, n = 0;
    BOOL ok = TRUE;

    memset(&st, 0, sizeof(st));
    if (!ustack_push(&st, PFS_ANODE_ROOTDIR)) return FALSE;
    while (st.n > 0 && ok) {
        ULONG a = st.v[--st.n];
        n++;
        ok = walk_dir(v, a, cb, ud, &st, &blocks);
    }
    ustack_free(&st);
    if (dirs) *dirs = n;
    if (!ok) return FALSE;

    /* deldir: deleted files keep their anode chains and data blocks */
    if (v->rext) {
        ULONG ndd = be16(v->rext, 54), i;
        char name[20];
        if (ndd > 32) ndd = 32;
        for (i = 0; i < ndd; i++) {
            ULONG blk = be32(v->rext, 144 + 4 * i), e;
            if (!blk) continue;
            if (!pfs_res_read(v, blk, v->dbuf) || be16(v->dbuf, 0) != PFS_ID_DD) continue;
            for (e = 0; e < 31; e++) {
                ULONG o = 32 + 32 * e, an = be32(v->dbuf, o), nl = v->dbuf[o + 14];
                if (!an) continue;
                if (nl > 15) nl = 15;
                copy_name(name, sizeof(name), v->dbuf + o + 15, nl);
                if (!cb(ud, an, PFS_ST_DELETED, name, 0)) return FALSE;
            }
        }
    }
    return TRUE;
}

struct Lookup { const char *name; ULONG found; };

static UBYTE upc(UBYTE c) { return (c >= 'a' && c <= 'z') ? (UBYTE)(c - 32) : c; }

static BOOL lookup_cb(void *ud, ULONG anodenr, LONG type, const char *name, ULONG parent)
{
    struct Lookup *l = (struct Lookup *)ud;
    const char *a = l->name, *b = name;
    (void)parent;
    if (type != PFS_ST_FILE) return TRUE;
    while (*a && *b && upc((UBYTE)*a) == upc((UBYTE)*b)) { a++; b++; }
    if (*a == '\0' && *b == '\0') { l->found = anodenr; return FALSE; }
    return TRUE;
}

ULONG pfs_root_lookup(struct PfsVol *v, const char *name)
{
    struct Lookup l;
    ULONG blocks = 0;
    l.name = name; l.found = 0;
    walk_dir(v, PFS_ANODE_ROOTDIR, lookup_cb, &l, NULL, &blocks);
    return l.found;
}
