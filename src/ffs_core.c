/*
 * ffs_core.c - see ffs_core.h.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include "clib.h"
#include "be.h"
#include "ffs_core.h"
#include "fsmap_int.h"      /* UStack */

static void putl(UBYTE *b, ULONG longidx, ULONG v)
{
    b[longidx * 4] = (UBYTE)(v >> 24); b[longidx * 4 + 1] = (UBYTE)(v >> 16);
    b[longidx * 4 + 2] = (UBYTE)(v >> 8); b[longidx * 4 + 3] = (UBYTE)v;
}

BOOL ffs_read(struct FfsVol *v, ULONG blk, ULONG count, void *buf)
{
    if (blk >= v->nblocks || count > v->nblocks - blk) return FALSE;
    return BlockDev_ReadBlocks(v->bd, v->part_start + blk * v->spb, count * v->spb, buf);
}

BOOL ffs_write(struct FfsVol *v, ULONG blk, ULONG count, const void *buf)
{
    if (blk >= v->nblocks || count > v->nblocks - blk) return FALSE;
    return BlockDev_WriteBlocks(v->bd, v->part_start + blk * v->spb, count * v->spb, buf);
}

void ffs_set_checksum(UBYTE *block, ULONG nlongs, ULONG at)
{
    ULONG sum = 0, i;
    putl(block, at, 0);
    for (i = 0; i < nlongs; i++) sum += bel(block, i);
    putl(block, at, (ULONG)(0UL - sum));
}

BOOL ffs_checksum_ok(const UBYTE *block, ULONG nlongs)
{
    ULONG sum = 0, i;
    for (i = 0; i < nlongs; i++) sum += bel(block, i);
    return sum == 0;
}

BOOL ffs_is_header(const UBYTE *b, ULONG nlongs, ULONG type, LONG sectype)
{
    return bel(b, 0) == type && (LONG)bel(b, nlongs - FFS_E_SECTYPE) == sectype &&
           ffs_checksum_ok(b, nlongs);
}

/* --- bitmap -------------------------------------------------------- */

BOOL ffs_bm_isfree(const struct FfsVol *v, ULONG blk)
{
    ULONG i;
    if (blk < v->reserved || blk >= v->nblocks) return FALSE;
    i = blk - v->reserved;
    return (v->bm[i >> 5] >> (i & 31)) & 1;
}

void ffs_bm_set(struct FfsVol *v, ULONG blk, BOOL isfree)
{
    ULONG i, mask;
    BOOL was;
    if (blk < v->reserved || blk >= v->nblocks) return;
    i = blk - v->reserved;
    mask = 1UL << (i & 31);
    was = (v->bm[i >> 5] & mask) != 0;
    if (was == (isfree != FALSE)) return;
    if (isfree) { v->bm[i >> 5] |= mask;  v->free_blocks++; v->used_blocks--; }
    else        { v->bm[i >> 5] &= ~mask; v->free_blocks--; v->used_blocks++; }
    v->page_dirty[i / v->bpb] = 1;
}

ULONG ffs_bm_find_run(const struct FfsVol *v, ULONG need)
{
    /* Best fit over the RAM bitmap, a longword at a time: fully used words
     * end a run at once, fully free words extend it by 32, only mixed
     * words are walked bit by bit.  Bits past the volume end are clear. */
    ULONG span = v->nblocks - v->reserved;
    ULONG li, run_start = 0, run_len = 0, best = 0, best_len = 0xFFFFFFFFUL;
    if (need == 0) return 0;
#define END_RUN() do { if (run_len >= need && run_len < best_len) { best = run_start; best_len = run_len; } run_len = 0; } while (0)
    for (li = 0; li < v->bm_longs && best_len != need; li++) {
        ULONG w = v->bm[li], base = li * 32;
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
                if (w & (1UL << bit)) { if (run_len == 0) run_start = base + bit; run_len++; }
                else if (run_len) END_RUN();
            }
        }
    }
    if (run_len) END_RUN();
#undef END_RUN
    return best_len != 0xFFFFFFFFUL ? best + v->reserved : 0;
}

/* Rebuild page `page` from the RAM bitmap and write it. */
static BOOL write_page(struct FfsVol *v, ULONG page)
{
    UBYTE *b = v->scratch;
    ULONG  j, first_long = page * (v->nlongs - 1);
    memset(b, 0, v->bsz);
    for (j = 1; j < v->nlongs; j++) {
        ULONG li = first_long + j - 1;
        putl(b, j, li < v->bm_longs ? v->bm[li] : 0xFFFFFFFFUL);
    }
    ffs_set_checksum(b, v->nlongs, 0);
    return ffs_write(v, v->page_key[page], 1, b);
}

