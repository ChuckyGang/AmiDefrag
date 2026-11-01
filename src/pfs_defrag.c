/*
 * pfs_defrag.c - PFS3 move engine (see pfs_defrag.h for the contract).
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include "clib.h"
#include "be.h"
#include "fsmap.h"
#include "fsmap_int.h"
#include "pfs_core.h"
#include "pfs_defrag.h"

#define CHUNK          32                 /* sectors per copy I/O */
#define SEC            512UL
#define JOURNAL_MAGIC  0x41444A32UL       /* 'ADJ2' */
#define PIECES_PER_SEC 42                 /* 12 bytes each */

/* journal record sector */
#define J_MAGIC 0
#define J_STATE 4
#define J_SEQ   8
#define J_HEAD  12
#define J_DEST  16
#define J_NBLK  20
#define J_NPC   24
#define J_NEXT  28                        /* anode after the moved range (0 = end) */

struct Piece { ULONG an, blk, cs; };

/* Table of every data run on the volume, sorted by block, so the
 * consolidation pass can tell which anode owns the blocks next to a
 * hole.  Only an index: before a run is moved aside its anode is read
 * and must agree, and data is only ever copied into blocks the bitmap
 * says are free. */
struct TPiece { ULONG blk, cs, an; };      /* cs carries the flags below */
#define TP_FIXED  0x80000000UL             /* journal, rollover file: never moved aside */
#define TP_SINGLE 0x40000000UL             /* the only run of its file */
#define TP_FLAGS  (TP_FIXED | TP_SINGLE)
#define TP_CS(x)  ((x) & ~TP_FLAGS)
#define GROW_TRIES 8                       /* holes tried per file */

struct Eng {
    struct PfsVol v;
    const struct VolumeInfo *vi;
    const struct FfsDefragOpts *o;
    const struct FfsDefragPlatform *pf;
    BOOL   jext;                     /* journal in pf->journal_io, not in the volume */
    FfsDefragLog  log;
    FfsDefragMove move;
    void *ud;
    struct FfsDefragStats *st;
    char *err; ULONG errlen;
    /* references: one bit per data sector, set when a chain reaches it */
    ULONG *ref;
    ULONG  ref_longs;
    char   incons[96];               /* first inconsistency seen, "" if none */
    /* journal */
    ULONG janode;                    /* anodenr of the journal file, 0 = none */
    ULONG jsec[PFS_JOURNAL_MAX_SEC];
    ULONG jcount, jcap, jseq;
    /* candidates */
    struct UStack cands;
    /* buffers */
    UBYTE *chunk, *chunk2, *sec;
    struct Piece *pieces;
    ULONG  pieces_cap;
    BOOL   writes_begun;
    /* run table (consolidation) */
    struct TPiece *tab;
    ULONG  tab_n, tab_cap;
    BOOL   tab_ok;
    /* files pass 1 found no run for: anodenr, nblk pairs */
    struct UStack later;
};

static void putl(UBYTE *b, ULONG o, ULONG x)
{
    b[o] = (UBYTE)(x >> 24); b[o + 1] = (UBYTE)(x >> 16); b[o + 2] = (UBYTE)(x >> 8); b[o + 3] = (UBYTE)x;
}

static void elog(struct Eng *e, const char *fmt, ...)
{
    char line[200];
    va_list ap;
    if (!e->log) return;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    e->log(e->ud, line);
}

static void seterr(struct Eng *e, const char *msg)
{
    if (e->err && e->errlen) { strncpy(e->err, msg, e->errlen - 1); e->err[e->errlen - 1] = '\0'; }
}

static void note_incons(struct Eng *e, const char *msg)
{
    if (!e->incons[0]) { strncpy(e->incons, msg, sizeof(e->incons) - 1); }
}

static BOOL ref_test(const struct Eng *e, ULONG sector)
{
    ULONG i = sector - e->v.bitmapstart;
    return (e->ref[i >> 5] & (0x80000000UL >> (i & 31))) != 0;
}
static void ref_set(struct Eng *e, ULONG sector)
{
    ULONG i = sector - e->v.bitmapstart;
    e->ref[i >> 5] |= 0x80000000UL >> (i & 31);
}

static BOOL pieces_reserve(struct Eng *e, ULONG n)
{
    struct Piece *p;
    ULONG cap;
    if (n <= e->pieces_cap) return TRUE;
    cap = e->pieces_cap ? e->pieces_cap : 256;
    while (cap < n) cap *= 2;
    p = (struct Piece *)AllocVec(cap * sizeof(struct Piece), MEMF_PUBLIC);
    if (!p) return FALSE;
    if (e->pieces) { memcpy(p, e->pieces, e->pieces_cap * sizeof(struct Piece)); FreeVec(e->pieces); }
    e->pieces = p;
    e->pieces_cap = cap;
    return TRUE;
}

/* Read the chain of anodenr into e->pieces; returns the piece count or
 * ~0 on a broken chain.  *nblk = data sectors, *runs = non-empty pieces. */
