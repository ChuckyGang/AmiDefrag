/*
 * ffs_defrag.c - FFS move engine (see ffs_defrag.h for the contract).
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include "clib.h"
#include "be.h"
#include "fsmap.h"
#include "fsmap_int.h"
#include "ffs_core.h"
#include "ffs_defrag.h"

#define CHUNK        32                 /* data blocks per copy I/O */
#define JOURNAL_MAGIC 0x41444A31UL      /* 'ADJ1' */
#define J_MAGIC 0
#define J_STATE 1
#define J_HDR   2
#define J_IMGLEN 3
#define J_SEQ   4

struct Eng {
    struct FfsVol v;
    const struct VolumeInfo *vi;
    const struct FfsDefragOpts *o;
    const struct FfsDefragPlatform *pf;
    BOOL   jext;                     /* journal in pf->journal_io, not in the volume */
    FfsDefragLog  log;
    FfsDefragMove move;
    void *ud;
    struct FfsDefragStats *st;
    char *err; ULONG errlen;
    /* journal */
    ULONG jkey;                  /* header key of the journal file, 0 = none */
    ULONG jblk[FFS_JOURNAL_BLOCKS];
    ULONG jcount;
    ULONG payload;               /* usable bytes per journal data block */
    ULONG jseq;
    /* candidates */
    struct UStack cands;
    /* buffers */
    UBYTE *hdr, *img, *jbuf, *chunk, *chunk2, *extb;
    BOOL writes_begun;
    BOOL bm_bad;                     /* a header block is marked free in the bitmap */
};

static void putl(UBYTE *b, ULONG li, ULONG v)
{
    b[li * 4] = (UBYTE)(v >> 24); b[li * 4 + 1] = (UBYTE)(v >> 16);
    b[li * 4 + 2] = (UBYTE)(v >> 8); b[li * 4 + 3] = (UBYTE)v;
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
    if (e->err && e->errlen) strncpy(e->err, msg, e->errlen - 1);
}

static void name_of(const struct Eng *e, const UBYTE *hdr, char *out, ULONG outlen)
{
    const UBYTE *nm = hdr + (e->v.nlongs - FFS_E_NAME) * 4;
    ULONG n = nm[0];
    if (n > 30) n = 30;
    if (n >= outlen) n = outlen - 1;
    memcpy(out, nm + 1, n);
    out[n] = '\0';
}

/* ------------------------------------------------------------------ */
/* journal                                                             */
/* ------------------------------------------------------------------ */

static BOOL journal_find(struct Eng *e)
{
    ULONG nblk = 0, runs = 0, next = 0;
    if (e->jext) {                       /* sidecar: one FS block per index */
        e->jkey = 0;
        e->jcount = FFS_JOURNAL_BLOCKS;
        e->payload = e->v.bsz;
        return TRUE;
    }
    e->jkey = ffs_root_lookup(&e->v, FFS_JOURNAL_NAME);
    if (!e->jkey) return FALSE;
    if (!ffs_read(&e->v, e->jkey, 1, e->hdr) || !ffs_is_header(e->hdr, e->v.nlongs, FFS_T_SHORT, FFS_ST_FILE))
        return FALSE;
    if (!ffs_file_layout(&e->v, e->hdr, e->jblk, FFS_JOURNAL_BLOCKS, NULL, 0, &nblk, &runs, &next))
        return FALSE;
    e->jcount  = nblk > FFS_JOURNAL_BLOCKS ? FFS_JOURNAL_BLOCKS : nblk;
    e->payload = e->v.ofs ? e->v.bsz - FFS_OFS_HDR : e->v.bsz;
    /* record block + enough blocks for one header image */
    return e->jcount >= 1 + (e->v.bsz + e->payload - 1) / e->payload;
}

/* Write len bytes of data into the payload of journal data block idx. */
static BOOL journal_put(struct Eng *e, ULONG idx, const UBYTE *data, ULONG len)
{
    UBYTE *b = e->jbuf;
    ULONG off = e->v.ofs ? FFS_OFS_HDR : 0;
    if (e->jext) return e->pf->journal_io(e->pf->ud, TRUE, idx * e->v.bsz, (void *)data, len);
    if (!ffs_read(&e->v, e->jblk[idx], 1, b)) return FALSE;
    memset(b + off, 0, e->payload);
    memcpy(b + off, data, len);
    if (e->v.ofs) ffs_set_checksum(b, e->v.nlongs, FFS_OFSL_CHK);
    return ffs_write(&e->v, e->jblk[idx], 1, b);
}

