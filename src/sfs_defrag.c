/*
 * sfs_defrag.c - drive the SmartFilesystem handler's defragmenter.
 * See sfs_defrag.h.  Packet numbers and tags from the SFS 1.279 sources
 * (fs/packets.h, fs/query.h); the step protocol follows SFSdefrag.c.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#ifndef AMIDEFRAG_HOST
#include <utility/tagitem.h>
#endif
#include "clib.h"
#include "volume.h"
#include "sfs_defrag.h"

#define SFS_PACKET_BASE              0xF00000UL
#define ACTION_SFS_QUERY             (SFS_PACKET_BASE + 1)
#define ACTION_SFS_DEFRAGMENT_INIT   (SFS_PACKET_BASE + 301)
#define ACTION_SFS_DEFRAGMENT_STEP   (SFS_PACKET_BASE + 302)

#define ASQ_BLOCK_SIZE     (TAG_USER + 1001)
#define ASQ_TOTAL_BLOCKS   (TAG_USER + 1002)
#define ASQ_VERSION        (TAG_USER + 5001)

#define SFS_MIN_VERSION    ((1UL << 16) + 83)      /* SFSdefrag's own limit */
#define STEP_LONGS         1000                    /* as SFSdefrag: arg2 */
#define STEP_SLACK         20                      /* "at least 20 longwords" */
#define ID_MOVE            0x4D4F5645UL            /* 'MOVE' */
#define ID_DONE            0x444F4E45UL            /* 'DONE' */

static void seterr(char *err, ULONG errlen, const char *fmt, ULONG a, ULONG b)
{
    if (err && errlen) snprintf(err, errlen, fmt, (unsigned long)a, (unsigned long)b);
}

BOOL SfsDefrag_Available(const struct VolumeInfo *vi)
{
    const char *d = vi->devname;
    BOOL is_file = (d[0] == 'F' || d[0] == 'f') && (d[1] == 'I' || d[1] == 'i') &&
                   (d[2] == 'L' || d[2] == 'l') && (d[3] == 'E' || d[3] == 'e') && d[4] == ':';
    return vi->fskind == FS_SFS && vi->mounted && !is_file;
}

#ifdef AMIDEFRAG_HOST

BOOL SfsDefrag_Probe(const struct VolumeInfo *vi, struct SfsDefragStats *st, char *err, ULONG errlen)
{
    (void)vi;
    memset(st, 0, sizeof(*st));
    seterr(err, errlen, "SFS is defragmented through the SmartFilesystem handler: mount the volume on an Amiga", 0, 0);
    return FALSE;
}

LONG SfsDefrag_Run(const struct VolumeInfo *vi, ULONG expect_blocks, ULONG expect_bsize,
                   SfsDefragLog log, SfsDefragMove move, void *ud,
                   struct SfsDefragStats *st, char *err, ULONG errlen)
{
    (void)vi; (void)expect_blocks; (void)expect_bsize; (void)log; (void)move; (void)ud;
    memset(st, 0, sizeof(*st));
    seterr(err, errlen, "SFS is defragmented through the SmartFilesystem handler: mount the volume on an Amiga", 0, 0);
    return RETURN_ERROR;
}

#else

/* The handler task's stack: the defragmenter runs on it. */
static ULONG handler_stack(struct MsgPort *port)
{
    struct Task *t = port->mp_SigTask;
    if (!t || !t->tc_SPUpper || !t->tc_SPLower) return 0;
    return (ULONG)((UBYTE *)t->tc_SPUpper - (UBYTE *)t->tc_SPLower);
}

static struct MsgPort *probe(const struct VolumeInfo *vi, struct SfsDefragStats *st, char *err, ULONG errlen)
{
    struct MsgPort *port;
    struct TagItem tags[4];
    char dev[40];

    memset(st, 0, sizeof(*st));
    if (err && errlen) err[0] = '\0';
    if (!SfsDefrag_Available(vi)) {
        seterr(err, errlen, "SFS is defragmented through the SmartFilesystem handler: the volume must be mounted", 0, 0);
        return NULL;
    }
    snprintf(dev, sizeof(dev), "%s:", vi->dosname);
    port = DeviceProc((CONST_STRPTR)dev);
    if (!port) { seterr(err, errlen, "no handler process for the volume (error %lu)", (ULONG)IoErr(), 0); return NULL; }