BOOL ffs_bm_flush(struct FfsVol *v)
{
    ULONG p;
    for (p = 0; p < v->pages; p++) {
        if (!v->page_dirty[p]) continue;
        if (!write_page(v, p)) { strncpy(v->err, "bitmap page write failed", sizeof(v->err) - 1); return FALSE; }
        v->page_dirty[p] = 0;
    }
    return TRUE;
}

BOOL ffs_write_root_flag(struct FfsVol *v, BOOL valid)
{
    putl(v->rootbuf, v->nlongs - FFS_E_BMFLAG, valid ? 0xFFFFFFFFUL : 0);
    ffs_set_checksum(v->rootbuf, v->nlongs, FFS_L_CHECKSUM);
    if (!ffs_write(v, v->root, 1, v->rootbuf)) { strncpy(v->err, "root block write failed", sizeof(v->err) - 1); return FALSE; }
    v->bm_flag_valid = valid;
    return TRUE;
}

/* Read all bitmap pages into the RAM copy; records page and ext keys. */
static BOOL load_bitmap(struct FfsVol *v)
{
    ULONG nl = v->nlongs, span = v->nblocks - v->reserved;
    ULONG page, ext_slot = 0, ext = 0, li = 0;
    UBYTE *extb = NULL;

    v->bpb      = (nl - 1) * 32;
    v->pages    = (span + v->bpb - 1) / v->bpb;
    v->bm_longs = (span + 31) / 32;
    v->bm         = (ULONG *)AllocVec(v->bm_longs * 4 + 4, MEMF_PUBLIC | MEMF_CLEAR);
    v->page_key   = (ULONG *)AllocVec(v->pages * 4 + 4, MEMF_PUBLIC | MEMF_CLEAR);
    v->page_dirty = (UBYTE *)AllocVec(v->pages + 1, MEMF_PUBLIC | MEMF_CLEAR);
    v->ext_key    = (ULONG *)AllocVec((v->pages / (nl - 1) + 2) * 4, MEMF_PUBLIC | MEMF_CLEAR);
    extb          = (UBYTE *)AllocVec(v->bsz, MEMF_PUBLIC);
    if (!v->bm || !v->page_key || !v->page_dirty || !v->ext_key || !extb) {
        if (extb) FreeVec(extb);
        strncpy(v->err, "out of memory for the bitmap", sizeof(v->err) - 1);
        return FALSE;
    }
    v->bm_flag_valid = (bel(v->rootbuf, nl - FFS_E_BMFLAG) == 0xFFFFFFFFUL);
    ext = bel(v->rootbuf, nl - FFS_E_BMEXT);

    for (page = 0; page < v->pages; page++) {
        ULONG key, j;
        if (page < 25) {
            key = bel(v->rootbuf, nl - FFS_E_BMPAGES + page);
        } else {
            if (page == 25 || ext_slot == nl - 1) {
                if (page != 25) ext = bel(extb, nl - 1);
                if (ext == 0 || ext >= v->nblocks || !ffs_read(v, ext, 1, extb)) {
                    strncpy(v->err, "bitmap extension chain broken", sizeof(v->err) - 1);
                    FreeVec(extb); return FALSE;
                }
                v->ext_key[v->ext_count++] = ext;
                ext_slot = 0;
            }
            key = bel(extb, ext_slot++);
        }
        if (key == 0 || key >= v->nblocks || !ffs_read(v, key, 1, v->scratch) ||
            !ffs_checksum_ok(v->scratch, nl)) {
            /* With the flag invalid the bitmap is going to be rebuilt by the
             * filesystem anyway (a torn page write at the end of a run leaves
             * exactly this), so the volume still opens; the engines refuse
             * to write until it is validated. */
            if (!v->bm_flag_valid && key != 0 && key < v->nblocks) {
                v->page_key[page] = key;
                for (j = 1; j < nl && li < v->bm_longs; j++) v->bm[li++] = 0;   /* "all used": nothing is handed out */
                continue;
            }
            snprintf(v->err, sizeof(v->err), "bitmap page %lu unreadable or bad checksum", (unsigned long)page);
            FreeVec(extb); return FALSE;
        }
        v->page_key[page] = key;
        for (j = 1; j < nl && li < v->bm_longs; j++) v->bm[li++] = bel(v->scratch, j);
    }
    FreeVec(extb);

    /* bits past the end of the volume must read as "not free" */
    if (span & 31) v->bm[v->bm_longs - 1] &= (1UL << (span & 31)) - 1;
    {
        ULONG i, freec = 0;
        for (i = 0; i < v->bm_longs; i++) {
            ULONG w = v->bm[i];
            while (w) { freec += w & 1; w >>= 1; }
        }
        v->free_blocks = freec;
        v->used_blocks = span - freec;
    }
    return TRUE;
}

