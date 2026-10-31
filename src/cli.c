/*
 * cli.c - command-line mode (see cli.h).
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/rdargs.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "clib.h"
#include "blockdev.h"
#include "volume.h"
#include "fsmap.h"
#include "cli.h"
#include "ffs_defrag.h"
#include "version.h"

#define TEMPLATE "VOLUME,IMAGE/K,PART/K,LIST/S,MAP/S,WIDTH/N,DEBUG/S,DEFRAG/S,DRYRUN/S,FORCE/S"
enum { A_VOLUME, A_IMAGE, A_PART, A_LIST, A_MAP, A_WIDTH, A_DEBUG, A_DEFRAG, A_DRYRUN, A_FORCE, A_COUNT };

static void out(const char *s) { PutStr((CONST_STRPTR)s); }

static void outf(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    out(buf);
}

static void cli_log(void *ud, const char *line) { (void)ud; out(line); out("\n"); }
static BOOL cli_move(void *ud, ULONG blk, ULONG n, UBYTE cls);

static BOOL cli_confirm(void)
{
    char buf[16];
    LONG n;
    out("Type YES to defragment this volume: ");
    Flush(Output());
    n = Read(Input(), buf, sizeof(buf) - 1);
    if (n <= 0) return FALSE;
    buf[n] = '\0';
    return buf[0] == 'Y' && buf[1] == 'E' && buf[2] == 'S';
}

static LONG run_defrag(struct BlockDev *bd, const struct VolumeInfo *v, BOOL dry, BOOL force)
{
    struct FfsDefragOpts  o;
    struct FfsDefragStats st;
    char err[128];
    LONG rc;

    if (v->fskind != FS_FFS) { out("DEFRAG: only OFS/FFS volumes in this version.\n"); return RETURN_ERROR; }
    memset(&o, 0, sizeof(o));
    o.dry_run = dry;
    if (!dry && !force) {
        outf("EXPERIMENTAL SOFTWARE - UNDER DEVELOPMENT\n"
             "AmiDefrag rewrites file data and filesystem structures on %s (%s).\n"
             "A bug here can cause permanent data loss. Make sure you have a FULL BACKUP.\n"
             "The author accepts no responsibility for any loss of data.\n"
             "Fragmented files are moved one at a time; the volume is inhibited while this\n"
             "runs, so nothing else must write to it.\n", v->dosname, v->devname);
        if (!cli_confirm()) { out("Cancelled.\n"); return RETURN_WARN; }
    }
    rc = FfsDefrag_Run(bd, v, &o, FfsDefrag_DefaultPlatform(), cli_log, dry ? NULL : cli_move, NULL, &st, err, sizeof(err));
    if (err[0]) outf("%s\n", err);
    if (st.aborted) out("Stopped at your request (Ctrl-C) after the current file; the volume is consistent.\n");
    outf("%s: %lu fragmented, %lu %s (%lu blocks), %lu skipped for space, %lu skipped with errors%s\n",
         dry ? "Dry run" : "Done", (unsigned long)st.candidates, (unsigned long)st.moved,
         dry ? "would move" : "moved", (unsigned long)st.moved_blocks,
         (unsigned long)st.skipped_space, (unsigned long)st.skipped_error,
         st.needs_validation ? " - VOLUME NEEDS VALIDATION" : "");
    outf("Device I/O: %lu blocks read, %lu written\n", (unsigned long)bd->reads, (unsigned long)bd->writes);
    return rc;
}

static BOOL cli_progress(void *ud, ULONG done, ULONG total, const char *what)
{
    (void)ud; (void)done; (void)total; (void)what;
    return (SetSignal(0, 0) & SIGBREAKF_CTRL_C) == 0;
}

/* Engine move callback: Ctrl-C stops the run after the file in flight.
 * The engine calls this for every block range it changes as well as for
 * its between-files poll (cls 0xFF), so the stop must stick once seen. */
static BOOL cli_stop_seen;
static BOOL cli_move(void *ud, ULONG blk, ULONG n, UBYTE cls)
{
    (void)ud; (void)blk; (void)n; (void)cls;
    if (!cli_stop_seen && (SetSignal(0, 0) & SIGBREAKF_CTRL_C)) {
        SetSignal(0, SIGBREAKF_CTRL_C);
        cli_stop_seen = TRUE;
    }
    return !cli_stop_seen;
}

