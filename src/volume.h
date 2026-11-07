/*
 * volume.h - which partitions exist, and where they are on the disk.
 *
 * A VolumeInfo is everything the map builders need: the device to open,
 * the partition's first device block and length, the filesystem block
 * size, and the DOS type that picks the parser.  Sources:
 *
 *   Volume_EnumerateMounted  the DOS device list of the running system
 *                            (Amiga only; no disk I/O - a memory walk)
 *   Volume_EnumerateImage    the RDB of an opened disk/image, or the whole
 *                            image as one partition when it has no RDB
 */
#ifndef VOLUME_H
#define VOLUME_H

#include <exec/types.h>
#include "blockdev.h"

#define FS_UNKNOWN 0
#define FS_FFS     1
#define FS_PFS     2
#define FS_SFS     3

struct VolumeInfo {
    char  dosname[32];      /* "DH0" */
    char  volname[32];      /* mounted volume name, "" if none */
    char  devname[64];      /* "scsi.device" or "FILE:<path>" */
    ULONG unit;
    ULONG dostype;
    UWORD fskind;           /* FS_* */
    ULONG sector_size;      /* device block size in bytes (512) */
    ULONG spb;              /* sectors per filesystem block */
    ULONG reserved;         /* DE_RESERVED (FFS boot blocks) */
    ULONG start_block;      /* first device block of the partition */
    ULONG num_blocks;       /* device blocks in the partition */
    ULONG lowcyl, highcyl, heads, bpt;
    BOOL  mounted;
    BOOL  is_boot;          /* handler also serves SYS: */
    APTR  volnode;          /* Amiga: struct DosList of the mounted volume, NULL elsewhere */
    APTR  handler;          /* Amiga: the handler's MsgPort (device node dol_Task), NULL elsewhere */
};

#define MAX_VOLUMES 32

struct VolumeList {
    UWORD count;
    struct VolumeInfo v[MAX_VOLUMES];
};

UWORD       Volume_FsKind(ULONG dostype);
const char *Volume_FsName(ULONG dostype, char *buf, ULONG buflen);
void        Volume_SizeString(ULONG blocks, ULONG block_size, char *buf, ULONG buflen);

void Volume_EnumerateMounted(struct VolumeList *vl);
BOOL Volume_EnumerateImage(struct BlockDev *bd, struct VolumeList *vl,
                           char *err, ULONG errlen);

#endif /* VOLUME_H */
