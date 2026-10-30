/* host_main.c - Linux AmiDefrag entry point (image-file mode). */
#include <stdio.h>
#include "amiga_compat.h"
#include "cli.h"

int   host_argc;
char **host_argv;

int main(int argc, char **argv)
{
    LONG rc;
    host_argc = argc;
    host_argv = argv;
    rc = cli_run();
    if (rc == CLI_NO_ARGS) {
        printf("AmiDefrag (host build, image-file mode, read-only)\n"
               "Usage: amidefrag IMAGE=<file> LIST\n"
               "       amidefrag IMAGE=<file> [PART=<name|index>] [MAP] [WIDTH=<n>]\n"
               "       amidefrag ?\n");
        return 5;
    }
    return (int)rc;
}
