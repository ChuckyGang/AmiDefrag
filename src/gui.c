/*
 * gui.c - GadTools main window (see gui.h).
 *
 * Layout, top to bottom:
 *   listview   one line per partition (name, volume, filesystem, size)
 *   buttons    Analyze | Rescan | Defragment (disabled until an engine
 *              exists) - the close gadget quits
 *   map        the block grid: cell = FsMap_Cell() over a run of blocks,
 *              drawn as small filled squares, Windows-defrag style
 *   legend     one swatch per class
 *   status     progress while scanning, the summary afterwards
 *
 * The scan runs synchronously; its progress callback drains the window's
 * message port so the close gadget still works and cancels the scan.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <graphics/gfxbase.h>
#include <graphics/rastport.h>
#include <graphics/text.h>
#include <graphics/gfxmacros.h>
#include <libraries/gadtools.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/gadtools.h>
#include "gt_compat.h"

#include "clib.h"
#include "blockdev.h"
#include "volume.h"
#include "fsmap.h"
#include "ffs_defrag.h"
#include "sfs_defrag.h"
#include "gui.h"
#include "version.h"

extern struct GfxBase *GfxBase;

enum { GID_LIST = 1, GID_ANALYZE, GID_RESCAN, GID_DEFRAG, GID_STOP, GID_ABOUT };

#define CELL   5          /* square size in pixels */
#define PITCH  6          /* square + gap */

struct Gui {
    struct Screen  *scr;
    APTR            vi;
    struct Window  *win;
    struct Gadget  *glist, *lv_gad, *analyze_gad, *defrag_gad, *stop_gad;
    struct List     labels;
    struct Node     nodes[MAX_VOLUMES];
    char            texts[MAX_VOLUMES][96];
    struct VolumeList *vols;
    WORD            selected;
    /* geometry */
    UWORD fh, baseline, bor_l, bor_t;
    UWORD map_x, map_y, map_w, map_h, cols, rows;
    UWORD legend_y, status_y, win_w, win_h;
    /* pens */
    LONG  pen[BLK_CLASSES];
    BOOL  pen_obtained[BLK_CLASSES];
    /* state */
    struct FsMap map;
    BOOL  have_map;
    WORD  map_vol;               /* volume index the map belongs to */
    BOOL  quit;
    BOOL  scanning;
    BOOL  stop;                  /* Stop button pressed during a scan or run */
    char  status[300];
    ULONG last_pct;
};

/* ------------------------------------------------------------------ */
/* pens                                                                */
/* ------------------------------------------------------------------ */

static void obtain_pens(struct Gui *g)
{
    /* 0x00rrggbb-ish wishes, scaled to 32-bit per ObtainBestPen */
    static const ULONG wish[BLK_CLASSES][3] = {
        { 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF },   /* free: white      */
        { 0x00000000, 0x55555555, 0xFFFFFFFF },   /* data: blue       */
        { 0xFFFFFFFF, 0x22222222, 0x22222222 },   /* fragmented: red  */
        { 0x00000000, 0xAAAAAAAA, 0x33333333 },   /* system: green    */
        { 0x00000000, 0xBBBBBBBB, 0xBBBBBBBB },   /* reserved: cyan (grey = window bg) */
        { 0xFFFFFFFF, 0xDDDDDDDD, 0x00000000 },   /* unknown: yellow  */
    };
    /* 4-colour fallback: WB pens 0 grey 1 black 2 white 3 blue */
    static const LONG fallback[BLK_CLASSES] = { 2, 3, 1, 0, 0, 1 };
    UWORD i;
    BOOL deep = (GfxBase->LibNode.lib_Version >= 39) &&
                (g->scr->RastPort.BitMap->Depth >= 3) && g->scr->ViewPort.ColorMap;

    for (i = 0; i < BLK_CLASSES; i++) {
        g->pen[i] = fallback[i];
        g->pen_obtained[i] = FALSE;
        if (deep) {
            struct TagItem tags[] = { { OBP_Precision, PRECISION_GUI }, { TAG_DONE, 0 } };
            LONG p = ObtainBestPenA(g->scr->ViewPort.ColorMap, wish[i][0], wish[i][1], wish[i][2], tags);
            if (p >= 0) { g->pen[i] = p; g->pen_obtained[i] = TRUE; }
        }
    }
}

static void release_pens(struct Gui *g)
{
    UWORD i;
    for (i = 0; i < BLK_CLASSES; i++)
        if (g->pen_obtained[i]) ReleasePen(g->scr->ViewPort.ColorMap, g->pen[i]);
}

/* ------------------------------------------------------------------ */
/* drawing                                                             */
/* ------------------------------------------------------------------ */

static void draw_text_line(struct Gui *g, UWORD y, const char *s)
{
    struct RastPort *rp = g->win->RPort;
    UWORD x = g->bor_l + 4;
    SetAPen(rp, 0);
    RectFill(rp, x, y, g->win_w - g->win->BorderRight - 1, y + g->fh - 1);
    SetAPen(rp, 1);
    SetDrMd(rp, JAM1);
    Move(rp, x, y + g->baseline);
    Text(rp, (CONST_STRPTR)s, strlen(s));
}

