/*
 * sfs_defrag.h - SFS defragmentation through the SmartFilesystem handler.
 *
 * SFS keeps all metadata in B-trees and admin spaces and commits changes
 * through its own transaction log, so there is no single-sector commit
 * an external program could use.  The handler, however, contains a
 * defragmenter that is driven from outside in single steps through two
 * packets (fs/packets.h in the SFS sources):
 *
 *   ACTION_SFS_DEFRAGMENT_INIT   reset the defragmenter to the volume start
 *   ACTION_SFS_DEFRAGMENT_STEP   do one step; the reply buffer lists what
 *                                moved as {id, length, data[]} records:
 *                                "MOVE", 3, blocks, source, destination
 *                                "DONE", 0            nothing left to do
 *                                0                    end of the list
 *
 * Every step is one transaction of the handler, so the volume is
 * consistent at every moment and stays mounted and usable while this
 * runs.  The engine here only drives the packets, keeps count, reports
 * the moves to the caller (for the live map) and stops between steps when
 * asked.  It needs a mounted volume and a handler of version 1.83 or
 * newer; on image files (host build, IMAGE=) SFS stays map-only.
 */
#ifndef SFS_DEFRAG_H
#define SFS_DEFRAG_H

#include <exec/types.h>
#include "volume.h"

struct SfsDefragStats {
    ULONG version;           /* handler version, major << 16 | minor */
    ULONG total_blocks;      /* the handler's block count */
    ULONG block_size;
    ULONG stack;             /* the handler task's stack in bytes (0 = unknown) */
    ULONG steps;             /* STEP packets sent */
    ULONG moves;             /* MOVE records received */
    ULONG moved_blocks;      /* blocks moved in total */
    BOOL  aborted;           /* stopped by the caller between steps */
};

typedef void (*SfsDefragLog)(void *ud, const char *line);
/* n blocks moved from src to dst; n == 0 is the between-steps poll.
 * Return FALSE to stop before the next step (the current step is
 * complete already: the handler committed it before replying). */
typedef BOOL (*SfsDefragMove)(void *ud, ULONG src, ULONG dst, ULONG n);

/* TRUE when the volume has a handler this engine can talk to. */
BOOL SfsDefrag_Available(const struct VolumeInfo *vi);

/* Ask the handler who it is without changing anything: fills version,
 * total_blocks, block_size and stack.  The defragmenter runs inside the
 * handler task on the handler's stack, which is shown for information:
 * SFS's own SFSdefrag runs fine on the 4000-byte default. */
BOOL SfsDefrag_Probe(const struct VolumeInfo *vi, struct SfsDefragStats *st, char *err, ULONG errlen);

/* expect_blocks / expect_bsize: the map's view of the volume; the run is
 * refused when the handler disagrees.  Return: RETURN_OK done,
 * RETURN_WARN stopped early, RETURN_ERROR refused before the first step
 * (err says why), RETURN_FAIL a step failed (err has the handler's error;
 * the volume is consistent, the handler never leaves it otherwise). */
LONG SfsDefrag_Run(const struct VolumeInfo *vi, ULONG expect_blocks, ULONG expect_bsize,
                   SfsDefragLog log, SfsDefragMove move, void *ud,
                   struct SfsDefragStats *st, char *err, ULONG errlen);

#endif /* SFS_DEFRAG_H */
