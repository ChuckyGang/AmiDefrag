/*
 * blockdev.c - read-only block access (see blockdev.h).
 *
 * The device path follows what AmiPart learned the hard way on real
 * controllers: ask the driver with NSCMD_DEVICEQUERY which 64-bit read it
 * implements, use that (byte offset split across io_Offset / io_Actual),
 * and fall back to CMD_READ for drivers that know nothing newer.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/io.h>
#include <devices/trackdisk.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "clib.h"
#include "blockdev.h"

#ifndef TD_READ64
#define TD_READ64 (CMD_NONSTD + 15)        /* 24 */
#endif
#ifndef NSCMD_DEVICEQUERY
#define NSCMD_DEVICEQUERY 0x4000
#endif
#ifndef NSCMD_TD_READ64
#define NSCMD_TD_READ64   0xC000
#endif

struct NSDQuery {
    ULONG  DevQueryFormat;
    ULONG  SizeAvailable;
    UWORD  DeviceType;
    UWORD  DeviceSubType;
    UWORD *SupportedCommands;
    ULONG  reserved[4];
};

#ifdef AMIDEFRAG_HOST
LONG  HostFilePRead(BPTR fh, void *buf, ULONG len, UQUAD off);
UQUAD HostFileSize(BPTR fh);
#endif

/* ------------------------------------------------------------------ */
/* Image file backend                                                  */
/* ------------------------------------------------------------------ */

struct BlockDev *BlockDev_OpenFile(const char *path)
{
    struct BlockDev *bd = (struct BlockDev *)AllocVec(sizeof(*bd), MEMF_PUBLIC | MEMF_CLEAR);
    if (!bd) return NULL;
    bd->backend    = BD_FILE;
    bd->block_size = 512;
    strncpy(bd->filepath, path, sizeof(bd->filepath) - 1);
    AD_SNPRINTF(bd->devname, "FILE:%.58s", path);

    bd->fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
    if (!bd->fh) { FreeVec(bd); return NULL; }
    bd->open = TRUE;
#ifdef AMIDEFRAG_HOST
    bd->total_bytes = HostFileSize(bd->fh);
#else
    {
        LONG end;
        Seek(bd->fh, 0, OFFSET_END);
        end = Seek(bd->fh, 0, OFFSET_BEGINNING);
        bd->total_bytes = (end > 0) ? (UQUAD)(ULONG)end : 0;
    }
#endif
    return bd;
}

/* ------------------------------------------------------------------ */
/* Exec device backend                                                 */
/* ------------------------------------------------------------------ */

static void probe_nsd(struct BlockDev *bd)
{
    struct NSDQuery q;
    memset(&q, 0, sizeof(q));
    bd->iotd.iotd_Req.io_Command = NSCMD_DEVICEQUERY;
    bd->iotd.iotd_Req.io_Length  = sizeof(q);
    bd->iotd.iotd_Req.io_Data    = (APTR)&q;
    bd->iotd.iotd_Req.io_Flags   = 0;
    if (DoIO((struct IORequest *)&bd->iotd) != 0) return;
    if (bd->iotd.iotd_Req.io_Actual < 16 || !q.SupportedCommands) return;
    {
        UWORD *cmd;
        for (cmd = q.SupportedCommands; *cmd; cmd++)
            if (*cmd == NSCMD_TD_READ64) { bd->nsd_read64 = 1; break; }
    }
}

struct BlockDev *BlockDev_Open(const char *devname, ULONG unit)
{
    struct BlockDev *bd;
    struct DriveGeometry geom;

    if (devname &&
        (devname[0] == 'F' || devname[0] == 'f') &&
        (devname[1] == 'I' || devname[1] == 'i') &&
        (devname[2] == 'L' || devname[2] == 'l') &&
        (devname[3] == 'E' || devname[3] == 'e') && devname[4] == ':')
        return BlockDev_OpenFile(devname + 5);

    bd = (struct BlockDev *)AllocVec(sizeof(*bd), MEMF_PUBLIC | MEMF_CLEAR);
    if (!bd) return NULL;
    bd->backend    = BD_DEVICE;
    bd->block_size = 512;
    bd->unit       = unit;
    strncpy(bd->devname, devname, sizeof(bd->devname) - 1);

    bd->port = CreateMsgPort();
    if (!bd->port) { FreeVec(bd); return NULL; }
    bd->iotd.iotd_Req.io_Message.mn_Node.ln_Type = NT_REPLYMSG;
    bd->iotd.iotd_Req.io_Message.mn_Length       = sizeof(bd->iotd);
    bd->iotd.iotd_Req.io_Message.mn_ReplyPort    = bd->port;