static void set_status(struct Gui *g, const char *s)
{
    strncpy(g->status, s, sizeof(g->status) - 1);
    draw_text_line(g, g->status_y, g->status);
}

static void draw_legend(struct Gui *g)
{
    struct RastPort *rp = g->win->RPort;
    UWORD x = g->bor_l + 4, y = g->legend_y;
    UBYTE c;
    SetAPen(rp, 0);
    RectFill(rp, x, y, g->win_w - g->win->BorderRight - 1, y + g->fh - 1);
    for (c = 0; c < BLK_CLASSES; c++) {
        const char *nm = FsMap_ClassName(c);
        UWORD w;
        SetAPen(rp, g->pen[c]);
        RectFill(rp, x, y + 1, x + g->fh - 3, y + g->fh - 2);
        SetAPen(rp, 1);
        Move(rp, x, y + g->fh - 1); Draw(rp, x + g->fh - 2, y + g->fh - 1); Draw(rp, x + g->fh - 2, y + 1);
        x += g->fh + 2;
        Move(rp, x, y + g->baseline);
        Text(rp, (CONST_STRPTR)nm, strlen(nm));
        w = TextLength(rp, (CONST_STRPTR)nm, strlen(nm));
        x += w + 12;
    }
}

static void draw_map_frame(struct Gui *g)
{
    struct RastPort *rp = g->win->RPort;
    struct TagItem tags[] = { { GT_VisualInfo, (ULONG)g->vi }, { GTBB_Recessed, TRUE }, { TAG_DONE, 0 } };
    DrawBevelBoxA(rp, g->map_x - 2, g->map_y - 2, g->map_w + 4, g->map_h + 4, tags);
    SetAPen(rp, 0);
    RectFill(rp, g->map_x, g->map_y, g->map_x + g->map_w - 1, g->map_y + g->map_h - 1);
}

static void draw_map(struct Gui *g)
{
    struct RastPort *rp = g->win->RPort;
    ULONG cells = (ULONG)g->cols * g->rows, per, i;
    if (!g->have_map || cells == 0) return;
    per = (g->map.nblocks + cells - 1) / cells;
    if (per == 0) per = 1;
    for (i = 0; i * per < g->map.nblocks; i++) {
        UBYTE c = FsMap_Cell(&g->map, i * per, per);
        UWORD cx = g->map_x + (i % g->cols) * PITCH;
        UWORD cy = g->map_y + (i / g->cols) * PITCH;
        SetAPen(rp, g->pen[c]);
        RectFill(rp, cx, cy, cx + CELL - 1, cy + CELL - 1);
    }
}

static void redraw_all(struct Gui *g)
{
    draw_map_frame(g);
    draw_map(g);
    draw_legend(g);
    draw_text_line(g, g->status_y, g->status);
}

static void draw_cells(struct Gui *g, ULONG first_cell, ULONG last_cell)
{
    struct RastPort *rp = g->win->RPort;
    ULONG cells = (ULONG)g->cols * g->rows, per, i;
    if (!g->have_map || cells == 0) return;
    per = (g->map.nblocks + cells - 1) / cells;
    if (per == 0) per = 1;
    if (last_cell >= cells) last_cell = cells - 1;
    for (i = first_cell; i <= last_cell && i * per < g->map.nblocks; i++) {
        UBYTE c = FsMap_Cell(&g->map, i * per, per);
        UWORD cx = g->map_x + (i % g->cols) * PITCH;
        UWORD cy = g->map_y + (i / g->cols) * PITCH;
        SetAPen(rp, g->pen[c]);
        RectFill(rp, cx, cy, cx + CELL - 1, cy + CELL - 1);
    }
}

/* Repaint the cells covering blocks [blk, blk+n). */
static void draw_blocks(struct Gui *g, ULONG blk, ULONG n)
{
    ULONG cells = (ULONG)g->cols * g->rows, per;
    if (!g->have_map || cells == 0 || n == 0) return;
    per = (g->map.nblocks + cells - 1) / cells;
    if (per == 0) per = 1;
    draw_cells(g, blk / per, (blk + n - 1) / per);
}

static void set_defrag_enabled(struct Gui *g, BOOL on)
{
    struct TagItem t[] = { { GA_Disabled, !on }, { TAG_DONE, 0 } };
    GT_SetGadgetAttrsA(g->defrag_gad, g->win, NULL, t);
}

/* The Stop button is live only while a scan or a defragmentation runs. */
static void set_stop_enabled(struct Gui *g, BOOL on)
{
    struct TagItem t[] = { { GA_Disabled, !on }, { TAG_DONE, 0 } };
    g->stop = FALSE;
    GT_SetGadgetAttrsA(g->stop_gad, g->win, NULL, t);
}

/* ------------------------------------------------------------------ */
/* volume list                                                         */
/* ------------------------------------------------------------------ */

