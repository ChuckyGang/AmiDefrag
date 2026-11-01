/*
 * gui_gtk.c - the Linux window: GTK3 on the same core as the Amiga GUI
 * (src/gui.c), same layout and the same flow.
 *
 *   partition list   from the RDB of the image or raw disk
 *   buttons          Open | Analyze | Rescan | Defragment | Stop | About
 *   map              the block grid, FsMap_Cell() per square
 *   legend, status
 *
 * The analysis and the engines run in a worker thread; their callbacks
 * only write app fields under a mutex and the main loop picks those up
 * on a 100 ms tick (status line, map redraw).  Dialogs happen between
 * the phases on the main thread: dry run -> warning -> live run ->
 * result, as on the Amiga.
 *
 * FFS and PFS3 are defragmented; SFS is map only here (its engine drives
 * the handler through DOS packets, and there is no handler on Linux).
 * The run's journal is a sidecar file (host_platform.c).
 */
#include <gtk/gtk.h>
#include <string.h>
#include <stdlib.h>
#include <strings.h>
#include "amiga_compat.h"
#include "blockdev.h"
#include "volume.h"
#include "fsmap.h"
#include "ffs_defrag.h"
#include "pfs_defrag.h"
#include "version.h"

void        HostPlatform_SetImage(const char *path);
const char *HostPlatform_JournalPath(const struct VolumeInfo *v);

#define PITCH 6                 /* square + gap, as on the Amiga */

enum { COL_NAME, COL_VOL, COL_FS, COL_SIZE, COL_IDX, N_COLS };
enum { JOB_NONE, JOB_ANALYZE, JOB_DRYRUN, JOB_DEFRAG };

struct App {
    GtkWidget    *win, *tree, *b_analyze, *b_rescan, *b_defrag, *b_stop, *area, *status;
    GtkListStore *store;
    char          path[1024];
    struct VolumeList vols;
    int           selected;
    /* shared with the worker, under lock */
    GMutex        lock;
    struct FsMap  map;
    BOOL          have_map;
    int           map_vol;
    BOOL          dirty;
    char         *pending_status;
    ULONG         last_pct;
    /* the job */
    GThread      *thread;
    int           job;
    volatile gint stop;
    BOOL          quit_pending;
    struct BlockDev *bd;
    struct VolumeInfo *v;
    struct FfsDefragStats st;
    struct FfsDefragOpts  opts;
    char          err[160];
    LONG          rc;
    gint64        t0;
    BOOL          scan_ok;
    BOOL          auto_defrag;      /* test hook: defragment right after the analysis */
};

static const double colour[BLK_CLASSES][3] = {
    { 1.00, 1.00, 1.00 },   /* free: white */
    { 0.00, 0.33, 1.00 },   /* data: blue */
    { 1.00, 0.13, 0.13 },   /* fragmented: red */
    { 0.00, 0.67, 0.20 },   /* system: green */
    { 0.55, 0.70, 0.75 },   /* reserved */
    { 1.00, 0.87, 0.00 },   /* unknown: yellow */
};

/* ------------------------------------------------------------------ */
/* status + map updates from any thread                                 */
/* ------------------------------------------------------------------ */

static void post_status(struct App *a, const char *s)
{
    g_mutex_lock(&a->lock);
    g_free(a->pending_status);
    a->pending_status = g_strdup(s);
    g_mutex_unlock(&a->lock);
}

static void set_status_now(struct App *a, const char *s)
{
    gtk_label_set_text(GTK_LABEL(a->status), s);
}

