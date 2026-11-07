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
#include "pfs_defrag.h"
#include "sfs_defrag.h"
#include "pfs_core.h"

extern BOOL AmiDefrag_Debug;
#include "version.h"
#include "locale_support.h"

#define TEMPLATE "VOLUME,IMAGE/K,PART/K,LIST/S,MAP/S,WIDTH/N,FILES/S,DEBUG/S,DEFRAG/S,DRYRUN/S,FORCE/S,LIVE/S"
enum { A_VOLUME, A_IMAGE, A_PART, A_LIST, A_MAP, A_WIDTH, A_FILES, A_DEBUG, A_DEFRAG, A_DRYRUN, A_FORCE, A_LIVE, A_COUNT };

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
static BOOL cli_sfs_move(void *ud, ULONG src, ULONG dst, ULONG n);
static BOOL cli_progress(void *ud, ULONG done, ULONG total, const char *what);
static void print_stats(const struct VolumeInfo *v, const struct FsMap *m);

static BOOL cli_confirm(void)
{
    char buf[16];
    LONG n;
    out(GS(MSG_CLI_TYPE_YES));
    Flush(Output());
    n = Read(Input(), buf, sizeof(buf) - 1);
    if (n <= 0) return FALSE;
    buf[n] = '\0';
    return buf[0] == 'Y' && buf[1] == 'E' && buf[2] == 'S';
}

/* SFS: the SmartFilesystem handler's own defragmenter does the moving,
 * one transaction-safe step at a time, with the volume mounted. */
static LONG run_defrag_sfs(struct BlockDev *bd, const struct VolumeInfo *v, BOOL dry, BOOL force)
{
    struct SfsDefragStats st;
    struct FsMap map;
    ULONG nblocks, bsize;
    char err[160];
    LONG rc;

    if (dry) {
        out(GS(MSG_CLI_SFS_DRYRUN));
        return RETURN_WARN;
    }
    if (!SfsDefrag_Available(v)) {
        out(GS(MSG_CLI_SFS_NEEDS_MOUNT));
        return RETURN_ERROR;
    }
    if (!FsMap_Build(bd, v, &map, cli_progress, NULL)) { outf("%s: %s\n", v->dosname, map.note); return RETURN_WARN; }
    print_stats(v, &map);
    nblocks = map.nblocks; bsize = map.blocksize;
    FsMap_Free(&map);
    if (!SfsDefrag_Probe(v, &st, err, sizeof(err))) { outf("%s\n", err); return RETURN_ERROR; }
    outf(GS(MSG_CLI_SFS_HANDLER),
         (unsigned long)(st.version >> 16), (unsigned long)(st.version & 0xFFFF),
         (unsigned long)st.total_blocks, (unsigned long)st.block_size, (unsigned long)st.stack);
    if (!force) {
        outf(GS(MSG_CLI_SFS_WARNING), v->dosname, v->devname);
        if (!cli_confirm()) { out(GS(MSG_CLI_CANCELLED)); return RETURN_WARN; }
    }
    rc = SfsDefrag_Run(v, nblocks, bsize, cli_log, cli_sfs_move, NULL, &st, err, sizeof(err));
    if (err[0]) outf("%s\n", err);
    if (st.aborted) out(GS(MSG_CLI_SFS_STOPPED));
    outf(GS(MSG_CLI_SFS_DONE),
         rc == RETURN_OK ? GS(MSG_CLI_DONE) : GS(MSG_CLI_STOPPED), (unsigned long)st.moved_blocks,
         (unsigned long)st.steps, (unsigned long)st.moves);
    if ((rc == RETURN_OK || rc == RETURN_WARN) && FsMap_Build(bd, v, &map, cli_progress, NULL)) {
        print_stats(v, &map);
        FsMap_Free(&map);
    }
    return rc;
}