static void init_list(struct List *l)
{
    l->lh_Head     = (struct Node *)&l->lh_Tail;
    l->lh_Tail     = NULL;
    l->lh_TailPred = (struct Node *)&l->lh_Head;
}

static void fill_labels(struct Gui *g)
{
    UWORD i;
    char fs[32], sz[24];
    init_list(&g->labels);
    Volume_EnumerateMounted(g->vols);
    for (i = 0; i < g->vols->count; i++) {
        struct VolumeInfo *v = &g->vols->v[i];
        Volume_FsName(v->dostype, fs, sizeof(fs));
        Volume_SizeString(v->num_blocks, v->sector_size, sz, sizeof(sz));
        /* DH0  Work  FFS (DOS\3)  120 MB  scsi.device:0  - the device tells a
         * real partition from a directory mount or a CD at a glance. */
        AD_SNPRINTF(g->texts[i], "%-5s %-14s %-13s %8s  %s:%lu", v->dosname,
                    v->volname[0] ? v->volname : "-", fs, sz,
                    v->devname, (unsigned long)v->unit);
        g->nodes[i].ln_Name = g->texts[i];
        g->nodes[i].ln_Pri  = 0;
        AddTail(&g->labels, &g->nodes[i]);
    }
}

static void rescan(struct Gui *g)
{
    struct TagItem detach[] = { { GTLV_Labels, (ULONG)~0UL }, { TAG_DONE, 0 } };
    GT_SetGadgetAttrsA(g->lv_gad, g->win, NULL, detach);
    fill_labels(g);
    g->selected = g->vols->count ? 0 : -1;
    {
        struct TagItem attach[] = { { GTLV_Labels, (ULONG)&g->labels },
                                    { GTLV_Selected, (ULONG)(g->selected < 0 ? ~0UL : (ULONG)g->selected) },
                                    { TAG_DONE, 0 } };
        GT_SetGadgetAttrsA(g->lv_gad, g->win, NULL, attach);
    }
}

/* ------------------------------------------------------------------ */
/* scanning                                                            */
/* ------------------------------------------------------------------ */

static void handle_msg(struct Gui *g, ULONG iclass, UWORD code, struct Gadget *gad);

static BOOL scan_progress(void *ud, ULONG done, ULONG total, const char *what)
{
    struct Gui *g = (struct Gui *)ud;
    struct IntuiMessage *imsg;
    ULONG pct = total ? (done >= total ? 100 : done * 100 / total) : 0xFFFF;

    while ((imsg = GT_GetIMsg(g->win->UserPort)) != NULL) {
        ULONG  iclass = imsg->Class;
        UWORD  code   = imsg->Code;
        struct Gadget *gad = (struct Gadget *)imsg->IAddress;
        GT_ReplyIMsg(imsg);
        if (iclass == IDCMP_CLOSEWINDOW) g->quit = TRUE;
        else if (iclass == IDCMP_REFRESHWINDOW) { GT_BeginRefresh(g->win); redraw_all(g); GT_EndRefresh(g->win, TRUE); }
        else if (iclass == IDCMP_GADGETUP && gad->GadgetID == GID_LIST) g->selected = (WORD)code;
        else if (iclass == IDCMP_GADGETUP && gad->GadgetID == GID_STOP) g->stop = TRUE;
    }
    if (g->quit || g->stop) return FALSE;
    if (pct != g->last_pct) {
        char s[96];
        g->last_pct = pct;
        if (pct == 0xFFFF) AD_SNPRINTF(s, "%s... %lu", what, (unsigned long)done);
        else               AD_SNPRINTF(s, "%s... %lu%%", what, (unsigned long)pct);
        set_status(g, s);
    }
    return TRUE;
}

static void summarize(struct Gui *g)
{
    const struct FsMap *m = &g->map;
    char s[300], sz[24];
    ULONG p = m->files ? m->frag_files * 1000 / m->files : 0;
    Volume_SizeString(m->count[BLK_FREE], m->blocksize, sz, sizeof(sz));
    AD_SNPRINTF(s, "%s: %lu files, %lu fragmented (%lu.%lu%%), %lu extra pieces, %s free%s%s",
                m->volname, (unsigned long)m->files, (unsigned long)m->frag_files,
                (unsigned long)(p / 10), (unsigned long)(p % 10),
                (unsigned long)m->extra_runs, sz,
                m->note[0] ? " - " : "", m->note);
    set_status(g, s);
}