static gboolean tick(gpointer ud)
{
    struct App *a = (struct App *)ud;
    char *s = NULL;
    BOOL dirty;
    g_mutex_lock(&a->lock);
    s = a->pending_status; a->pending_status = NULL;
    dirty = a->dirty; a->dirty = FALSE;
    g_mutex_unlock(&a->lock);
    if (s) { set_status_now(a, s); g_free(s); }
    if (dirty) gtk_widget_queue_draw(a->area);
    return G_SOURCE_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* map                                                                 */
/* ------------------------------------------------------------------ */

static gboolean draw_map(GtkWidget *w, cairo_t *cr, gpointer ud)
{
    struct App *a = (struct App *)ud;
    int W = gtk_widget_get_allocated_width(w), H = gtk_widget_get_allocated_height(w);
    ULONG pitch = PITCH, cols, rows, cells, per, i;

    cairo_set_source_rgb(cr, 0.62, 0.62, 0.62);
    cairo_paint(cr);
    g_mutex_lock(&a->lock);
    /* a small volume has fewer blocks than squares: grow the squares so
     * the grid still fills the area (the Amiga window keeps them small) */
    if (a->have_map && a->map.nblocks) {
        while (pitch < 24 && (ULONG)(W / (pitch + 1)) * (ULONG)(H / (pitch + 1)) >= a->map.nblocks) pitch++;
    }
    cols = W / pitch; rows = H / pitch;
    cells = cols * rows;
    if (a->have_map && cells) {
        per = (a->map.nblocks + cells - 1) / cells;
        if (per == 0) per = 1;
        for (i = 0; i * per < a->map.nblocks; i++) {
            UBYTE c = FsMap_Cell(&a->map, i * per, per);
            if (c >= BLK_CLASSES) c = BLK_UNKN;
            cairo_set_source_rgb(cr, colour[c][0], colour[c][1], colour[c][2]);
            cairo_rectangle(cr, (i % cols) * pitch, (i / cols) * pitch, pitch - 1, pitch - 1);
            cairo_fill(cr);
        }
    }
    g_mutex_unlock(&a->lock);
    return FALSE;
}

static gboolean draw_swatch(GtkWidget *w, cairo_t *cr, gpointer ud)
{
    int c = GPOINTER_TO_INT(ud);
    (void)w;
    cairo_set_source_rgb(cr, colour[c][0], colour[c][1], colour[c][2]);
    cairo_rectangle(cr, 0, 2, 12, 12);
    cairo_fill(cr);
    cairo_set_source_rgb(cr, 0.2, 0.2, 0.2);
    cairo_set_line_width(cr, 1);
    cairo_rectangle(cr, 0.5, 2.5, 11, 11);
    cairo_stroke(cr);
    return FALSE;
}

/* ------------------------------------------------------------------ */
/* volume list                                                         */
/* ------------------------------------------------------------------ */

static void drop_map(struct App *a)
{
    g_mutex_lock(&a->lock);
    if (a->have_map) FsMap_Free(&a->map);
    a->have_map = FALSE;
    a->map_vol = -1;
    g_mutex_unlock(&a->lock);
    gtk_widget_queue_draw(a->area);
}

static void fill_list(struct App *a)
{
    UWORD i;
    char fs[32], sz[24];
    gtk_list_store_clear(a->store);
    for (i = 0; i < a->vols.count; i++) {
        struct VolumeInfo *v = &a->vols.v[i];
        GtkTreeIter it;
        Volume_FsName(v->dostype, fs, sizeof(fs));
        Volume_SizeString(v->num_blocks, v->sector_size, sz, sizeof(sz));
        gtk_list_store_append(a->store, &it);
        gtk_list_store_set(a->store, &it, COL_NAME, v->dosname, COL_VOL, v->volname[0] ? v->volname : "-",
                           COL_FS, fs, COL_SIZE, sz, COL_IDX, (gint)i, -1);
    }
    a->selected = a->vols.count ? 0 : -1;
    if (a->vols.count) {
        GtkTreePath *p = gtk_tree_path_new_first();
        gtk_tree_selection_select_path(gtk_tree_view_get_selection(GTK_TREE_VIEW(a->tree)), p);
        gtk_tree_path_free(p);
    }
}

static void open_path(struct App *a, const char *path)
{
    struct BlockDev *bd;
    char err[128], s[1200];

    drop_map(a);
    gtk_widget_set_sensitive(a->b_defrag, FALSE);
    strncpy(a->path, path, sizeof(a->path) - 1);
    a->path[sizeof(a->path) - 1] = '\0';
    HostPlatform_SetImage(a->path);
    memset(&a->vols, 0, sizeof(a->vols));
    bd = BlockDev_OpenFile(a->path);
    if (!bd) {
        snprintf(s, sizeof(s), "Cannot open %s%s", a->path,
                 strncmp(a->path, "/dev/", 5) == 0 ? " - a raw disk needs root (sudo)" : "");
        fill_list(a);
        set_status_now(a, s);
        return;
    }
    err[0] = '\0';
    if (!Volume_EnumerateImage(bd, &a->vols, err, sizeof(err))) {
        snprintf(s, sizeof(s), "%s: %s", a->path, err[0] ? err : "no partitions found");
        set_status_now(a, s);
    } else {
        snprintf(s, sizeof(s), "%s: %u partition(s). Select one and press Analyze.", a->path, (unsigned)a->vols.count);
        set_status_now(a, s);
    }
    BlockDev_Close(bd);
    fill_list(a);
    gtk_widget_set_sensitive(a->b_rescan, TRUE);
}

static void on_selection(GtkTreeSelection *sel, gpointer ud)
{
    struct App *a = (struct App *)ud;
    GtkTreeModel *m;
    GtkTreeIter it;
    gint idx;
    if (gtk_tree_selection_get_selected(sel, &m, &it)) {
        gtk_tree_model_get(m, &it, COL_IDX, &idx, -1);
        a->selected = idx;
    }
}

static void on_open(GtkButton *b, gpointer ud)
{
    struct App *a = (struct App *)ud;
    GtkWidget *d;
    (void)b;
    if (a->job != JOB_NONE) return;
    d = gtk_file_chooser_dialog_new("Open disk image", GTK_WINDOW(a->win), GTK_FILE_CHOOSER_ACTION_OPEN,
                                    "_Cancel", GTK_RESPONSE_CANCEL, "_Open", GTK_RESPONSE_ACCEPT, NULL);
    if (a->path[0] && strncmp(a->path, "/dev/", 5) != 0)
        gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(d), a->path);
    if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_ACCEPT) {
        char *f = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(d));
        gtk_widget_destroy(d);
        if (f) { open_path(a, f); g_free(f); }
        return;
    }
    gtk_widget_destroy(d);
}

