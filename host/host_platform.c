/* host_platform.c - engine OS hooks on Linux.  There is no handler on an
 * image file or a raw disk, so inhibit is a no-op, and the journal lives
 * in a SIDECAR FILE instead of in the volume (nothing here can create a
 * file inside the filesystem):
 *
 *   image /path/img.hdf, partition at block S  ->  /path/img.hdf.AmiDefrag-journal-S
 *   raw disk /dev/sdb, partition at block S    ->  /var/tmp/amidefrag-sdb-S.journal
 *
 * The sidecar is created when a live run starts and removed when the run
 * ends cleanly; one left behind means an interrupted run, and the next
 * run on the same partition reads it to finish or undo that file's move.
 * AMIDEFRAG_JOURNAL_IN_VOLUME=1 selects the Amiga behaviour instead (the
 * AmiDefrag.journal file must then already exist in the volume); the test
 * harnesses use that to exercise the path the Amiga build takes. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include "amiga_compat.h"
#include "volume.h"
#include "ffs_defrag.h"

static char g_image[1024];          /* full path of the open image/device */
static char g_jpath[1100];
static int  g_jfd = -1;

void HostPlatform_SetImage(const char *path)
{
    strncpy(g_image, path ? path : "", sizeof(g_image) - 1);
    g_image[sizeof(g_image) - 1] = '\0';
}

const char *HostPlatform_JournalPath(const struct VolumeInfo *v)
{
    const char *base = g_image[0] ? g_image : (strncmp(v->devname, "FILE:", 5) == 0 ? v->devname + 5 : v->devname);
    if (strncmp(base, "/dev/", 5) == 0) {
        const char *b = strrchr(base, '/');
        snprintf(g_jpath, sizeof(g_jpath), "/var/tmp/amidefrag-%s-%lu.journal", b ? b + 1 : base, (unsigned long)v->start_block);
    } else {
        snprintf(g_jpath, sizeof(g_jpath), "%s.AmiDefrag-journal-%lu", base, (unsigned long)v->start_block);
    }
    return g_jpath;
}

static BOOL in_volume(void)
{
    const char *e = getenv("AMIDEFRAG_JOURNAL_IN_VOLUME");
    return e && *e && *e != '0';
}

static BOOL pf_inhibit(void *ud, const struct VolumeInfo *v, BOOL on, char *err, ULONG errlen)
{ (void)ud; (void)v; (void)on; (void)err; (void)errlen; return TRUE; }

static BOOL pf_create_journal(void *ud, const struct VolumeInfo *v, ULONG bytes, char *err, ULONG errlen)
{
    (void)ud;
    if (in_volume()) return TRUE;
    HostPlatform_JournalPath(v);
    if (g_jfd >= 0) close(g_jfd);
    g_jfd = open(g_jpath, O_RDWR | O_CREAT, 0644);
    if (g_jfd < 0) {
        if (err && errlen) snprintf(err, errlen, "cannot create the journal file %s", g_jpath);
        return FALSE;
    }
    {
        struct stat st;
        if (fstat(g_jfd, &st) == 0 && (ULONG)st.st_size < bytes) {
            /* new (or short) sidecar: zero it so a record read finds no magic */
            static const char z[4096];
            off_t off = st.st_size;
            while ((ULONG)off < bytes) {
                size_t n = bytes - (ULONG)off; if (n > sizeof(z)) n = sizeof(z);
                if (pwrite(g_jfd, z, n, off) != (ssize_t)n) break;
                off += (off_t)n;
            }
        }
    }
    return TRUE;
}

static BOOL pf_delete_journal(void *ud, const struct VolumeInfo *v)
{
    (void)ud; (void)v;
    if (in_volume()) return TRUE;
    if (g_jfd >= 0) { close(g_jfd); g_jfd = -1; }
    if (g_jpath[0]) unlink(g_jpath);
    return TRUE;
}

static BOOL pf_journal_io(void *ud, BOOL write, ULONG off, void *buf, ULONG len)
{
    (void)ud;
    if (g_jfd < 0) return FALSE;
    if (write) {
        if (pwrite(g_jfd, buf, len, (off_t)off) != (ssize_t)len) return FALSE;
        return fdatasync(g_jfd) == 0;       /* the record must be on disk before the commit write */
    }
    {
        ssize_t n = pread(g_jfd, buf, len, (off_t)off);
        if (n < 0) return FALSE;
        if ((ULONG)n < len) memset((char *)buf + n, 0, len - (ULONG)n);
        return TRUE;
    }
}

static const struct FfsDefragPlatform host_pf   = { pf_inhibit, pf_create_journal, pf_delete_journal, NULL, pf_journal_io };
static const struct FfsDefragPlatform host_pf_iv = { pf_inhibit, pf_create_journal, pf_delete_journal, NULL, NULL };
const struct FfsDefragPlatform *FfsDefrag_DefaultPlatform(void) { return in_volume() ? &host_pf_iv : &host_pf; }
