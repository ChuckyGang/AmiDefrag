/*
 * cli.h - command-line mode, shared by the Amiga binary and the host build.
 *
 *   AmiDefrag VOLUME,IMAGE/K,PART/K,LIST/S,MAP/S,WIDTH/N,DEBUG/S,DEFRAG/S,DRYRUN/S,FORCE/S
 *
 *   AmiDefrag DH0:                 analyse the mounted volume DH0:
 *   AmiDefrag DH0: MAP WIDTH=72    ... and print an ASCII block map
 *   AmiDefrag LIST                 list the mounted volumes
 *   AmiDefrag IMAGE=disk.hdf LIST  list the partitions of an image
 *   AmiDefrag IMAGE=disk.hdf PART=DH1 MAP
 *
 *   AmiDefrag DH1: DRYRUN           show what DEFRAG would move
 *   AmiDefrag DH1: DEFRAG           defragment (asks for YES unless FORCE)
 *
 * Read-only unless DEFRAG is given.
 */
#ifndef CLI_H
#define CLI_H

#include <exec/types.h>

#define CLI_NO_ARGS (-1)   /* empty command line: caller falls through to the GUI */

LONG cli_run(void);

#endif /* CLI_H */
