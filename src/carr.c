/*
 * carr.c - chunked sorted array (see carr.h)
 *
 * Invariants: every chunk holds 1..percap records; keys ascend within a
 * chunk and from one chunk to the next; first[ci] is the key of chunk
 * ci's record 0; tag[ci] is tagfn() of the chunk's records (0 without a
 * callback).  The end position is ci == nch.
 */
#include "fsmap_int.h"
#include "carr.h"

#define KEY(a, ci, i) (*(const ULONG *)((a)->ch[ci] + (ULONG)(i) * (a)->esz))
#define REC(a, ci, i) ((a)->ch[ci] + (ULONG)(i) * (a)->esz)

void carr_init(struct Carr *a, UWORD esz, UWORD percap, CarrTag tagfn)
{
    memset(a, 0, sizeof(*a));
    a->esz = esz;
    a->percap = percap < 4 ? 4 : percap;
    a->tagfn = tagfn;
}

void carr_free(struct Carr *a)
{
    ULONG i;
    if (a->ch) { for (i = 0; i < a->nch; i++) if (a->ch[i]) FreeVec(a->ch[i]); FreeVec(a->ch); }
    if (a->cnt) FreeVec(a->cnt);
    if (a->first) FreeVec(a->first);
    if (a->tag) FreeVec(a->tag);
    a->ch = NULL; a->cnt = NULL; a->first = NULL; a->tag = NULL;
    a->n = 0; a->nch = 0; a->chcap = 0;
    a->ok = FALSE;
}

static void refresh(struct Carr *a, ULONG ci)
{
    a->first[ci] = KEY(a, ci, 0);
    a->tag[ci] = a->tagfn ? a->tagfn(a->ch[ci], a->cnt[ci]) : 0;
}

/* room for at least `want` chunks in the directory */
static BOOL dir_reserve(struct Carr *a, ULONG want)
{
    ULONG cap;
    UBYTE **ch; UWORD *cnt; ULONG *first, *tag;
    if (want <= a->chcap) return TRUE;
    cap = a->chcap ? a->chcap : 16;
    while (cap < want) cap *= 2;
    ch = (UBYTE **)AllocVec(cap * sizeof(UBYTE *), MEMF_PUBLIC);
    cnt = (UWORD *)AllocVec(cap * sizeof(UWORD), MEMF_PUBLIC);
    first = (ULONG *)AllocVec(cap * sizeof(ULONG), MEMF_PUBLIC);
    tag = (ULONG *)AllocVec(cap * sizeof(ULONG), MEMF_PUBLIC);
    if (!ch || !cnt || !first || !tag) {
        if (ch) FreeVec(ch);
        if (cnt) FreeVec(cnt);
        if (first) FreeVec(first);
        if (tag) FreeVec(tag);
        return FALSE;
    }
    if (a->nch) {
        memcpy(ch, a->ch, a->nch * sizeof(UBYTE *));
        memcpy(cnt, a->cnt, a->nch * sizeof(UWORD));
        memcpy(first, a->first, a->nch * sizeof(ULONG));
        memcpy(tag, a->tag, a->nch * sizeof(ULONG));
    }
    if (a->ch) FreeVec(a->ch);
    if (a->cnt) FreeVec(a->cnt);
    if (a->first) FreeVec(a->first);
    if (a->tag) FreeVec(a->tag);
    a->ch = ch; a->cnt = cnt; a->first = first; a->tag = tag;
    a->chcap = cap;
    return TRUE;
}

/* open directory slot ci (the caller fills it) */
static BOOL dir_insert(struct Carr *a, ULONG ci)
{
    ULONG tail;
    if (!dir_reserve(a, a->nch + 1)) return FALSE;
    tail = a->nch - ci;
    if (tail) {
        memmove(&a->ch[ci + 1], &a->ch[ci], tail * sizeof(UBYTE *));
        memmove(&a->cnt[ci + 1], &a->cnt[ci], tail * sizeof(UWORD));
        memmove(&a->first[ci + 1], &a->first[ci], tail * sizeof(ULONG));
        memmove(&a->tag[ci + 1], &a->tag[ci], tail * sizeof(ULONG));
    }
    a->nch++;
    return TRUE;
}

static void dir_remove(struct Carr *a, ULONG ci)
{
    ULONG tail = a->nch - ci - 1;
    if (tail) {
        memmove(&a->ch[ci], &a->ch[ci + 1], tail * sizeof(UBYTE *));
        memmove(&a->cnt[ci], &a->cnt[ci + 1], tail * sizeof(UWORD));
        memmove(&a->first[ci], &a->first[ci + 1], tail * sizeof(ULONG));
        memmove(&a->tag[ci], &a->tag[ci + 1], tail * sizeof(ULONG));
    }
    a->nch--;
}

BOOL carr_build(struct Carr *a, const void *sorted, ULONG n)
{
    const UBYTE *src = (const UBYTE *)sorted;
    ULONG fill = a->percap - a->percap / 4, nch, ci, done = 0;
    carr_free(a);
    nch = (n + fill - 1) / fill;
    if (!dir_reserve(a, nch + 8)) return FALSE;
    for (ci = 0; ci < nch; ci++) {
        ULONG take = n - done < fill ? n - done : fill;
        a->ch[ci] = (UBYTE *)AllocVec((ULONG)a->percap * a->esz, MEMF_PUBLIC);
        if (!a->ch[ci]) { carr_free(a); return FALSE; }
        memcpy(a->ch[ci], src + done * a->esz, take * a->esz);
        a->cnt[ci] = (UWORD)take;
        a->nch = ci + 1;
        refresh(a, ci);
        done += take;
    }
    a->n = n;
    a->ok = TRUE;
    return TRUE;
}

