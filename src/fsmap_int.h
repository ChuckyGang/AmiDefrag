/*
 * fsmap_int.h - shared plumbing for the per-filesystem map builders.
 */
#ifndef FSMAP_INT_H
#define FSMAP_INT_H

#include <exec/types.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include "clib.h"
#include "be.h"
#include "blockdev.h"
#include "volume.h"
#include "fsmap.h"

struct FsCtx {
    struct BlockDev         *bd;
    const struct VolumeInfo *v;
    struct FsMap            *m;
    FsMapProgress            cb;
    void                    *ud;
    ULONG                    part_start;   /* device block of FS block 0 */
    ULONG                    spb;          /* device blocks per FS block */
    ULONG                    blocksize;    /* bytes per FS block */
    BOOL                     aborted;
};

/* Read FS block fsblk (blocksize bytes) into buf. */
static inline BOOL fs_read(struct FsCtx *c, ULONG fsblk, void *buf)
{
    if (fsblk >= c->m->nblocks) return FALSE;
    return BlockDev_ReadBlocks(c->bd, c->part_start + fsblk * c->spb, c->spb, buf);
}

/* Read `sectors` device blocks starting at partition-relative sector. */
static inline BOOL fs_read_sectors(struct FsCtx *c, ULONG sector, ULONG sectors, void *buf)
{
    return BlockDev_ReadBlocks(c->bd, c->part_start + sector, sectors, buf);
}

static inline BOOL fs_progress(struct FsCtx *c, ULONG done, ULONG total, const char *what)
{
    if (c->aborted) return FALSE;
    if (c->cb && !c->cb(c->ud, done, total, what)) { c->aborted = TRUE; return FALSE; }
    return TRUE;
}

static inline void fs_note(struct FsCtx *c, const char *s)
{
    if (c->m->note[0] == '\0') strncpy(c->m->note, s, sizeof(c->m->note) - 1);
}

/* Growable ULONG stack for directory walks. */
struct UStack { ULONG *v; ULONG n, cap; };
BOOL ustack_push(struct UStack *s, ULONG x);
void ustack_free(struct UStack *s);

/* Sorted ULONG set: sort once, then binary search. */
void keyset_sort(ULONG *k, ULONG n);
BOOL keyset_has(const ULONG *k, ULONG n, ULONG key);

BOOL FfsMap_Build(struct FsCtx *c);
BOOL PfsMap_Build(struct FsCtx *c);
BOOL SfsMap_Build(struct FsCtx *c);

#endif /* FSMAP_INT_H */