static void on_rescan(GtkButton *b, gpointer ud)
{
    struct App *a = (struct App *)ud;
    (void)b;
    if (a->job != JOB_NONE || !a->path[0]) return;
    open_path(a, a->path);
}

/* ------------------------------------------------------------------ */
/* dialogs                                                             */
/* ------------------------------------------------------------------ */

static int ask(struct App *a, const char *title, const char *body, const char *ok_label, BOOL cancel)
{
    GtkWidget *d;
    /* test hook: AMIDEFRAG_GUI_TEST=1 answers every dialog with OK, so the
     * harness can drive a run without a human (scratch images only) */
    if (getenv("AMIDEFRAG_GUI_TEST")) { g_printerr("[gui] %s\n%s\n", title, body); return 1; }
    d = gtk_message_dialog_new(GTK_WINDOW(a->win), GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                          cancel ? GTK_MESSAGE_WARNING : GTK_MESSAGE_INFO, GTK_BUTTONS_NONE, "%s", body);
    int r;
    gtk_window_set_title(GTK_WINDOW(d), title);
    if (cancel) gtk_dialog_add_button(GTK_DIALOG(d), "_Cancel", GTK_RESPONSE_CANCEL);
    gtk_dialog_add_button(GTK_DIALOG(d), ok_label, GTK_RESPONSE_OK);
    gtk_dialog_set_default_response(GTK_DIALOG(d), cancel ? GTK_RESPONSE_CANCEL : GTK_RESPONSE_OK);
    r = gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
    return r == GTK_RESPONSE_OK;
}

