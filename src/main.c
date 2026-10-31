/*
 * main.c - AmiDefrag entry point.
 *
 * CLI launch with arguments -> command-line mode (cli.c, no GUI libraries
 * needed).  CLI launch without arguments, or a Workbench launch -> the
 * GadTools window (gui.c).
 *
 * AmigaOS 2.x+ (Kickstart v37+), m68k-amiga-elf-gcc (Bartman toolchain,
 * -nostdlib: support/gcc8_c_support.c provides _start and the libc bits).
 */
#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <exec/tasks.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <intuition/intuition.h>
#include <graphics/gfxbase.h>
#include <libraries/gadtools.h>
#include <workbench/startup.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/gadtools.h>

#include "clib.h"
#include "cli.h"
#include "gui.h"
#include "version.h"

/* "Version AmiDefrag" string, kept alive through a volatile read below. */
const char amidefrag_ver[] = AMIDEFRAG_VERSTRING;

struct ExecBase      *SysBase;
struct DosLibrary    *DOSBase       = NULL;
struct IntuitionBase *IntuitionBase = NULL;
struct GfxBase       *GfxBase       = NULL;
struct Library       *GadToolsBase  = NULL;

/* Set by Bartman's _start on a Workbench launch (support/gcc8_c_support.c). */
struct WBStartup *AmiDefrag_WBStartup = NULL;

/* Set by cli.c when DEBUG is given: the stack high-water mark is printed. */
BOOL AmiDefrag_Debug = FALSE;

/* AmiDefrag runs on its own stack: the icon gives 8 KB, a Shell often 4 KB,
 * and the engines run dos packets, drawing and text formatting on top of
 * each other.  The GUI SFS run froze the machine on the icon's 8 KB while
 * the same run from a 200 KB Shell was fine (2026-10-31). */
#define OWN_STACK_SIZE (64UL * 1024UL)
#define STACK_FILL     0xA5

static int amidefrag_main(void);

/* support/gcc8_a_support.s: swap to the given stack, call fn, swap back.
 * Must not be done from C: with -fomit-frame-pointer the second
 * StackSwap(&sss) would address sss relative to the NEW stack pointer. */
extern LONG RunOnStack(struct StackSwapStruct *sss, LONG (*fn)(void));
static LONG run_main(void) { return amidefrag_main(); }

static ULONG stack_used(const UBYTE *lower, ULONG size)
{
    ULONG i;
    for (i = 0; i < size; i++)
        if (lower[i] != STACK_FILL) break;
    return size - i;
}

int main(void)
{
    struct StackSwapStruct sss;
    UBYTE *stk;
    int rc;

    SysBase = *((struct ExecBase **)4UL);
    stk = (UBYTE *)AllocMem(OWN_STACK_SIZE, MEMF_PUBLIC);
    if (!stk) return amidefrag_main();
    memset(stk, STACK_FILL, OWN_STACK_SIZE);
    sss.stk_Lower   = stk;
    sss.stk_Upper   = (ULONG)(stk + OWN_STACK_SIZE);
    sss.stk_Pointer = (APTR)sss.stk_Upper;
    rc = (int)RunOnStack(&sss, run_main);
    if (AmiDefrag_Debug) {
        struct DosLibrary *dl = (struct DosLibrary *)OpenLibrary("dos.library", 37);
        if (dl) {
            struct DosLibrary *saved = DOSBase;
            DOSBase = dl;
            Printf("Stack used: %lu of %lu bytes\n", stack_used(stk, OWN_STACK_SIZE), OWN_STACK_SIZE);
            DOSBase = saved;
            CloseLibrary((struct Library *)dl);
        }
    }
    FreeMem(stk, OWN_STACK_SIZE);
    return rc;
}

static int amidefrag_main(void)
{
    int result = 0;
    { const char *volatile keep = amidefrag_ver; (void)*keep; }

    DOSBase = (struct DosLibrary *)OpenLibrary("dos.library", 37);
    if (!DOSBase) goto cleanup;

    {
        struct Process *proc = (struct Process *)FindTask(NULL);
        if (proc->pr_CLI) {
            LONG rc = cli_run();
            if (rc != CLI_NO_ARGS) { result = (int)rc; goto cleanup; }
        }
    }

    IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 37);
    GfxBase       = (struct GfxBase *)OpenLibrary("graphics.library", 37);
    GadToolsBase  = OpenLibrary("gadtools.library", 37);
    if (!IntuitionBase || !GfxBase || !GadToolsBase) { result = 20; goto cleanup; }

    result = gui_run();

cleanup:
    if (GadToolsBase)  CloseLibrary(GadToolsBase);
    if (GfxBase)       CloseLibrary((struct Library *)GfxBase);
    if (IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
    if (DOSBase)       CloseLibrary((struct Library *)DOSBase);
    return result;
}