static LONG run_defrag(struct BlockDev *bd, const struct VolumeInfo *v, BOOL dry, BOOL force, BOOL live)
{
    struct FfsDefragOpts  o;
    struct FfsDefragStats st;
    char err[128];
    LONG rc;

    if (v->fskind == FS_SFS) return run_defrag_sfs(bd, v, dry, force);
    if (v->fskind != FS_FFS && v->fskind != FS_PFS) { out(GS(MSG_CLI_ONLY_SUPPORTED_FS)); return RETURN_ERROR; }
    memset(&o, 0, sizeof(o));
    o.dry_run = dry;
    o.live_boot = live;
    if (v->is_boot && !dry && !live) { outf(GS(MSG_CLI_BOOT_NEEDS_LIVE), v->dosname); return RETURN_ERROR; }
    if (!dry && !force) {
        outf(GS(MSG_CLI_WARNING), v->dosname, v->devname);
        if (!cli_confirm()) { out(GS(MSG_CLI_CANCELLED)); return RETURN_WARN; }
    }
    rc = (v->fskind == FS_PFS ? PfsDefrag_Run : FfsDefrag_Run)
             (bd, v, &o, FfsDefrag_DefaultPlatform(), cli_log, dry ? NULL : cli_move, NULL, &st, err, sizeof(err));
    if (err[0]) outf("%s\n", err);
    if (st.aborted) out(GS(MSG_CLI_FILE_STOPPED));
    outf(GS(MSG_CLI_RUN_SUMMARY),
         dry ? GS(MSG_CLI_DRY_RUN) : GS(MSG_CLI_DONE), (unsigned long)st.candidates, (unsigned long)st.moved,
         dry ? GS(MSG_CLI_WOULD_MOVE) : GS(MSG_CLI_MOVED), (unsigned long)st.moved_blocks,
         (unsigned long)st.skipped_space, (unsigned long)st.skipped_error,
         st.needs_validation ? (v->fskind == FS_PFS ? GS(MSG_CLI_RUN_AGAIN_SUFFIX) : GS(MSG_CLI_NEEDS_VALIDATION_SUFFIX)) : "");
    if (st.skipped_open)
        outf(GS(MSG_CLI_OPEN_SKIPPED), (unsigned long)st.skipped_open);
    if (st.needs_room)
        outf(GS(MSG_CLI_NEEDS_ROOM),
             (unsigned long)st.needs_room);
    if (st.moved_aside)
        outf(GS(MSG_CLI_MOVED_ASIDE), (unsigned long)st.moved_aside, (unsigned long)st.moved_aside_blocks);
    outf(GS(MSG_CLI_DEVICE_IO), (unsigned long)bd->reads, (unsigned long)bd->writes);
    return rc;
}

static BOOL cli_progress(void *ud, ULONG done, ULONG total, const char *what)
{
    (void)ud; (void)done; (void)total; (void)what;
    return (SetSignal(0, 0) & SIGBREAKF_CTRL_C) == 0;
}

/* Ctrl-C stops a run after the file / step in flight.  The engines call
 * their move callbacks for every block range they change as well as for
 * the between-items poll, so the stop must stick once seen. */
static BOOL cli_stop_seen;
static BOOL cli_break(void)
{
    if (!cli_stop_seen && (SetSignal(0, 0) & SIGBREAKF_CTRL_C)) {
        SetSignal(0, SIGBREAKF_CTRL_C);
        cli_stop_seen = TRUE;
    }
    return cli_stop_seen;
}
static BOOL cli_move(void *ud, ULONG blk, ULONG n, UBYTE cls)
{
    (void)ud; (void)blk; (void)n; (void)cls;
    return !cli_break();
}
static BOOL cli_sfs_move(void *ud, ULONG src, ULONG dst, ULONG n)
{
    (void)ud; (void)src; (void)dst; (void)n;
    return !cli_break();
}