static void on_about(GtkButton *b, gpointer ud)
{
    struct App *a = (struct App *)ud;
    char body[700], title[64];
    (void)b;
    snprintf(body, sizeof(body),
        "AmiDefrag %s (Linux GTK frontend)\nBuilt: %s\n"
        "Disk defragmenter for Amiga volumes with the classic block map.\n\n"
        "Files are moved one at a time with a journaled, single-write\n"
        "commit per file, so the volume stays valid at every moment.\n"
        "OFS/FFS and PFS3 defragmentation; SFS map only on Linux.\n\n"
        "Same core as the AmigaOS program; the window follows it.\n\n"
        "Director: John Hertell\n"
        "Code: Claude Code (Anthropic)\n\n"
        "https://github.com/ChuckyGang/AmiDefrag\n\n"
        "MIT License \302\251 2026 John Hertell",
        AMIDEFRAG_VERSION, AmiDefrag_BuildStamp);
    snprintf(title, sizeof(title), "About %s", AMIDEFRAG_VERTITLE);
    ask(a, title, body, "_OK", FALSE);
}

/* ------------------------------------------------------------------ */
/* worker: callbacks                                                   */
/* ------------------------------------------------------------------ */

static BOOL scan_progress(void *ud, ULONG done, ULONG total, const char *what)
{
    struct App *a = (struct App *)ud;
    ULONG pct = total ? (done >= total ? 100 : done * 100 / total) : 0xFFFF;
    if (pct != a->last_pct) {
        char s[200];
        a->last_pct = pct;
        if (pct == 0xFFFF) snprintf(s, sizeof(s), "%s... %lu", what, (unsigned long)done);
        else               snprintf(s, sizeof(s), "%s... %lu%%", what, (unsigned long)pct);
        post_status(a, s);
    }
    return !g_atomic_int_get(&a->stop);
}

static void defrag_log(void *ud, const char *line)
{
    post_status((struct App *)ud, line);
}

static BOOL defrag_move(void *ud, ULONG blk, ULONG n, UBYTE cls)
{
    struct App *a = (struct App *)ud;
    if (cls != 0xFF) {
        g_mutex_lock(&a->lock);
        if (a->have_map) { FsMap_SetRange(&a->map, blk, n, cls); a->dirty = TRUE; }
        g_mutex_unlock(&a->lock);
    }
    return !g_atomic_int_get(&a->stop);
}

/* ------------------------------------------------------------------ */
/* worker: the jobs                                                    */
/* ------------------------------------------------------------------ */

static gboolean job_done(gpointer ud);

static gpointer worker(gpointer ud)
{
    struct App *a = (struct App *)ud;
    switch (a->job) {
    case JOB_ANALYZE:
        g_mutex_lock(&a->lock);
        memset(&a->map, 0, sizeof(a->map));
        g_mutex_unlock(&a->lock);
        a->last_pct = 0xFFFE;
        a->scan_ok = FsMap_Build(a->bd, a->v, &a->map, scan_progress, a);
        break;
    case JOB_DRYRUN:
    case JOB_DEFRAG:
        a->rc = (a->v->fskind == FS_PFS ? PfsDefrag_Run : FfsDefrag_Run)
                    (a->bd, a->v, &a->opts, FfsDefrag_DefaultPlatform(), defrag_log,
                     a->job == JOB_DEFRAG ? defrag_move : NULL, a, &a->st, a->err, sizeof(a->err));
        break;
    }
    g_idle_add(job_done, a);
    return NULL;
}

static void set_busy(struct App *a, BOOL busy)
{
    gtk_widget_set_sensitive(a->b_analyze, !busy);
    gtk_widget_set_sensitive(a->b_rescan, !busy && a->path[0]);
    gtk_widget_set_sensitive(a->b_stop, busy);
    gtk_widget_set_sensitive(a->tree, !busy);
    if (busy) gtk_widget_set_sensitive(a->b_defrag, FALSE);
}