    if (OpenDevice((CONST_STRPTR)devname, unit, (struct IORequest *)&bd->iotd, 0) != 0) {
        DeleteMsgPort(bd->port);
        FreeVec(bd);
        return NULL;
    }
    bd->open = TRUE;

    memset(&geom, 0, sizeof(geom));
    bd->iotd.iotd_Req.io_Command = TD_GETGEOMETRY;
    bd->iotd.iotd_Req.io_Length  = sizeof(geom);
    bd->iotd.iotd_Req.io_Data    = (APTR)&geom;
    bd->iotd.iotd_Req.io_Flags   = 0;
    if (DoIO((struct IORequest *)&bd->iotd) == 0 && geom.dg_SectorSize >= 512) {
        bd->block_size  = geom.dg_SectorSize;
        bd->total_bytes = (UQUAD)geom.dg_TotalSectors * geom.dg_SectorSize;
    }
    probe_nsd(bd);
    return bd;
}

void BlockDev_Close(struct BlockDev *bd)
{
    if (!bd) return;
    if (bd->backend == BD_FILE) {
        if (bd->fh) Close(bd->fh);
    } else {
        if (bd->open) CloseDevice((struct IORequest *)&bd->iotd);
        if (bd->port) DeleteMsgPort(bd->port);
    }
    FreeVec(bd);
}

/* 64-bit read: TD_READ64 and NSCMD_TD_READ64 both take the LOW 32 bits of
 * the byte offset in io_Offset and the HIGH 32 bits in io_Actual. */
static BOOL rd_64(struct BlockDev *bd, ULONG start, ULONG count, void *buf)
{
    UQUAD off = (UQUAD)start * bd->block_size;
    bd->iotd.iotd_Req.io_Command = bd->nsd_read64 ? NSCMD_TD_READ64 : TD_READ64;
    bd->iotd.iotd_Req.io_Length  = count * bd->block_size;
    bd->iotd.iotd_Req.io_Data    = buf;
    bd->iotd.iotd_Req.io_Offset  = (ULONG)off;
    bd->iotd.iotd_Req.io_Actual  = (ULONG)(off >> 32);
    bd->iotd.iotd_Req.io_Flags   = 0;
    return DoIO((struct IORequest *)&bd->iotd) == 0;
}

static BOOL rd_cmd(struct BlockDev *bd, ULONG start, ULONG count, void *buf)
{
    UQUAD off = (UQUAD)start * bd->block_size;
    if ((off >> 32) != 0) return FALSE;          /* beyond 4 GB: CMD_READ can't */
    bd->iotd.iotd_Req.io_Command = CMD_READ;
    bd->iotd.iotd_Req.io_Length  = count * bd->block_size;
    bd->iotd.iotd_Req.io_Data    = buf;
    bd->iotd.iotd_Req.io_Offset  = (ULONG)off;
    bd->iotd.iotd_Req.io_Actual  = 0;
    bd->iotd.iotd_Req.io_Flags   = 0;
    return DoIO((struct IORequest *)&bd->iotd) == 0;
}

BOOL BlockDev_ReadBlocks(struct BlockDev *bd, ULONG start, ULONG count, void *buf)
{
    if (!bd || !bd->open || count == 0) return FALSE;
    bd->reads += count;

    if (bd->backend == BD_FILE) {
#ifdef AMIDEFRAG_HOST
        ULONG len = count * bd->block_size;
        return HostFilePRead(bd->fh, buf, len, (UQUAD)start * bd->block_size) == (LONG)len;
#else
        /* dos.library Seek is signed 32-bit: images above 2 GB are out of
         * reach on the Amiga side (the host build has no such limit). */
        UQUAD off = (UQUAD)start * bd->block_size;
        LONG  len = (LONG)(count * bd->block_size);
        if (off > 0x7FFFFFFFULL) return FALSE;
        if (Seek(bd->fh, (LONG)off, OFFSET_BEGINNING) < 0) return FALSE;
        return Read(bd->fh, buf, len) == len;
#endif
    }

    if (bd->rd_pref == 2) return rd_cmd(bd, start, count, buf);
    if (rd_64(bd, start, count, buf)) { bd->rd_pref = 1; return TRUE; }
    if (bd->rd_pref == 1) return FALSE;          /* 64-bit path worked before: real error */
    if (rd_cmd(bd, start, count, buf)) { bd->rd_pref = 2; return TRUE; }
    return FALSE;
}
