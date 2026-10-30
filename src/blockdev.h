/*
 * blockdev.h - read-only block access for AmiDefrag.
 *
 * Two backends behind one handle:
 *   BD_DEVICE  an exec device/unit (scsi.device 0, ...) opened with
 *              trackdisk-style I/O: NSCMD_TD_READ64 / TD_READ64 when the
 *              driver speaks them, CMD_READ otherwise.
 *   BD_FILE    a hard-disk image file ("FILE:<path>" as the device name).
 *
 * Block numbers are device blocks (bd->block_size bytes, normally 512),
 * counted from the start of the DISK, not the partition.
 *
 * There is deliberately NO write entry point in this version: the map
 * and analysis code must stay provably read-only until the per-filesystem
 * move engines exist and are reviewed.
 */
#ifndef BLOCKDEV_H
#define BLOCKDEV_H

#include <exec/types.h>
#include <exec/ports.h>
#include <devices/trackdisk.h>

/* Bartman's exec/types.h has no 64-bit type; the host shim defines its own. */
#ifndef AD_UQUAD_DEFINED
#define AD_UQUAD_DEFINED
typedef unsigned long long UQUAD;
#endif

#define BD_DEVICE 0
#define BD_FILE   1

struct BlockDev {
    UBYTE            backend;
    BOOL             open;
    ULONG            block_size;      /* bytes per device block */
    UQUAD            total_bytes;     /* 0 when unknown */
    char             devname[64];
    ULONG            unit;
    struct MsgPort  *port;            /* BD_DEVICE */
    struct IOExtTD   iotd;            /* BD_DEVICE */
    UBYTE            nsd_read64;      /* driver lists NSCMD_TD_READ64 */
    UBYTE            rd_pref;         /* 0 unknown, 1 TD64/NSD64, 2 CMD_READ */
    BPTR             fh;              /* BD_FILE */
    char             filepath[108];
    ULONG            reads;           /* statistics: block reads issued */
};

struct BlockDev *BlockDev_Open(const char *devname, ULONG unit);
struct BlockDev *BlockDev_OpenFile(const char *path);
void             BlockDev_Close(struct BlockDev *bd);

/* Read count consecutive device blocks starting at start into buf
 * (count * block_size bytes).  FALSE on any error. */
BOOL BlockDev_ReadBlocks(struct BlockDev *bd, ULONG start, ULONG count, void *buf);

#endif /* BLOCKDEV_H */
