/*
 * ffs_defrag.h - the FFS move engine.
 *
 * Makes every fragmented file on an OFS/FFS volume contiguous, one file at
 * a time, with the volume inhibited.  Per file:
 *
 *   1. find the smallest free run that holds the data plus the extension
 *      blocks the file needs (best fit); no fit -> the file is skipped
 *   2. copy the data blocks into the run (OFS: next-pointers and
 *      checksums patched), read every block back and compare
 *   3. write the new extension blocks into the run
 *   4. save the complete new header image in the journal file, then
 *   5. rewrite the file header in place - the one commit write -
 *   6. mark old blocks free / new blocks used in the bitmap pages, flush
 *      the driver (CMD_UPDATE), clear the journal entry
 *
 * The root block carries bitmap-flag = invalid from the first write until
 * the last bitmap page is on disk, exactly as FFS does for its own writes.
 * A crash anywhere leaves either the old or the new copy referenced by the
 * header, never both, so the FFS validator can only ever find leaked
 * blocks, which it frees.  A torn header write is repaired from the
 * journal on the next run.
 *
 * File headers, directories and the volume identity never move or change,
 * so locks held on the volume survive the inhibit / uninhibit cycle.
 */
#ifndef FFS_DEFRAG_H
#define FFS_DEFRAG_H

#include <exec/types.h>
#include "blockdev.h"
#include "volume.h"

#define FFS_JOURNAL_NAME   "AmiDefrag.journal"
#define FFS_JOURNAL_BLOCKS 4            /* file size in FS blocks */

struct FfsDefragOpts {
    BOOL dry_run;        /* plan and report only, nothing written */
};

struct FfsDefragStats {
    ULONG candidates;        /* fragmented files found */
    ULONG cand_blocks;       /* their data blocks */
    ULONG moved;             /* files made contiguous */
    ULONG moved_blocks;
    ULONG skipped_space;     /* no free run large enough */
    ULONG skipped_error;     /* unreadable / inconsistent file */
    ULONG repaired;          /* headers restored from the journal */
    BOOL  needs_validation;  /* bitmap flag left invalid on purpose */
    BOOL  aborted;           /* stopped by the caller between files */
};

typedef void (*FfsDefragLog)(void *ud, const char *line);
/* Block range changed class (BLK_* from fsmap.h); return FALSE to stop
 * after the current file. */
typedef BOOL (*FfsDefragMove)(void *ud, ULONG blk, ULONG n, UBYTE cls);

/* OS services the engine needs; the Amiga and host builds provide them. */
struct FfsDefragPlatform {
    BOOL (*inhibit)(void *ud, const struct VolumeInfo *v, BOOL on, char *err, ULONG errlen);
    BOOL (*create_journal)(void *ud, const struct VolumeInfo *v, ULONG bytes, char *err, ULONG errlen);
    BOOL (*delete_journal)(void *ud, const struct VolumeInfo *v);
    void *ud;
};
const struct FfsDefragPlatform *FfsDefrag_DefaultPlatform(void);

/* Return: RETURN_OK done, RETURN_WARN done with skipped files or nothing
 * to do, RETURN_ERROR refused before any write (err says why),
 * RETURN_FAIL error after writes began (stats->needs_validation). */
LONG FfsDefrag_Run(struct BlockDev *bd, const struct VolumeInfo *vi,
                   const struct FfsDefragOpts *opts, const struct FfsDefragPlatform *pf,
                   FfsDefragLog log, FfsDefragMove move, void *ud,
                   struct FfsDefragStats *stats, char *err, ULONG errlen);

#endif /* FFS_DEFRAG_H */