static void list_volumes(const struct VolumeList *vl)
{
    UWORD i;
    char fs[32], sz[24];
    if (vl->count == 0) { out(GS(MSG_CLI_NO_PARTITIONS)); return; }
    out(GS(MSG_CLI_LIST_HEADER));
    for (i = 0; i < vl->count; i++) {
        const struct VolumeInfo *v = &vl->v[i];
        Volume_FsName(v->dostype, fs, sizeof(fs));
        Volume_SizeString(v->num_blocks, v->sector_size, sz, sizeof(sz));
        outf(GS(MSG_CLI_LIST_ROW), v->dosname, v->volname, fs, sz,
             v->devname, (unsigned long)v->unit, v->mounted ? "" : GS(MSG_CLI_NOT_MOUNTED_SUFFIX));
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
    outf(GS(MSG_CLI_CLASS_ROW), FsMap_ClassChar(cls),
         FsMap_ClassName(cls), (unsigned long)m->count[cls], sz,
         (unsigned long)(p / 10), (unsigned long)(p % 10));
}

static void print_stats(const struct VolumeInfo *v, const struct FsMap *m)
{
    char sz[24];
    UBYTE c;
    Volume_SizeString(m->nblocks, m->blocksize, sz, sizeof(sz));
    outf(GS(MSG_CLI_VOLUME_HEADER),
         m->volname, v->dosname, m->fsname, sz,
         (unsigned long)m->nblocks, (unsigned long)m->blocksize);
    for (c = 0; c < BLK_CLASSES; c++) print_class(m, c);
    {
        ULONG p = pct10(m->frag_files, m->files);
        outf(GS(MSG_CLI_FILES_LINE),
             (unsigned long)m->files, (unsigned long)m->frag_files,
             (unsigned long)(p / 10), (unsigned long)(p % 10),
             (unsigned long)m->extra_runs, (unsigned long)m->worst_runs);
    }
    if (m->dirs) outf(GS(MSG_CLI_DIRECTORIES), (unsigned long)m->dirs);
    if (m->fskind == FS_FFS)
        outf(GS(MSG_CLI_BITMAP_FLAG), m->bitmap_valid ? GS(MSG_CLI_BITMAP_VALID) : GS(MSG_CLI_BITMAP_INVALID));
    if (m->note[0]) outf(GS(MSG_CLI_NOTE), m->note);
}

/* FILES: the fragmented files by path (all files with DEBUG).  PFS3 only
 * so far: the walker reports each directory before its contents, so a
 * table anodenr -> path of the directories seen is enough. */
struct FileList {
    struct PfsVol *v;
    ULONG  *dir_an; char **dir_path; ULONG ndirs, cap;
    BOOL    all;
    ULONG   listed, first_blk;
};

static const char *dir_path_of(struct FileList *fl, ULONG an)
{
    ULONG i;
    for (i = 0; i < fl->ndirs; i++) if (fl->dir_an[i] == an) return fl->dir_path[i];
    return "?/";
}

static BOOL files_cb(void *ud, ULONG anodenr, LONG type, const char *name, ULONG parent)
{
    struct FileList *fl = (struct FileList *)ud;
    if (type == PFS_ST_USERDIR) {
        const char *pp = dir_path_of(fl, parent);
        ULONG len = strlen(pp) + strlen(name) + 2;
        char *s;
        if (fl->ndirs == fl->cap) {
            ULONG ncap = fl->cap ? fl->cap * 2 : 64;
            ULONG *na = (ULONG *)AllocVec(ncap * sizeof(ULONG), MEMF_PUBLIC);
            char **np = (char **)AllocVec(ncap * sizeof(char *), MEMF_PUBLIC);
            if (!na || !np) { if (na) FreeVec(na); if (np) FreeVec(np); return FALSE; }
            if (fl->ndirs) { memcpy(na, fl->dir_an, fl->ndirs * sizeof(ULONG)); memcpy(np, fl->dir_path, fl->ndirs * sizeof(char *)); }
            if (fl->dir_an) FreeVec(fl->dir_an);
            if (fl->dir_path) FreeVec(fl->dir_path);
            fl->dir_an = na; fl->dir_path = np; fl->cap = ncap;
        }
        s = (char *)AllocVec(len, MEMF_PUBLIC);
        if (!s) return FALSE;
        snprintf(s, len, "%s%s/", pp, name);
        fl->dir_an[fl->ndirs] = anodenr; fl->dir_path[fl->ndirs] = s; fl->ndirs++;
        return TRUE;
    }
    if (type == PFS_ST_FILE || type == PFS_ST_ROLLOVER) {
        ULONG a = anodenr, guard = 0, runs = 0, nblk = 0, cs, blk, next;
        fl->first_blk = 0;
        while (a && guard++ < 1000000UL) {
            if (!pfs_get_anode(fl->v, a, &cs, &blk, &next)) break;
            if (cs) { if (!runs) fl->first_blk = blk; runs++; nblk += cs; }
            a = next;
        }
        if (runs > 1 || fl->all) {
            if (AmiDefrag_Debug)
                outf("%6lu %8lu  %s%s  [anode %lu first %lu]\n", (unsigned long)runs, (unsigned long)nblk,
                     dir_path_of(fl, parent), name, (unsigned long)anodenr, (unsigned long)fl->first_blk);
            else
                outf("%6lu %8lu  %s%s\n", (unsigned long)runs, (unsigned long)nblk, dir_path_of(fl, parent), name);
            fl->listed++;
        }
    }
    return (SetSignal(0, 0) & SIGBREAKF_CTRL_C) == 0;
}

static void print_files(struct BlockDev *bd, const struct VolumeInfo *v, BOOL all)
{
    struct PfsVol pv;
    struct FileList fl;
    ULONG i;
    static char root[] = "";
    if (v->fskind != FS_PFS) { out(GS(MSG_CLI_FILES_PFS_ONLY)); return; }
    memset(&pv, 0, sizeof(pv));
    memset(&fl, 0, sizeof(fl));
    if (!pfs_open(&pv, bd, v)) { outf(GS(MSG_CLI_FILES_ERROR), pv.err); return; }
    fl.v = &pv; fl.all = all;
    fl.dir_an = (ULONG *)AllocVec(64 * sizeof(ULONG), MEMF_PUBLIC);
    fl.dir_path = (char **)AllocVec(64 * sizeof(char *), MEMF_PUBLIC);
    if (!fl.dir_an || !fl.dir_path) { out(GS(MSG_CLI_FILES_NO_MEMORY)); goto done; }
    fl.cap = 64;
    fl.dir_an[0] = PFS_ANODE_ROOTDIR; fl.dir_path[0] = root; fl.ndirs = 1;
    outf(GS(MSG_CLI_FILES_HEADER), all ? GS(MSG_CLI_FILES_ALL) : GS(MSG_CLI_FILES_FRAGMENTED));
    pfs_walk(&pv, files_cb, &fl, NULL);
    outf(GS(MSG_CLI_FILES_LISTED), (unsigned long)fl.listed);
done:
    for (i = 1; i < fl.ndirs; i++) FreeVec(fl.dir_path[i]);
    if (fl.dir_an) FreeVec(fl.dir_an);
    if (fl.dir_path) FreeVec(fl.dir_path);
    pfs_close(&pv);
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
    outf(GS(MSG_CLI_MAP_HEADER), (unsigned long)per);
    for (i = 0; i < cells; i++) {
        line[i % width] = FsMap_ClassChar(FsMap_Cell(m, i * per, per));
        if ((i % width) == width - 1 || i == cells - 1) {
            line[(i % width) + 1] = '\n';
            line[(i % width) + 2] = '\0';
            out(line);
        }
    }
    out(GS(MSG_CLI_LEGEND));
    for (c = 0; c < BLK_CLASSES; c++) outf("  %c %s", FsMap_ClassChar(c), FsMap_ClassName(c));
    out("\n");
}

/* Find a partition by DOS name ("DH1", "DH1:") or by index ("1"). */
static BOOL name_eq(const char *a, const char *b)
{
    while (*a || *b) {
        char ca = *a++, cb = *b++;
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb) return FALSE;
    }
    return TRUE;
}

