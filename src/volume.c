/*
 * volume.c - partition discovery (see volume.h).
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/filehandler.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "clib.h"
#include "be.h"
#include "blockdev.h"
#include "volume.h"

UWORD Volume_FsKind(ULONG dostype)
{
    ULONG hi = dostype & 0xFFFFFF00UL;
    UBYTE lo = (UBYTE)dostype;
    if (hi == 0x444F5300UL && lo <= 7) return FS_FFS;           /* DOS\0..7 */
    if ((hi == 0x50465300UL || hi == 0x50445300UL) && lo <= 3)  /* PFS\x PDS\x */
        return FS_PFS;
    if (hi == 0x53465300UL && (lo == 0 || lo == 2)) return FS_SFS; /* SFS\0 SFS\2 */
    return FS_UNKNOWN;
}

const char *Volume_FsName(ULONG dostype, char *buf, ULONG buflen)
{
    char id[5];
    UWORD i;
    for (i = 0; i < 4; i++) {
        UBYTE c = (UBYTE)(dostype >> (24 - 8 * i));
        id[i] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
    }
    id[4] = '\0';
    switch (Volume_FsKind(dostype)) {
    case FS_FFS:
        snprintf(buf, buflen, "%s (%s\\%u)",
                 (dostype & 1) ? "FFS" : "OFS", "DOS", (unsigned)(dostype & 7));
        break;
    case FS_PFS: snprintf(buf, buflen, "PFS3 (%.3s\\%u)", id, (unsigned)(dostype & 0xFF)); break;
    case FS_SFS: snprintf(buf, buflen, "SFS (SFS\\%u)", (unsigned)(dostype & 0xFF)); break;
    default:     snprintf(buf, buflen, "unknown (%s)", id); break;
    }
    return buf;
}

void Volume_SizeString(ULONG blocks, ULONG block_size, char *buf, ULONG buflen)
{
    UQUAD bytes = (UQUAD)blocks * block_size;
    ULONG mb = (ULONG)(bytes >> 20);
    if (mb >= 4096)
        snprintf(buf, buflen, "%lu.%lu GB", (unsigned long)(mb >> 10),
                 (unsigned long)(((mb & 1023) * 10) >> 10));
    else if (mb > 0)
        snprintf(buf, buflen, "%lu MB", (unsigned long)mb);
    else
        snprintf(buf, buflen, "%lu KB", (unsigned long)(bytes >> 10));
}

/* Fill the derived fields from a DosEnvec-style geometry. */
static void finish_from_envec(struct VolumeInfo *v, ULONG sizeblock_longs,
                              ULONG surfaces, ULONG spb, ULONG bpt,
                              ULONG reserved, ULONG lowcyl, ULONG highcyl,
                              ULONG dostype)
{
    ULONG cyl_blocks = surfaces * bpt;
    v->sector_size = sizeblock_longs ? sizeblock_longs * 4 : 512;
    v->spb         = spb ? spb : 1;
    v->reserved    = reserved;
    v->heads       = surfaces;
    v->bpt         = bpt;
    v->lowcyl      = lowcyl;
    v->highcyl     = highcyl;
    v->start_block = lowcyl * cyl_blocks;
    v->num_blocks  = (highcyl >= lowcyl) ? (highcyl - lowcyl + 1) * cyl_blocks : 0;
    v->dostype     = dostype;
    v->fskind      = Volume_FsKind(dostype);
}

/* ------------------------------------------------------------------ */
/* Mounted volumes (Amiga)                                             */
/* ------------------------------------------------------------------ */

#ifndef AMIDEFRAG_HOST

static void bstr_copy(char *dst, ULONG dstlen, BPTR bstr)
{
    const UBYTE *s = (const UBYTE *)BADDR(bstr);
    ULONG n;
    dst[0] = '\0';
    if (!s || (ULONG)s < 1024) return;
    n = s[0];
    if (n >= dstlen) n = dstlen - 1;
    memcpy(dst, s + 1, n);
    dst[n] = '\0';
}

