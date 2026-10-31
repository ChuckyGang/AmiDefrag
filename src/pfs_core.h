/*
 * pfs_core.h - Professional File System 3 volume access shared by the map
 * builder (pfs_map.c) and the move engine (pfs_defrag.c).
 *
 * Layouts from pfs3aio (blocks.h, allocation.c, anodes.c, update.c).
 * PFS3 counts in 512-byte sectors relative to the partition:
 *   0, 1            boot sectors
 *   2               rootblock (512 bytes) followed, inside the same
 *                   rblkcluster-sector cluster, by the RESERVED bitmap
 *   firstreserved .. lastreserved   reserved area of reserved_blksize-byte
 *                   blocks (1/2/4 KB): bitmap index 'MI', bitmap 'BM',
 *                   anode index 'IB', anode 'AB', super index 'SB',
 *                   directory 'DB', deldir 'DD', extension 'EX'
 *   lastreserved+1 .. disksize-1    user data, one bitmap bit per sector
 *
 * Anodes {clustersize, blocknr, next} (12 bytes) from offset 16 of an
 * 'AB' block; a file is its anode chain, one anode per contiguous run.
 * anodenr -> (seqnr, offset): split mode seqnr = anodenr >> 16, offset =
 * anodenr & 0xffff, otherwise divide by anodesperblock.  The 'AB' block
 * of seqnr comes from anode index block seqnr / indexperblock, slot
 * seqnr % indexperblock; index blocks are listed in the rootblock (small
 * mode) or in 'SB' blocks listed in the rootblock extension (superindex).
 *
 * Reads go straight to the device.  Writes exist only on an armed
 * BlockDev (blockdev.h) and are issued by the engine alone: single
 * anode sectors and single data-bitmap sectors, nothing else.  The
 * rootblock, the reserved bitmap, index, super, directory and deldir
 * blocks are never written by AmiDefrag.
 */
#ifndef PFS_CORE_H
#define PFS_CORE_H

#include <exec/types.h>
#include "blockdev.h"
#include "volume.h"
#include "fsmap.h"

#define PFS_ID_DB 0x4442
#define PFS_ID_AB 0x4142
#define PFS_ID_IB 0x4942
#define PFS_ID_BM 0x424D
#define PFS_ID_MI 0x4D49
#define PFS_ID_EX 0x4558
#define PFS_ID_SB 0x5342
#define PFS_ID_DD 0x4444

#define PFS_MODE_SPLITTED_ANODES 2
#define PFS_MODE_EXTENSION       32
#define PFS_MODE_SUPERINDEX      128

#define PFS_ANODE_ROOTDIR 5
#define PFS_ANODE_USERFIRST 6

/* direntry types */
#define PFS_ST_USERDIR   2
#define PFS_ST_SOFTLINK  3
#define PFS_ST_LINKDIR   4
#define PFS_ST_FILE      (-3)
#define PFS_ST_LINKFILE  (-4)
#define PFS_ST_ROLLOVER  (-16)
/* pseudo type the walker reports for deldir entries */
#define PFS_ST_DELETED   (-100)

struct PfsVol {
    struct BlockDev *bd;
    ULONG part_start;          /* device block of partition sector 0 */
    ULONG nsectors;            /* sectors in the partition */
    ULONG options, firstres, lastres, resblksize, rescluster, rblkcluster;
    ULONG disksize, extension, bitmapstart, longsperbmb, indexperblock, anodesperblock;
    ULONG datestamp, blocksfree, reserved_free, deldir_old;
    UBYTE *root;               /* rootblock cluster (rblkcluster sectors) */
    UBYTE *rext;               /* rootblock extension or NULL */
    UBYTE *ibuf;  ULONG ibuf_nr;    /* anode index / bitmap index block cache */
    UBYTE *abuf;  ULONG abuf_seq, abuf_blk;   /* anode block cache + its sector */
    UBYTE *sbuf;  ULONG sbuf_nr;    /* super block cache */
    UBYTE *dbuf;               /* directory / deldir block */
    UBYTE *tbuf;               /* scratch: one reserved block */
    /* data bitmap, RAM copy: bit i = sector bitmapstart + i, 1 = free,
     * stored exactly as on disk (longs, bit 31 - n) */
    ULONG *bm;
    ULONG  bm_longs;           /* longs covering the data area */
    ULONG  nbmb;               /* bitmap blocks */
    ULONG *bmb_key;            /* sector of each bitmap block */
    UBYTE *bmb_dirty;          /* per bitmap block: bit k = sector k dirty */
    BOOL   bm_loaded;
    ULONG  free_blocks;        /* counted from the bitmap */
    /* progress callback for long loops (may be NULL) */
    FsMapProgress cb;
    void *cb_ud;
    BOOL  aborted;
    char  err[96];
};

BOOL pfs_open(struct PfsVol *v, struct BlockDev *bd, const struct VolumeInfo *vi);
void pfs_close(struct PfsVol *v);

BOOL pfs_read_sectors (struct PfsVol *v, ULONG sector, ULONG n, void *buf);
BOOL pfs_write_sectors(struct PfsVol *v, ULONG sector, ULONG n, const void *buf);
/* one reserved block (resblksize bytes) at sector nr */
BOOL pfs_res_read(struct PfsVol *v, ULONG nr, UBYTE *buf);

/* anodes */
BOOL pfs_get_anode(struct PfsVol *v, ULONG anodenr, ULONG *cs, ULONG *blk, ULONG *next);
/* Rewrite one anode in place: read-modify-write of the single 512-byte
 * sector that holds it (the one commit write of the engine). */
BOOL pfs_set_anode(struct PfsVol *v, ULONG anodenr, ULONG cs, ULONG blk, ULONG next);

/* data bitmap */
BOOL  pfs_bm_load(struct PfsVol *v);                    /* whole bitmap into RAM */
BOOL  pfs_bm_isfree(const struct PfsVol *v, ULONG sector);
void  pfs_bm_set(struct PfsVol *v, ULONG sector, BOOL isfree);   /* marks the sector dirty */
BOOL  pfs_bm_flush(struct PfsVol *v);                   /* write dirty bitmap sectors */
/* smallest free run of at least `need` sectors (best fit); 0 = none */
ULONG pfs_bm_find_run(const struct PfsVol *v, ULONG need);

/* Directory walk: cb(ud, anodenr, type, name) for every entry reached
 * from the root directory and every deldir entry (type PFS_ST_DELETED).
 * name is a NUL-terminated copy valid during the call.  Return FALSE from
 * cb to stop (the walk returns FALSE then). */
typedef BOOL (*PfsWalkCb)(void *ud, ULONG anodenr, LONG type, const char *name);
BOOL  pfs_walk(struct PfsVol *v, PfsWalkCb cb, void *ud, ULONG *dirs);

/* anodenr of a file entry with this name in the root directory, 0 = none */
ULONG pfs_root_lookup(struct PfsVol *v, const char *name);

#endif /* PFS_CORE_H */
