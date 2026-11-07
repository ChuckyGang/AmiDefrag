/*
 * fsmap.c - block map storage, cell condensation and the dispatcher.
 */
#include "fsmap_int.h"
#include "locale_support.h"

void FsMap_Set(struct FsMap *m, ULONG blk, UBYTE cls)
{
    UBYTE *p;
    if (blk >= m->nblocks) return;
    p = &m->nib[blk >> 1];
    if (blk & 1) *p = (UBYTE)((*p & 0x0F) | (cls << 4));
    else         *p = (UBYTE)((*p & 0xF0) | (cls & 0x0F));
}

void FsMap_SetRange(struct FsMap *m, ULONG first, ULONG n, UBYTE cls)
{
    ULONG i;
    if (first >= m->nblocks) return;
    if (n > m->nblocks - first) n = m->nblocks - first;
    for (i = 0; i < n; i++) FsMap_Set(m, first + i, cls);
}

UBYTE FsMap_Cell(const struct FsMap *m, ULONG first, ULONG n)
{
    ULONG cnt[BLK_CLASSES];
    ULONG i;
    UBYTE best = BLK_FREE;
    memset(cnt, 0, sizeof(cnt));
    if (first >= m->nblocks) return BLK_FREE;
    if (n > m->nblocks - first) n = m->nblocks - first;
    for (i = 0; i < n; i++) {
        UBYTE c = FsMap_Get(m, first + i);
        if (c == BLK_FRAG) return BLK_FRAG;
        if (c < BLK_CLASSES) cnt[c]++;
    }
    for (i = 1; i < BLK_CLASSES; i++)
        if (cnt[i] > 0 && (best == BLK_FREE || cnt[i] > cnt[best])) best = (UBYTE)i;
    return best;
}

void FsMap_Recount(struct FsMap *m)
{
    ULONG i;
    memset(m->count, 0, sizeof(m->count));
    for (i = 0; i < m->nblocks; i++) {
        UBYTE c = FsMap_Get(m, i);
        if (c < BLK_CLASSES) m->count[c]++;
    }
}

char FsMap_ClassChar(UBYTE cls)
{
    static const char ch[BLK_CLASSES] = { '.', '#', 'F', 'M', 'r', '?' };
    return cls < BLK_CLASSES ? ch[cls] : '!';
}

const char *FsMap_ClassName(UBYTE cls)
{
    static const WORD id[BLK_CLASSES] =
        { MSG_CLASS_FREE, MSG_CLASS_DATA, MSG_CLASS_FRAG, MSG_CLASS_SYSTEM, MSG_CLASS_RESERVED, MSG_CLASS_UNKNOWN };
    return cls < BLK_CLASSES ? GS(id[cls]) : "?";
}

BOOL ustack_push(struct UStack *s, ULONG x)
{
    if (s->n == s->cap) {
        ULONG  ncap = s->cap ? s->cap * 2 : 64;
        ULONG *nv   = (ULONG *)AllocVec(ncap * sizeof(ULONG), MEMF_PUBLIC);
        if (!nv) return FALSE;
        if (s->v) { memcpy(nv, s->v, s->n * sizeof(ULONG)); FreeVec(s->v); }
        s->v = nv; s->cap = ncap;
    }
    s->v[s->n++] = x;
    return TRUE;
}

void ustack_free(struct UStack *s)
{
    if (s->v) FreeVec(s->v);
    s->v = NULL; s->n = s->cap = 0;
}

/* shell sort: no libc qsort on the Amiga side, n is small */
void keyset_sort(ULONG *k, ULONG n)
{
    ULONG gap, i, j, t;
    for (gap = n / 2; gap > 0; gap /= 2)
        for (i = gap; i < n; i++) {
            t = k[i];
            for (j = i; j >= gap && k[j - gap] > t; j -= gap) k[j] = k[j - gap];
            k[j] = t;
        }
}

BOOL keyset_has(const ULONG *k, ULONG n, ULONG key)
{
    ULONG lo = 0, hi = n;
    while (lo < hi) {
        ULONG mid = lo + (hi - lo) / 2;
        if (k[mid] == key) return TRUE;
        if (k[mid] < key) lo = mid + 1; else hi = mid;
    }
    return FALSE;
}

void FsMap_Free(struct FsMap *m)
{
    if (m->nib) FreeVec(m->nib);
    m->nib = NULL;
    m->nblocks = 0;
}

BOOL FsMap_Build(struct BlockDev *bd, const struct VolumeInfo *v,
                 struct FsMap *m, FsMapProgress cb, void *ud)
{
    struct FsCtx c;
    BOOL ok;

    memset(m, 0, sizeof(*m));
    memset(&c, 0, sizeof(c));
    c.bd = bd; c.v = v; c.m = m; c.cb = cb; c.ud = ud;
    c.part_start = v->start_block;
    c.spb        = v->spb ? v->spb : 1;
    c.blocksize  = v->sector_size * c.spb;

    m->fskind    = v->fskind;
    m->blocksize = c.blocksize;
    m->nblocks   = v->num_blocks / c.spb;
    Volume_FsName(v->dostype, m->fsname, sizeof(m->fsname));
    strncpy(m->volname, v->volname[0] ? v->volname : v->dosname, sizeof(m->volname) - 1);

    if (v->fskind == FS_PFS) {
        /* PFS3 bitmaps are per 512-byte sector regardless of DE_SECSPERBLK. */
        c.spb = 1;
        c.blocksize = v->sector_size;
        m->blocksize = c.blocksize;
        m->nblocks = v->num_blocks;
    }

    if (m->nblocks == 0 || v->fskind == FS_UNKNOWN) {
        strncpy(m->note, v->fskind == FS_UNKNOWN ? GS(MSG_NOTE_UNSUPPORTED_FS)
                                                 : GS(MSG_NOTE_EMPTY_PARTITION), sizeof(m->note) - 1);
        return FALSE;
    }

    m->nib = (UBYTE *)AllocVec((m->nblocks + 1) / 2, MEMF_PUBLIC | MEMF_CLEAR);
    if (!m->nib) {
        snprintf(m->note, sizeof(m->note), GS(MSG_NOTE_NO_MEMORY_MAP),
                 (unsigned long)(((m->nblocks + 1) / 2 + 1023) / 1024));
        return FALSE;
    }

    switch (v->fskind) {
    case FS_FFS: ok = FfsMap_Build(&c); break;
    case FS_PFS: ok = PfsMap_Build(&c); break;
    case FS_SFS: ok = SfsMap_Build(&c); break;
    default:     ok = FALSE; break;
    }
    if (c.aborted) { strncpy(m->note, GS(MSG_NOTE_CANCELLED), sizeof(m->note) - 1); ok = FALSE; }
    if (ok) FsMap_Recount(m);
    else { FsMap_Free(m); if (!m->note[0]) strncpy(m->note, GS(MSG_NOTE_SCAN_FAILED), sizeof(m->note) - 1); }
    return ok;
}
