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

int main(void)
{
    int result = 0;
    { const char *volatile keep = amidefrag_ver; (void)*keep; }

    SysBase = *((struct ExecBase **)4UL);

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