static void analyze(struct Gui *g)
{
    struct VolumeInfo *v;
    struct BlockDev *bd;
    struct TagItem dis[] = { { GA_Disabled, TRUE }, { TAG_DONE, 0 } };
    struct TagItem ena[] = { { GA_Disabled, FALSE }, { TAG_DONE, 0 } };

    if (g->selected < 0 || g->selected >= (WORD)g->vols->count) { set_status(g, "Select a volume first."); return; }
    v = &g->vols->v[g->selected];
    if (v->fskind == FS_UNKNOWN) { set_status(g, "Unsupported filesystem on this partition."); return; }

    if (g->have_map) { FsMap_Free(&g->map); g->have_map = FALSE; }
    set_defrag_enabled(g, FALSE);
    draw_map_frame(g);

    bd = BlockDev_Open(v->devname, v->unit);
    if (!bd) {
        char s[120];
        AD_SNPRINTF(s, "Cannot open %s unit %lu", v->devname, (unsigned long)v->unit);
        set_status(g, s);
        return;
    }
    GT_SetGadgetAttrsA(g->analyze_gad, g->win, NULL, dis);
    set_stop_enabled(g, TRUE);
    g->scanning = TRUE;
    g->last_pct = 0xFFFFFFFFUL;
    if (FsMap_Build(bd, v, &g->map, scan_progress, g)) {
        g->have_map = TRUE;
        g->map_vol  = g->selected;
        draw_map(g);
        summarize(g);
        set_defrag_enabled(g, g->map.frag_files > 0 &&
                              ((v->fskind == FS_FFS && !v->is_boot) || SfsDefrag_Available(v)));
    } else {
        char s[160];
        AD_SNPRINTF(s, "%s: %s", v->dosname, g->map.note);
        set_status(g, s);
    }
    g->scanning = FALSE;
    set_stop_enabled(g, FALSE);
    GT_SetGadgetAttrsA(g->analyze_gad, g->win, NULL, ena);
    BlockDev_Close(bd);
}

/* ------------------------------------------------------------------ */
/* defragment                                                          */
/* ------------------------------------------------------------------ */

static void defrag_log(void *ud, const char *line)
{
    set_status((struct Gui *)ud, line);
}

/* Drain the window's messages during an engine run.  Returns FALSE when
 * the run must stop (Stop button or close gadget); the engines stop
 * between files / steps, so the item in flight is committed first. */
static BOOL drain_run_msgs(struct Gui *g, const char *stop_text)
{
    struct IntuiMessage *imsg;
    while ((imsg = GT_GetIMsg(g->win->UserPort)) != NULL) {
        ULONG iclass = imsg->Class;
        struct Gadget *gad = (struct Gadget *)imsg->IAddress;
        GT_ReplyIMsg(imsg);
        if (iclass == IDCMP_CLOSEWINDOW) g->quit = TRUE;
        else if (iclass == IDCMP_REFRESHWINDOW) { GT_BeginRefresh(g->win); redraw_all(g); GT_EndRefresh(g->win, TRUE); }
        else if (iclass == IDCMP_GADGETUP && gad->GadgetID == GID_STOP && !g->stop) {
            g->stop = TRUE;
            set_status(g, stop_text);
        }
    }
    return !g->quit && !g->stop;
}

/* FFS engine callback: a block range changed class (cls 0xFF = idle poll). */
static BOOL defrag_move(void *ud, ULONG blk, ULONG n, UBYTE cls)
{
    struct Gui *g = (struct Gui *)ud;
    if (cls != 0xFF && g->have_map) {
        FsMap_SetRange(&g->map, blk, n, cls);
        draw_blocks(g, blk, n);
    }
    return drain_run_msgs(g, "Stopping after the current file...");
}

/* Seconds since 1978 from a DateStamp, for elapsed-time display. */
static ULONG now_seconds(void)
{
    struct DateStamp ds;
    DateStamp(&ds);
    return (ULONG)ds.ds_Days * 86400UL + (ULONG)ds.ds_Minute * 60UL + (ULONG)ds.ds_Tick / 50UL;
}

/* SFS engine callback: the handler moved n blocks src -> dst (n == 0 is
 * the between-steps poll).  Whether the destination is now contiguous
 * is not known here; the re-analysis after the run colours it right. */
struct SfsRunUi { struct Gui *g; ULONG t0, last_secs, moved; };

/* The engine hands the same user pointer to the log and move callbacks,
 * and in the SFS run that pointer is the SfsRunUi, not the Gui. */
static void sfs_log(void *ud, const char *line)
{
    set_status(((struct SfsRunUi *)ud)->g, line);
}

static BOOL sfs_move(void *ud, ULONG src, ULONG dst, ULONG n)
{
    struct SfsRunUi *u = (struct SfsRunUi *)ud;
    struct Gui *g = u->g;
    ULONG secs;
    if (n && g->have_map) {
        FsMap_SetRange(&g->map, src, n, BLK_FREE); draw_blocks(g, src, n);
        FsMap_SetRange(&g->map, dst, n, BLK_DATA); draw_blocks(g, dst, n);
        u->moved += n;
    }
    secs = now_seconds() - u->t0;
    if (secs != u->last_secs) {
        char s[160], mv[24];
        u->last_secs = secs;
        Volume_SizeString(u->moved, g->map.blocksize, mv, sizeof(mv));
        AD_SNPRINTF(s, "%s: handler defragmenting, %s moved, %lu min %lu s%s", g->map.volname, mv,
                    (unsigned long)(secs / 60), (unsigned long)(secs % 60),
                    g->stop ? " - stopping after this step" : "");
        set_status(g, s);
    }
    return drain_run_msgs(g, "Stopping after the current step...");
}

