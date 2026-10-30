/*
 * sfs_map.c - block map of a Smart File System volume (SFS\0, SFS\2),
 * read-only.  Layouts from the SFS 1.279 sources (fs/blockstructure.h,
 * bitmap.h, adminspaces.h, btreenodes.h).
 *
 *   root block (block 0)     bheader 12 bytes, version @12, totalblocks @48,
 *                            blocksize @52, bitmapbase @96,
 *                            adminspacecontainer @100, rootobjectcontainer
 *                            @104, extentbnoderoot @108
 *   bitmap 'BTMP'            bits from @12, MSB first, 1 = free, one block
 *                            per bit; pages consecutive from bitmapbase
 *   admin space 'ADMC'       next @12, entries {space, bits} from @24: bit
 *                            31 = first block of the 32-block space, set =
 *                            in use.  All object containers, hash tables,
 *                            node containers and B-tree blocks live here.
 *   extent B-tree 'BNDC'     BTreeContainer @12: nodecount(UW) isleaf(UB)
 *                            nodesize(UB), nodes from @16.  Inner node
 *                            {key, child}; leaf node fsExtentBNode {key,
 *                            next, prev, blocks(UW)}.  A file's first
 *                            extent has prev = objectnode | 0x80000000;
 *                            next chains the rest of the file.
 *
 * Fragmentation falls straight out of the tree: an extent is a whole
 * contiguous file when it is a chain head with next == 0; every other
 * extent belongs to a fragmented file.  No directory walk is needed,
 * so m->dirs stays 0 here.
 */
#include "fsmap_int.h"

#define SFS_ID_BTMP 0x42544D50UL
#define SFS_ID_ADMC 0x41444D43UL
#define SFS_ID_BNDC 0x424E4443UL
#define SFS_ID_OBJC 0x4F424A43UL

struct Sfs {
    struct FsCtx *c;
    ULONG total, bsz, bitmapbase, adminspace, rootobj, extroot;
    BOOL  v2;
    ULONG depth;
    ULONG extents;
};

static BOOL sfs_checksum_ok(const UBYTE *b, ULONG bsz, BOOL v2)
{
    ULONG sum = 0, i;
    for (i = 0; i < bsz / 4; i++) sum += bel(b, i);
    return sum == (v2 ? 0xFFFFFFFEUL : 0xFFFFFFFFUL) || sum == 0;
}

static BOOL read_bitmap(struct Sfs *s, UBYTE *buf)
{
    struct FsCtx *c = s->c;
    struct FsMap *m = c->m;
    ULONG bits_per = (s->bsz - 12) * 8;
    ULONG pages = (s->total + bits_per - 1) / bits_per;
    ULONG page, used = 0;

    for (page = 0; page < pages; page++) {
        ULONG blk = s->bitmapbase + page, j;
        if (!fs_read(c, blk, buf) || bel(buf, 0) != SFS_ID_BTMP) {
            fs_note(c, "bitmap block missing or unreadable");
            return FALSE;
        }
        for (j = 0; j < bits_per / 32; j++) {
            ULONG word = bel(buf, 3 + j);
            ULONG base = page * bits_per + j * 32, bit;
            if (base >= s->total) break;
            if (word == 0xFFFFFFFFUL) continue;
            for (bit = 0; bit < 32 && base + bit < s->total; bit++)
                if (!(word & (0x80000000UL >> bit))) { FsMap_Set(m, base + bit, BLK_UNKN); used++; }
        }
        if (!fs_progress(c, page + 1, pages, "Reading bitmap")) return FALSE;
    }
    FsMap_SetRange(m, s->bitmapbase, pages, BLK_META);
    FsMap_Set(m, 0, BLK_META);
    FsMap_Set(m, s->total - 1, BLK_META);
    m->bitmap_free = s->total - used;
    return TRUE;
}

static BOOL read_adminspaces(struct Sfs *s, UBYTE *buf)
{
    struct FsCtx *c = s->c;
    struct FsMap *m = c->m;
    ULONG blk = s->adminspace, guard = 0;

    while (blk && blk < s->total && guard++ < 65536) {
        ULONG off;
        if (!fs_read(c, blk, buf) || bel(buf, 0) != SFS_ID_ADMC) {
            fs_note(c, "admin space chain broken");
            return TRUE;
        }
        FsMap_Set(m, blk, BLK_META);
        /* Every admin space is 32 blocks, all marked used in the main
         * bitmap; the ones without their bit set are reserved for future
         * containers, not lost. */
        for (off = 24; off + 8 <= s->bsz; off += 8) {
            ULONG space = be32(buf, off), bits = be32(buf, off + 4), k;
            if (space == 0) continue;
            for (k = 0; k < 32; k++)
                FsMap_Set(m, space + k, (bits & (0x80000000UL >> k)) ? BLK_META : BLK_RESV);
        }
        blk = be32(buf, 12);
    }
    return TRUE;
}

/* Recursive descent of the extent B-tree.  Depth is tiny (a few levels),
 * one block buffer per level. */
