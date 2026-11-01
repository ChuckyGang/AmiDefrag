/* host_main.c - Linux AmiDefrag entry point (image-file mode). */
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "amiga_compat.h"
#include "cli.h"

BOOL  AmiDefrag_Debug;   /* set by cli.c DEBUG; the stack readout is Amiga-only */
int   host_argc;
char **host_argv;

void HostPlatform_SetImage(const char *path);

int main(int argc, char **argv)
{
    LONG rc;
    int i;
    host_argc = argc;
    host_argv = argv;
    for (i = 1; i < argc; i++)
        if (strncasecmp(argv[i], "IMAGE=", 6) == 0) HostPlatform_SetImage(argv[i] + 6);
    SetSignal(0, 0);          /* arm the Ctrl-C handler before any long run */
    rc = cli_run();
    if (rc == CLI_NO_ARGS) {
        printf("AmiDefrag (host build, image-file mode)\n"
               "Usage: amidefrag IMAGE=<file> LIST\n"
               "       amidefrag IMAGE=<file> [PART=<name|index>] [MAP] [WIDTH=<n>]\n"
               "       amidefrag IMAGE=<file> [PART=<name|index>] DRYRUN | DEFRAG [FORCE]\n"
               "       amidefrag ?\n"
               "The image may be a raw disk (/dev/sdX, needs root). The run's journal is a\n"
               "sidecar file next to the image (/var/tmp for a raw disk), removed when the\n"
               "run ends cleanly; run again on the same partition to finish an interrupted one.\n");
        return 5;
    }
    return (int)rc;
}
