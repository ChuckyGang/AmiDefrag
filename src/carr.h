/*
 * carr.h - chunked sorted array
 *
 * A sorted array of fixed-size records whose first ULONG is the key,
 * kept in chunks of at most `percap` records.  An insert or a removal
 * shifts the records of one chunk (a few KB) instead of the whole array:
 * the PFS3 room-making pass keeps a table of every data run on the
 * volume (about 100,000 entries on a 1 GB torture volume) and changes it
 * once per run moved aside; as one flat array that was 112 GB of memmove
 * per run, hours of 68000 time.
 *
 * Positions are (chunk, slot) pairs.  A change at a position leaves every
 * position before it valid; positions after it must be looked up again.
 * The optional tag callback summarises a chunk (the free-run index puts a
 * size-class mask there) and is called again after every change to that
 * chunk, so a search can skip chunks by their tag.
 */
#ifndef CARR_H
#define CARR_H

#include <exec/types.h>

struct CarrPos { ULONG ci; UWORD pos; };

typedef ULONG (*CarrTag)(const void *recs, ULONG n);

struct Carr {
    UWORD   esz, percap;      /* record size (multiple of 4), records per chunk */
    ULONG   n;                /* records in all chunks */
    ULONG   nch, chcap;       /* chunks in use, directory capacity */
    UBYTE **ch;               /* chunk storage */
    UWORD  *cnt;              /* records in each chunk (never 0) */
    ULONG  *first;            /* key of each chunk's first record */
    ULONG  *tag;              /* tag of each chunk, 0 without a callback */
    CarrTag tagfn;
    BOOL    ok;               /* TRUE between a successful build and a failure/free */
};

void  carr_init(struct Carr *a, UWORD esz, UWORD percap, CarrTag tagfn);
void  carr_free(struct Carr *a);
/* replace the contents with n records already sorted by key */
BOOL  carr_build(struct Carr *a, const void *sorted, ULONG n);
/* the last record with key <= `key`; FALSE (p = first position) when none */
BOOL  carr_find(const struct Carr *a, ULONG key, struct CarrPos *p);
/* the record at p, NULL at the end */
void *carr_at(const struct Carr *a, const struct CarrPos *p);
/* advance p; FALSE when it reaches the end */
BOOL  carr_next(const struct Carr *a, struct CarrPos *p);
/* insert before the record at p (at the end when p is the end position);
 * p then names the new record.  FALSE and a->ok = FALSE when out of memory. */
BOOL  carr_insert(struct Carr *a, struct CarrPos *p, const void *rec);
/* remove the record at p; p then names its successor */
void  carr_remove(struct Carr *a, struct CarrPos *p);
/* records of chunk ci were edited in place (keys may change as long as
 * the order holds): refresh the chunk's first key and tag */
void  carr_touched(struct Carr *a, ULONG ci);

#endif
