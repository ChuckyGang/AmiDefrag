/*
 * platform_amiga.c - OS services for the move engines: inhibit the
 * volume, create / delete the journal file.
 *
 * Everything from the inhibit onwards goes to the handler's message port
 * directly, never through a name.  dos.library resolves "NAME:" by
 * walking the DosList, and when the volume carries the same name as its
 * device (volume SCSI on device SCSI:) the volume node is found first.
 * During the inhibit that node has no task (the handler parked its locks
 * there), so DOS would put up "Please insert volume ... in any drive"
 * instead of talking to the handler - and the uninhibit would never
 * arrive.  PFS3 additionally answers the uninhibit before it has
 * remounted the volume, so even a distinct name could race right after
 * it; a packet to the port waits for the handler, which remounts first.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include "clib.h"
#include "volume.h"
#include "ffs_defrag.h"

static void devname_colon(const struct VolumeInfo *v, char *out, ULONG outlen)
{
    snprintf(out, outlen, "%s:", v->dosname);
}

/* Image files and unmounted partitions have no handler to talk to: like
 * the host build, the journal must already be in the image then. */
static BOOL no_handler(const struct VolumeInfo *v)
{
    const char *d = v->devname;
    BOOL is_file = (d[0] == 'F' || d[0] == 'f') && (d[1] == 'I' || d[1] == 'i') &&
                   (d[2] == 'L' || d[2] == 'l') && (d[3] == 'E' || d[3] == 'e') && d[4] == ':';
    return !v->mounted || is_file;
}

static struct MsgPort *handler_port(const struct VolumeInfo *v)
{
    char name[40];
    if (v->handler) return (struct MsgPort *)v->handler;
    devname_colon(v, name, sizeof(name));           /* image / unmounted: not reached */
    return DeviceProc((CONST_STRPTR)name);
}

static BOOL pf_inhibit(void *ud, const struct VolumeInfo *v, BOOL on, char *err, ULONG errlen)
{
    struct MsgPort *port;
    int  tries;
    (void)ud;
    if (no_handler(v)) return TRUE;
    port = handler_port(v);
    if (!port) { if (err) snprintf(err, errlen, "no handler for %s:", v->dosname); return FALSE; }
    if (!on) { DoPkt(port, ACTION_INHIBIT, DOSFALSE, 0, 0, 0, 0); return TRUE; }
    /* FFS refuses while its own writes are unflushed (up to ~3 s). */
    for (tries = 0; tries < 20; tries++) {
        if (DoPkt(port, ACTION_INHIBIT, DOSTRUE, 0, 0, 0, 0)) return TRUE;
        Delay(25);
    }
    if (err) snprintf(err, errlen, "cannot inhibit %s: (error %ld): files open or writes pending",
                      v->dosname, (long)IoErr());
    return FALSE;
}

static BOOL pf_create_journal(void *ud, const struct VolumeInfo *v, ULONG bytes, char *err, ULONG errlen)
{
    char path[64];
    BPTR fh;
    UBYTE *zero;
    BOOL ok = TRUE;
    (void)ud;
    if (no_handler(v)) return TRUE;
    snprintf(path, sizeof(path), "%s:" FFS_JOURNAL_NAME, v->dosname);
    zero = (UBYTE *)AllocVec(bytes, MEMF_PUBLIC | MEMF_CLEAR);
    if (!zero) { if (err) strncpy(err, "out of memory", errlen - 1); return FALSE; }
    fh = Open((CONST_STRPTR)path, MODE_NEWFILE);
    if (!fh) {
        if (err) snprintf(err, errlen, "cannot create %s (error %ld)", path, (long)IoErr());
        FreeVec(zero); return FALSE;
    }
    if (Write(fh, zero, (LONG)bytes) != (LONG)bytes) {
        if (err) snprintf(err, errlen, "cannot write %s (error %ld)", path, (long)IoErr());
        ok = FALSE;
    }
    Close(fh);
    FreeVec(zero);
    if (!ok) DeleteFile((CONST_STRPTR)path);
    return ok;
}

/* Called right after the uninhibit: ACTION_DELETE_OBJECT with a NULL lock
 * (= the root of the volume) and a BSTR name, straight to the handler. */
static BOOL pf_delete_journal(void *ud, const struct VolumeInfo *v)
{
    struct MsgPort *port;
    UBYTE *bname;
    ULONG len = strlen(FFS_JOURNAL_NAME);
    BOOL ok;
    (void)ud;
    if (no_handler(v)) return TRUE;
    port = handler_port(v);
    if (!port) return FALSE;
    bname = (UBYTE *)AllocVec(len + 2, MEMF_PUBLIC);   /* longword aligned for the BPTR */
    if (!bname) return FALSE;
    bname[0] = (UBYTE)len;
    memcpy(bname + 1, FFS_JOURNAL_NAME, len);
    bname[len + 1] = 0;
    ok = DoPkt(port, ACTION_DELETE_OBJECT, 0, (LONG)MKBADDR(bname), 0, 0, 0) != DOSFALSE;
    FreeVec(bname);
    /* The delete marks the root dirty; FFS would set the bitmap flag valid
     * again only when its idle timer fires.  Ask it to flush now so an
     * Analyze right after the run does not show "needs validation". */
    DoPkt(port, ACTION_FLUSH, 0, 0, 0, 0, 0);
    return ok;
}

/* Keys of every lock the handler holds on the volume.  Called after the
 * inhibit: FFS moves its LockQueue into the volume node's dol_LockList
 * then (ForgetLocks/StashLocks in main.asm), PFS3 links its file entries
 * there (DiskRemoveSequence in volume.c).  Open files have a lock too
 * (FFS: the rwdata's private lock, PFS3: the fileentry), so this is the
 * complete set of objects whose cached state must not be invalidated.
 * The node pointer was taken at enumeration; it is only dereferenced
 * while it is still found on the DosList (a volume without locks is
 * forgotten by FFS on inhibit, node and all). */
static ULONG pf_open_keys(void *ud, const struct VolumeInfo *v, ULONG *keys, ULONG max)
{
    struct DosList *dol, *d;
    ULONG n = 0;
    (void)ud;
    if (no_handler(v) || !v->volnode) return 0;
    dol = LockDosList(LDF_VOLUMES | LDF_READ);
    d = dol;
    while ((d = NextDosEntry(d, LDF_VOLUMES)) != NULL) {
        if (d != (struct DosList *)v->volnode) continue;
        Forbid();
        {
            BPTR l = d->dol_misc.dol_volume.dol_LockList;
            ULONG guard = 0;
            while (l && guard++ < 100000UL) {
                struct FileLock *fl = (struct FileLock *)BADDR(l);
                if (n < max) keys[n] = fl->fl_Key;
                n++;
                l = fl->fl_Link;
            }
        }
        Permit();
        break;
    }
    UnLockDosList(LDF_VOLUMES | LDF_READ);
    return n;
}

static const struct FfsDefragPlatform amiga_pf = { pf_inhibit, pf_create_journal, pf_delete_journal, NULL, NULL, pf_open_keys };

const struct FfsDefragPlatform *FfsDefrag_DefaultPlatform(void)
{
    return &amiga_pf;
}