static void start_job(struct App *a, int job)
{
    a->job = job;
    g_atomic_int_set(&a->stop, 0);
    set_busy(a, TRUE);
    a->t0 = g_get_monotonic_time();
    a->thread = g_thread_new("amidefrag", worker, a);
}

static void on_stop(GtkButton *b, gpointer ud)
{
    struct App *a = (struct App *)ud;
    (void)b;
    if (a->job == JOB_NONE) return;
    g_atomic_int_set(&a->stop, 1);
    set_status_now(a, a->job == JOB_ANALYZE ? "Stopping..." : "Stopping after the current file...");
}

static BOOL defrag_allowed(struct App *a)
{
    const struct VolumeInfo *v;
    if (!a->have_map || a->map_vol < 0) return FALSE;
    v = &a->vols.v[a->map_vol];
    return (v->fskind == FS_FFS || v->fskind == FS_PFS) && a->map.frag_files > 0;
}

static void summarize(struct App *a)
{
    const struct FsMap *m = &a->map;
    char s[400], sz[24];
    ULONG p = m->files ? m->frag_files * 1000 / m->files : 0;
    Volume_SizeString(m->count[BLK_FREE], m->blocksize, sz, sizeof(sz));
    snprintf(s, sizeof(s), "%s: %lu files, %lu fragmented (%lu.%lu%%), %lu extra pieces, %s free%s%s%s",
             m->volname, (unsigned long)m->files, (unsigned long)m->frag_files,
             (unsigned long)(p / 10), (unsigned long)(p % 10),
             (unsigned long)m->extra_runs, sz,
             m->note[0] ? " - " : "", m->note,
             m->fskind == FS_SFS ? "  (SFS: map only on Linux; defragment it on the Amiga)" : "");
    set_status_now(a, s);
}

static void on_analyze(GtkButton *b, gpointer ud)
{
    struct App *a = (struct App *)ud;
    struct VolumeInfo *v;
    char s[1200];
    (void)b;
    if (a->job != JOB_NONE) return;
    if (a->selected < 0 || a->selected >= a->vols.count) { set_status_now(a, "Open an image and select a partition first."); return; }
    v = &a->vols.v[a->selected];
    if (v->fskind == FS_UNKNOWN) { set_status_now(a, "Unsupported filesystem on this partition."); return; }
    drop_map(a);
    a->bd = BlockDev_OpenFile(a->path);
    if (!a->bd) { snprintf(s, sizeof(s), "Cannot open %s", a->path); set_status_now(a, s); return; }
    a->v = v;
    a->map_vol = a->selected;
    start_job(a, JOB_ANALYZE);
}

static void start_defrag(struct App *a);

static void on_defrag(GtkButton *b, gpointer ud)
{
    struct App *a = (struct App *)ud;
    (void)b;
    if (a->job != JOB_NONE || !defrag_allowed(a)) return;
    start_defrag(a);
}

static void start_defrag(struct App *a)
{
    struct VolumeInfo *v = &a->vols.v[a->map_vol];
    char s[1200];
    a->bd = BlockDev_OpenFile(a->path);
    if (!a->bd) { snprintf(s, sizeof(s), "Cannot open %s", a->path); set_status_now(a, s); return; }
    a->v = v;
    memset(&a->opts, 0, sizeof(a->opts));
    a->opts.dry_run = TRUE;
    set_status_now(a, "Planning (dry run)...");
    start_job(a, JOB_DRYRUN);
}