/* Device name (HD0:) first, then mounted volume name (Work:), then list index. */
static const struct VolumeInfo *pick(const struct VolumeList *vl, const char *name)
{
    UWORD i;
    char  want[32];
    ULONG n = 0;
    if (!name) return vl->count ? &vl->v[0] : NULL;
    while (name[n] && name[n] != ':' && n < sizeof(want) - 1) { want[n] = name[n]; n++; }
    want[n] = '\0';
    for (i = 0; i < vl->count; i++)
        if (name_eq(vl->v[i].dosname, want)) return &vl->v[i];
    for (i = 0; i < vl->count; i++)
        if (vl->v[i].volname[0] && name_eq(vl->v[i].volname, want)) return &vl->v[i];
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
    if (!vl) { FreeArgs(rda); out(GS(MSG_CLI_NO_MEMORY)); return RETURN_FAIL; }

    if (args[A_IMAGE]) {
        char err[80];
        bd = BlockDev_OpenFile((const char *)args[A_IMAGE]);
        if (!bd) { outf(GS(MSG_CLI_CANNOT_OPEN_IMAGE), (const char *)args[A_IMAGE]); rc = RETURN_ERROR; goto done; }
        if (!Volume_EnumerateImage(bd, vl, err, sizeof(err))) { outf("%s: %s\n", bd->devname, err); rc = RETURN_ERROR; goto done; }
    } else {
        Volume_EnumerateMounted(vl);
    }

    if (args[A_DEBUG]) AmiDefrag_Debug = TRUE;
    if (args[A_LIST]) { list_volumes(vl); goto done; }

    v = pick(vl, args[A_IMAGE] ? (const char *)args[A_PART] : (const char *)args[A_VOLUME]);
    if (!v) { out(GS(MSG_CLI_PARTITION_NOT_FOUND)); rc = RETURN_ERROR; goto done; }

    if (!bd) {
        bd = BlockDev_Open(v->devname, v->unit);
        if (!bd) { outf(GS(MSG_CLI_CANNOT_OPEN_UNIT), v->devname, (unsigned long)v->unit); rc = RETURN_ERROR; goto done; }
    }

    /* DEFRAG goes straight to the engine: it has its own checks and its
     * journal recovery must run even when the map cannot be built. */
    if (args[A_DEFRAG] || args[A_DRYRUN]) {
        rc = run_defrag(bd, v, args[A_DRYRUN] != 0, args[A_FORCE] != 0, args[A_LIVE] != 0);
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
    if (args[A_FILES]) print_files(bd, v, args[A_DEBUG] != 0);
    FsMap_Free(&map);

done:
    if (bd) BlockDev_Close(bd);
    FreeVec(vl);
    FreeArgs(rda);
    return rc;
}