static void list_volumes(const struct VolumeList *vl)
{
    UWORD i;
    char fs[32], sz[24];
    if (vl->count == 0) { out("No partitions found.\n"); return; }
    out("Name   Volume            Filesystem        Size      Device\n");
    for (i = 0; i < vl->count; i++) {
        const struct VolumeInfo *v = &vl->v[i];
        Volume_FsName(v->dostype, fs, sizeof(fs));
        Volume_SizeString(v->num_blocks, v->sector_size, sz, sizeof(sz));
        outf("%-6s %-17s %-17s %-9s %s unit %lu%s\n", v->dosname, v->volname, fs, sz,
             v->devname, (unsigned long)v->unit, v->mounted ? "" : " (not mounted)");
    }
}

static ULONG pct10(ULONG part, ULONG total)   /* tenths of a percent */
{
    if (total == 0) return 0;
    if (part > 0xFFFFFFFFUL / 1000) return (ULONG)(((UQUAD)part * 1000) / total);
    return part * 1000 / total;
}

static void print_class(const struct FsMap *m, UBYTE cls)
{
    char sz[24];
    ULONG p = pct10(m->count[cls], m->nblocks);
    Volume_SizeString(m->count[cls], m->blocksize, sz, sizeof(sz));
    outf("  %c %-11s %10lu blocks  %9s  %3lu.%lu%%\n", FsMap_ClassChar(cls),
         FsMap_ClassName(cls), (unsigned long)m->count[cls], sz,
         (unsigned long)(p / 10), (unsigned long)(p % 10));
}

static void print_stats(const struct VolumeInfo *v, const struct FsMap *m)
{
    char sz[24];
    UBYTE c;
    Volume_SizeString(m->nblocks, m->blocksize, sz, sizeof(sz));
    outf("Volume %s (%s:)  %s  %s, %lu blocks of %lu bytes\n",
         m->volname, v->dosname, m->fsname, sz,
         (unsigned long)m->nblocks, (unsigned long)m->blocksize);
    for (c = 0; c < BLK_CLASSES; c++) print_class(m, c);
    {
        ULONG p = pct10(m->frag_files, m->files);
        outf("Files: %lu, fragmented: %lu (%lu.%lu%%), extra fragments: %lu, worst file: %lu pieces\n",
             (unsigned long)m->files, (unsigned long)m->frag_files,
             (unsigned long)(p / 10), (unsigned long)(p % 10),
             (unsigned long)m->extra_runs, (unsigned long)m->worst_runs);
    }
    if (m->dirs) outf("Directories: %lu\n", (unsigned long)m->dirs);
    if (m->fskind == FS_FFS)
        outf("Bitmap flag: %s\n", m->bitmap_valid ? "valid" : "INVALID (volume needs validation)");
    if (m->note[0]) outf("Note: %s\n", m->note);
}

static void print_map(const struct FsMap *m, ULONG width)
{
    char line[260];
    ULONG per, cells, i;
    UBYTE c;
    if (width < 16) width = 16;
    if (width > 250) width = 250;
    per = (m->nblocks + width * 16 - 1) / (width * 16);
    if (per == 0) per = 1;
    cells = (m->nblocks + per - 1) / per;
    outf("\nBlock map, %lu blocks per character:\n", (unsigned long)per);
    for (i = 0; i < cells; i++) {
        line[i % width] = FsMap_ClassChar(FsMap_Cell(m, i * per, per));
        if ((i % width) == width - 1 || i == cells - 1) {
            line[(i % width) + 1] = '\n';
            line[(i % width) + 2] = '\0';
            out(line);
        }
    }
    out("Legend:");
    for (c = 0; c < BLK_CLASSES; c++) outf("  %c %s", FsMap_ClassChar(c), FsMap_ClassName(c));
    out("\n");
}

