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
#include "gui.h"
#include "version.h"

extern struct GfxBase *GfxBase;

enum { GID_LIST = 1, GID_ANALYZE, GID_RESCAN, GID_DEFRAG };

#define CELL   5          /* square size in pixels */
#define PITCH  6          /* square + gap */

struct Gui {
    struct Screen  *scr;
    APTR            vi;
    struct Window  *win;
    struct Gadget  *glist, *lv_gad, *analyze_gad, *defrag_gad;
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
    BOOL  quit;
    BOOL  scanning;
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
        { 0x88888888, 0x88888888, 0x88888888 },   /* reserved: grey   */
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
        AD_SNPRINTF(g->texts[i], "%-5s %-16s %-14s %9s  %s", v->dosname,
                    v->volname[0] ? v->volname : "-", fs, sz,
                    v->fskind == FS_UNKNOWN ? "(unsupported)" : "");
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
    }
    if (g->quit) return FALSE;
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
    draw_map_frame(g);

    bd = BlockDev_Open(v->devname, v->unit);
    if (!bd) {
        char s[120];
        AD_SNPRINTF(s, "Cannot open %s unit %lu", v->devname, (unsigned long)v->unit);
        set_status(g, s);
        return;
    }
    GT_SetGadgetAttrsA(g->analyze_gad, g->win, NULL, dis);
    g->scanning = TRUE;
    g->last_pct = 0xFFFFFFFFUL;
    if (FsMap_Build(bd, v, &g->map, scan_progress, g)) {
        g->have_map = TRUE;
        draw_map(g);
        summarize(g);
    } else {
        char s[160];
        AD_SNPRINTF(s, "%s: %s", v->dosname, g->map.note);
        set_status(g, s);
    }
    g->scanning = FALSE;
    GT_SetGadgetAttrsA(g->analyze_gad, g->win, NULL, ena);
    BlockDev_Close(bd);
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
        case GID_DEFRAG:  break;
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
    btn_w    = (inner_w - pad * 4) / 3;
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
    ng.ng_LeftEdge += btn_w + pad;              ng.ng_GadgetText = "Defragment"; ng.ng_GadgetID = GID_DEFRAG;
    { struct TagItem t[] = { { GA_Disabled, TRUE }, { TAG_DONE, 0 } };
      prev = g->defrag_gad = CreateGadgetA(BUTTON_KIND, prev, &ng, t); if (!prev) return FALSE; }
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
            { WA_Title,     (ULONG)AMIDEFRAG_VERTITLE " - read-only preview" },
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
    set_status(g, g->vols->count ? "Select a volume and press Analyze. Nothing is written in this version."
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
