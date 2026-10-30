/*
 * ffs_map.c - block map of an OFS/FFS volume (DOS\0 .. DOS\7), read-only.
 *
 * Block layout (all big-endian longwords, nlongs = blocksize / 4):
 *
 *   header blocks   [0] type (T_SHORT=2 header, T_LIST=16 extension,
 *                   T_DIRCACHE=33), [1] own key, [2] high_seq = number of
 *                   valid data pointers, [5] checksum (all longs sum to 0),
 *                   [6 .. nlongs-51] the hash table (directories) or the
 *                   data block table (files), filled from the END: data
 *                   block k of the file is at long nlongs-51-k.
 *                   Tail, counted from the end: [-4] hash_chain, [-3]
 *                   parent, [-2] extension (files: next T_LIST block;
 *                   dirs: dircache block), [-1] sec_type.
 *   root block      same, plus [-50] bm_flag (-1 = bitmap valid),
 *                   [-49..-25] bm_pages[25], [-24] bm_ext.
 *   bitmap block    [0] checksum, [1..] bits, LSB first, 1 = free; the
 *                   first bit is block `reserved` (the boot blocks are
 *                   outside the bitmap).  Each page covers (nlongs-1)*32
 *                   blocks.  bm_ext blocks hold nlongs-1 more page
 *                   pointers and the next bm_ext in the last long.
 *   dircache block  (DOS\4/5) [0] 33, [4] next dircache, chained from the
 *                   directory's extension field.
 *
 * Fragmentation of a file = its data block numbers, in order, broken
 * into runs of consecutive blocks.  One run = contiguous.
 */
#include "fsmap_int.h"

#define T_SHORT      2UL
#define T_LIST       16UL
#define T_DIRCACHE   33UL
#define ST_ROOT      1L
#define ST_USERDIR   2L
#define ST_SOFTLINK  3L
#define ST_LINKDIR   4L
#define ST_FILE      (-3L)
#define ST_LINKFILE  (-4L)

struct Ffs {
    struct FsCtx *c;
    ULONG  ref_free;         /* referenced blocks the bitmap says are free */
    ULONG  nlongs;
    ULONG  reserved;
    ULONG  root;
    ULONG  used_blocks;      /* from the bitmap: progress denominator */
    ULONG  visited;
    UBYTE *dirbuf, *hdrbuf, *extbuf;
    BOOL   dircache_fs;
};

/* Classify a referenced block; a block the bitmap holds as free is a
 * real inconsistency (lost structure or stale bitmap) and is counted. */
static void mark_blk(struct Ffs *f, ULONG blk, UBYTE cls)
{
    if (blk >= f->c->m->nblocks) return;
    if (FsMap_Get(f->c->m, blk) == BLK_FREE && blk >= f->reserved) f->ref_free++;
    FsMap_Set(f->c->m, blk, cls);
}

static BOOL ffs_checksum_ok(const UBYTE *b, ULONG nlongs)
{
    ULONG sum = 0, i;
    for (i = 0; i < nlongs; i++) sum += bel(b, i);
    return sum == 0;
}

static inline LONG tail(const UBYTE *b, ULONG nlongs, ULONG from_end)
{
    return (LONG)bel(b, nlongs - from_end);
}

static BOOL is_header(const UBYTE *b, ULONG nlongs, ULONG type, LONG sec)
{
    return bel(b, 0) == type && tail(b, nlongs, 1) == sec &&
           ffs_checksum_ok(b, nlongs);
}

/* --- bitmap -------------------------------------------------------- */

/* Visit every bitmap page: pass 0 decodes the bits, pass 1 (after all
 * decoding, so the decode cannot overwrite it) marks the pages and the
 * bm_ext blocks themselves as system blocks. */
static BOOL bitmap_pass(struct Ffs *f, int pass, ULONG pages)
{
    struct FsCtx *c = f->c;
    struct FsMap *m = c->m;
    ULONG nl = f->nlongs;
    ULONG bpb = (nl - 1) * 32;
    ULONG page, ext = 0, ext_slot = 0, bad = 0;
    UBYTE *bm = f->hdrbuf, *extb = f->extbuf;
    const UBYTE *root = f->dirbuf;

    ext = (ULONG)tail(root, nl, 24);
    for (page = 0; page < pages; page++) {
        ULONG ptr, j, base;
        if (page < 25) {
            ptr = (ULONG)tail(root, nl, 49 - page);
        } else {
            if (page == 25 || ext_slot == nl - 1) {
                if (page != 25) ext = bel(extb, nl - 1);
                if (ext == 0 || ext >= m->nblocks || !fs_read(c, ext, extb)) {
                    fs_note(c, "bitmap extension chain ends early");
                    break;
                }
                if (pass == 1) FsMap_Set(m, ext, BLK_META);
                ext_slot = 0;
            }
            ptr = bel(extb, ext_slot++);
        }
        if (ptr == 0 || ptr >= m->nblocks) { fs_note(c, "bitmap page pointer missing"); break; }
        if (pass == 1) { FsMap_Set(m, ptr, BLK_META); continue; }

        if (!fs_read(c, ptr, bm)) { fs_note(c, "bitmap page unreadable"); break; }
        if (!ffs_checksum_ok(bm, nl)) bad++;
        base = f->reserved + page * bpb;
        for (j = 1; j < nl; j++) {
            ULONG word = bel(bm, j);
            ULONG blk  = base + (j - 1) * 32;
            ULONG bit;
            if (blk >= m->nblocks) break;
            if (word == 0xFFFFFFFFUL) continue;                   /* all free */
            for (bit = 0; bit < 32 && blk + bit < m->nblocks; bit++) {
                if (!(word & (1UL << bit))) {
                    FsMap_Set(m, blk + bit, BLK_UNKN);
                    f->used_blocks++;
                }
            }
        }
        if (!fs_progress(c, page + 1, pages, "Reading bitmap")) return FALSE;
    }
    if (bad) fs_note(c, "bitmap block(s) with bad checksum");
    return TRUE;
}