static void finish_analyze(struct App *a)
{
    char s[300];
    BlockDev_Close(a->bd); a->bd = NULL;
    g_mutex_lock(&a->lock);
    a->have_map = a->scan_ok;
    g_mutex_unlock(&a->lock);
    gtk_widget_queue_draw(a->area);
    if (a->scan_ok) summarize(a);
    else {
        snprintf(s, sizeof(s), "%s: %s", a->v->dosname, a->map.note[0] ? a->map.note : "analysis failed");
        set_status_now(a, s);
        g_mutex_lock(&a->lock); FsMap_Free(&a->map); g_mutex_unlock(&a->lock);
    }
    gtk_widget_set_sensitive(a->b_defrag, defrag_allowed(a));
    if (a->auto_defrag && defrag_allowed(a)) { a->auto_defrag = FALSE; start_defrag(a); }
}

static void finish_dryrun(struct App *a)
{
    struct VolumeInfo *v = a->v;
    char body[1200];
    if (a->rc == RETURN_ERROR || a->st.candidates == 0) {
        snprintf(body, sizeof(body), "%s", a->err[0] ? a->err : "Nothing to defragment.");
        set_status_now(a, body);
        BlockDev_Close(a->bd); a->bd = NULL;
        gtk_widget_set_sensitive(a->b_defrag, defrag_allowed(a));
        return;
    }
    snprintf(body, sizeof(body),
        "EXPERIMENTAL SOFTWARE - UNDER DEVELOPMENT\n\n"
        "AmiDefrag rewrites file data and filesystem structures on\n"
        "%s (%s). A bug here can cause permanent data loss.\n"
        "Make sure you have a FULL BACKUP of this volume before\n"
        "proceeding. The author accepts NO responsibility for any\n"
        "loss of data caused by this software.\n\n"
        "%lu fragmented file(s), %lu blocks.\n"
        "%lu can be made contiguous directly, %lu have no free run\n"
        "large enough%s.\n\n"
        "Nothing else may have this image or disk open or mounted while\n"
        "this runs. Files are moved one at a time; a crash leaves the\n"
        "volume valid. The run's journal is kept in\n%s\n"
        "until the run ends; run again on this partition to finish an\n"
        "interrupted one.",
        a->map.volname, v->dosname, (unsigned long)a->st.candidates, (unsigned long)a->st.cand_blocks,
        (unsigned long)a->st.moved, (unsigned long)(a->st.skipped_space + a->st.needs_room),
        v->fskind == FS_PFS ? " (room is made for them by moving other files aside)" : "",
        HostPlatform_JournalPath(v));
    if (!ask(a, "AmiDefrag - Warning", body, "I have a backup - _Defragment", TRUE)) {
        set_status_now(a, "Cancelled.");
        BlockDev_Close(a->bd); a->bd = NULL;
        gtk_widget_set_sensitive(a->b_defrag, defrag_allowed(a));
        return;
    }
    a->opts.dry_run = FALSE;
    start_job(a, JOB_DEFRAG);
}