static ULONG gather_chain(struct Eng *e, ULONG anodenr, ULONG *nblk, ULONG *runs)
{
    ULONG a = anodenr, n = 0, guard = 0, cs, blk, next;
    *nblk = 0; *runs = 0;
    while (a && guard++ < 1000000UL) {
        if (!pfs_get_anode(&e->v, a, &cs, &blk, &next)) return 0xFFFFFFFFUL;
        if (!pieces_reserve(e, n + 1)) return 0xFFFFFFFFUL;
        e->pieces[n].an = a; e->pieces[n].blk = blk; e->pieces[n].cs = cs;
        n++;
        if (cs) { *nblk += cs; (*runs)++; }
        a = next;
    }
    return a ? 0xFFFFFFFFUL : n;
}


/* ------------------------------------------------------------------ */
/* run table                                                           */
/* ------------------------------------------------------------------ */

static BOOL tab_reserve(struct Eng *e, ULONG n)
{
    struct TPiece *t;
    ULONG cap;
    if (n <= e->tab_cap) return TRUE;
    cap = e->tab_cap ? e->tab_cap : 1024;
    while (cap < n) cap *= 2;
    t = (struct TPiece *)AllocVec(cap * sizeof(struct TPiece), MEMF_PUBLIC);
    if (!t) { e->tab_ok = FALSE; return FALSE; }
    if (e->tab) { memcpy(t, e->tab, e->tab_n * sizeof(struct TPiece)); FreeVec(e->tab); }
    e->tab = t;
    e->tab_cap = cap;
    return TRUE;
}

static void tab_push(struct Eng *e, ULONG blk, ULONG cs, ULONG an)
{
    if (!e->tab_ok || !tab_reserve(e, e->tab_n + 1)) return;
    e->tab[e->tab_n].blk = blk; e->tab[e->tab_n].cs = cs; e->tab[e->tab_n].an = an;
    e->tab_n++;
}

/* heapsort by blk (no qsort in this runtime) */
static void tab_sift(struct TPiece *t, ULONG root, ULONG n)
{
    for (;;) {
        ULONG child = 2 * root + 1;
        struct TPiece tmp;
        if (child >= n) return;
        if (child + 1 < n && t[child + 1].blk > t[child].blk) child++;
        if (t[root].blk >= t[child].blk) return;
        tmp = t[root]; t[root] = t[child]; t[child] = tmp;
        root = child;
    }
}

static void tab_sort(struct Eng *e)
{
    struct TPiece *t = e->tab;
    ULONG n = e->tab_n, i;
    if (n < 2) return;
    for (i = n / 2; i-- > 0; ) tab_sift(t, i, n);
    for (i = n - 1; i > 0; i--) { struct TPiece tmp = t[0]; t[0] = t[i]; t[i] = tmp; tab_sift(t, 0, i); }
}

/* index of the entry whose run covers blk, or -1 */
static LONG tab_find(const struct Eng *e, ULONG blk)
{
    ULONG lo = 0, hi = e->tab_n;
    if (!e->tab_ok || e->tab_n == 0) return -1;
    while (lo < hi) {                                /* first entry with .blk > blk */
        ULONG mid = (lo + hi) / 2;
        if (e->tab[mid].blk <= blk) lo = mid + 1; else hi = mid;
    }
    if (lo == 0) return -1;
    lo--;
    return blk < e->tab[lo].blk + TP_CS(e->tab[lo].cs) ? (LONG)lo : -1;
}

static void tab_remove(struct Eng *e, LONG idx)
{
    if (idx < 0 || (ULONG)idx >= e->tab_n) return;
    memmove(&e->tab[idx], &e->tab[idx + 1], (e->tab_n - idx - 1) * sizeof(struct TPiece));
    e->tab_n--;
}

static void tab_insert(struct Eng *e, ULONG blk, ULONG cs, ULONG an)
{
    ULONG lo = 0, hi = e->tab_n;
    if (!e->tab_ok || !tab_reserve(e, e->tab_n + 1)) return;
    while (lo < hi) { ULONG mid = (lo + hi) / 2; if (e->tab[mid].blk < blk) lo = mid + 1; else hi = mid; }
    memmove(&e->tab[lo + 1], &e->tab[lo], (e->tab_n - lo) * sizeof(struct TPiece));
    e->tab[lo].blk = blk; e->tab[lo].cs = cs; e->tab[lo].an = an;
    e->tab_n++;
}

/* a run moved: drop the old entries, add the new one */
static void tab_moved(struct Eng *e, ULONG first, ULONG n, ULONG dest, ULONG nblk, ULONG flags)
{
    ULONG i;
    if (!e->tab_ok) return;
    for (i = first; i < first + n; i++) {
        LONG idx;
        if (!e->pieces[i].cs) continue;
        idx = tab_find(e, e->pieces[i].blk);
        if (idx >= 0 && e->tab[idx].blk == e->pieces[i].blk && e->tab[idx].an == e->pieces[i].an) tab_remove(e, idx);
    }
    tab_insert(e, dest, nblk | flags, e->pieces[first].an);
}