static LONG ask(struct Gui *g, const char *title, const char *body, const char *gadgets)
{
    struct EasyStruct es;
    es.es_StructSize   = sizeof(es);
    es.es_Flags        = 0;
    es.es_Title        = (UBYTE *)title;
    es.es_TextFormat   = (UBYTE *)body;
    es.es_GadgetFormat = (UBYTE *)gadgets;
    return EasyRequestArgs(g->win, &es, NULL, NULL);
}

static void show_about(struct Gui *g)
{
    char body[600];
    static char title[64];
    AD_SNPRINTF(body,
        "AmiDefrag %s\nBuilt: %s\n"
        "Disk defragmenter for AmigaOS with the classic block map.\n\n"
        "Files are moved one at a time with a journaled, single-write\n"
        "commit per file, so the volume stays valid at every moment.\n"
        "FFS/OFS and SFS defragmentation; PFS3 map only.\n\n"
        "A native GadTools application, no dependencies beyond the ROM.\n\n"
        "Director: John Hertell\n"
        "Code: Claude Code (Anthropic)\n\n"
        "https://github.com/ChuckyGang/AmiDefrag\n\n"
        "MIT License \251 2026 John Hertell",
        AMIDEFRAG_VERSION, AmiDefrag_BuildStamp);
    AD_SNPRINTF(title, "About %s", AMIDEFRAG_VERTITLE);
    {
        struct EasyStruct es;
        es.es_StructSize   = sizeof(es);
        es.es_Flags        = 0;
        es.es_Title        = (UBYTE *)title;
        es.es_TextFormat   = (UBYTE *)body;
        es.es_GadgetFormat = (UBYTE *)"OK";
        EasyRequestArgs(g->win, &es, NULL, NULL);
    }
}

static void analyze(struct Gui *g);

/* SFS: drive the handler's own defragmenter, follow it on the map, then
 * analyze again for the real after-numbers. */
static void defragment_sfs(struct Gui *g, struct VolumeInfo *v)
{
    struct SfsDefragStats st;
    struct SfsRunUi u;
    char err[160], body[900], volname[32], moved[24];
    ULONG frag0 = g->map.frag_files, runs0 = g->map.extra_runs, files0 = g->map.files, secs;
    LONG rc;

    if (!SfsDefrag_Available(v)) { set_status(g, "SFS is defragmented through its handler: the volume must be mounted."); return; }
    if (!SfsDefrag_Probe(v, &st, err, sizeof(err))) { set_status(g, err); ask(g, "AmiDefrag", err, "OK"); return; }
    strncpy(volname, g->map.volname, sizeof(volname) - 1); volname[sizeof(volname) - 1] = '\0';
    AD_SNPRINTF(body,
        "EXPERIMENTAL SOFTWARE - UNDER DEVELOPMENT\n\n"
        "AmiDefrag will drive the defragmenter built into the\n"
        "SmartFilesystem handler of %s (%s:). The handler moves the\n"
        "blocks itself, one transaction-safe step at a time, and the\n"
        "volume stays mounted and usable meanwhile. It also packs files\n"
        "together to consolidate free space, so it moves more than the\n"
        "fragmented files and can take a long time; Stop ends the run\n"
        "after the current step.\n\n"
        "Handler: SmartFilesystem %lu.%lu, stack %lu bytes\n"
        "%lu files, %lu fragmented, %lu extra pieces.\n\n"
        "Keep a FULL BACKUP anyway. The author accepts NO responsibility\n"
        "for any loss of data caused by this software.",
        volname, v->dosname,
        (unsigned long)(st.version >> 16), (unsigned long)(st.version & 0xFFFF), (unsigned long)st.stack,
        (unsigned long)files0, (unsigned long)frag0, (unsigned long)runs0);
    if (ask(g, "AmiDefrag - Warning", body, "I have a backup - Defragment|Cancel") != 1) {
        set_status(g, "Cancelled.");
        return;
    }

    set_defrag_enabled(g, FALSE);
    g->scanning = TRUE;
    set_stop_enabled(g, TRUE);
    u.g = g; u.t0 = now_seconds(); u.last_secs = 0xFFFFFFFFUL; u.moved = 0;
    rc = SfsDefrag_Run(v, g->map.nblocks, g->map.blocksize, sfs_log, sfs_move, &u, &st, err, sizeof(err));
    secs = now_seconds() - u.t0;
    g->scanning = FALSE;
    set_stop_enabled(g, FALSE);
    if (g->quit) return;

    if (rc == RETURN_ERROR || rc == RETURN_FAIL) {
        set_status(g, err);
        ask(g, "AmiDefrag", err, "OK");
        set_defrag_enabled(g, TRUE);
        return;
    }

    /* fresh analysis: colours and counts the handler's result properly */
    Volume_SizeString(st.moved_blocks, g->map.blocksize, moved, sizeof(moved));
    g->selected = g->map_vol;
    {
        struct TagItem t[] = { { GTLV_Selected, (ULONG)g->selected }, { TAG_DONE, 0 } };
        GT_SetGadgetAttrsA(g->lv_gad, g->win, NULL, t);
    }
    analyze(g);
    if (!g->have_map) return;
    AD_SNPRINTF(body,
        "Defragmentation of %s %s.\n\n"
        "Moved by the handler: %s (%lu blocks) in %lu steps\n"
        "Time: %lu min %lu s\n\n"
        "Fragmented files: %lu -> %lu\n"
        "Extra pieces: %lu -> %lu%s",
        volname, st.aborted ? "stopped at your request" : "finished",
        moved, (unsigned long)st.moved_blocks, (unsigned long)st.steps,
        (unsigned long)(secs / 60), (unsigned long)(secs % 60),
        (unsigned long)frag0, (unsigned long)g->map.frag_files,
        (unsigned long)runs0, (unsigned long)g->map.extra_runs,
        st.aborted ? "\nThe volume is consistent; run again to continue." : "");
    ask(g, "AmiDefrag", body, "OK");
}

