/* host_main.c - Linux AmiDefrag entry point (image-file mode). */
#include <stdio.h>
#include "amiga_compat.h"
#include "cli.h"

BOOL  AmiDefrag_Debug;   /* set by cli.c DEBUG; the stack readout is Amiga-only */
int   host_argc;
char **host_argv;

int main(int argc, char **argv)
{
    LONG rc;
    host_argc = argc;
    host_argv = argv;
    SetSignal(0, 0);          /* arm the Ctrl-C handler before any long run */
    rc = cli_run();
    if (rc == CLI_NO_ARGS) {
        printf("AmiDefrag (host build, image-file mode)\n"
               "Usage: amidefrag IMAGE=<file> LIST\n"
               "       amidefrag IMAGE=<file> [PART=<name|index>] [MAP] [WIDTH=<n>]\n"
               "       amidefrag IMAGE=<file> [PART=<name|index>] DRYRUN | DEFRAG [FORCE]\n"
               "       amidefrag ?\n"
               "DEFRAG on an image needs a 2 KB file named AmiDefrag.journal in its root\n"
               "directory (e.g. xdftool img write journal.bin AmiDefrag.journal).\n");
        return 5;
    }
    return (int)rc;
}