BOOL carr_find(const struct Carr *a, ULONG key, struct CarrPos *p)
{
    ULONG lo = 0, hi = a->nch, ci;
    p->ci = 0; p->pos = 0;
    if (!a->ok || a->nch == 0 || key < a->first[0]) return FALSE;
    while (lo < hi) {                                 /* last chunk with first <= key */
        ULONG mid = (lo + hi) / 2;
        if (a->first[mid] <= key) lo = mid + 1; else hi = mid;
    }
    ci = lo - 1;
    lo = 0; hi = a->cnt[ci];
    while (lo < hi) {                                 /* last record with key <= key */
        ULONG mid = (lo + hi) / 2;
        if (KEY(a, ci, mid) <= key) lo = mid + 1; else hi = mid;
    }
    p->ci = ci; p->pos = (UWORD)(lo - 1);
    return TRUE;
}

void *carr_at(const struct Carr *a, const struct CarrPos *p)
{
    if (!a->ok || p->ci >= a->nch || p->pos >= a->cnt[p->ci]) return NULL;
    return REC(a, p->ci, p->pos);
}

BOOL carr_next(const struct Carr *a, struct CarrPos *p)
{
    if (p->ci >= a->nch) return FALSE;
    if ((ULONG)p->pos + 1 < a->cnt[p->ci]) { p->pos++; return TRUE; }
    p->ci++; p->pos = 0;
    return p->ci < a->nch;
}

BOOL carr_insert(struct Carr *a, struct CarrPos *p, const void *rec)
{
    ULONG ci, pos, tail;
    if (!a->ok) return FALSE;
    if (a->nch == 0) {                               /* first record */
        if (!dir_reserve(a, 8)) { a->ok = FALSE; return FALSE; }
        a->ch[0] = (UBYTE *)AllocVec((ULONG)a->percap * a->esz, MEMF_PUBLIC);
        if (!a->ch[0]) { a->ok = FALSE; return FALSE; }
        a->nch = 1; a->cnt[0] = 0;
        ci = 0; pos = 0;
    } else if (p->ci >= a->nch) {                    /* end: append to the last chunk */
        ci = a->nch - 1; pos = a->cnt[ci];
    } else {
        ci = p->ci; pos = p->pos;
    }
    if (a->cnt[ci] == a->percap) {                   /* full: split, upper half to a new chunk */
        ULONG half = a->percap / 2;
        UBYTE *nc = (UBYTE *)AllocVec((ULONG)a->percap * a->esz, MEMF_PUBLIC);
        if (!nc) { a->ok = FALSE; return FALSE; }
        if (!dir_insert(a, ci + 1)) { FreeVec(nc); a->ok = FALSE; return FALSE; }
        memcpy(nc, REC(a, ci, half), (a->percap - half) * a->esz);
        a->ch[ci + 1] = nc;
        a->cnt[ci + 1] = (UWORD)(a->percap - half);
        a->cnt[ci] = (UWORD)half;
        refresh(a, ci);
        refresh(a, ci + 1);
        if (pos > half) { ci++; pos -= half; }
    }
    tail = a->cnt[ci] - pos;
    if (tail) memmove(REC(a, ci, pos + 1), REC(a, ci, pos), tail * a->esz);
    memcpy(REC(a, ci, pos), rec, a->esz);
    a->cnt[ci]++;
    a->n++;
    refresh(a, ci);
    p->ci = ci; p->pos = (UWORD)pos;
    return TRUE;
}

void carr_remove(struct Carr *a, struct CarrPos *p)
{
    ULONG ci = p->ci, pos = p->pos, tail;
    if (!a->ok || ci >= a->nch || pos >= a->cnt[ci]) return;
    tail = a->cnt[ci] - pos - 1;
    if (tail) memmove(REC(a, ci, pos), REC(a, ci, pos + 1), tail * a->esz);
    a->cnt[ci]--;
    a->n--;
    if (a->cnt[ci] == 0) {                           /* chunk gone: p lands on the next chunk */
        FreeVec(a->ch[ci]);
        dir_remove(a, ci);
        p->pos = 0;
        return;
    }
    refresh(a, ci);
    /* sparse chunk: pull the next one in when both fit in half a chunk,
     * so a churned table does not degrade into many tiny chunks */
    if (a->cnt[ci] < a->percap / 4 && ci + 1 < a->nch && (ULONG)a->cnt[ci] + a->cnt[ci + 1] <= a->percap / 2) {
        memcpy(REC(a, ci, a->cnt[ci]), a->ch[ci + 1], (ULONG)a->cnt[ci + 1] * a->esz);
        a->cnt[ci] += a->cnt[ci + 1];
        FreeVec(a->ch[ci + 1]);
        dir_remove(a, ci + 1);
        refresh(a, ci);
    }
    if (pos >= a->cnt[ci]) { p->ci = ci + 1; p->pos = 0; }  /* successor is in the next chunk (or the end) */
}

void carr_touched(struct Carr *a, ULONG ci)
{
    if (a->ok && ci < a->nch) refresh(a, ci);
}