static void defragment(struct Gui *g)
{
    struct VolumeInfo *v;
    struct BlockDev *bd;
    struct FfsDefragOpts  o;
    struct FfsDefragStats st;
    char err[128], body[900];
    LONG rc;

    if (!g->have_map || g->map_vol < 0 || g->map_vol >= (WORD)g->vols->count) return;
    v = &g->vols->v[g->map_vol];
    if (v->fskind == FS_SFS) { defragment_sfs(g, v); return; }
    if (v->fskind != FS_FFS) { set_status(g, "Only OFS/FFS and SFS volumes can be defragmented in this version."); return; }
    if (v->is_boot) { set_status(g, "The boot volume is not defragmented in this version."); return; }

    bd = BlockDev_Open(v->devname, v->unit);
    if (!bd) { set_status(g, "Cannot open the device."); return; }

    /* dry run first: tells the user what will happen before anything is written */
    memset(&o, 0, sizeof(o));
    o.dry_run = TRUE;
    set_defrag_enabled(g, FALSE);
    rc = FfsDefrag_Run(bd, v, &o, FfsDefrag_DefaultPlatform(), defrag_log, NULL, g, &st, err, sizeof(err));
    if (rc == RETURN_ERROR || st.candidates == 0) {
        AD_SNPRINTF(body, "%s", err[0] ? err : "Nothing to defragment.");
        set_status(g, body);
        BlockDev_Close(bd);
        set_defrag_enabled(g, TRUE);
        return;
    }
    AD_SNPRINTF(body,
        "EXPERIMENTAL SOFTWARE - UNDER DEVELOPMENT\n\n"
        "AmiDefrag rewrites file data and filesystem structures\n"
        "on %s (%s:). A bug here can cause permanent data loss.\n"
        "Make sure you have a FULL BACKUP of this volume before\n"
        "proceeding. The author accepts NO responsibility for any\n"
        "loss of data caused by this software.\n\n"
        "%lu fragmented file(s), %lu blocks.\n"
        "%lu can be made contiguous, %lu have no free run large enough.\n\n"
        "The volume is inhibited while this runs: nothing else must\n"
        "write to it. Files are moved one at a time; a crash leaves the\n"
        "volume valid (the filesystem may validate it on the next mount).",
        g->map.volname, v->dosname, (unsigned long)st.candidates, (unsigned long)st.cand_blocks,
        (unsigned long)st.moved, (unsigned long)st.skipped_space);
    if (ask(g, "AmiDefrag - Warning", body, "I have a backup - Defragment|Cancel") != 1) {
        set_status(g, "Cancelled.");
        BlockDev_Close(bd);
        set_defrag_enabled(g, TRUE);
        return;
    }

    o.dry_run = FALSE;
    g->scanning = TRUE;
    set_stop_enabled(g, TRUE);
    {
        ULONG t0 = now_seconds(), secs;
        rc = FfsDefrag_Run(bd, v, &o, FfsDefrag_DefaultPlatform(), defrag_log, defrag_move, g, &st, err, sizeof(err));
        secs = now_seconds() - t0;
        g->scanning = FALSE;
        set_stop_enabled(g, FALSE);
        BlockDev_Close(bd);
        if (rc != RETURN_FAIL && !st.needs_validation && !g->quit) {
            char moved[24];
            Volume_SizeString(st.moved_blocks, g->map.blocksize, moved, sizeof(moved));
            AD_SNPRINTF(body,
                "Defragmentation of %s finished.\n\n"
                "Files made contiguous: %lu of %lu\n"
                "Data moved: %s (%lu blocks)\n"
                "Time: %lu min %lu s\n"
                "Skipped, no free run large enough: %lu\n%s",
                g->map.volname, (unsigned long)st.moved, (unsigned long)st.candidates,
                moved, (unsigned long)st.moved_blocks,
                (unsigned long)(secs / 60), (unsigned long)(secs % 60),
                (unsigned long)st.skipped_space,
                st.aborted ? "\nStopped at your request; the volume is consistent." : "");
            ask(g, "AmiDefrag", body, "OK");
        }
    }

    if (g->have_map) { FsMap_Recount(&g->map); g->map.frag_files = st.candidates - st.moved; draw_map(g); }
    AD_SNPRINTF(body, "%s: %lu of %lu file(s) made contiguous (%lu blocks), %lu skipped for space%s%s%s",
        g->map.volname, (unsigned long)st.moved, (unsigned long)st.candidates,
        (unsigned long)st.moved_blocks, (unsigned long)st.skipped_space,
        st.needs_validation ? " - VOLUME NEEDS VALIDATION" : "",
        err[0] ? " - " : "", err);
    set_status(g, body);
    if (rc == RETURN_FAIL || st.needs_validation)
        ask(g, "AmiDefrag", "The run stopped after writes began.\nThe bitmap was left marked invalid on purpose:\nthe filesystem will validate the volume on the next mount.\nRun AmiDefrag again afterwards to finish.", "OK");
    set_defrag_enabled(g, !st.needs_validation && (st.candidates - st.moved) > 0);
}

