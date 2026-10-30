/*
 * ffs_core.h - OFS/FFS volume access shared by the map builder and the
 * move engine: root location, block layout constants, a RAM copy of the
 * bitmap with dirty-page write-back, checksums, and a directory walker.
 *
 * All block numbers are FS blocks relative to the partition start.
 * Reads go straight to the device; writes only exist on an armed
 * BlockDev (see blockdev.h) and are issued by the engine alone.
 */
#ifndef FFS_CORE_H
#define FFS_CORE_H

#include <exec/types.h>
#include "blockdev.h"
#include "volume.h"

/* block types / secondary types */
#define FFS_T_SHORT     2UL
#define FFS_T_DATA      8UL
#define FFS_T_LIST      16UL
#define FFS_T_DIRCACHE  33UL
#define FFS_ST_ROOT     1L
#define FFS_ST_USERDIR  2L
#define FFS_ST_SOFTLINK 3L
#define FFS_ST_LINKDIR  4L
#define FFS_ST_FILE     (-3L)
#define FFS_ST_LINKFILE (-4L)

/* longword indices inside header blocks */
#define FFS_L_TYPE      0
#define FFS_L_OWNKEY    1
#define FFS_L_HIGHSEQ   2
#define FFS_L_DATASIZE  3
#define FFS_L_FIRSTDATA 4
#define FFS_L_CHECKSUM  5
#define FFS_L_TABLE     6          /* hash table / data block table start */
/* counted from the end (nlongs - n) */
#define FFS_E_BMFLAG    50
#define FFS_E_BMPAGES   49         /* 25 entries: 49 .. 25 */
#define FFS_E_BMEXT     24
#define FFS_E_DISKMOD   10         /* vrb_DiskMod days (3 longs) */
#define FFS_E_NAME      20         /* volume name BCPL string (root) */
#define FFS_E_CREATE    7          /* creation days/mins/ticks (root) */
#define FFS_E_HASHCHAIN 4
#define FFS_E_PARENT    3
#define FFS_E_EXTENSION 2
#define FFS_E_SECTYPE   1
/* OFS data block header */
#define FFS_OFS_HDR     24         /* bytes before payload */
#define FFS_OFSL_HDRKEY 1
#define FFS_OFSL_SEQ    2
#define FFS_OFSL_SIZE   3
#define FFS_OFSL_NEXT   4
#define FFS_OFSL_CHK    5

struct FfsVol {
    struct BlockDev *bd;
    ULONG  part_start;         /* device block of FS block 0 */
    ULONG  spb;                /* device blocks per FS block */
    ULONG  bsz;                /* FS block size in bytes */
    ULONG  nlongs;
    ULONG  nblocks;
    ULONG  reserved;
    ULONG  tsize;              /* table entries per header: nlongs - 56 */
    ULONG  root;
    BOOL   ofs;                /* OFS data blocks carry headers */
    BOOL   dircache;           /* DOS\4 / DOS\5 */
    BOOL   intl;               /* DOS\2+ international hashing */
    UBYTE *rootbuf;            /* root block as read */
    /* bitmap, RAM copy: one bit per block from `reserved`, LSB first, 1 = free */
    ULONG *bm;
    ULONG  bm_longs;
    ULONG  bpb;                /* blocks per bitmap page */
    ULONG  pages;
    ULONG *page_key;           /* block number of each page */
    UBYTE *page_dirty;
    ULONG  ext_count;          /* bm_ext blocks in the chain */
    ULONG *ext_key;
    BOOL   bm_flag_valid;
    ULONG  free_blocks;
    ULONG  used_blocks;
    char   err[96];
    UBYTE *scratch;            /* one FS block for internal use */
};

BOOL ffs_open(struct FfsVol *v, struct BlockDev *bd, const struct VolumeInfo *vi);
void ffs_close(struct FfsVol *v);

BOOL ffs_read (struct FfsVol *v, ULONG blk, ULONG count, void *buf);
BOOL ffs_write(struct FfsVol *v, ULONG blk, ULONG count, const void *buf);

/* sum of all longs == 0 convention; `at` = index of the checksum long */
void  ffs_set_checksum(UBYTE *block, ULONG nlongs, ULONG at);
BOOL  ffs_checksum_ok (const UBYTE *block, ULONG nlongs);
BOOL  ffs_is_header   (const UBYTE *b, ULONG nlongs, ULONG type, LONG sectype);

/* bitmap */
BOOL  ffs_bm_isfree(const struct FfsVol *v, ULONG blk);
void  ffs_bm_set   (struct FfsVol *v, ULONG blk, BOOL isfree);
BOOL  ffs_bm_flush (struct FfsVol *v);                 /* write dirty pages */
/* smallest free run of at least `need` blocks (best fit); 0 = none */
ULONG ffs_bm_find_run(const struct FfsVol *v, ULONG need);

/* root block bitmap-valid flag, written immediately */
BOOL  ffs_write_root_flag(struct FfsVol *v, BOOL valid);

/* Data pointers of the file whose header (already read) is in hdr:
 * fills ptrs[0..max-1] (NULL allowed), exts[] with the extension chain,
 * returns block count in *nblk, run count in *runs (own extension blocks
 * sitting inside a run do not split it), ext count in *next.  FALSE on a
 * broken chain. */
BOOL  ffs_file_layout(struct FfsVol *v, const UBYTE *hdr, ULONG *ptrs, ULONG max,
                      ULONG *exts, ULONG maxext, ULONG *nblk, ULONG *runs, ULONG *next);

/* Directory walk: cb(ud, key, header, sectype) for every header reached
 * from the root (files, dirs, links).  The header buffer belongs to the
 * walker and is valid during the call only. */
typedef BOOL (*FfsWalkCb)(void *ud, ULONG key, const UBYTE *hdr, LONG sectype);
BOOL  ffs_walk(struct FfsVol *v, FfsWalkCb cb, void *ud);

/* Look a name up in the root directory; returns the header key or 0. */
ULONG ffs_root_lookup(struct FfsVol *v, const char *name);

#endif /* FFS_CORE_H */