static void finish_defrag(struct App *a)
{
    struct VolumeInfo *v = a->v;
    const struct FfsDefragStats *st = &a->st;
    char body[900], moved[24], aside[24];
    ULONG secs = (ULONG)((g_get_monotonic_time() - a->t0) / 1000000);

    BlockDev_Close(a->bd); a->bd = NULL;
    if (a->rc != RETURN_FAIL && !st->needs_validation) {
        Volume_SizeString(st->moved_blocks, a->map.blocksize, moved, sizeof(moved));
        Volume_SizeString(st->moved_aside_blocks, a->map.blocksize, aside, sizeof(aside));
        snprintf(body, sizeof(body),
            "Defragmentation of %s finished.\n\n"
            "Files made contiguous: %lu of %lu\n"
            "Data moved: %s (%lu blocks)\n"
            "Moved aside to make room: %lu run(s), %s\n"
            "Time: %lu min %lu s\n"
            "Skipped, no room could be made: %lu\n%s",
            a->map.volname, (unsigned long)st->moved, (unsigned long)st->candidates,
            moved, (unsigned long)st->moved_blocks,
            (unsigned long)st->moved_aside, aside,
            (unsigned long)(secs / 60), (unsigned long)(secs % 60),
            (unsigned long)st->skipped_space,
            st->aborted ? "\nStopped at your request; the volume is consistent." : "");
        ask(a, "AmiDefrag", body, "_OK", FALSE);
    }
    g_mutex_lock(&a->lock);
    if (a->have_map) { FsMap_Recount(&a->map); a->map.frag_files = st->candidates - st->moved; }
    g_mutex_unlock(&a->lock);
    gtk_widget_queue_draw(a->area);
    snprintf(body, sizeof(body), "%s: %lu of %lu file(s) made contiguous (%lu blocks), %lu skipped for space%s%s%s",
        a->map.volname, (unsigned long)st->moved, (unsigned long)st->candidates,
        (unsigned long)st->moved_blocks, (unsigned long)st->skipped_space,
        st->needs_validation ? (v->fskind == FS_PFS ? " - RUN AGAIN TO FINISH" : " - VOLUME NEEDS VALIDATION") : "",
        a->err[0] ? " - " : "", a->err);
    set_status_now(a, body);
    if (a->rc == RETURN_FAIL || st->needs_validation)
        ask(a, "AmiDefrag", v->fskind == FS_PFS
            ? "The run stopped after writes began.\nThe volume is readable, but one file's move is only\nrecorded in the journal: run AmiDefrag again on this\npartition to let it finish or undo that move."
            : "The run stopped after writes began.\nThe bitmap was left marked invalid on purpose:\nthe filesystem will validate the volume on the next mount.\nRun AmiDefrag again afterwards to finish.", "_OK", FALSE);
    gtk_widget_set_sensitive(a->b_defrag, !st->needs_validation && defrag_allowed(a));
}

static gboolean job_done(gpointer ud)
{
    struct App *a = (struct App *)ud;
    int job = a->job;
    g_thread_join(a->thread);
    a->thread = NULL;
    a->job = JOB_NONE;
    tick(a);                                     /* last status / map update */
    set_busy(a, FALSE);
    if (a->quit_pending) { if (a->bd) BlockDev_Close(a->bd); gtk_main_quit(); return G_SOURCE_REMOVE; }
    switch (job) {
    case JOB_ANALYZE: finish_analyze(a); break;
    case JOB_DRYRUN:  finish_dryrun(a);  break;
    case JOB_DEFRAG:  finish_defrag(a);  break;
    }
    return G_SOURCE_REMOVE;
}

static gboolean on_delete(GtkWidget *w, GdkEvent *ev, gpointer ud)
{
    struct App *a = (struct App *)ud;
    (void)w; (void)ev;
    if (a->job != JOB_NONE) {
        a->quit_pending = TRUE;
        g_atomic_int_set(&a->stop, 1);
        set_status_now(a, "Stopping after the current file, then closing...");
        return TRUE;
    }
    gtk_main_quit();
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* window                                                              */
/* ------------------------------------------------------------------ */

static GtkWidget *button(GtkWidget *box, const char *label, GCallback cb, gpointer ud)
{
    GtkWidget *b = gtk_button_new_with_mnemonic(label);
    g_signal_connect(b, "clicked", cb, ud);
    gtk_box_pack_start(GTK_BOX(box), b, TRUE, TRUE, 0);
    return b;
}

static void add_column(GtkWidget *tree, const char *title, int col, gboolean expand)
{
    GtkCellRenderer *r = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *c = gtk_tree_view_column_new_with_attributes(title, r, "text", col, NULL);
    gtk_tree_view_column_set_expand(c, expand);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree), c);
}

