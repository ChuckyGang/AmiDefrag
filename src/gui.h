/*
 * gui.h - the AmiDefrag window: volume list, block map, legend, status.
 */
#ifndef GUI_H
#define GUI_H

#include <exec/types.h>

/* Runs the main window until the user closes it.  Returns a DOS return
 * code.  Needs intuition, graphics and gadtools open (main.c). */
int gui_run(void);

#endif /* GUI_H */