    tags[0].ti_Tag = ASQ_VERSION;      tags[0].ti_Data = 0;
    tags[1].ti_Tag = ASQ_TOTAL_BLOCKS; tags[1].ti_Data = 0;
    tags[2].ti_Tag = ASQ_BLOCK_SIZE;   tags[2].ti_Data = 0;
    tags[3].ti_Tag = TAG_DONE;         tags[3].ti_Data = 0;
    if (DoPkt(port, (LONG)ACTION_SFS_QUERY, (LONG)tags, 0, 0, 0, 0) == DOSFALSE) {
        seterr(err, errlen, "the handler does not answer ACTION_SFS_QUERY (error %lu): not SmartFilesystem?", (ULONG)IoErr(), 0);
        return NULL;
    }
    st->version      = tags[0].ti_Data;
    st->total_blocks = tags[1].ti_Data;
    st->block_size   = tags[2].ti_Data;
    st->stack        = handler_stack(port);
    if (st->version < SFS_MIN_VERSION) {
        seterr(err, errlen, "SmartFilesystem %lu.%lu is too old: the defragment packets need 1.83 or newer",
               st->version >> 16, st->version & 0xFFFF);
        return NULL;
    }
    return port;
}

BOOL SfsDefrag_Probe(const struct VolumeInfo *vi, struct SfsDefragStats *st, char *err, ULONG errlen)
{
    return probe(vi, st, err, errlen) != NULL;
}

LONG SfsDefrag_Run(const struct VolumeInfo *vi, ULONG expect_blocks, ULONG expect_bsize,
                   SfsDefragLog log, SfsDefragMove move, void *ud,
                   struct SfsDefragStats *st, char *err, ULONG errlen)
{
    struct MsgPort *port;
    ULONG *buf = NULL;
    LONG   rc = RETURN_ERROR;
    BOOL   stop = FALSE, done = FALSE;
    ULONG  last_log_blocks = 0;

    port = probe(vi, st, err, errlen);
    if (!port) return RETURN_ERROR;
    if (st->total_blocks != expect_blocks || st->block_size != expect_bsize) {
        seterr(err, errlen, "the handler reports %lu blocks of %lu bytes, the map differs: analyze again",
               st->total_blocks, st->block_size);
        return RETURN_ERROR;
    }

    buf = (ULONG *)AllocVec((STEP_LONGS + STEP_SLACK) * sizeof(ULONG), MEMF_PUBLIC | MEMF_CLEAR);
    if (!buf) { seterr(err, errlen, "out of memory", 0, 0); return RETURN_ERROR; }

    if (DoPkt(port, (LONG)ACTION_SFS_DEFRAGMENT_INIT, 0, 0, 0, 0, 0) == DOSFALSE) {
        seterr(err, errlen, "ACTION_SFS_DEFRAGMENT_INIT failed (error %lu)", (ULONG)IoErr(), 0);
        FreeVec(buf);
        return RETURN_ERROR;
    }
    if (log) {
        char line[96];
        snprintf(line, sizeof(line), "SmartFilesystem %lu.%lu, %lu blocks, handler stack %lu: defragmenter started",
                 (unsigned long)(st->version >> 16), (unsigned long)(st->version & 0xFFFF),
                 (unsigned long)st->total_blocks, (unsigned long)st->stack);
        log(ud, line);
    }

    while (!done && !stop) {
        ULONG i = 0;
        buf[0] = 0;
        if (DoPkt(port, (LONG)ACTION_SFS_DEFRAGMENT_STEP, (LONG)buf, STEP_LONGS, 0, 0, 0) == DOSFALSE) {
            seterr(err, errlen, "ACTION_SFS_DEFRAGMENT_STEP failed after %lu steps (error %lu)", st->steps, (ULONG)IoErr());
            rc = RETURN_FAIL;
            goto out;
        }
        st->steps++;
        /* The handler committed this whole step before replying, so every
         * MOVE is applied to the map even when the caller asks to stop. */
        while (i + 2 <= STEP_LONGS + STEP_SLACK) {
            ULONG id = buf[i], len = buf[i + 1];
            if (id == 0) break;
            if (id == ID_MOVE && len == 3 && i + 5 <= STEP_LONGS + STEP_SLACK) {
                ULONG n = buf[i + 2], src = buf[i + 3], dst = buf[i + 4];
                st->moves++;
                st->moved_blocks += n;
                if (n && src < st->total_blocks && dst < st->total_blocks && move &&
                    !move(ud, src, dst, n)) stop = TRUE;
            } else if (id == ID_DONE) {
                done = TRUE;
            }
            if (i + 2 + len < i) break;          /* corrupt length */
            i += 2 + len;
        }
        if (!done && !stop && move && !move(ud, 0, 0, 0)) stop = TRUE;
        if (log && st->moved_blocks - last_log_blocks >= 4096) {
            char line[96];
            last_log_blocks = st->moved_blocks;
            snprintf(line, sizeof(line), "%lu blocks moved in %lu steps",
                     (unsigned long)st->moved_blocks, (unsigned long)st->steps);
            log(ud, line);
        }
    }
    if (stop && !done) { st->aborted = TRUE; rc = RETURN_WARN; }
    else rc = RETURN_OK;

out:
    /* so that an analysis right after the run reads the final state */
    DoPkt(port, ACTION_FLUSH, 0, 0, 0, 0, 0);
    FreeVec(buf);
    return rc;
}

#endif /* AMIDEFRAG_HOST */