static BOOL read_bitmap(struct Ffs *f)
{
    struct FsMap *m = f->c->m;
    ULONG nl = f->nlongs;
    ULONG bpb = (nl - 1) * 32;
    ULONG span = m->nblocks > f->reserved ? m->nblocks - f->reserved : 0;
    ULONG pages = (span + bpb - 1) / bpb;

    m->bitmap_valid = (tail(f->dirbuf, nl, 50) == -1L);
    if (!bitmap_pass(f, 0, pages)) return FALSE;
    if (!bitmap_pass(f, 1, pages)) return FALSE;
    m->bitmap_free = span - f->used_blocks;
    return TRUE;
}

/* --- files --------------------------------------------------------- */

/* Walk the data pointers of the file whose header is in f->hdrbuf.
 * mark < 0: count runs only.  mark >= 0: classify every data block as
 * mark and every extension block as META.  Returns the run count. */
static ULONG walk_file(struct Ffs *f, LONG mark, ULONG *nblk)
{
    struct FsCtx *c = f->c;
    struct FsMap *m = c->m;
    ULONG nl = f->nlongs;
    const UBYTE *b = f->hdrbuf;
    ULONG runs = 0, prev = 0xFFFFFFFFUL, count = 0, guard = 0;
    ULONG own = 0xFFFFFFFFUL;    /* block holding the table being read */

    for (;;) {
        ULONG high = bel(b, 2), k;
        if (high > nl - 56) high = nl - 56;
        for (k = 0; k < high; k++) {
            ULONG p = bel(b, nl - 51 - k);
            if (p == 0 || p >= m->nblocks) { fs_note(c, "file with data pointer out of range"); goto out; }
            /* FFS drops the next extension block right between two data
             * blocks of the same file; that one-block hop is no seek, so
             * it does not count as a fragment. */
            if (prev == 0xFFFFFFFFUL) runs++;
            else if (p != prev + 1 && !(p == prev + 2 && prev + 1 == own)) runs++;
            prev = p;
            count++;
            if (mark >= 0) mark_blk(f, p, (UBYTE)mark);
        }
        {
            ULONG ext = (ULONG)tail(b, nl, 2);
            if (ext == 0) break;
            if (ext >= m->nblocks || ++guard > m->nblocks ||
                !fs_read(c, ext, f->extbuf) || bel(f->extbuf, 0) != T_LIST) {
                fs_note(c, "broken file extension chain");
                break;
            }
            if (mark >= 0) mark_blk(f, ext, BLK_META);
            b = f->extbuf;
            own = ext;
        }
    }
out:
    if (nblk) *nblk = count;
    return runs;
}

static void account_file(struct Ffs *f)
{
    struct FsMap *m = f->c->m;
    ULONG nblk = 0;
    ULONG runs = walk_file(f, -1, &nblk);
    if (nblk == 0) { m->files++; return; }
    m->files++;
    if (runs > 1) {
        m->frag_files++;
        m->extra_runs += runs - 1;
        if (runs > m->worst_runs) m->worst_runs = runs;
    }
    walk_file(f, runs > 1 ? BLK_FRAG : BLK_DATA, NULL);
    f->visited += nblk;
}

/* --- directories --------------------------------------------------- */

static void follow_dircache(struct Ffs *f, ULONG blk)
{
    struct FsCtx *c = f->c;
    ULONG guard = 0;
    while (blk && blk < c->m->nblocks && guard++ < 4096) {
        if (FsMap_Get(c->m, blk) == BLK_META) break;
        if (!fs_read(c, blk, f->extbuf) || bel(f->extbuf, 0) != T_DIRCACHE) break;
        mark_blk(f, blk, BLK_META);
        blk = bel(f->extbuf, 4);
    }
}