static BOOL journal_get(struct Eng *e, ULONG idx, UBYTE *data, ULONG len)
{
    UBYTE *b = e->jbuf;
    ULONG off = e->v.ofs ? FFS_OFS_HDR : 0;
    if (e->jext) return e->pf->journal_io(e->pf->ud, FALSE, idx * e->v.bsz, data, len);
    if (!ffs_read(&e->v, e->jblk[idx], 1, b)) return FALSE;
    memcpy(data, b + off, len);
    return TRUE;
}

/* state 1: image (bsz bytes) of header hdrkey is about to be committed.
 * Image blocks go first, the record last, so a half-written entry is
 * never believed. */
static BOOL journal_set(struct Eng *e, ULONG state, ULONG hdrkey, const UBYTE *image)
{
    UBYTE rec[32];
    if (state == 1) {
        ULONG done = 0, idx = 1;
        while (done < e->v.bsz) {
            ULONG n = e->v.bsz - done;
            if (n > e->payload) n = e->payload;
            if (!journal_put(e, idx++, image + done, n)) return FALSE;
            done += n;
        }
    }
    memset(rec, 0, sizeof(rec));
    putl(rec, J_MAGIC, JOURNAL_MAGIC);
    putl(rec, J_STATE, state);
    putl(rec, J_HDR, hdrkey);
    putl(rec, J_IMGLEN, state ? e->v.bsz : 0);
    putl(rec, J_SEQ, ++e->jseq);
    return journal_put(e, 0, rec, sizeof(rec));
}

/* On start: finish a commit that a crash may have torn. */
static BOOL journal_recover(struct Eng *e)
{
    UBYTE rec[32];
    ULONG hdrkey, imglen;
    if (!journal_get(e, 0, rec, sizeof(rec))) return FALSE;
    if (be32(rec, 0) != JOURNAL_MAGIC) return TRUE;             /* fresh file */
    e->jseq = be32(rec, J_SEQ * 4);
    if (be32(rec, J_STATE * 4) != 1) return TRUE;
    hdrkey = be32(rec, J_HDR * 4);
    imglen = be32(rec, J_IMGLEN * 4);
    if (imglen != e->v.bsz || hdrkey < e->v.reserved || hdrkey >= e->v.nblocks) return TRUE;
    {
        ULONG done = 0, idx = 1;
        while (done < imglen) {
            ULONG n = imglen - done;
            if (n > e->payload) n = e->payload;
            if (!journal_get(e, idx++, e->img + done, n)) return FALSE;
            done += n;
        }
    }
    if (!ffs_checksum_ok(e->img, e->v.nlongs) || bel(e->img, FFS_L_OWNKEY) != hdrkey) return TRUE;
    if (!ffs_read(&e->v, hdrkey, 1, e->hdr)) return FALSE;
    if (ffs_checksum_ok(e->hdr, e->v.nlongs) && bel(e->hdr, FFS_L_OWNKEY) == hdrkey) {
        /* old or new header is intact either way: nothing to repair */
    } else {
        elog(e, "Journal: repairing torn header block %lu", (unsigned long)hdrkey);
        BlockDev_ArmWrites(e->v.bd);
        if (!ffs_write(&e->v, hdrkey, 1, e->img)) return FALSE;
        e->st->repaired++;
    }
    BlockDev_ArmWrites(e->v.bd);
    return journal_set(e, 0, 0, NULL);
}

/* The root block is rewritten twice per run (flag off, flag on).  A torn
 * root write would make the whole volume unmountable, so its new image is
 * journaled first like any header; see recover_root() for the other half. */
static BOOL root_flag_journaled(struct Eng *e, BOOL valid)
{
    struct FfsVol *v = &e->v;
    memcpy(e->img, v->rootbuf, v->bsz);
    putl(e->img, v->nlongs - FFS_E_BMFLAG, valid ? 0xFFFFFFFFUL : 0);
    ffs_set_checksum(e->img, v->nlongs, FFS_L_CHECKSUM);
    if (!journal_set(e, 1, v->root, e->img)) return FALSE;
    if (!ffs_write_root_flag(v, valid)) return FALSE;
    return journal_set(e, 0, 0, NULL);
}

