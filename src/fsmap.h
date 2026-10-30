/*
 * fsmap.h - the block map: one classification per filesystem block.
 *
 * This is the data behind the Windows-style grid.  A map is built by
 * reading the volume's own structures (bitmap, directories, extent lists)
 * READ-ONLY; nothing here ever writes.  Each FS block gets one of:
 *
 *   BLK_FREE   free according to the filesystem bitmap
 *   BLK_DATA   file data of a file stored in one contiguous run
 *   BLK_FRAG   file data of a file stored in two or more runs
 *   BLK_META   filesystem structures: boot/root/bitmap blocks, file and
 *              directory headers, extension blocks, anodes, admin space...
 *   BLK_RESV   reserved for the filesystem but currently unused (PFS3's
 *              reserved area, SFS end block, ...) - never user data
 *   BLK_UNKN   marked in use by the bitmap but not reached from any
 *              structure (lost blocks, or a parser gap)
 *
 * Storage is one nibble per block (nblocks/2 bytes): a 4 GB FFS volume at
 * 512-byte blocks needs 4 MB.  The GUI condenses that into cells.
 */
#ifndef FSMAP_H
#define FSMAP_H

#include <exec/types.h>
#include "blockdev.h"
#include "volume.h"

#define BLK_FREE 0
#define BLK_DATA 1
#define BLK_FRAG 2
#define BLK_META 3
#define BLK_RESV 4
#define BLK_UNKN 5
#define BLK_CLASSES 6

struct FsMap {
    ULONG  blocksize;        /* FS block size in bytes */
    ULONG  nblocks;          /* FS blocks in the volume */
    UBYTE *nib;              /* (nblocks + 1) / 2 bytes, two blocks per byte */
    ULONG  count[BLK_CLASSES];
    ULONG  files;            /* files with data (SFS: extent chains) */
    ULONG  dirs;             /* directories visited (0 where not walked) */
    ULONG  frag_files;       /* files in more than one run */
    ULONG  extra_runs;       /* sum over files of (runs - 1) */
    ULONG  worst_runs;       /* most runs seen in one file */
    ULONG  bitmap_free;      /* free count the filesystem itself claims */
    BOOL   bitmap_valid;     /* FFS bm_flag / SFS-PFS consistency */
    UWORD  fskind;
    char   fsname[32];
    char   volname[32];
    char   note[128];        /* warning or error text, "" when clean */
};

/* Progress callback: done/total in whatever unit the stage uses; return
 * FALSE to abort the scan.  what is a short stage name. */
typedef BOOL (*FsMapProgress)(void *ud, ULONG done, ULONG total, const char *what);

/* Build the map of volume v (opened on bd).  TRUE when a map exists -
 * possibly with m->note set to a warning.  FALSE: m->note says why. */
BOOL FsMap_Build(struct BlockDev *bd, const struct VolumeInfo *v,
                 struct FsMap *m, FsMapProgress cb, void *ud);
void FsMap_Free(struct FsMap *m);

static inline UBYTE FsMap_Get(const struct FsMap *m, ULONG blk)
{
    UBYTE b = m->nib[blk >> 1];
    return (blk & 1) ? (UBYTE)(b >> 4) : (UBYTE)(b & 15);
}

void FsMap_Set(struct FsMap *m, ULONG blk, UBYTE cls);
void FsMap_SetRange(struct FsMap *m, ULONG first, ULONG n, UBYTE cls);

/* Condense blocks [first, first+n) into one class for a display cell:
 * any fragmented data wins, otherwise the most common non-free class,
 * otherwise free. */
UBYTE FsMap_Cell(const struct FsMap *m, ULONG first, ULONG n);

/* Recount m->count[] from the nibbles (after all marking is done). */
void FsMap_Recount(struct FsMap *m);

/* Single-character legend used by the CLI map. */
char FsMap_ClassChar(UBYTE cls);
const char *FsMap_ClassName(UBYTE cls);

#endif /* FSMAP_H */
