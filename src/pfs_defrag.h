/*
 * pfs_defrag.h - the PFS3 move engine.
 *
 * Makes every fragmented file on a PFS3 volume contiguous, one file at a
 * time, with the volume inhibited.  A PFS3 file is a chain of anodes, one
 * per contiguous run; the file's directory entry names the first anode
 * and never changes.  Per file:
 *
 *   1. find the smallest free run that holds the whole file (best fit);
 *      no fit -> the file is skipped
 *   2. copy the data there, read every sector back and compare
 *   3. write the file's old chain and the new run into the journal file
 *   4. mark the new run used in the data bitmap (sector writes)
*   5. rewrite the FIRST anode in place to {whole size, new run, end of
 *      chain} - a 512-byte sector write, the commit (an anode that
 *      straddles a sector boundary takes two, blocknr's sector first,
 *      and the recovery below decides by blocknr); the anode is read
 *      back and compared
 *   6. clear the other anodes of the old chain (free anodes are all
 *      zero, exactly what the handler writes), mark the old runs free in
 *      the bitmap, flush the driver, clear the journal entry
 *
 * Every write before the commit only ever marks free blocks as used, and
 * every write after it only releases blocks nothing references any more,
 * so a crash at any sector boundary leaves a volume the handler reads
 * correctly: at worst some blocks or anode slots are leaked, and the
 * journal entry lets the next run finish (chain committed) or undo (not
 * committed) the interrupted file, after checking that no file reaches
 * the blocks it is about to release.
 *
 * pfs3aio itself commits by relocating its dirty reserved blocks and
 * rewriting the rootblock last; AmiDefrag never writes the rootblock, the
 * reserved bitmap or any index, super, directory or deldir block, and
 * the handler re-reads everything after the uninhibit.  Datestamps in
 * blocks are not checked by the handler (pfs3aio sources), so the
 * in-place anode and bitmap sectors keep theirs.
 *
 * Shares the option, statistics, platform and callback types of the FFS
 * engine (ffs_defrag.h) so the CLI and GUI drive both the same way.
 * stats->needs_validation here means "run AmiDefrag again: the journal
 * will finish or undo the interrupted file".
 */
#ifndef PFS_DEFRAG_H
#define PFS_DEFRAG_H

#include <exec/types.h>
#include "blockdev.h"
#include "volume.h"
#include "ffs_defrag.h"

#define PFS_JOURNAL_BYTES   (128UL * 1024UL)   /* created with this size: 10900 pieces */
#define PFS_JOURNAL_MAX_SEC 256                 /* sectors used of it */

/* Return: RETURN_OK done, RETURN_WARN done with skipped files or nothing
 * to do, RETURN_ERROR refused before any write (err says why),
 * RETURN_FAIL error after writes began (stats->needs_validation). */
LONG PfsDefrag_Run(struct BlockDev *bd, const struct VolumeInfo *vi,
                   const struct FfsDefragOpts *opts, const struct FfsDefragPlatform *pf,
                   FfsDefragLog log, FfsDefragMove move, void *ud,
                   struct FfsDefragStats *stats, char *err, ULONG errlen);

#endif /* PFS_DEFRAG_H */