/* Disaster path: the root block does not verify.  Scan the partition for
 * the journal file's header (its name is unique enough), and if the
 * journal holds a committed image for the root key, write it back.
 * Reads the whole partition in the worst case; only runs when the volume
 * is already unmountable, so the cost is acceptable. */
static BOOL recover_root(struct Eng *e, struct BlockDev *bd, const struct VolumeInfo *vi)
{
    struct FfsVol t;
    UBYTE *buf, *rec;
    ULONG blk, jkey = 0, root, i, n;
    BOOL ok = FALSE;

    memset(&t, 0, sizeof(t));
    t.bd = bd; t.part_start = vi->start_block; t.spb = vi->spb ? vi->spb : 1;
    t.bsz = vi->sector_size * t.spb; t.nlongs = t.bsz / 4; t.nblocks = vi->num_blocks / t.spb;
    t.reserved = vi->reserved ? vi->reserved : 2; t.tsize = t.nlongs - 56;
    t.ofs = (vi->dostype & 1) == 0; t.intl = (vi->dostype & 7) >= 2;
    if (t.nlongs < 128 || t.nlongs > 8192 || t.nblocks < 8) return FALSE;
    root = (t.nblocks - 1 + t.reserved) / 2;

    buf = (UBYTE *)AllocVec(t.bsz * (CHUNK + 2), MEMF_PUBLIC);
    if (!buf) return FALSE;
    rec = buf + t.bsz * CHUNK;
    elog(e, "Root block damaged - searching for the journal file");
    if (e->jext) jkey = 1;                       /* sidecar: nothing to find on disk */
    for (blk = t.reserved; blk < t.nblocks && !jkey; blk += n) {
        n = t.nblocks - blk; if (n > CHUNK) n = CHUNK;
        if (!ffs_read(&t, blk, n, buf)) continue;
        for (i = 0; i < n; i++) {
            const UBYTE *b = buf + i * t.bsz;
            const UBYTE *nm = b + (t.nlongs - FFS_E_NAME) * 4;
            if (bel(b, 0) != FFS_T_SHORT || (LONG)bel(b, t.nlongs - FFS_E_SECTYPE) != FFS_ST_FILE) continue;
            if (nm[0] != strlen(FFS_JOURNAL_NAME) || memcmp(nm + 1, FFS_JOURNAL_NAME, nm[0]) != 0) continue;
            if (!ffs_checksum_ok(b, t.nlongs) || bel(b, FFS_L_OWNKEY) != blk + i) continue;
            jkey = blk + i;
            memcpy(e->hdr, b, t.bsz);
            break;
        }
    }
    if (!jkey) { elog(e, "Journal file not found"); goto out; }
    {
        ULONG nblk = 0, runs = 0, next = 0;
        struct FfsVol *save = &e->v;
        struct FfsVol keep = *save;
        *save = t;                                   /* journal helpers work on e->v */
        e->jkey = jkey;
        if (e->jext || ffs_file_layout(save, e->hdr, e->jblk, FFS_JOURNAL_BLOCKS, NULL, 0, &nblk, &runs, &next)) {
            e->jcount  = e->jext ? FFS_JOURNAL_BLOCKS : (nblk > FFS_JOURNAL_BLOCKS ? FFS_JOURNAL_BLOCKS : nblk);
            e->payload = e->jext ? t.bsz : (t.ofs ? t.bsz - FFS_OFS_HDR : t.bsz);
            /* the root is at the computed place or where the boot block
             * points (ffs_open accepts both); the record names which */
            ULONG alt = 0, rkey = e->jcount >= 1 && journal_get(e, 0, rec, 32) ? be32(rec, J_HDR * 4) : 0;
            if (ffs_read(save, 0, 1, e->chunk)) alt = bel(e->chunk, 2);
            if (rkey && (rkey == root || (alt && rkey == alt))) root = rkey;
            if (e->jcount >= 1 && journal_get(e, 0, rec, 32) && be32(rec, 0) == JOURNAL_MAGIC &&
                be32(rec, J_STATE * 4) == 1 && be32(rec, J_HDR * 4) == root && be32(rec, J_IMGLEN * 4) == t.bsz) {
                ULONG done = 0, idx = 1;
                BOOL got = TRUE;
                while (done < t.bsz && got) {
                    ULONG m = t.bsz - done; if (m > e->payload) m = e->payload;
                    got = journal_get(e, idx++, e->img + done, m);
                    done += m;
                }
                if (got && ffs_is_header(e->img, t.nlongs, FFS_T_SHORT, FFS_ST_ROOT)) {
                    BlockDev_ArmWrites(bd);
                    if (ffs_write(save, root, 1, e->img)) {
                        elog(e, "Journal: root block %lu restored", (unsigned long)root);
                        e->st->repaired++;
                        ok = TRUE;
                    }
                }
            }
        }
        *save = keep;
        e->jkey = 0; e->jcount = 0;
    }
out:
    FreeVec(buf);
    return ok;
}