void Volume_EnumerateMounted(struct VolumeList *vl)
{
    struct DosList *dol;
    struct MsgPort *tasks[MAX_VOLUMES];

    memset(vl, 0, sizeof(*vl));
    memset(tasks, 0, sizeof(tasks));

    dol = LockDosList(LDF_DEVICES | LDF_VOLUMES | LDF_READ);

    /* Pass 1: devices with a FileSysStartupMsg and a usable DosEnvec. */
    {
        struct DosList *d = dol;
        while ((d = NextDosEntry(d, LDF_DEVICES)) != NULL && vl->count < MAX_VOLUMES) {
            struct FileSysStartupMsg *fssm;
            const ULONG *de;
            struct VolumeInfo *v;
            BPTR startup = d->dol_misc.dol_handler.dol_Startup;
            char devname[64];
            ULONG len;

            /* AUX:, SER:, PIPE: keep a small integer here, not a BPTR. */
            if ((ULONG)startup < 1024) continue;
            fssm = (struct FileSysStartupMsg *)BADDR(startup);
            if ((ULONG)fssm->fssm_Device < 1024 || (ULONG)fssm->fssm_Environ < 1024) continue;

            bstr_copy(devname, sizeof(devname), fssm->fssm_Device);
            len = strlen(devname);
            if (len < 8 || strcmp(devname + len - 7, ".device") != 0) continue;

            de = (const ULONG *)BADDR(fssm->fssm_Environ);
            if (de[DE_TABLESIZE] < DE_DOSTYPE) continue;
            if (de[DE_NUMHEADS] == 0 || de[DE_BLKSPERTRACK] == 0) continue;

            v = &vl->v[vl->count];
            memset(v, 0, sizeof(*v));
            bstr_copy(v->dosname, sizeof(v->dosname), d->dol_Name);   /* dol_Name is a BSTR already */
            strncpy(v->devname, devname, sizeof(v->devname) - 1);
            v->unit    = fssm->fssm_Unit;
            v->mounted = (d->dol_Task != NULL);
            finish_from_envec(v, de[DE_SIZEBLOCK], de[DE_NUMHEADS], de[DE_SECSPERBLK],
                              de[DE_BLKSPERTRACK], de[DE_RESERVEDBLKS],
                              de[DE_LOWCYL], de[DE_UPPERCYL], de[DE_DOSTYPE]);
            tasks[vl->count] = d->dol_Task;
            vl->count++;
        }
    }

    /* Pass 2: volume names, matched to the device by handler port. */
    {
        struct DosList *d = dol;
        while ((d = NextDosEntry(d, LDF_VOLUMES)) != NULL) {
            UWORD i;
            if (!d->dol_Task) continue;
            for (i = 0; i < vl->count; i++) {
                if (tasks[i] == d->dol_Task && vl->v[i].volname[0] == '\0') {
                    bstr_copy(vl->v[i].volname, sizeof(vl->v[i].volname), d->dol_Name);
                    vl->v[i].volnode = d;
                    break;
                }
            }
        }
    }

    UnLockDosList(LDF_DEVICES | LDF_VOLUMES | LDF_READ);

    /* Which of them is the boot volume: the handler behind SYS:. */
    {
        BPTR l = Lock((CONST_STRPTR)"SYS:", ACCESS_READ);
        if (l) {
            struct MsgPort *boot = ((struct FileLock *)BADDR(l))->fl_Task;
            UWORD i;
            for (i = 0; i < vl->count; i++)
                if (tasks[i] && tasks[i] == boot) vl->v[i].is_boot = TRUE;
            UnLock(l);
        }
    }
}

#else  /* host */

void Volume_EnumerateMounted(struct VolumeList *vl)
{
    memset(vl, 0, sizeof(*vl));
}

#endif

/* ------------------------------------------------------------------ */
/* Partitions of a disk or image: RDB walk                             */
/* ------------------------------------------------------------------ */

#define ID_RDSK 0x5244534BUL
#define ID_PART 0x50415254UL
#define RDB_LOCATION_LIMIT 16

/* RigidDiskBlock / PartitionBlock byte offsets (devices/hardblocks.h) */
#define RDB_PARTITIONLIST 28
#define PB_NEXT           16
#define PB_DRIVENAME      36    /* BSTR, 32 bytes */
#define PB_ENVIRONMENT    128   /* DosEnvec longs */