static void build_window(struct App *a)
{
    GtkWidget *vbox, *hbox, *scroll, *legend, *frame;
    int i;

    a->win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(a->win), AMIDEFRAG_VERTITLE);
    gtk_window_set_default_size(GTK_WINDOW(a->win), 760, 560);
    g_signal_connect(a->win, "delete-event", G_CALLBACK(on_delete), a);

    vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 6);
    gtk_container_add(GTK_CONTAINER(a->win), vbox);

    a->store = gtk_list_store_new(N_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT);
    a->tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(a->store));
    add_column(a->tree, "Name", COL_NAME, FALSE);
    add_column(a->tree, "Volume", COL_VOL, TRUE);
    add_column(a->tree, "Filesystem", COL_FS, FALSE);
    add_column(a->tree, "Size", COL_SIZE, FALSE);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(a->tree)), "changed", G_CALLBACK(on_selection), a);
    scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(scroll, -1, 110);
    gtk_container_add(GTK_CONTAINER(scroll), a->tree);
    gtk_box_pack_start(GTK_BOX(vbox), scroll, FALSE, FALSE, 0);

    hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_set_homogeneous(GTK_BOX(hbox), TRUE);
    button(hbox, "_Open...", G_CALLBACK(on_open), a);
    a->b_analyze = button(hbox, "_Analyze", G_CALLBACK(on_analyze), a);
    a->b_rescan  = button(hbox, "_Rescan", G_CALLBACK(on_rescan), a);
    a->b_defrag  = button(hbox, "_Defragment", G_CALLBACK(on_defrag), a);
    a->b_stop    = button(hbox, "_Stop", G_CALLBACK(on_stop), a);
    button(hbox, "A_bout", G_CALLBACK(on_about), a);
    gtk_widget_set_sensitive(a->b_defrag, FALSE);
    gtk_widget_set_sensitive(a->b_stop, FALSE);
    gtk_widget_set_sensitive(a->b_rescan, FALSE);
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);

    frame = gtk_frame_new(NULL);
    gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_IN);
    a->area = gtk_drawing_area_new();
    g_signal_connect(a->area, "draw", G_CALLBACK(draw_map), a);
    gtk_container_add(GTK_CONTAINER(frame), a->area);
    gtk_box_pack_start(GTK_BOX(vbox), frame, TRUE, TRUE, 0);

    legend = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    for (i = 0; i < BLK_CLASSES; i++) {
        GtkWidget *item = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        GtkWidget *sw = gtk_drawing_area_new();
        gtk_widget_set_size_request(sw, 13, 16);
        g_signal_connect(sw, "draw", G_CALLBACK(draw_swatch), GINT_TO_POINTER(i));
        gtk_box_pack_start(GTK_BOX(item), sw, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(item), gtk_label_new(FsMap_ClassName((UBYTE)i)), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(legend), item, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(vbox), legend, FALSE, FALSE, 0);

    a->status = gtk_label_new("Open a disk image (or a raw disk such as /dev/sdb, as root) to begin.");
    gtk_label_set_xalign(GTK_LABEL(a->status), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(a->status), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(vbox), a->status, FALSE, FALSE, 0);
}

int main(int argc, char **argv)
{
    struct App *a;
    gtk_init(&argc, &argv);
    a = g_new0(struct App, 1);
    g_mutex_init(&a->lock);
    a->selected = -1;
    a->map_vol = -1;
    build_window(a);
    gtk_widget_show_all(a->win);
    g_timeout_add(100, tick, a);
    if (argc > 1) open_path(a, argv[1]);
    if (argc > 2) {                              /* amidefrag-gui IMAGE PART: analyse PART at once */
        int i;
        for (i = 0; i < a->vols.count; i++)
            if (strcasecmp(a->vols.v[i].dosname, argv[2]) == 0) {
                GtkTreePath *p = gtk_tree_path_new_from_indices(i, -1);
                gtk_tree_selection_select_path(gtk_tree_view_get_selection(GTK_TREE_VIEW(a->tree)), p);
                gtk_tree_path_free(p);
                a->selected = i;
                a->auto_defrag = argc > 3 && strcasecmp(argv[3], "DEFRAG") == 0;
                on_analyze(NULL, a);
                break;
            }
    }
    gtk_main();
    if (a->have_map) FsMap_Free(&a->map);
    return 0;
}