/* ------------------------------------------------------------------ */
/* candidates                                                          */
/* ------------------------------------------------------------------ */

static BOOL collect_cb(void *ud, ULONG key, const UBYTE *hdr, LONG sectype)
{
    struct Eng *e = (struct Eng *)ud;
    ULONG nblk = 0, runs = 0, next = 0;
    if (ffs_bm_isfree(&e->v, key)) e->bm_bad = TRUE;
    if (sectype != FFS_ST_FILE || key == e->jkey) return TRUE;
    if (!ffs_file_layout(&e->v, hdr, NULL, 0, NULL, 0, &nblk, &runs, &next)) {
        char nm[32];
        name_of(e, hdr, nm, sizeof(nm));
        elog(e, "Skipping %s: broken block chain", nm);
        e->st->skipped_error++;
        return TRUE;
    }
    if (runs > 1) {
        e->st->candidates++;
        e->st->cand_blocks += nblk;
        return ustack_push(&e->cands, key);
    }
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* one file                                                            */
/* ------------------------------------------------------------------ */

static void report_moves(struct Eng *e, const ULONG *old, ULONG nblk, const ULONG *oldext, ULONG next,
                         ULONG dest, ULONG new_ext)
{
    ULONG i, start = 0;
    if (!e->move) return;
    for (i = 1; i <= nblk; i++) {
        if (i == nblk || old[i] != old[i - 1] + 1) { e->move(e->ud, old[start], i - start, BLK_FREE); start = i; }
    }
    for (i = 0; i < next; i++) e->move(e->ud, oldext[i], 1, BLK_FREE);
    if (new_ext) e->move(e->ud, dest, new_ext, BLK_META);
    e->move(e->ud, dest + new_ext, nblk, BLK_DATA);
}

/* Returns FALSE only on a fatal error (stop the run). */
static BOOL process_file(struct Eng *e, ULONG hdrkey)
{
    struct FfsVol *v = &e->v;
    ULONG nblk = 0, runs = 0, next = 0, new_ext, need, dest, newdata, i;
    ULONG *old = NULL, *oldext = NULL;
    char nm[32];
    BOOL ok = FALSE;

    if (!ffs_read(v, hdrkey, 1, e->hdr) || !ffs_is_header(e->hdr, v->nlongs, FFS_T_SHORT, FFS_ST_FILE)) {
        elog(e, "Skipping block %lu: header changed", (unsigned long)hdrkey);
        e->st->skipped_error++;
        return TRUE;
    }
    name_of(e, e->hdr, nm, sizeof(nm));
    if (!ffs_file_layout(v, e->hdr, NULL, 0, NULL, 0, &nblk, &runs, &next) || nblk == 0) {
        e->st->skipped_error++;
        return TRUE;
    }
    if (runs <= 1) return TRUE;

    old    = (ULONG *)AllocVec((nblk + 1) * 4, MEMF_PUBLIC);
    oldext = (ULONG *)AllocVec((next + 1) * 4, MEMF_PUBLIC);
    if (!old || !oldext) { seterr(e, "out of memory"); goto done; }
    if (!ffs_file_layout(v, e->hdr, old, nblk, oldext, next, &nblk, &runs, &next)) {
        e->st->skipped_error++; ok = TRUE; goto done;
    }
    for (i = 0; i < nblk + next; i++)
        if (ffs_bm_isfree(v, i < nblk ? old[i] : oldext[i - nblk])) {
            elog(e, "Skipping %s: its blocks are marked free in the bitmap", nm);
            e->st->skipped_error++; ok = TRUE; goto done;
        }

    new_ext = (nblk > v->tsize) ? (nblk - v->tsize + v->tsize - 1) / v->tsize : 0;
    need    = nblk + new_ext;
    dest    = ffs_bm_find_run(v, need);
    if (!dest) {
        elog(e, "No free run of %lu blocks for %s (%lu pieces)", (unsigned long)need, nm, (unsigned long)runs);
        e->st->skipped_space++;
        ok = TRUE; goto done;
    }
    newdata = dest + new_ext;

    if (e->o->dry_run) {
        elog(e, "Would move %s: %lu blocks in %lu pieces -> %lu..%lu",
             nm, (unsigned long)nblk, (unsigned long)runs, (unsigned long)dest, (unsigned long)(dest + need - 1));
        for (i = 0; i < nblk; i++) ffs_bm_set(v, old[i], TRUE);
        for (i = 0; i < next; i++) ffs_bm_set(v, oldext[i], TRUE);
        for (i = 0; i < need; i++) ffs_bm_set(v, dest + i, FALSE);
        e->st->moved++; e->st->moved_blocks += nblk;
        ok = TRUE; goto done;
    }

    /* 2. copy data, chunked, grouping consecutive old blocks per read */
    for (i = 0; i < nblk; ) {
        ULONG n = nblk - i, k = 0;
        if (n > CHUNK) n = CHUNK;
        while (k < n) {
            ULONG run = 1;
            while (k + run < n && old[i + k + run] == old[i + k] + 1) run++;
            if (!ffs_read(v, old[i + k], run, e->chunk + k * v->bsz)) { seterr(e, "read error in file data"); goto done; }
            k += run;
        }
        if (v->ofs) {
            for (k = 0; k < n; k++) {
                UBYTE *b = e->chunk + k * v->bsz;
                putl(b, FFS_OFSL_NEXT, (i + k + 1 < nblk) ? newdata + i + k + 1 : 0);
                ffs_set_checksum(b, v->nlongs, FFS_OFSL_CHK);
            }
        }
        if (!ffs_write(v, newdata + i, n, e->chunk)) { seterr(e, "write error copying file data"); goto done; }
        if (!ffs_read(v, newdata + i, n, e->chunk2) || memcmp(e->chunk, e->chunk2, n * v->bsz) != 0) {
            seterr(e, "verify failed: copied data reads back differently"); goto done;
        }
        i += n;
    }

    /* 3. new extension blocks */
    for (i = 0; i < new_ext; i++) {
        UBYTE *b = e->extb;
        ULONG first = v->tsize * (i + 1), cnt = nblk - first, k;
        if (cnt > v->tsize) cnt = v->tsize;
        memset(b, 0, v->bsz);
        putl(b, FFS_L_TYPE, FFS_T_LIST);
        putl(b, FFS_L_OWNKEY, dest + i);
        putl(b, FFS_L_HIGHSEQ, cnt);
        putl(b, FFS_L_FIRSTDATA, newdata + first);
        for (k = 0; k < cnt; k++) putl(b, v->nlongs - 51 - k, newdata + first + k);
        putl(b, v->nlongs - FFS_E_PARENT, hdrkey);
        putl(b, v->nlongs - FFS_E_EXTENSION, (i + 1 < new_ext) ? dest + i + 1 : 0);
        putl(b, v->nlongs - FFS_E_SECTYPE, (ULONG)FFS_ST_FILE);
        ffs_set_checksum(b, v->nlongs, FFS_L_CHECKSUM);
        if (!ffs_write(v, dest + i, 1, b)) { seterr(e, "write error on extension block"); goto done; }
        if (!ffs_read(v, dest + i, 1, e->chunk2) || memcmp(b, e->chunk2, v->bsz) != 0) {
            seterr(e, "verify failed on extension block"); goto done;
        }
    }

    /* 4. new header image */
    memcpy(e->img, e->hdr, v->bsz);
    {
        ULONG cnt = nblk < v->tsize ? nblk : v->tsize, k;
        for (k = 0; k < v->tsize; k++) putl(e->img, FFS_L_TABLE + k, 0);
        for (k = 0; k < cnt; k++) putl(e->img, v->nlongs - 51 - k, newdata + k);
        putl(e->img, FFS_L_HIGHSEQ, cnt);
        putl(e->img, FFS_L_FIRSTDATA, newdata);
        putl(e->img, v->nlongs - FFS_E_EXTENSION, new_ext ? dest : 0);
        ffs_set_checksum(e->img, v->nlongs, FFS_L_CHECKSUM);
    }
    if (!journal_set(e, 1, hdrkey, e->img)) { seterr(e, "journal write failed"); goto done; }

    /* 5. commit */
    if (!ffs_write(v, hdrkey, 1, e->img)) {
        seterr(e, "header write failed - run again to let the journal repair it");
        e->st->needs_validation = TRUE;
        goto done;
    }

    /* 6. bitmap + flush + journal clear */
    for (i = 0; i < nblk; i++) ffs_bm_set(v, old[i], TRUE);
    for (i = 0; i < next; i++) ffs_bm_set(v, oldext[i], TRUE);
    for (i = 0; i < need; i++) ffs_bm_set(v, dest + i, FALSE);
    if (!ffs_bm_flush(v)) { seterr(e, v->err); e->st->needs_validation = TRUE; goto done; }
    BlockDev_Update(v->bd);
    if (!journal_set(e, 0, 0, NULL)) { seterr(e, "journal clear failed"); e->st->needs_validation = TRUE; goto done; }

    e->st->moved++;
    e->st->moved_blocks += nblk;
    elog(e, "Moved %s: %lu blocks, %lu pieces -> 1", nm, (unsigned long)nblk, (unsigned long)runs);
    report_moves(e, old, nblk, oldext, next, dest, new_ext);
    ok = TRUE;

done:
    if (old)    FreeVec(old);
    if (oldext) FreeVec(oldext);
    return ok;
}

/* ------------------------------------------------------------------ */
/* run                                                                 */
/* ------------------------------------------------------------------ */

LONG FfsDefrag_Run(struct BlockDev *bd, const struct VolumeInfo *vi,
                   const struct FfsDefragOpts *opts, const struct FfsDefragPlatform *pf,
                   FfsDefragLog log, FfsDefragMove move, void *ud,
                   struct FfsDefragStats *stats, char *err, ULONG errlen)
{
    struct Eng *e;
    LONG rc = RETURN_ERROR;
    BOOL inhibited = FALSE, opened = FALSE, journal_made = FALSE, journal_checked = FALSE;
    ULONG i;

    memset(stats, 0, sizeof(*stats));
    if (err && errlen) err[0] = '\0';
    if (vi->fskind != FS_FFS) { if (err) strncpy(err, "not an OFS/FFS volume", errlen - 1); return RETURN_ERROR; }
    if (vi->is_boot && !opts->dry_run) {
        if (err) strncpy(err, "the boot volume is not defragmented in this version", errlen - 1);
        return RETURN_ERROR;
    }

    e = (struct Eng *)AllocVec(sizeof(*e), MEMF_PUBLIC | MEMF_CLEAR);
    if (!e) { if (err) strncpy(err, "out of memory", errlen - 1); return RETURN_ERROR; }
    e->vi = vi; e->o = opts; e->log = log; e->move = move; e->ud = ud; e->st = stats;
    e->pf = pf; e->jext = pf->journal_io != NULL;
    e->err = err; e->errlen = errlen;

    if (!opts->dry_run) {
        if (!pf->create_journal(pf->ud, vi, FFS_JOURNAL_BLOCKS * vi->sector_size * (vi->spb ? vi->spb : 1), err, errlen))
            goto done;
        journal_made = TRUE;
        if (!pf->inhibit(pf->ud, vi, TRUE, err, errlen)) goto done;
        inhibited = TRUE;
    }

    {
        ULONG bsz = vi->sector_size * (vi->spb ? vi->spb : 1);
        e->hdr = (UBYTE *)AllocVec(bsz * 4 + bsz * CHUNK * 2, MEMF_PUBLIC);
        if (!e->hdr) { seterr(e, "out of memory for buffers"); goto done; }
        e->img    = e->hdr + bsz;
        e->jbuf   = e->img + bsz;
        e->extb   = e->jbuf + bsz;
        e->chunk  = e->extb + bsz;
        e->chunk2 = e->chunk + bsz * CHUNK;
    }
    if (!ffs_open(&e->v, bd, vi)) {
        char first[96];
        strncpy(first, e->v.err, sizeof(first) - 1); first[sizeof(first) - 1] = '\0';
        if (opts->dry_run || !recover_root(e, bd, vi) || !ffs_open(&e->v, bd, vi)) {
            seterr(e, first);
            goto done;
        }
    }
    opened = TRUE;

    if (!opts->dry_run) {
        if (!journal_find(e)) { seterr(e, "journal file " FFS_JOURNAL_NAME " not found in the root directory"); goto done; }
        if (!journal_recover(e)) { seterr(e, "journal check failed"); goto done; }
        journal_checked = TRUE;
    }
    if (!e->v.bm_flag_valid) {
        seterr(e, "bitmap flag is invalid: mount the volume and let the filesystem validate it first");
        rc = RETURN_WARN;
        goto done;
    }
    /* a bitmap that frees its own pages or the root is corrupt even with
     * the flag valid; writing by it would overwrite them */
    {
        BOOL bad = ffs_bm_isfree(&e->v, e->v.root);
        for (i = 0; i < e->v.pages && !bad; i++) bad = ffs_bm_isfree(&e->v, e->v.page_key[i]);
        for (i = 0; i < e->v.ext_count && !bad; i++) bad = ffs_bm_isfree(&e->v, e->v.ext_key[i]);
        if (bad) { seterr(e, "bitmap marks the root or a bitmap block free: let the filesystem validate the volume first"); goto done; }
    }

    if (!ffs_walk(&e->v, collect_cb, e)) { seterr(e, "directory walk failed"); goto done; }
    if (e->bm_bad) { seterr(e, "bitmap marks a file or directory block free: let the filesystem validate the volume first"); goto done; }
    elog(e, "%lu fragmented file(s), %lu blocks, %lu blocks free",
         (unsigned long)stats->candidates, (unsigned long)stats->cand_blocks, (unsigned long)e->v.free_blocks);
    if (stats->candidates == 0) { rc = RETURN_WARN; goto done; }

    if (!opts->dry_run) {
        BlockDev_ArmWrites(bd);
        if (!root_flag_journaled(e, FALSE)) { seterr(e, e->v.err[0] ? e->v.err : "journal write failed"); goto done; }
        e->writes_begun = TRUE;
    }

    for (i = 0; i < e->cands.n; i++) {
        if (!process_file(e, e->cands.v[i])) {
            rc = e->writes_begun ? RETURN_FAIL : RETURN_ERROR;
            goto done;
        }
        if (e->move && !e->move(e->ud, 0, 0, 0xFF)) { stats->aborted = TRUE; break; }   /* poll for abort */
    }

    if (!opts->dry_run) {
        if (!ffs_bm_flush(&e->v)) { seterr(e, e->v.err); stats->needs_validation = TRUE; rc = RETURN_FAIL; goto done; }
        if (!root_flag_journaled(e, TRUE)) { seterr(e, e->v.err[0] ? e->v.err : "journal write failed"); stats->needs_validation = TRUE; rc = RETURN_FAIL; goto done; }
        BlockDev_Update(bd);
    }
    rc = (stats->skipped_space || stats->skipped_error || stats->aborted) ? RETURN_WARN : RETURN_OK;

done:
    if (e->writes_begun && stats->needs_validation)
        elog(e, "Bitmap left marked invalid; the filesystem will validate the volume on the next mount.");
    if (opened) ffs_close(&e->v);
    if (e->hdr) FreeVec(e->hdr);
    ustack_free(&e->cands);
    if (inhibited) pf->inhibit(pf->ud, vi, FALSE, NULL, 0);
    /* a journal that was never checked may hold the record of an earlier
     * interrupted run: keep it for the next attempt */
    if (journal_made && journal_checked && !stats->needs_validation) pf->delete_journal(pf->ud, vi);
    FreeVec(e);
    return rc;
}