static BOOL walk_btree(struct Sfs *s, ULONG blk)
{
    struct FsCtx *c = s->c;
    struct FsMap *m = c->m;
    UBYTE *buf;
    ULONG n, i, nodesize;
    BOOL leaf, ok = TRUE;

    if (blk == 0 || blk >= s->total) return TRUE;
    if (++s->depth > 16) { s->depth--; fs_note(c, "extent tree too deep"); return TRUE; }
    buf = (UBYTE *)AllocVec(s->bsz, MEMF_PUBLIC);
    if (!buf) { s->depth--; return FALSE; }

    if (!fs_read(c, blk, buf) || bel(buf, 0) != SFS_ID_BNDC) {
        fs_note(c, "extent tree block missing or unreadable");
        goto out;
    }
    n        = be16(buf, 12);
    leaf     = buf[14] != 0;
    nodesize = buf[15];
    if (nodesize < 8 || 16 + n * nodesize > s->bsz) { fs_note(c, "extent tree node layout"); goto out; }

    for (i = 0; i < n && ok; i++) {
        ULONG off = 16 + i * nodesize;
        if (!leaf) {
            ok = walk_btree(s, be32(buf, off + 4));
        } else {
            ULONG key = be32(buf, off), next = be32(buf, off + 4), prev = be32(buf, off + 8);
            ULONG blocks = be16(buf, off + 12);
            BOOL  head = (prev & 0x80000000UL) != 0;
            UBYTE cls;
            if (key >= s->total || blocks == 0) { fs_note(c, "extent out of range"); continue; }
            if (head) {
                m->files++;
                if (next) { m->frag_files++; cls = BLK_FRAG; } else cls = BLK_DATA;
            } else {
                m->extra_runs++;
                cls = BLK_FRAG;
            }
            FsMap_SetRange(m, key, blocks, cls);
            s->extents++;
            if ((s->extents & 255) == 0 && !fs_progress(c, s->extents, 0, "Reading extents")) ok = FALSE;
        }
    }
out:
    FreeVec(buf);
    s->depth--;
    return ok;
}

BOOL SfsMap_Build(struct FsCtx *c)
{
    struct FsMap *m = c->m;
    struct Sfs s;
    UBYTE *buf;
    BOOL ok = FALSE;

    memset(&s, 0, sizeof(s));
    s.c = c;
    buf = (UBYTE *)AllocVec(c->blocksize, MEMF_PUBLIC);
    if (!buf) { fs_note(c, "out of memory"); return FALSE; }

    if (!fs_read(c, 0, buf)) { fs_note(c, "cannot read root block"); goto done; }
    if ((bel(buf, 0) & 0xFFFFFF00UL) != 0x53465300UL) { fs_note(c, "no SFS root block"); goto done; }
    s.v2         = (buf[3] == 2);
    s.total      = be32(buf, 48);
    s.bsz        = be32(buf, 52);
    s.bitmapbase = be32(buf, 96);
    s.adminspace = be32(buf, 100);
    s.rootobj    = be32(buf, 104);
    s.extroot    = be32(buf, 108);

    if (s.bsz != c->blocksize) {
        snprintf(m->note, sizeof(m->note), "SFS block size %lu differs from the mount's %lu",
                 (unsigned long)s.bsz, (unsigned long)c->blocksize);
        goto done;
    }
    if (s.total == 0 || s.total > m->nblocks) {
        snprintf(m->note, sizeof(m->note), "SFS claims %lu blocks, partition has %lu",
                 (unsigned long)s.total, (unsigned long)m->nblocks);
        goto done;
    }
    if (!sfs_checksum_ok(buf, s.bsz, s.v2)) fs_note(c, "root block checksum mismatch");
    if (s.v2) fs_note(c, "SFS\\2 extent layout not yet verified against a real volume");

    /* Volume name: root object in the root object container, name at
     * object offset 24 (+2 for SFS\2's wider object header). */
    if (s.rootobj < s.total && fs_read(c, s.rootobj, buf) && bel(buf, 0) == SFS_ID_OBJC) {
        const UBYTE *nm = buf + 24 + 24 + (s.v2 ? 2 : 0) + 1;   /* fsObject + bits */
        ULONG n = 0;
        while (n < 30 && nm[n]) n++;
        memcpy(m->volname, nm, n);
        m->volname[n] = '\0';
        m->bitmap_free = be32(buf, s.bsz - 28);                 /* fsRootInfo.freeblocks */
    }

    /* Blocks past totalblocks (partition slack) are reserved, not free. */
    if (s.total < m->nblocks) FsMap_SetRange(m, s.total, m->nblocks - s.total, BLK_RESV);

    {
        ULONG claimed = m->bitmap_free;
        if (!read_bitmap(&s, buf)) goto done;
        if (claimed && claimed != m->bitmap_free)
            fs_note(c, "free count in root differs from the bitmap");
    }
    if (!read_adminspaces(&s, buf)) goto done;
    /* Blocks between the root block and the first admin space are the
     * DE_RESERVED area SFS keeps unused but allocated. */
    if (s.adminspace > 1 && s.adminspace < s.total) FsMap_SetRange(m, 1, s.adminspace - 1, BLK_RESV);
    if (!walk_btree(&s, s.extroot)) goto done;
    if (c->aborted) goto done;
    ok = TRUE;

done:
    FreeVec(buf);
    return ok;
}
