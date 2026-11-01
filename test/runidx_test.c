/*
 * runidx_test.c - host-only randomized check of the PFS3 bitmap helpers:
 * pfs_bm_set_range against a shadow bitmap (bits, free count, dirty
 * flags), the chunked free-run index against a fresh scan after every
 * change (contents, chunk first keys and size-class tags), and the index
 * finders against the bitmap scan (best fit exact up to 16 sectors,
 * largest run exact).  Build and run with  make -C host check-index.
 */
#include "../src/pfs_core.c"
#include "../src/carr.c"
#include <stdio.h>
#include <stdlib.h>
BOOL ustack_push(struct UStack *s, ULONG x) { (void)s; (void)x; return FALSE; }
void ustack_free(struct UStack *s) { (void)s; }
static struct PfsRun tmp[400000];
static ULONG shadow[20000];
static int check(struct PfsVol *v, int step, const char *what, ULONG a, ULONG b)
{
    ULONG n = runs_collect(v, tmp, 400000), i = 0;
    struct CarrPos p; const struct PfsRun *r;
    if (!v->runs.ok) { printf("index dropped at step %d\n", step); return 1; }
    { ULONG fb = 0, k; for (k = 0; k < n; k++) fb += tmp[k].len;
      if (fb != v->free_blocks) { printf("step %d: free_blocks %lu, bitmap says %lu\n", step, (unsigned long)v->free_blocks, (unsigned long)fb); return 1; } }
    { ULONG k; for (k = 0; k < v->bm_longs; k++) if (((v->bm[k] ^ shadow[k]) != 0)) { printf("step %d: long %lu differs from the shadow\n", step, (unsigned long)k); return 1; } }
    if (n != v->runs.n) { printf("step %d %s [%lu,%lu): count %lu vs %lu\n", step, what, (unsigned long)a, (unsigned long)b, (unsigned long)v->runs.n, (unsigned long)n); return 1; }
    p.ci = 0; p.pos = 0;
    for (i = 0; i < n; i++) {
        r = carr_at(&v->runs, &p);
        if (!r || tmp[i].start != r->start || tmp[i].len != r->len) {
            printf("step %d %s [%lu,%lu): run %lu idx {%lu,%lu} real {%lu,%lu}\n", step, what, (unsigned long)a, (unsigned long)b, (unsigned long)i,
                   r ? (unsigned long)r->start : 0, r ? (unsigned long)r->len : 0, (unsigned long)tmp[i].start, (unsigned long)tmp[i].len); return 1; }
        carr_next(&v->runs, &p);
    }
    if (carr_at(&v->runs, &p)) { printf("step %d: index longer than the scan\n", step); return 1; }
    /* chunk invariants: first keys, tags */
    for (i = 0; i < v->runs.nch; i++) {
        if (v->runs.cnt[i] == 0 || v->runs.cnt[i] > v->runs.percap) { printf("step %d: chunk %lu count %u\n", step, (unsigned long)i, v->runs.cnt[i]); return 1; }
        if (v->runs.first[i] != ((struct PfsRun *)v->runs.ch[i])->start) { printf("step %d: chunk %lu first key stale\n", step, (unsigned long)i); return 1; }
        if (v->runs.tag[i] != run_tag(v->runs.ch[i], v->runs.cnt[i])) { printf("step %d: chunk %lu tag stale\n", step, (unsigned long)i); return 1; }
    }
    return 0;
}
static ULONG runlen_at(struct PfsVol *v, ULONG s, ULONG ex_lo, ULONG ex_hi)
{
    ULONG n = 0;
    while (s + n < v->disksize && pfs_bm_isfree(v, s + n) && !(ex_hi > ex_lo && s + n >= ex_lo && s + n < ex_hi)) n++;
    return n;
}
int main(void)
{
    struct PfsVol v; memset(&v, 0, sizeof(v));
    ULONG nbits = 300000, i, c;
    int bad = 0;
    for (i = 1; i < 3000000UL; i += (i < 100000 ? 1 : 997)) {       /* class bounds agree with run_class */
        ULONG lo, hi; class_bounds(run_class(i), &lo, &hi);
        if (i < lo || i > hi) { printf("class of %lu out of its bounds\n", (unsigned long)i); return 1; }
    }
    for (c = 1; c < 32; c++) { ULONG lo, hi, lo2, hi2; class_bounds(c - 1, &lo, &hi); class_bounds(c, &lo2, &hi2); if (lo2 != hi + 1) { printf("class gap at %lu\n", (unsigned long)c); return 1; } }
    v.bitmapstart = 100; v.disksize = 100 + nbits; v.bm_longs = (nbits + 31) / 32; v.longsperbmb = 1000; v.nbmb = 1;
    v.bm = calloc(v.bm_longs + 1, 4); v.bmb_dirty = calloc(64, 1); v.nbmb = 10; v.bm_loaded = TRUE;
    srand(7);
    for (i = 0; i < v.bm_longs; i++) v.bm[i] = rand() ^ (rand() << 16);
    { ULONG k, b; for (k = 0; k < nbits; k++) if (v.bm[k >> 5] & (0x80000000UL >> (k & 31))) v.free_blocks++; memcpy(shadow, v.bm, v.bm_longs * 4); (void)b; }
    runs_build(&v);
    if (check(&v, 0, "build", 0, 0)) return 1;
    for (i = 1; i < 100000; i++) {
        ULONG s = v.bitmapstart + rand() % nbits, n = 1 + rand() % (rand() % 4 ? 6 : 200); BOOL f = rand() & 1;
        pfs_bm_set_range(&v, s, n, f);
        { ULONG k; int changed = 0; for (k = s; k < s + n && k < v.disksize; k++) { ULONG bi = k - v.bitmapstart, m = 0x80000000UL >> (bi & 31); if (f && !(shadow[bi >> 5] & m)) { shadow[bi >> 5] |= m; changed = 1; } if (!f && (shadow[bi >> 5] & m)) { shadow[bi >> 5] &= ~m; changed = 1; } }
          { ULONG d = 0, q; for (q = 0; q < 10; q++) d |= v.bmb_dirty[q]; if (changed && d == 0) { printf("step %lu: nothing marked dirty\n", (unsigned long)i); return 1; }
          memset(v.bmb_dirty, 0, 64); } }
        if (i % 97 == 0 && check(&v, i, f ? "free" : "used", s, s + n)) return 1;
        if (i % 500 == 0) {                                    /* finders vs the bitmap scan */
            ULONG need = 1 + rand() % (rand() & 1 ? 16 : 300), ex_lo = 0, ex_hi = 0, a, b, la, lb;
            if (rand() & 1) { ex_lo = v.bitmapstart + rand() % nbits; ex_hi = ex_lo + rand() % 2000; }
            a = idx_find_run(&v, need, ex_lo, ex_hi);
            b = bm_scan(&v, 0, need, ex_lo, ex_hi, NULL, 0, NULL);
            if ((a == 0) != (b == 0)) { printf("step %lu: find need %lu: index %lu scan %lu\n", (unsigned long)i, (unsigned long)need, (unsigned long)a, (unsigned long)b); return 1; }
            if (a) {
                la = runlen_at(&v, a, ex_lo, ex_hi); lb = runlen_at(&v, b, ex_lo, ex_hi);
                if (la < need) { printf("step %lu: index run %lu too short (%lu < %lu)\n", (unsigned long)i, (unsigned long)a, (unsigned long)la, (unsigned long)need); return 1; }
                if (need <= 16 && ex_hi == 0 && la != lb) { printf("step %lu: need %lu: index len %lu scan len %lu\n", (unsigned long)i, (unsigned long)need, (unsigned long)la, (unsigned long)lb); return 1; }
                if (la > 2 * lb + 1 && ex_hi == 0) { printf("step %lu: need %lu: index len %lu far from scan len %lu\n", (unsigned long)i, (unsigned long)need, (unsigned long)la, (unsigned long)lb); bad++; }
            }
            { ULONG skip[2], ns = rand() % 3, k; for (k = 0; k < ns; k++) skip[k] = v.bitmapstart + rand() % nbits;
              a = idx_largest_run(&v, skip, ns, &la); b = bm_scan(&v, 1, 0, 0, 0, skip, ns, &lb);
              if (la != lb) { printf("step %lu: largest: index %lu/%lu scan %lu/%lu\n", (unsigned long)i, (unsigned long)a, (unsigned long)la, (unsigned long)b, (unsigned long)lb); return 1; } }
        }
    }
    printf("ok %lu runs in %lu chunks, %d loose fits\n", (unsigned long)v.runs.n, (unsigned long)v.runs.nch, bad);
    /* second phase: single bits freed all over a used volume grow the index
     * from nothing, so chunks fill up and split; the used flips merge again */
    memset(v.bm, 0, v.bm_longs * 4); v.free_blocks = 0;                 /* everything used: the index starts empty */
    memcpy(shadow, v.bm, v.bm_longs * 4);
    runs_build(&v);
    if (check(&v, 0, "build2", 0, 0)) return 1;
    { ULONG nch0 = v.runs.nch, splits = 0;
      for (i = 1; i < 60000; i++) {
          ULONG s = v.bitmapstart + rand() % nbits, n = 1; BOOL f = (rand() % 4) != 0;   /* mostly freeing: new runs appear between used bits */
          ULONG bi = s - v.bitmapstart, m = 0x80000000UL >> (bi & 31);
          if (f) shadow[bi >> 5] |= m; else shadow[bi >> 5] &= ~m;
          pfs_bm_set_range(&v, s, n, f);
          if (v.runs.nch > nch0) { splits++; nch0 = v.runs.nch; }
          if (i % 61 == 0 && check(&v, i, f ? "free" : "used", s, s + n)) return 1;
      }
      if (check(&v, i, "end", 0, 0)) return 1;
      printf("ok phase 2: %lu runs in %lu chunks after %lu chunk count increases\n", (unsigned long)v.runs.n, (unsigned long)v.runs.nch, (unsigned long)splits);
      if (!splits) { printf("phase 2 never split a chunk\n"); return 1; }
    }
    return 0;
}