static BOOL block_checksum_ok(const UBYTE *b, ULONG summed_longs)
{
    ULONG sum = 0, i;
    if (summed_longs < 2 || summed_longs > 128) return FALSE;
    for (i = 0; i < summed_longs; i++) sum += bel(b, i);
    return sum == 0;
}

BOOL Volume_EnumerateImage(struct BlockDev *bd, struct VolumeList *vl,
                           char *err, ULONG errlen)
{
    UBYTE *buf;
    ULONG  i, rdb_blk = 0xFFFFFFFFUL, part, guard = 0;

    memset(vl, 0, sizeof(*vl));
    if (err && errlen) err[0] = '\0';

    buf = (UBYTE *)AllocVec(bd->block_size, MEMF_PUBLIC | MEMF_CLEAR);
    if (!buf) { if (err) strncpy(err, "out of memory", errlen - 1); return FALSE; }

    for (i = 0; i < RDB_LOCATION_LIMIT; i++) {
        if (!BlockDev_ReadBlocks(bd, i, 1, buf)) break;
        if (bel(buf, 0) == ID_RDSK && block_checksum_ok(buf, bel(buf, 1))) { rdb_blk = i; break; }
    }

    if (rdb_blk == 0xFFFFFFFFUL) {
        /* No RDB: treat the whole image as one partition, 512-byte blocks,
         * FFS-style two reserved blocks, type from the boot block. */
        struct VolumeInfo *v = &vl->v[0];
        if (!BlockDev_ReadBlocks(bd, 0, 1, buf)) {
            if (err) strncpy(err, "cannot read block 0", errlen - 1);
            FreeVec(buf); return FALSE;
        }
        memset(v, 0, sizeof(*v));
        strncpy(v->dosname, "IMAGE", sizeof(v->dosname) - 1);
        strncpy(v->devname, bd->devname, sizeof(v->devname) - 1);
        v->sector_size = bd->block_size;
        v->spb         = 1;
        v->reserved    = 2;
        v->start_block = 0;
        v->num_blocks  = (ULONG)(bd->total_bytes / bd->block_size);
        v->dostype     = bel(buf, 0);
        v->fskind      = Volume_FsKind(v->dostype);
        v->heads = 1; v->bpt = 1; v->lowcyl = 0; v->highcyl = v->num_blocks ? v->num_blocks - 1 : 0;
        vl->count = 1;
        FreeVec(buf);
        return TRUE;
    }

    part = bel(buf, RDB_PARTITIONLIST / 4);
    while (part != 0xFFFFFFFFUL && part != 0 && vl->count < MAX_VOLUMES && guard++ < 64) {
        struct VolumeInfo *v;
        ULONG n;
        if (!BlockDev_ReadBlocks(bd, part, 1, buf)) break;
        if (bel(buf, 0) != ID_PART || !block_checksum_ok(buf, bel(buf, 1))) break;

        v = &vl->v[vl->count];
        memset(v, 0, sizeof(*v));
        n = buf[PB_DRIVENAME];
        if (n > 31) n = 31;
        memcpy(v->dosname, buf + PB_DRIVENAME + 1, n);
        v->dosname[n] = '\0';
        strncpy(v->devname, bd->devname, sizeof(v->devname) - 1);
        v->unit = bd->unit;
        {
            ULONG tsz = be32(buf, PB_ENVIRONMENT);
#define DEV(ix) ((tsz >= (ix)) ? be32(buf, PB_ENVIRONMENT + 4 * (ix)) : 0)
            finish_from_envec(v, DEV(DE_SIZEBLOCK), DEV(DE_NUMHEADS), DEV(DE_SECSPERBLK),
                              DEV(DE_BLKSPERTRACK), DEV(DE_RESERVEDBLKS),
                              DEV(DE_LOWCYL), DEV(DE_UPPERCYL), DEV(DE_DOSTYPE));
#undef DEV
        }
        if (v->heads && v->bpt) vl->count++;
        part = bel(buf, PB_NEXT / 4);
    }

    FreeVec(buf);
    if (vl->count == 0 && err) strncpy(err, "RDB has no usable partitions", errlen - 1);
    return vl->count > 0;
}