BOOL ffs_open(struct FfsVol *v, struct BlockDev *bd, const struct VolumeInfo *vi)
{
    memset(v, 0, sizeof(*v));
    v->bd         = bd;
    v->part_start = vi->start_block;
    v->spb        = vi->spb ? vi->spb : 1;
    v->bsz        = vi->sector_size * v->spb;
    v->nlongs     = v->bsz / 4;
    v->nblocks    = vi->num_blocks / v->spb;
    v->reserved   = vi->reserved ? vi->reserved : 2;
    v->tsize      = v->nlongs - 56;
    v->ofs        = (vi->dostype & 1) == 0;
    v->dircache   = ((vi->dostype & 7) == 4 || (vi->dostype & 7) == 5);
    v->intl       = (vi->dostype & 7) >= 2;
    if (v->nlongs < 128 || v->nlongs > 8192 || v->nblocks <= v->reserved + 1) {
        strncpy(v->err, "unsupported block size or empty partition", sizeof(v->err) - 1);
        return FALSE;
    }
    v->rootbuf = (UBYTE *)AllocVec(v->bsz * 2, MEMF_PUBLIC);
    if (!v->rootbuf) { strncpy(v->err, "out of memory", sizeof(v->err) - 1); return FALSE; }
    v->scratch = v->rootbuf + v->bsz;

    v->root = (v->nblocks - 1 + v->reserved) / 2;
    if (!ffs_read(v, v->root, 1, v->rootbuf) || !ffs_is_header(v->rootbuf, v->nlongs, FFS_T_SHORT, FFS_ST_ROOT)) {
        ULONG alt = 0;
        if (ffs_read(v, 0, 1, v->scratch)) alt = bel(v->scratch, 2);
        if (alt == 0 || alt >= v->nblocks || !ffs_read(v, alt, 1, v->rootbuf) ||
            !ffs_is_header(v->rootbuf, v->nlongs, FFS_T_SHORT, FFS_ST_ROOT)) {
            strncpy(v->err, "no valid root block", sizeof(v->err) - 1);
            ffs_close(v); return FALSE;
        }
        v->root = alt;
    }
    if (!load_bitmap(v)) { ffs_close(v); return FALSE; }
    return TRUE;
}

void ffs_close(struct FfsVol *v)
{
    if (v->rootbuf)    FreeVec(v->rootbuf);
    if (v->bm)         FreeVec(v->bm);
    if (v->page_key)   FreeVec(v->page_key);
    if (v->page_dirty) FreeVec(v->page_dirty);
    if (v->ext_key)    FreeVec(v->ext_key);
    v->rootbuf = NULL; v->bm = NULL; v->page_key = NULL; v->page_dirty = NULL; v->ext_key = NULL;
}

/* --- files --------------------------------------------------------- */

BOOL ffs_file_layout(struct FfsVol *v, const UBYTE *hdr, ULONG *ptrs, ULONG max,
                     ULONG *exts, ULONG maxext, ULONG *nblk, ULONG *runs, ULONG *next)
{
    UBYTE *ext = (UBYTE *)AllocVec(v->bsz, MEMF_PUBLIC);
    const UBYTE *b = hdr;
    ULONG count = 0, nruns = 0, next_cnt = 0, prev = 0xFFFFFFFFUL, own = 0xFFFFFFFFUL, guard = 0;
    BOOL ok = TRUE;

    if (!ext) return FALSE;
    for (;;) {
        ULONG high = bel(b, FFS_L_HIGHSEQ), k;
        if (high > v->tsize) { ok = FALSE; break; }
        for (k = 0; k < high; k++) {
            ULONG p = bel(b, v->nlongs - 51 - k);
            if (p < v->reserved || p >= v->nblocks) { ok = FALSE; break; }
            if (prev == 0xFFFFFFFFUL) nruns++;
            else if (p != prev + 1 && !(p == prev + 2 && prev + 1 == own)) nruns++;
            prev = p;
            if (ptrs && count < max) ptrs[count] = p;
            count++;
        }
        if (!ok) break;
        {
            ULONG e = bel(b, v->nlongs - FFS_E_EXTENSION);
            if (e == 0) break;
            if (e >= v->nblocks || ++guard > v->nblocks || !ffs_read(v, e, 1, ext) ||
                bel(ext, 0) != FFS_T_LIST || !ffs_checksum_ok(ext, v->nlongs)) { ok = FALSE; break; }
            if (exts && next_cnt < maxext) exts[next_cnt] = e;
            next_cnt++;
            own = e;
            b = ext;
        }
    }
    FreeVec(ext);
    if (nblk) *nblk = count;
    if (runs) *runs = nruns;
    if (next) *next = next_cnt;
    return ok;
}

