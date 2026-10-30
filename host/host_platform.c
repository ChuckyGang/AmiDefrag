/* host_platform.c - engine OS hooks on Linux: there is no handler on an
 * image file, so inhibit is a no-op and the journal file must already be
 * in the image (the test harness writes it with xdftool). */
#include "amiga_compat.h"
#include "volume.h"
#include "ffs_defrag.h"

static BOOL pf_inhibit(void *ud, const struct VolumeInfo *v, BOOL on, char *err, ULONG errlen)
{ (void)ud; (void)v; (void)on; (void)err; (void)errlen; return TRUE; }
static BOOL pf_create_journal(void *ud, const struct VolumeInfo *v, ULONG bytes, char *err, ULONG errlen)
{ (void)ud; (void)v; (void)bytes; (void)err; (void)errlen; return TRUE; }
static BOOL pf_delete_journal(void *ud, const struct VolumeInfo *v)
{ (void)ud; (void)v; return TRUE; }

static const struct FfsDefragPlatform host_pf = { pf_inhibit, pf_create_journal, pf_delete_journal, NULL };
const struct FfsDefragPlatform *FfsDefrag_DefaultPlatform(void) { return &host_pf; }
