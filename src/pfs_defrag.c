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

struct Piece { ULONG an, blk, cs; };

struct Eng {
    struct PfsVol v;
    const struct VolumeInfo *vi;
    const struct FfsDefragOpts *o;
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
/* walk: references, consistency, candidates                           */
/* ------------------------------------------------------------------ */

static BOOL collect_cb(void *ud, ULONG anodenr, LONG type, const char *name, ULONG parent)
{
    struct Eng *e = (struct Eng *)ud;
    struct PfsVol *v = &e->v;
    ULONG a = anodenr, guard = 0, runs = 0, nblk = 0, cs, blk, next;
    (void)name; (void)parent;

    /* deleted files keep their anode chain for undelete, but the handler
     * frees their blocks in the bitmap: no reference, no consistency rule */
    if (type == PFS_ST_DELETED) return TRUE;

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
        }
        a = next;
    }
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

static BOOL journal_find(struct Eng *e)
{
    ULONG nblk, runs, np, i;
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

static BOOL journal_record(struct Eng *e, ULONG state, ULONG head, ULONG dest, ULONG nblk, ULONG np)
{
    memset(e->sec, 0, SEC);
    putl(e->sec, J_MAGIC, JOURNAL_MAGIC);
    putl(e->sec, J_STATE, state);
    putl(e->sec, J_SEQ, ++e->jseq);
    putl(e->sec, J_HEAD, head);
    putl(e->sec, J_DEST, dest);
    putl(e->sec, J_NBLK, nblk);
    putl(e->sec, J_NPC, np);
    return pfs_write_sectors(&e->v, e->jsec[0], 1, e->sec);
}

/* state 1: the pieces (old chain) go first, the record last, so a
 * half-written entry is never believed. */
static BOOL journal_set(struct Eng *e, ULONG dest, ULONG nblk, ULONG np)
{
    ULONG i, s;
    for (s = 0; s * PIECES_PER_SEC < np; s++) {
        memset(e->sec, 0, SEC);
        for (i = 0; i < PIECES_PER_SEC && s * PIECES_PER_SEC + i < np; i++) {
            const struct Piece *p = &e->pieces[s * PIECES_PER_SEC + i];
            putl(e->sec, 12 * i, p->an); putl(e->sec, 12 * i + 4, p->blk); putl(e->sec, 12 * i + 8, p->cs);
        }
        if (!pfs_write_sectors(&e->v, e->jsec[1 + s], 1, e->sec)) return FALSE;
    }
    return journal_record(e, 1, e->pieces[0].an, dest, nblk, np);
}

static BOOL journal_clear(struct Eng *e)
{
    return journal_record(e, 0, 0, 0, 0, 0);
}

/* On start: finish or undo the file a crash interrupted.  Needs the
 * reference bitmap (collect_cb) so that nothing a file reaches is freed. */
static BOOL journal_recover(struct Eng *e)
{
    struct PfsVol *v = &e->v;
    ULONG head, dest, nblk, np, i, s, cs, blk, next;
    BOOL committed;

    if (!pfs_read_sectors(v, e->jsec[0], 1, e->sec)) return FALSE;
    if (be32(e->sec, J_MAGIC) != JOURNAL_MAGIC) return TRUE;         /* fresh file */
    e->jseq = be32(e->sec, J_SEQ);
    if (be32(e->sec, J_STATE) != 1) return TRUE;
    head = be32(e->sec, J_HEAD); dest = be32(e->sec, J_DEST);
    nblk = be32(e->sec, J_NBLK); np = be32(e->sec, J_NPC);
    if (np == 0 || np > e->jcap || nblk == 0 || dest < v->bitmapstart || dest + nblk > v->disksize) return TRUE;
    if (!pieces_reserve(e, np)) return FALSE;
    for (s = 0; s * PIECES_PER_SEC < np; s++) {
        if (!pfs_read_sectors(v, e->jsec[1 + s], 1, e->sec)) return FALSE;
        for (i = 0; i < PIECES_PER_SEC && s * PIECES_PER_SEC + i < np; i++) {
            struct Piece *p = &e->pieces[s * PIECES_PER_SEC + i];
            p->an = be32(e->sec, 12 * i); p->blk = be32(e->sec, 12 * i + 4); p->cs = be32(e->sec, 12 * i + 8);
        }
    }
    if (e->pieces[0].an != head) return TRUE;
    if (!pfs_get_anode(v, head, &cs, &blk, &next)) return TRUE;
    committed = (blk == dest && cs == nblk && next == 0);

    BlockDev_ArmWrites(v->bd);
    if (committed) {
        elog(e, "Journal: finishing the interrupted move of anode %lu", (unsigned long)head);
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
        elog(e, "Journal: undoing the interrupted move of anode %lu", (unsigned long)head);
        for (s = dest; s < dest + nblk; s++)
            if (!ref_test(e, s) && !pfs_bm_isfree(v, s)) pfs_bm_set(v, s, TRUE);
    }
    if (!pfs_bm_flush(v)) return FALSE;
    BlockDev_Update(v->bd);
    e->st->repaired++;
    return journal_clear(e);
}

/* ------------------------------------------------------------------ */
/* one file                                                            */
/* ------------------------------------------------------------------ */

static BOOL process_file(struct Eng *e, ULONG anodenr, const char *what)
{
    struct PfsVol *v = &e->v;
    ULONG np, nblk, runs, dest, i, pos;
    (void)what;

    np = gather_chain(e, anodenr, &nblk, &runs);
    if (np == 0xFFFFFFFFUL || nblk == 0) { e->st->skipped_error++; return TRUE; }
    if (runs <= 1) return TRUE;
    for (i = 0; i < np; i++) {
        const struct Piece *p = &e->pieces[i];
        ULONG s;
        if (!p->cs) continue;
        if (p->blk < v->bitmapstart || p->blk + p->cs > v->disksize || p->blk + p->cs < p->blk) {
            elog(e, "Skipping anode %lu: data outside the data area", (unsigned long)anodenr);
            e->st->skipped_error++; return TRUE;
        }
        for (s = p->blk; s < p->blk + p->cs; s++)
            if (pfs_bm_isfree(v, s)) {
                elog(e, "Skipping anode %lu: its blocks are marked free", (unsigned long)anodenr);
                e->st->skipped_error++; return TRUE;
            }
    }
    if (!e->o->dry_run && np > e->jcap) {
        elog(e, "Skipping anode %lu: %lu pieces do not fit the journal (%lu)", (unsigned long)anodenr,
             (unsigned long)np, (unsigned long)e->jcap);
        e->st->skipped_error++; return TRUE;
    }
    dest = pfs_bm_find_run(v, nblk);
    if (!dest) {
        elog(e, "No free run of %lu blocks for anode %lu (%lu pieces)", (unsigned long)nblk,
             (unsigned long)anodenr, (unsigned long)runs);
        e->st->skipped_space++; return TRUE;
    }

    if (e->o->dry_run) {
        elog(e, "Would move anode %lu: %lu blocks in %lu pieces -> %lu..%lu", (unsigned long)anodenr,
             (unsigned long)nblk, (unsigned long)runs, (unsigned long)dest, (unsigned long)(dest + nblk - 1));
        for (i = 0; i < np; i++) { ULONG s; for (s = 0; s < e->pieces[i].cs; s++) pfs_bm_set(v, e->pieces[i].blk + s, TRUE); }
        for (i = 0; i < nblk; i++) pfs_bm_set(v, dest + i, FALSE);
        e->st->moved++; e->st->moved_blocks += nblk;
        return TRUE;
    }

    /* 2. copy + verify */
    pos = 0;
    for (i = 0; i < np; i++) {
        const struct Piece *p = &e->pieces[i];
        ULONG off;
        for (off = 0; off < p->cs; ) {
            ULONG n = p->cs - off;
            if (n > CHUNK) n = CHUNK;
            if (!pfs_read_sectors(v, p->blk + off, n, e->chunk)) { seterr(e, "read error in file data"); return FALSE; }
            if (!pfs_write_sectors(v, dest + pos, n, e->chunk)) { seterr(e, "write error copying file data"); return FALSE; }
            if (!pfs_read_sectors(v, dest + pos, n, e->chunk2) || memcmp(e->chunk, e->chunk2, n * SEC) != 0) {
                seterr(e, "verify failed: copied data reads back differently"); return FALSE;
            }
            off += n; pos += n;
        }
    }

    /* 3. journal, 4. new run used */
    if (!journal_set(e, dest, nblk, np)) { seterr(e, "journal write failed"); return FALSE; }
    for (i = 0; i < nblk; i++) pfs_bm_set(v, dest + i, FALSE);
    if (!pfs_bm_flush(v)) { seterr(e, v->err); return FALSE; }

    /* 5. commit: the first anode now describes the whole file */
    e->writes_begun = TRUE;
    if (!pfs_set_anode(v, anodenr, nblk, dest, 0)) {
        seterr(e, "anode write failed - run again to let the journal finish or undo it");
        e->st->needs_validation = TRUE;
        return FALSE;
    }

    /* 6. release the old chain */
    for (i = 1; i < np; i++)
        if (!pfs_set_anode(v, e->pieces[i].an, 0, 0, 0)) {
            seterr(e, "anode clear failed - run again to let the journal finish");
            e->st->needs_validation = TRUE;
            return FALSE;
        }
    for (i = 0; i < np; i++) { ULONG s; for (s = 0; s < e->pieces[i].cs; s++) pfs_bm_set(v, e->pieces[i].blk + s, TRUE); }
    if (!pfs_bm_flush(v)) { seterr(e, v->err); e->st->needs_validation = TRUE; return FALSE; }
    BlockDev_Update(v->bd);
    if (!journal_clear(e)) { seterr(e, "journal clear failed"); e->st->needs_validation = TRUE; return FALSE; }

    e->st->moved++;
    e->st->moved_blocks += nblk;
    elog(e, "Moved anode %lu: %lu blocks, %lu pieces -> 1", (unsigned long)anodenr, (unsigned long)nblk, (unsigned long)runs);
    if (e->move) {
        for (i = 0; i < np; i++) if (e->pieces[i].cs) e->move(e->ud, e->pieces[i].blk, e->pieces[i].cs, BLK_FREE);
        e->move(e->ud, dest, nblk, BLK_DATA);
    }
    return TRUE;
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
    BOOL inhibited = FALSE, opened = FALSE, journal_made = FALSE;
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
    }
    elog(e, "%lu fragmented file(s), %lu blocks, %lu blocks free",
         (unsigned long)stats->candidates, (unsigned long)stats->cand_blocks, (unsigned long)e->v.free_blocks);
    if (stats->candidates == 0) { rc = RETURN_WARN; goto done; }

    if (!opts->dry_run) BlockDev_ArmWrites(bd);

    for (i = 0; i < e->cands.n; i++) {
        if (!process_file(e, e->cands.v[i], NULL)) {
            rc = e->writes_begun ? RETURN_FAIL : RETURN_ERROR;
            goto done;
        }
        if (e->move && !e->move(e->ud, 0, 0, 0xFF)) { stats->aborted = TRUE; break; }   /* poll for abort */
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
    if (inhibited) pf->inhibit(pf->ud, vi, FALSE, NULL, 0);
    if (journal_made && !stats->needs_validation) pf->delete_journal(pf->ud, vi);
    FreeVec(e);
    return rc;
}
