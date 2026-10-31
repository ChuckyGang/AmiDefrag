/*
 * pfs_map.c - block map of a Professional File System 3 volume, read-only.
 * Volume access and layouts live in pfs_core.c; this file only classifies.
 *
 * Reserved area: every reserved block is META when the reserved bitmap
 * says used, RESV when free.  Data area: bitmap-used blocks start as
 * UNKN and become DATA / FRAG when a file's anode chain reaches them, so
 * whatever stays UNKN is marked in use but referenced by nothing we walk
 * (deleted files in the deldir are walked too).
 */
#include "fsmap_int.h"
#include "pfs_core.h"

struct PfsMapCtx {
    struct FsCtx  *c;
    struct PfsVol *v;
    ULONG visited, used_blocks;
};

static void res_mark(struct PfsMapCtx *p, ULONG nr, UBYTE cls)
{
    FsMap_SetRange(p->c->m, nr, p->v->rescluster, cls);
}

static BOOL read_reserved_bitmap(struct PfsMapCtx *p)
{
    struct PfsVol *v = p->v;
    ULONG numres = (v->lastres - v->firstres + 1) / v->rescluster;
    const UBYTE *bm = v->root + 512;
    ULONG avail_bits = (v->rblkcluster * 512 - 512 - 12) * 8;
    ULONG i;
    if (be16(bm, 0) != PFS_ID_BM) { fs_note(p->c, "reserved bitmap not found"); return FALSE; }
    for (i = 0; i < numres && i < avail_bits; i++) {
        ULONG word = be32(bm, 12 + 4 * (i / 32));
        BOOL  fr   = (word & (0x80000000UL >> (i % 32))) != 0;
        res_mark(p, v->firstres + i * v->rescluster, fr ? BLK_RESV : BLK_META);
    }
    return TRUE;
}

/* data bitmap: every used sector starts as UNKN */
static void mark_data_bitmap(struct PfsMapCtx *p)
{
    struct PfsVol *v = p->v;
    struct FsMap *m = p->c->m;
    ULONG nbits = v->disksize - v->bitmapstart, li, used = 0;
    for (li = 0; li < v->bm_longs; li++) {
        ULONG w = v->bm[li], base = v->bitmapstart + li * 32, bit;
        if (w == 0xFFFFFFFFUL) continue;
        for (bit = 0; bit < 32 && li * 32 + bit < nbits; bit++)
            if (!(w & (0x80000000UL >> bit))) { FsMap_Set(m, base + bit, BLK_UNKN); used++; }
    }
    p->used_blocks = used;
    m->bitmap_free = nbits - used;
}

/* Walk a file's anode chain: pass 1 counts clusters, pass 2 marks. */
static void account_file(struct PfsMapCtx *p, ULONG anodenr)
{
    struct PfsVol *v = p->v;
    struct FsMap *m = p->c->m;
    ULONG runs = 0, a = anodenr, guard = 0, cs, blk, next;
    UBYTE cls;

    while (a && guard++ < 1000000UL) {
        if (!pfs_get_anode(v, a, &cs, &blk, &next)) { fs_note(p->c, "file with unreachable anode"); break; }
        if (cs) runs++;
        a = next;
    }
    m->files++;
    if (runs > 1) { m->frag_files++; m->extra_runs += runs - 1; if (runs > m->worst_runs) m->worst_runs = runs; }
    cls = runs > 1 ? BLK_FRAG : BLK_DATA;

    a = anodenr; guard = 0;
    while (a && guard++ < 1000000UL) {
        if (!pfs_get_anode(v, a, &cs, &blk, &next)) break;
        if (cs) {
            if (blk < v->bitmapstart || blk + cs > v->disksize) { fs_note(p->c, "file data outside the data area"); break; }
            FsMap_SetRange(m, blk, cs, cls);
            p->visited += cs;
        }
        a = next;
    }
}

static BOOL walk_cb(void *ud, ULONG anodenr, LONG type, const char *name, ULONG parent)
{
    struct PfsMapCtx *p = (struct PfsMapCtx *)ud;
    (void)name; (void)parent;
    if (type == PFS_ST_FILE || type == PFS_ST_ROLLOVER) account_file(p, anodenr);
    /* directories are walked by the core; links own no data blocks;
     * deleted files keep their blocks, which stay UNKN on the map */
    return !p->c->aborted;
}

/* the core reports directory blocks read; the map shows data blocks
 * reached against data blocks in use instead */
static BOOL progress_cb(void *ud, ULONG done, ULONG total, const char *what)
{
    struct PfsMapCtx *p = (struct PfsMapCtx *)ud;
    if (total == 0) return fs_progress(p->c, p->visited, p->used_blocks, what);
    return fs_progress(p->c, done, total, what);
}

BOOL PfsMap_Build(struct FsCtx *c)
{
    struct FsMap *m = c->m;
    struct PfsMapCtx p;
    struct PfsVol v;
    ULONG dirs = 0;
    BOOL ok = FALSE;

    memset(&p, 0, sizeof(p));
    memset(&v, 0, sizeof(v));
    p.c = c; p.v = &v;
    v.cb = progress_cb; v.cb_ud = &p;
    if (!pfs_open(&v, c->bd, c->v)) { fs_note(c, v.err); return FALSE; }
    {
        ULONG n = v.root[20];
        if (n > 31) n = 31;
        memcpy(m->volname, v.root + 21, n);
        m->volname[n] = '\0';
    }
    m->bitmap_valid = TRUE;
    if (!v.rext && (v.options & PFS_MODE_SUPERINDEX)) fs_note(c, "rootblock extension missing (needed for superindex)");

    FsMap_SetRange(m, 0, 2, BLK_META);                                /* boot */
    FsMap_SetRange(m, 2, v.rblkcluster, BLK_META);                    /* rootblock cluster */
    if (v.disksize < m->nblocks) FsMap_SetRange(m, v.disksize, m->nblocks - v.disksize, BLK_RESV);
    if (!read_reserved_bitmap(&p)) goto done;
    FsMap_SetRange(m, 2, v.rblkcluster, BLK_META);                    /* (bitmap covers it too) */
    if (!pfs_bm_load(&v)) { fs_note(c, v.err); goto done; }
    mark_data_bitmap(&p);
    if (!pfs_walk(&v, walk_cb, &p, &dirs)) goto done;
    if (v.err[0]) fs_note(c, v.err);
    m->dirs = dirs;
    if (c->aborted) goto done;
    ok = TRUE;

done:
    pfs_close(&v);
    return ok;
}