static BOOL walk_directories(struct Ffs *f)
{
    struct FsCtx *c = f->c;
    struct FsMap *m = c->m;
    ULONG nl = f->nlongs;
    struct UStack st;
    BOOL ok = TRUE;

    memset(&st, 0, sizeof(st));
    if (!ustack_push(&st, f->root)) return FALSE;

    while (st.n > 0 && ok) {
        ULONG dir = st.v[--st.n];
        ULONG slot;

        if (!fs_read(c, dir, f->dirbuf)) { fs_note(c, "unreadable directory block"); continue; }
        if (bel(f->dirbuf, 0) != T_SHORT ||
            (tail(f->dirbuf, nl, 1) != ST_ROOT && tail(f->dirbuf, nl, 1) != ST_USERDIR)) {
            fs_note(c, "directory block with bad type");
            continue;
        }
        mark_blk(f, dir, BLK_META);
        m->dirs++;
        f->visited++;
        if (f->dircache_fs) follow_dircache(f, (ULONG)tail(f->dirbuf, nl, 2));

        for (slot = 0; slot < nl - 56 && ok; slot++) {
            ULONG blk = bel(f->dirbuf, 6 + slot);
            ULONG guard = 0;
            while (blk != 0) {
                LONG sec;
                if (blk >= m->nblocks) { fs_note(c, "hash chain points outside the volume"); break; }
                if (FsMap_Get(m, blk) == BLK_META) break;          /* loop / revisit */
                if (++guard > m->nblocks) break;
                if (!fs_read(c, blk, f->hdrbuf)) { fs_note(c, "unreadable header block"); break; }
                if (bel(f->hdrbuf, 0) != T_SHORT || !ffs_checksum_ok(f->hdrbuf, nl)) {
                    fs_note(c, "header block with bad checksum");
                    break;
                }
                mark_blk(f, blk, BLK_META);
                f->visited++;
                sec = tail(f->hdrbuf, nl, 1);
                {
                    ULONG next = (ULONG)tail(f->hdrbuf, nl, 4);
                    if (sec == ST_USERDIR) {
                        if (!ustack_push(&st, blk)) { ok = FALSE; break; }
                    } else if (sec == ST_FILE) {
                        account_file(f);
                    } /* links and softlinks: the header is all there is */
                    blk = next;
                }
                if ((f->visited & 63) == 0 &&
                    !fs_progress(c, f->visited, f->used_blocks, "Reading directories")) {
                    ok = FALSE; break;
                }
            }
        }
    }
    ustack_free(&st);
    return ok;
}

/* --- entry --------------------------------------------------------- */

BOOL FfsMap_Build(struct FsCtx *c)
{
    struct FsMap *m = c->m;
    struct Ffs f;
    BOOL ok = FALSE;

    memset(&f, 0, sizeof(f));
    f.c        = c;
    f.nlongs   = c->blocksize / 4;
    f.reserved = c->v->reserved ? c->v->reserved : 2;
    f.dircache_fs = ((c->v->dostype & 7) == 4 || (c->v->dostype & 7) == 5);

    if (f.nlongs < 128 || f.nlongs > 8192) { fs_note(c, "unsupported FFS block size"); return FALSE; }

    f.dirbuf = (UBYTE *)AllocVec(c->blocksize * 3, MEMF_PUBLIC);
    if (!f.dirbuf) { fs_note(c, "out of memory"); return FALSE; }
    f.hdrbuf = f.dirbuf + c->blocksize;
    f.extbuf = f.hdrbuf + c->blocksize;

    /* Locate the root block: the FFS formula first, the boot block's
     * pointer second (floppies and some old tools leave it stale). */
    f.root = (m->nblocks - 1 + f.reserved) / 2;
    if (!fs_read(c, f.root, f.dirbuf) || !is_header(f.dirbuf, f.nlongs, T_SHORT, ST_ROOT)) {
        ULONG alt;
        if (!fs_read(c, 0, f.hdrbuf)) { fs_note(c, "cannot read boot block"); goto done; }
        alt = bel(f.hdrbuf, 2);
        if (alt == 0 || alt >= m->nblocks || !fs_read(c, alt, f.dirbuf) ||
            !is_header(f.dirbuf, f.nlongs, T_SHORT, ST_ROOT)) {
            fs_note(c, "no valid root block found");
            goto done;
        }
        f.root = alt;
    }

    /* Volume name: BCPL string at long -20 (nlongs-20), max 30 chars. */
    {
        const UBYTE *nm = f.dirbuf + (f.nlongs - 20) * 4;
        ULONG n = nm[0];
        if (n > 30) n = 30;
        memcpy(m->volname, nm + 1, n);
        m->volname[n] = '\0';
    }

    FsMap_SetRange(m, 0, f.reserved, BLK_META);          /* boot blocks */
    if (!read_bitmap(&f)) goto done;
    if (!walk_directories(&f)) goto done;
    if (f.ref_free) {
        snprintf(m->note, sizeof(m->note), "%lu block(s) in use but free in the bitmap - validate this volume",
                 (unsigned long)f.ref_free);
    }
    ok = TRUE;

done:
    FreeVec(f.dirbuf);
    return ok;
}