/* ------------------------------------------------------------------ */
/* window                                                              */
/* ------------------------------------------------------------------ */

static void handle_msg(struct Gui *g, ULONG iclass, UWORD code, struct Gadget *gad)
{
    switch (iclass) {
    case IDCMP_CLOSEWINDOW:
        g->quit = TRUE;
        break;
    case IDCMP_REFRESHWINDOW:
        GT_BeginRefresh(g->win);
        redraw_all(g);
        GT_EndRefresh(g->win, TRUE);
        break;
    case IDCMP_GADGETUP:
        switch (gad->GadgetID) {
        case GID_LIST:    g->selected = (WORD)code; break;
        case GID_ANALYZE: analyze(g); break;
        case GID_RESCAN:  rescan(g); set_status(g, "Volume list refreshed."); break;
        case GID_DEFRAG:  if (!g->scanning) defragment(g); break;
        case GID_STOP:    break;   /* only meaningful inside a run; handled by the callbacks */
        case GID_ABOUT:   show_about(g); break;
        }
        break;
    }
}

static BOOL build_window(struct Gui *g)
{
    struct Gadget *gctx, *prev;
    struct NewGadget ng;
    UWORD pad = 6, btn_h, btn_w, lv_h, inner_w, y, avail_h, bor_r, bor_b;

    g->scr = LockPubScreen(NULL);
    if (!g->scr) return FALSE;
    g->vi = GetVisualInfoA(g->scr, NULL);
    if (!g->vi) return FALSE;

    g->fh       = g->scr->Font->ta_YSize;
    g->baseline = g->scr->RastPort.Font ? g->scr->RastPort.Font->tf_Baseline : g->fh - 2;
    g->bor_l    = g->scr->WBorLeft;
    g->bor_t    = (UWORD)(g->scr->WBorTop + g->fh + 1);
    bor_r       = g->scr->WBorRight;
    bor_b       = g->scr->WBorBottom;

    g->win_w = g->scr->Width  < 640 ? g->scr->Width : 640;
    g->win_h = g->scr->Height < 480 ? g->scr->Height - 2 : 480;
    inner_w  = g->win_w - g->bor_l - bor_r;
    btn_h    = g->fh + 6;
    btn_w    = (inner_w - pad * 6) / 5;
    lv_h     = 4 * (g->fh + 1) + 4;

    obtain_pens(g);

    if (!CreateContext(&g->glist)) return FALSE;
    gctx = g->glist;
    memset(&ng, 0, sizeof(ng));
    ng.ng_VisualInfo = g->vi;
    ng.ng_TextAttr   = g->scr->Font;

    y = g->bor_t + pad;
    ng.ng_LeftEdge = g->bor_l + pad; ng.ng_TopEdge = y;
    ng.ng_Width = inner_w - pad * 2;  ng.ng_Height = lv_h;
    ng.ng_GadgetID = GID_LIST;
    {
        struct TagItem t[] = { { GTLV_Labels, (ULONG)&g->labels },
                               { GTLV_Selected, (ULONG)(g->selected < 0 ? ~0UL : (ULONG)g->selected) },
                               { GTLV_ShowSelected, 0 }, { TAG_DONE, 0 } };
        prev = g->lv_gad = CreateGadgetA(LISTVIEW_KIND, gctx, &ng, t);
        if (!prev) return FALSE;
    }
    y += lv_h + pad;

    ng.ng_TopEdge = y; ng.ng_Height = btn_h; ng.ng_Width = btn_w;
    ng.ng_LeftEdge = g->bor_l + pad;            ng.ng_GadgetText = "_Analyze";    ng.ng_GadgetID = GID_ANALYZE;
    { struct TagItem t[] = { { GT_Underscore, '_' }, { TAG_DONE, 0 } };
      prev = g->analyze_gad = CreateGadgetA(BUTTON_KIND, prev, &ng, t); if (!prev) return FALSE; }
    ng.ng_LeftEdge += btn_w + pad;              ng.ng_GadgetText = "_Rescan";     ng.ng_GadgetID = GID_RESCAN;
    { struct TagItem t[] = { { GT_Underscore, '_' }, { TAG_DONE, 0 } };
      prev = CreateGadgetA(BUTTON_KIND, prev, &ng, t); if (!prev) return FALSE; }
    ng.ng_LeftEdge += btn_w + pad;              ng.ng_GadgetText = "_Defragment"; ng.ng_GadgetID = GID_DEFRAG;
    { struct TagItem t[] = { { GA_Disabled, TRUE }, { GT_Underscore, '_' }, { TAG_DONE, 0 } };
      prev = g->defrag_gad = CreateGadgetA(BUTTON_KIND, prev, &ng, t); if (!prev) return FALSE; }
    ng.ng_LeftEdge += btn_w + pad;              ng.ng_GadgetText = "_Stop";       ng.ng_GadgetID = GID_STOP;
    { struct TagItem t[] = { { GA_Disabled, TRUE }, { GT_Underscore, '_' }, { TAG_DONE, 0 } };
      prev = g->stop_gad = CreateGadgetA(BUTTON_KIND, prev, &ng, t); if (!prev) return FALSE; }
    ng.ng_LeftEdge += btn_w + pad;              ng.ng_GadgetText = "A_bout";      ng.ng_GadgetID = GID_ABOUT;
    { struct TagItem t[] = { { GT_Underscore, '_' }, { TAG_DONE, 0 } };
      prev = CreateGadgetA(BUTTON_KIND, prev, &ng, t); if (!prev) return FALSE; }
    y += btn_h + pad;

    /* map takes what is left above legend + status */
    g->status_y = g->win_h - bor_b - pad - g->fh;
    g->legend_y = g->status_y - g->fh - 4;
    avail_h     = g->legend_y - pad - 2 - (y + 2);
    g->map_x    = g->bor_l + pad + 2;
    g->map_y    = y + 2;
    g->cols     = (inner_w - pad * 2 - 4) / PITCH;
    g->rows     = avail_h / PITCH;
    g->map_w    = g->cols * PITCH - 1;
    g->map_h    = g->rows * PITCH - 1;

    {
        struct TagItem wt[] = {
            { WA_Left,      (ULONG)((g->scr->Width  - g->win_w) / 2) },
            { WA_Top,       (ULONG)((g->scr->Height - g->win_h) / 2) },
            { WA_Width,     g->win_w },
            { WA_Height,    g->win_h },
            { WA_Title,     (ULONG)AMIDEFRAG_VERTITLE },
            { WA_Gadgets,   (ULONG)g->glist },
            { WA_PubScreen, (ULONG)g->scr },
            { WA_IDCMP,     IDCMP_CLOSEWINDOW | IDCMP_GADGETUP | IDCMP_REFRESHWINDOW |
                            LISTVIEWIDCMP | BUTTONIDCMP },
            { WA_Flags,     WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_CLOSEGADGET |
                            WFLG_ACTIVATE | WFLG_SMART_REFRESH },
            { TAG_DONE, 0 }
        };
        g->win = OpenWindowTagList(NULL, wt);
    }
    if (!g->win) return FALSE;
    GT_RefreshWindow(g->win, NULL);
    return TRUE;
}