/* ------------------------------------------------------------------ */
/* walk: references, consistency, candidates                           */
/* ------------------------------------------------------------------ */

static BOOL collect_cb(void *ud, ULONG anodenr, LONG type, const char *name, ULONG parent)
{
    struct Eng *e = (struct Eng *)ud;
    struct PfsVol *v = &e->v;
    ULONG a = anodenr, guard = 0, runs = 0, nblk = 0, cs, blk, next;
    ULONG tab0 = e->tab_n, tflags = 0;
    (void)name; (void)parent;

    /* deleted files keep their anode chain for undelete, but the handler
     * frees their blocks in the bitmap: no reference, no consistency rule */
    if (type == PFS_ST_DELETED) return TRUE;
    if (type != PFS_ST_FILE || anodenr == e->janode) tflags = TP_FIXED;

    while (a && guard++ < 1000000UL) {
        if (!pfs_get_anode(v, a, &cs, &blk, &next)) { note_incons(e, "entry with an unreachable anode"); break; }
        if (cs && blk >= v->bitmapstart) {
            ULONG s;
            if (blk + cs > v->disksize || blk + cs < blk) { note_incons(e, "file data outside the data area"); break; }
            for (s = blk; s < blk + cs; s++) {
                if (ref_test(e, s)) { note_incons(e, "cross-linked blocks (two chains share a block)"); break; }
                if (pfs_bm_isfree(v, s)) { note_incons(e, "a file's block is marked free in the bitmap"); break; }
                ref_set(e, s);
            }
            runs++; nblk += cs;
            tab_push(e, blk, cs | tflags, a);
        }
        a = next;
    }
    if (runs == 1 && e->tab_ok && e->tab_n == tab0 + 1) e->tab[tab0].cs |= TP_SINGLE;
    if (type == PFS_ST_FILE && runs > 1 && anodenr != e->janode) {
        e->st->candidates++;
        e->st->cand_blocks += nblk;
        if (!ustack_push(&e->cands, anodenr)) return FALSE;
    }
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* journal                                                             */
/* ------------------------------------------------------------------ */

/* journal sector idx: in the volume's journal file or in the sidecar */
static BOOL jread(struct Eng *e, ULONG idx, UBYTE *buf)
{
    if (e->jext) return e->pf->journal_io(e->pf->ud, FALSE, idx * SEC, buf, SEC);
    return pfs_read_sectors(&e->v, e->jsec[idx], 1, buf);
}
static BOOL jwrite(struct Eng *e, ULONG idx, const UBYTE *buf)
{
    if (e->jext) return e->pf->journal_io(e->pf->ud, TRUE, idx * SEC, (void *)buf, SEC);
    return pfs_write_sectors(&e->v, e->jsec[idx], 1, buf);
}

static BOOL journal_find(struct Eng *e)
{
    ULONG nblk, runs, np, i;
    if (e->jext) {
        e->janode = 0;
        e->jcount = PFS_JOURNAL_MAX_SEC;
        e->jcap   = (e->jcount - 1) * PIECES_PER_SEC;
        return TRUE;
    }
    e->janode = pfs_root_lookup(&e->v, FFS_JOURNAL_NAME);
    if (!e->janode) return FALSE;
    np = gather_chain(e, e->janode, &nblk, &runs);
    if (np == 0xFFFFFFFFUL) return FALSE;
    e->jcount = 0;
    for (i = 0; i < np && e->jcount < PFS_JOURNAL_MAX_SEC; i++) {
        ULONG s;
        if (e->pieces[i].blk < e->v.bitmapstart) continue;
        for (s = 0; s < e->pieces[i].cs && e->jcount < PFS_JOURNAL_MAX_SEC; s++)
            e->jsec[e->jcount++] = e->pieces[i].blk + s;
    }
    if (e->jcount < 2) return FALSE;
    e->jcap = (e->jcount - 1) * PIECES_PER_SEC;
    return TRUE;
}

static BOOL journal_record(struct Eng *e, ULONG state, ULONG head, ULONG dest, ULONG nblk, ULONG np, ULONG next_after)
{
    memset(e->sec, 0, SEC);
    putl(e->sec, J_MAGIC, JOURNAL_MAGIC);
    putl(e->sec, J_STATE, state);
    putl(e->sec, J_SEQ, ++e->jseq);
    putl(e->sec, J_HEAD, head);
    putl(e->sec, J_DEST, dest);
    putl(e->sec, J_NBLK, nblk);
    putl(e->sec, J_NPC, np);
    putl(e->sec, J_NEXT, next_after);
    return jwrite(e, 0, e->sec);
}

/* state 1: the pieces (old chain) go first, the record last, so a
 * half-written entry is never believed. */
static BOOL journal_set(struct Eng *e, ULONG first, ULONG np, ULONG next_after, ULONG dest, ULONG nblk)
{
    ULONG i, s;
    for (s = 0; s * PIECES_PER_SEC < np; s++) {
        memset(e->sec, 0, SEC);
        for (i = 0; i < PIECES_PER_SEC && s * PIECES_PER_SEC + i < np; i++) {
            const struct Piece *p = &e->pieces[first + s * PIECES_PER_SEC + i];
            putl(e->sec, 12 * i, p->an); putl(e->sec, 12 * i + 4, p->blk); putl(e->sec, 12 * i + 8, p->cs);
        }
        if (!jwrite(e, 1 + s, e->sec)) return FALSE;
    }
    return journal_record(e, 1, e->pieces[first].an, dest, nblk, np, next_after);
}

static BOOL journal_clear(struct Eng *e)
{
    return journal_record(e, 0, 0, 0, 0, 0, 0);
}

/* On start: finish or undo the file a crash interrupted.  Needs the
 * reference bitmap (collect_cb) so that nothing a file reaches is freed. */
static BOOL journal_recover(struct Eng *e)
{
    struct PfsVol *v = &e->v;
    ULONG head, dest, nblk, np, i, s, cs, blk, next, next_after;
    BOOL committed;

    if (!jread(e, 0, e->sec)) return FALSE;
    if (be32(e->sec, J_MAGIC) != JOURNAL_MAGIC) return TRUE;         /* fresh file */
    e->jseq = be32(e->sec, J_SEQ);
    if (be32(e->sec, J_STATE) != 1) return TRUE;
    head = be32(e->sec, J_HEAD); dest = be32(e->sec, J_DEST);
    nblk = be32(e->sec, J_NBLK); np = be32(e->sec, J_NPC); next_after = be32(e->sec, J_NEXT);
    if (np == 0 || np > e->jcap || nblk == 0 || dest < v->bitmapstart || dest + nblk > v->disksize) return TRUE;
    if (!pieces_reserve(e, np)) return FALSE;
    for (s = 0; s * PIECES_PER_SEC < np; s++) {
        if (!jread(e, 1 + s, e->sec)) return FALSE;
        for (i = 0; i < PIECES_PER_SEC && s * PIECES_PER_SEC + i < np; i++) {
            struct Piece *p = &e->pieces[s * PIECES_PER_SEC + i];
            p->an = be32(e->sec, 12 * i); p->blk = be32(e->sec, 12 * i + 4); p->cs = be32(e->sec, 12 * i + 8);
        }
    }
    if (e->pieces[0].an != head) return TRUE;
    if (!pfs_get_anode(v, head, &cs, &blk, &next)) return TRUE;
    /* blocknr decides: it is written first when the anode spans two
     * sectors, so the other fields may still be the old or the new ones */
    committed = (blk == dest);
    if (!committed && blk != e->pieces[0].blk) {
        elog(e, "Journal: anode %lu points to neither the old nor the new run, leaving it alone", (unsigned long)head);
        return TRUE;
    }

    BlockDev_ArmWrites(v->bd);
    if (committed) {
        elog(e, "Journal: finishing the interrupted move of anode %lu", (unsigned long)head);
        if (cs != nblk || next != next_after)
            if (!pfs_set_anode(v, head, nblk, dest, next_after)) return FALSE;
        for (i = 1; i < np; i++) {
            ULONG c2, b2, n2;
            if (!pfs_get_anode(v, e->pieces[i].an, &c2, &b2, &n2)) continue;
            if ((c2 || b2 || n2) && b2 == e->pieces[i].blk && c2 == e->pieces[i].cs)
                if (!pfs_set_anode(v, e->pieces[i].an, 0, 0, 0)) return FALSE;
        }
        for (i = 0; i < np; i++) {
            if (e->pieces[i].blk < v->bitmapstart || e->pieces[i].blk + e->pieces[i].cs > v->disksize) continue;
            for (s = e->pieces[i].blk; s < e->pieces[i].blk + e->pieces[i].cs; s++)
                if (!ref_test(e, s) && !pfs_bm_isfree(v, s)) pfs_bm_set(v, s, TRUE);
        }
        for (s = dest; s < dest + nblk; s++)
            if (pfs_bm_isfree(v, s)) pfs_bm_set(v, s, FALSE);
    } else {
        ULONG next0 = np > 1 ? e->pieces[1].an : next_after;
        elog(e, "Journal: undoing the interrupted move of anode %lu", (unsigned long)head);
        if (cs != e->pieces[0].cs || next != next0)
            if (!pfs_set_anode(v, head, e->pieces[0].cs, e->pieces[0].blk, next0)) return FALSE;
        for (s = dest; s < dest + nblk; s++)
            if (!ref_test(e, s) && !pfs_bm_isfree(v, s)) pfs_bm_set(v, s, TRUE);
    }
    if (!pfs_bm_flush(v)) return FALSE;
    BlockDev_Update(v->bd);
    e->st->repaired++;
    return journal_clear(e);
}

/* ------------------------------------------------------------------ */
/* the move                                                            */
/* ------------------------------------------------------------------ */

/* Move e->pieces[first .. first+n), consecutive anodes of one chain with
 * next_after the anode following them (0 = end of chain), to the free
 * run dest of nblk sectors.  Afterwards anode pieces[first] describes
 * {nblk, dest, next_after} and the other n-1 anodes are free.  With
 * n = whole chain this makes the file contiguous; with n = 1 it moves
 * one run aside.  flags: TP_* for the table entry of the new run.
 * Returns FALSE on a fatal error (err set). */
static BOOL move_pieces(struct Eng *e, ULONG first, ULONG n, ULONG next_after, ULONG dest, ULONG nblk, ULONG flags)
{
    struct PfsVol *v = &e->v;
    ULONG head = e->pieces[first].an, i, pos;

    if (e->o->dry_run) {
        for (i = first; i < first + n; i++) { ULONG s; for (s = 0; s < e->pieces[i].cs; s++) pfs_bm_set(v, e->pieces[i].blk + s, TRUE); }
        for (i = 0; i < nblk; i++) pfs_bm_set(v, dest + i, FALSE);
        tab_moved(e, first, n, dest, nblk, flags);
        return TRUE;
    }

    /* 2. copy + verify */
    pos = 0;
    for (i = first; i < first + n; i++) {
        const struct Piece *p = &e->pieces[i];
        ULONG off;
        for (off = 0; off < p->cs; ) {
            ULONG k = p->cs - off;
            if (k > CHUNK) k = CHUNK;
            if (!pfs_read_sectors(v, p->blk + off, k, e->chunk)) { seterr(e, "read error in file data"); return FALSE; }
            if (!pfs_write_sectors(v, dest + pos, k, e->chunk)) { seterr(e, "write error copying file data"); return FALSE; }
            if (!pfs_read_sectors(v, dest + pos, k, e->chunk2) || memcmp(e->chunk, e->chunk2, k * SEC) != 0) {
                seterr(e, "verify failed: copied data reads back differently"); return FALSE;
            }
            off += k; pos += k;
        }
    }

    /* 3. journal, 4. new run used */
    if (!journal_set(e, first, n, next_after, dest, nblk)) { seterr(e, "journal write failed"); return FALSE; }
    for (i = 0; i < nblk; i++) pfs_bm_set(v, dest + i, FALSE);
    if (!pfs_bm_flush(v)) { seterr(e, v->err); return FALSE; }

    /* 5. commit: the first anode of the range now describes the new run */
    e->writes_begun = TRUE;
    if (!pfs_set_anode(v, head, nblk, dest, next_after)) {
        seterr(e, "anode write failed - run again to let the journal finish or undo it");
        e->st->needs_validation = TRUE;
        return FALSE;
    }
    {
        ULONG c2, b2, n2;
        v->abuf_seq = 0xFFFFFFFFUL;          /* read it back from disk, not from the cache */
        if (!pfs_get_anode(v, head, &c2, &b2, &n2) || c2 != nblk || b2 != dest || n2 != next_after) {
            seterr(e, "anode reads back wrong after the commit - run again to let the journal finish");
            e->st->needs_validation = TRUE;
            return FALSE;
        }
    }

    /* 6. release the old runs */
    for (i = first + 1; i < first + n; i++)
        if (!pfs_set_anode(v, e->pieces[i].an, 0, 0, 0)) {
            seterr(e, "anode clear failed - run again to let the journal finish");
            e->st->needs_validation = TRUE;
            return FALSE;
        }
    for (i = first; i < first + n; i++) { ULONG s; for (s = 0; s < e->pieces[i].cs; s++) pfs_bm_set(v, e->pieces[i].blk + s, TRUE); }
    if (!pfs_bm_flush(v)) { seterr(e, v->err); e->st->needs_validation = TRUE; return FALSE; }
    BlockDev_Update(v->bd);
    if (!journal_clear(e)) { seterr(e, "journal clear failed"); e->st->needs_validation = TRUE; return FALSE; }

    tab_moved(e, first, n, dest, nblk, flags);
    if (e->move) {
        for (i = first; i < first + n; i++) if (e->pieces[i].cs) e->move(e->ud, e->pieces[i].blk, e->pieces[i].cs, BLK_FREE);
        e->move(e->ud, dest, nblk, (flags & TP_SINGLE) ? BLK_DATA : BLK_FRAG);
    }
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* making room                                                         */
/* ------------------------------------------------------------------ */

/* Move the run that starts at blk (right = TRUE) or ends at blk (FALSE)
 * aside, to the best-fit free run outside [ex_lo, ex_hi).  Returns the
 * run's length, 0 when it cannot be moved, -1 on a fatal error. */
static LONG evict_at(struct Eng *e, ULONG blk, ULONG ex_lo, ULONG ex_hi, BOOL right)
{
    struct PfsVol *v = &e->v;
    LONG idx = tab_find(e, blk);
    ULONG cs, acs, ablk, anext, dest, flags;
    struct TPiece t;
    if (idx < 0) return 0;
    t = e->tab[idx];
    if (t.cs & TP_FIXED) return 0;
    cs = TP_CS(t.cs); flags = t.cs & TP_FLAGS;
    if (cs == 0 || (right && t.blk != blk) || (!right && t.blk + cs - 1 != blk)) return 0;
    if (!pfs_get_anode(v, t.an, &acs, &ablk, &anext) || acs != cs || ablk != t.blk) return 0;  /* stale index */
    dest = pfs_bm_find_run_ex(v, cs, ex_lo, ex_hi);
    if (!dest) return 0;
    if (!pieces_reserve(e, 1)) return 0;
    e->pieces[0].an = t.an; e->pieces[0].blk = t.blk; e->pieces[0].cs = cs;
    if (!move_pieces(e, 0, 1, anext, dest, cs, flags)) return -1;
    e->st->moved_aside++;
    e->st->moved_aside_blocks += cs;
    return (LONG)cs;
}

/* Grow a free run to `need` sectors by moving its neighbours aside.
 * Largest hole first; evicted runs go outside the zone the hole can
 * grow into, so nothing is moved twice.  Returns the run's start, 0 when
 * no hole could be grown, ~0 on a fatal error. */
static ULONG grow_hole(struct Eng *e, ULONG need)
{
    struct PfsVol *v = &e->v;
    ULONG skip[GROW_TRIES], nskip = 0;

    while (nskip < GROW_TRIES) {
        ULONG start, len, lo, hi, ex_lo, ex_hi;
        BOOL rblocked = FALSE, lblocked = FALSE;
        start = pfs_bm_largest_run(v, skip, nskip, &len);
        if (!start) return 0;
        if (len >= need) return start;
        lo = start; hi = start + len;
        ex_lo = hi > v->bitmapstart + need ? hi - need : v->bitmapstart;
        ex_hi = lo + need < v->disksize ? lo + need : v->disksize;
        while (hi - lo < need && !(rblocked && lblocked)) {
            LONG r = 0;
            if (!rblocked) {
                r = hi < v->disksize ? evict_at(e, hi, ex_lo, ex_hi, TRUE) : 0;
                if (r < 0) return 0xFFFFFFFFUL;
                if (r == 0) rblocked = TRUE;
                else { hi += r; while (hi < v->disksize && pfs_bm_isfree(v, hi)) hi++; }
            }
            if (r == 0 && !lblocked) {
                r = lo > v->bitmapstart ? evict_at(e, lo - 1, ex_lo, ex_hi, FALSE) : 0;
                if (r < 0) return 0xFFFFFFFFUL;
                if (r == 0) lblocked = TRUE;
                else { lo -= r; while (lo > v->bitmapstart && pfs_bm_isfree(v, lo - 1)) lo--; }
            }
            if (r > 0 && e->move && !e->move(e->ud, 0, 0, 0xFF)) { e->st->aborted = TRUE; return 0; }
        }
        if (hi - lo >= need) return lo;
        skip[nskip++] = start;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* one file                                                            */
/* ------------------------------------------------------------------ */

#define PF_MOVED   1
#define PF_NOSPACE 0
#define PF_SKIPPED 2
#define PF_FATAL  -1

static LONG validate_chain(struct Eng *e, ULONG anodenr, ULONG np)
{
    struct PfsVol *v = &e->v;
    ULONG i;
    for (i = 0; i < np; i++) {
        const struct Piece *p = &e->pieces[i];
        ULONG s;
        if (!p->cs) continue;
        if (p->blk < v->bitmapstart || p->blk + p->cs > v->disksize || p->blk + p->cs < p->blk) {
            elog(e, "Skipping anode %lu: data outside the data area", (unsigned long)anodenr);
            return FALSE;
        }
        for (s = p->blk; s < p->blk + p->cs; s++)
            if (pfs_bm_isfree(v, s)) {
                elog(e, "Skipping anode %lu: its blocks are marked free", (unsigned long)anodenr);
                return FALSE;
            }
    }
    return TRUE;
}

static LONG process_file(struct Eng *e, ULONG anodenr, BOOL make_room, ULONG *nblk_out)
{
    struct PfsVol *v = &e->v;
    ULONG np, nblk, runs, dest, aside0 = e->st->moved_aside;

    np = gather_chain(e, anodenr, &nblk, &runs);
    if (nblk_out) *nblk_out = nblk;
    if (np == 0xFFFFFFFFUL || nblk == 0) { e->st->skipped_error++; return PF_SKIPPED; }
    if (runs <= 1) return PF_MOVED;
    if (!validate_chain(e, anodenr, np)) { e->st->skipped_error++; return PF_SKIPPED; }
    if (!e->o->dry_run && np > e->jcap) {
        elog(e, "Skipping anode %lu: %lu pieces do not fit the journal (%lu)", (unsigned long)anodenr,
             (unsigned long)np, (unsigned long)e->jcap);
        e->st->skipped_error++; return PF_SKIPPED;
    }
    dest = pfs_bm_find_run(v, nblk);
    if (!dest && make_room && e->tab_ok && !e->o->dry_run) {
        ULONG np2, nblk2, runs2;
        dest = grow_hole(e, nblk);
        if (dest == 0xFFFFFFFFUL) return PF_FATAL;
        if (e->st->aborted) return PF_NOSPACE;
        if (dest) {
            elog(e, "Made room for anode %lu: %lu run(s) moved aside", (unsigned long)anodenr,
                 (unsigned long)(e->st->moved_aside - aside0));
            /* the file's own runs may have moved: read the chain again */
            np2 = gather_chain(e, anodenr, &nblk2, &runs2);
            if (np2 != np || nblk2 != nblk || !validate_chain(e, anodenr, np)) { e->st->skipped_error++; return PF_SKIPPED; }
        }
    }
    if (!dest) {
        if (!make_room) return PF_NOSPACE;
        elog(e, "No free run of %lu blocks for anode %lu (%lu pieces)", (unsigned long)nblk,
             (unsigned long)anodenr, (unsigned long)runs);
        return PF_NOSPACE;
    }
    if (e->o->dry_run)
        elog(e, "Would move anode %lu: %lu blocks in %lu pieces -> %lu..%lu", (unsigned long)anodenr,
             (unsigned long)nblk, (unsigned long)runs, (unsigned long)dest, (unsigned long)(dest + nblk - 1));
    if (!move_pieces(e, 0, np, 0, dest, nblk, TP_SINGLE)) return PF_FATAL;
    e->st->moved++;
    e->st->moved_blocks += nblk;
    if (!e->o->dry_run)
        elog(e, "Moved anode %lu: %lu blocks, %lu pieces -> 1", (unsigned long)anodenr, (unsigned long)nblk, (unsigned long)runs);
    return PF_MOVED;
}

/* pass 2 order: smallest files first, so the holes they leave merge
 * for the larger ones (insertion sort on anodenr/nblk pairs) */
static void later_sort(struct UStack *l)
{
    ULONG i, j, n = l->n / 2;
    for (i = 1; i < n; i++) {
        ULONG an = l->v[2 * i], nb = l->v[2 * i + 1];
        for (j = i; j > 0 && l->v[2 * (j - 1) + 1] > nb; j--) { l->v[2 * j] = l->v[2 * (j - 1)]; l->v[2 * j + 1] = l->v[2 * (j - 1) + 1]; }
        l->v[2 * j] = an; l->v[2 * j + 1] = nb;
    }
}

/* ------------------------------------------------------------------ */
/* run                                                                 */
/* ------------------------------------------------------------------ */

LONG PfsDefrag_Run(struct BlockDev *bd, const struct VolumeInfo *vi,
                   const struct FfsDefragOpts *opts, const struct FfsDefragPlatform *pf,
                   FfsDefragLog log, FfsDefragMove move, void *ud,
                   struct FfsDefragStats *stats, char *err, ULONG errlen)
{
    struct Eng *e;
    LONG rc = RETURN_ERROR;
    BOOL inhibited = FALSE, opened = FALSE, journal_made = FALSE, journal_checked = FALSE;
    ULONG i, dirs = 0;

    memset(stats, 0, sizeof(*stats));
    if (err && errlen) err[0] = '\0';
    if (vi->fskind != FS_PFS) { if (err) strncpy(err, "not a PFS3 volume", errlen - 1); return RETURN_ERROR; }
    if (vi->is_boot && !opts->dry_run) {
        if (err) strncpy(err, "the boot volume is not defragmented in this version", errlen - 1);
        return RETURN_ERROR;
    }

    e = (struct Eng *)AllocVec(sizeof(*e), MEMF_PUBLIC | MEMF_CLEAR);
    if (!e) { if (err) strncpy(err, "out of memory", errlen - 1); return RETURN_ERROR; }
    e->vi = vi; e->o = opts; e->log = log; e->move = move; e->ud = ud; e->st = stats;
    e->err = err; e->errlen = errlen;
    e->pf = pf; e->jext = pf->journal_io != NULL;
    e->tab_ok = TRUE;

    if (!opts->dry_run) {
        if (!pf->create_journal(pf->ud, vi, PFS_JOURNAL_BYTES, err, errlen)) goto done;
        journal_made = TRUE;
        if (!pf->inhibit(pf->ud, vi, TRUE, err, errlen)) goto done;
        inhibited = TRUE;
    }

    e->chunk = (UBYTE *)AllocVec(CHUNK * SEC * 2 + SEC, MEMF_PUBLIC);
    if (!e->chunk) { seterr(e, "out of memory for buffers"); goto done; }
    e->chunk2 = e->chunk + CHUNK * SEC;
    e->sec    = e->chunk2 + CHUNK * SEC;

    if (!pfs_open(&e->v, bd, vi)) { seterr(e, e->v.err); goto done; }
    opened = TRUE;
    if (e->v.deldir_old) { seterr(e, "volume has a pre-17.9 deldir: mount it with a current PFS3 first"); goto done; }
    if ((e->v.options & PFS_MODE_SUPERINDEX) && !e->v.rext) { seterr(e, "rootblock extension missing"); goto done; }
    if (!pfs_bm_load(&e->v)) { seterr(e, e->v.err); goto done; }

    e->ref_longs = (e->v.disksize - e->v.bitmapstart + 31) / 32;
    e->ref = (ULONG *)AllocVec(e->ref_longs * 4, MEMF_PUBLIC | MEMF_CLEAR);
    if (!e->ref) { seterr(e, "out of memory for the reference map"); goto done; }

    /* the journal file is never a candidate; a live run needs it */
    if (!journal_find(e) && !opts->dry_run) {
        seterr(e, "journal file " FFS_JOURNAL_NAME " not found in the root directory"); goto done;
    }
    if (!pfs_walk(&e->v, collect_cb, e, &dirs)) { seterr(e, "directory walk failed"); goto done; }
    if (e->v.err[0]) note_incons(e, e->v.err);
    if (e->incons[0]) {
        char msg[160];
        snprintf(msg, sizeof(msg), "volume inconsistent, nothing written: %s", e->incons);
        seterr(e, msg);
        goto done;
    }
    if (!opts->dry_run) {
        if (!journal_recover(e)) { seterr(e, "journal check failed"); goto done; }
        journal_checked = TRUE;
    }
    elog(e, "%lu fragmented file(s), %lu blocks, %lu blocks free",
         (unsigned long)stats->candidates, (unsigned long)stats->cand_blocks, (unsigned long)e->v.free_blocks);
    if (stats->candidates == 0) { rc = RETURN_WARN; goto done; }

    if (!opts->dry_run) BlockDev_ArmWrites(bd);
    if (e->tab_ok) tab_sort(e);
    else elog(e, "Not enough memory for the run table: files without a free run stay as they are");

    /* pass 1: every file with a free run large enough */
    for (i = 0; i < e->cands.n; i++) {
        ULONG nblk;
        LONG r = process_file(e, e->cands.v[i], FALSE, &nblk);
        if (r == PF_FATAL) { rc = e->writes_begun ? RETURN_FAIL : RETURN_ERROR; goto done; }
        if (r == PF_NOSPACE && (!ustack_push(&e->later, e->cands.v[i]) || !ustack_push(&e->later, nblk))) {
            seterr(e, "out of memory"); goto done;
        }
        if (e->move && !e->move(e->ud, 0, 0, 0xFF)) { stats->aborted = TRUE; break; }   /* poll for abort */
    }
    /* pass 2: make room for the rest by moving their neighbours aside.
     * Not simulated in a dry run (anodes are not rewritten there, so the
     * chains read back would no longer match the simulated bitmap). */
    if (opts->dry_run) stats->needs_room = e->later.n / 2;
    else if (!stats->aborted && e->later.n) {
        /* rounds: a file whose holes were all blocked may get its room
         * once the later files have moved things around, so the
         * leftovers are tried again while a round still gets some done */
        ULONG round = 0, before, left;
        later_sort(&e->later);
        elog(e, "%lu file(s) need room made: moving neighbours aside", (unsigned long)(e->later.n / 2));
        do {
            before = e->later.n / 2;
            left = 0;
            for (i = 0; i < before; i++) {
                LONG r = process_file(e, e->later.v[2 * i], TRUE, NULL);
                if (r == PF_FATAL) { rc = e->writes_begun ? RETURN_FAIL : RETURN_ERROR; goto done; }
                if (r == PF_NOSPACE) { e->later.v[2 * left] = e->later.v[2 * i]; e->later.v[2 * left + 1] = e->later.v[2 * i + 1]; left++; }
                if (stats->aborted || (e->move && !e->move(e->ud, 0, 0, 0xFF))) { stats->aborted = TRUE; break; }
            }
            e->later.n = 2 * left;
            round++;
            if (!stats->aborted && left && left < before)
                elog(e, "Round %lu: %lu file(s) still without room, trying again", (unsigned long)round, (unsigned long)left);
        } while (!stats->aborted && left && left < before && round < 6);
        if (!stats->aborted) stats->skipped_space += left;
    }
    if (!opts->dry_run) BlockDev_Update(bd);
    rc = (stats->skipped_space || stats->skipped_error || stats->aborted) ? RETURN_WARN : RETURN_OK;

done:
    if (e->writes_begun && stats->needs_validation)
        elog(e, "Interrupted after writes began; run AmiDefrag again on this volume to let the journal finish.");
    if (opened) pfs_close(&e->v);
    if (e->chunk) FreeVec(e->chunk);
    if (e->ref) FreeVec(e->ref);
    if (e->pieces) FreeVec(e->pieces);
    ustack_free(&e->cands);
    ustack_free(&e->later);
    if (e->tab) FreeVec(e->tab);
    if (inhibited) pf->inhibit(pf->ud, vi, FALSE, NULL, 0);
    if (journal_made && journal_checked && !stats->needs_validation) pf->delete_journal(pf->ud, vi);
    FreeVec(e);
    return rc;
}
