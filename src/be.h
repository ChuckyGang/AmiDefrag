/*
 * be.h - big-endian field access for on-disk structures.
 *
 * Every filesystem block AmiDefrag reads (FFS, PFS3, SFS, RDB) is a bag of
 * big-endian words and longwords.  Reading them through these helpers
 * instead of casting to ULONG keeps the exact same source correct on the
 * little-endian Linux host build, and needs no alignment.
 */
#ifndef BE_H
#define BE_H

#include <exec/types.h>

static inline ULONG be32(const UBYTE *b, ULONG o)
{
    return ((ULONG)b[o] << 24) | ((ULONG)b[o + 1] << 16) |
           ((ULONG)b[o + 2] << 8) | (ULONG)b[o + 3];
}

static inline UWORD be16(const UBYTE *b, ULONG o)
{
    return (UWORD)(((UWORD)b[o] << 8) | b[o + 1]);
}

/* Longword index helper for the FFS blocks (array of longs). */
static inline ULONG bel(const UBYTE *b, ULONG longidx)
{
    return be32(b, longidx * 4);
}

#endif /* BE_H */