int gui_run(void)
{
    struct Gui *g = (struct Gui *)AllocVec(sizeof(*g), MEMF_PUBLIC | MEMF_CLEAR);
    int rc = 0;

    if (!g) return 20;
    g->vols = (struct VolumeList *)AllocVec(sizeof(*g->vols), MEMF_PUBLIC | MEMF_CLEAR);
    if (!g->vols) { FreeVec(g); return 20; }

    fill_labels(g);
    g->selected = g->vols->count ? 0 : -1;

    if (!build_window(g)) { rc = 20; goto done; }

    redraw_all(g);
    set_status(g, g->vols->count ? "Select a volume and press Analyze."
                                 : "No partitions found in the DOS device list.");

    while (!g->quit) {
        struct IntuiMessage *imsg;
        WaitPort(g->win->UserPort);
        while ((imsg = GT_GetIMsg(g->win->UserPort)) != NULL) {
            ULONG  iclass = imsg->Class;
            UWORD  code   = imsg->Code;
            struct Gadget *gad = (struct Gadget *)imsg->IAddress;
            GT_ReplyIMsg(imsg);
            handle_msg(g, iclass, code, gad);
            if (g->quit) break;
        }
    }

done:
    if (g->have_map) FsMap_Free(&g->map);
    if (g->win)   CloseWindow(g->win);
    if (g->glist) FreeGadgets(g->glist);
    if (g->scr)   release_pens(g);
    if (g->vi)    FreeVisualInfo(g->vi);
    if (g->scr)   UnlockPubScreen(NULL, g->scr);
    FreeVec(g->vols);
    FreeVec(g);
    return rc;
}