/* --- directory walk ------------------------------------------------ */

BOOL ffs_walk(struct FfsVol *v, FfsWalkCb cb, void *ud)
{
    struct UStack st;
    UBYTE *dirbuf, *hdrbuf, *seen;
    ULONG nl = v->nlongs;
    BOOL ok = TRUE;

    memset(&st, 0, sizeof(st));
    dirbuf = (UBYTE *)AllocVec(v->bsz * 2, MEMF_PUBLIC);
    seen   = (UBYTE *)AllocVec((v->nblocks + 7) / 8, MEMF_PUBLIC | MEMF_CLEAR);   /* loop guard */
    if (!dirbuf || !seen) { if (dirbuf) FreeVec(dirbuf); if (seen) FreeVec(seen); return FALSE; }
    hdrbuf = dirbuf + v->bsz;
#define SEEN(k)    (seen[(k) >> 3] & (1 << ((k) & 7)))
#define MARK(k)    (seen[(k) >> 3] |= (1 << ((k) & 7)))

    if (!ustack_push(&st, v->root)) ok = FALSE;
    while (ok && st.n > 0) {
        ULONG dir = st.v[--st.n], slot;
        /* A directory is pushed exactly once (its header was MARKed when
         * found in a hash chain), so no SEEN test here - it would skip it. */
        MARK(dir);
        if (!ffs_read(v, dir, 1, dirbuf) || bel(dirbuf, 0) != FFS_T_SHORT) continue;
        for (slot = 0; slot < v->tsize && ok; slot++) {
            ULONG key = bel(dirbuf, FFS_L_TABLE + slot);
            while (key && ok) {
                LONG sec;
                if (key >= v->nblocks || SEEN(key)) break;
                MARK(key);
                if (!ffs_read(v, key, 1, hdrbuf) || bel(hdrbuf, 0) != FFS_T_SHORT ||
                    !ffs_checksum_ok(hdrbuf, nl)) break;
                sec = (LONG)bel(hdrbuf, nl - FFS_E_SECTYPE);
                if (!cb(ud, key, hdrbuf, sec)) { ok = FALSE; break; }
                if (sec == FFS_ST_USERDIR && !ustack_push(&st, key)) { ok = FALSE; break; }
                key = bel(hdrbuf, nl - FFS_E_HASHCHAIN);
            }
        }
    }
#undef SEEN
#undef MARK
    ustack_free(&st);
    FreeVec(dirbuf);
    FreeVec(seen);
    return ok;
}

static UBYTE upcase(UBYTE c, BOOL intl)
{
    if (c >= 'a' && c <= 'z') return (UBYTE)(c - 32);
    if (intl && c >= 0xE0 && c <= 0xFE && c != 0xF7) return (UBYTE)(c - 32);
    return c;
}

ULONG ffs_root_lookup(struct FfsVol *v, const char *name)
{
    ULONG nl = v->nlongs, slot, nlen = strlen(name);
    UBYTE *hdr = (UBYTE *)AllocVec(v->bsz, MEMF_PUBLIC);
    ULONG found = 0;
    if (!hdr) return 0;
    for (slot = 0; slot < v->tsize && !found; slot++) {
        ULONG key = bel(v->rootbuf, FFS_L_TABLE + slot), guard = 0;
        while (key && key < v->nblocks && guard++ < v->nblocks) {
            const UBYTE *nm;
            ULONG n, i;
            BOOL eq = TRUE;
            if (!ffs_read(v, key, 1, hdr) || bel(hdr, 0) != FFS_T_SHORT) break;
            nm = hdr + (nl - FFS_E_NAME) * 4;
            n  = nm[0];
            if (n == nlen) {
                for (i = 0; i < n; i++)
                    if (upcase(nm[1 + i], v->intl) != upcase((UBYTE)name[i], v->intl)) { eq = FALSE; break; }
                if (eq) { found = key; break; }
            }
            key = bel(hdr, nl - FFS_E_HASHCHAIN);
        }
    }
    FreeVec(hdr);
    return found;
}