/* Find a partition by DOS name ("DH1", "DH1:") or by index ("1"). */
static const struct VolumeInfo *pick(const struct VolumeList *vl, const char *name)
{
    UWORD i;
    char  want[32];
    ULONG n = 0;
    if (!name) return vl->count ? &vl->v[0] : NULL;
    while (name[n] && name[n] != ':' && n < sizeof(want) - 1) { want[n] = name[n]; n++; }
    want[n] = '\0';
    for (i = 0; i < vl->count; i++) {
        const char *a = vl->v[i].dosname, *b = want;
        BOOL eq = TRUE;
        while (*a || *b) {
            char ca = *a++, cb = *b++;
            if (ca >= 'a' && ca <= 'z') ca -= 32;
            if (cb >= 'a' && cb <= 'z') cb -= 32;
            if (ca != cb) { eq = FALSE; break; }
        }
        if (eq) return &vl->v[i];
    }
    if (want[0] >= '0' && want[0] <= '9') {
        ULONG ix = strtoul(want, NULL, 10);
        if (ix < vl->count) return &vl->v[ix];
    }
    return NULL;
}

LONG cli_run(void)
{
    SIPTR args[A_COUNT];
    struct RDArgs *rda;
    struct VolumeList *vl;
    struct BlockDev *bd = NULL;
    struct FsMap map;
    const struct VolumeInfo *v;
    LONG rc = RETURN_OK;

    memset(args, 0, sizeof(args));
    rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
    if (!rda) {
        PrintFault(IoErr(), (CONST_STRPTR)"AmiDefrag");
        return RETURN_ERROR;
    }
    if (!args[A_VOLUME] && !args[A_IMAGE] && !args[A_LIST]) {
        FreeArgs(rda);
        return CLI_NO_ARGS;
    }

    vl = (struct VolumeList *)AllocVec(sizeof(*vl), MEMF_PUBLIC | MEMF_CLEAR);
    if (!vl) { FreeArgs(rda); out("Out of memory\n"); return RETURN_FAIL; }

    if (args[A_IMAGE]) {
        char err[80];
        bd = BlockDev_OpenFile((const char *)args[A_IMAGE]);
        if (!bd) { outf("Cannot open image %s\n", (const char *)args[A_IMAGE]); rc = RETURN_ERROR; goto done; }
        if (!Volume_EnumerateImage(bd, vl, err, sizeof(err))) { outf("%s: %s\n", bd->devname, err); rc = RETURN_ERROR; goto done; }
    } else {
        Volume_EnumerateMounted(vl);
    }

    if (args[A_LIST]) { list_volumes(vl); goto done; }

    v = pick(vl, args[A_IMAGE] ? (const char *)args[A_PART] : (const char *)args[A_VOLUME]);
    if (!v) { out("Partition not found. Use LIST to see what is there.\n"); rc = RETURN_ERROR; goto done; }

    if (!bd) {
        bd = BlockDev_Open(v->devname, v->unit);
        if (!bd) { outf("Cannot open %s unit %lu\n", v->devname, (unsigned long)v->unit); rc = RETURN_ERROR; goto done; }
    }

    /* DEFRAG goes straight to the engine: it has its own checks and its
     * journal recovery must run even when the map cannot be built. */
    if (args[A_DEFRAG] || args[A_DRYRUN]) {
        rc = run_defrag(bd, v, args[A_DRYRUN] != 0, args[A_FORCE] != 0);
        goto done;
    }
    if (!FsMap_Build(bd, v, &map, cli_progress, NULL)) {
        outf("%s: %s\n", v->dosname, map.note);
        rc = RETURN_WARN;
        goto done;
    }
    print_stats(v, &map);
    if (args[A_DEBUG]) {
        ULONG i, shown = 0;
        outf("Device reads: %lu blocks. Unknown (bitmap-used, unreferenced) blocks:",
             (unsigned long)bd->reads);
        for (i = 0; i < map.nblocks && shown < 24; i++)
            if (FsMap_Get(&map, i) == BLK_UNKN) { outf(" %lu", (unsigned long)i); shown++; }
        out(shown ? "\n" : " none\n");
    }
    if (args[A_MAP]) print_map(&map, args[A_WIDTH] ? (ULONG)*(LONG *)args[A_WIDTH] : 64);
    FsMap_Free(&map);

done:
    if (bd) BlockDev_Close(bd);
    FreeVec(vl);
    FreeArgs(rda);
    return rc;
}
