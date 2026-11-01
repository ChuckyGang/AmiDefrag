# Host (Linux) AmiDefrag: the CLI (plain gcc, no deps) and the GTK3 window
# (needs libgtk-3-dev).  Both run the same core on image files and raw disks.
CORE = volume fsmap ffs_map pfs_map pfs_core pfs_defrag sfs_map ffs_core ffs_defrag sfs_defrag blockdev clib build
CORESRC = $(addprefix ../src/,$(addsuffix .c,$(CORE)))
SRCS = ../src/cli.c $(CORESRC) amiga_shim.c host_rdargs.c host_platform.c host_main.c
GUISRCS = $(CORESRC) amiga_shim.c host_platform.c gui_gtk.c
CFLAGS = -O2 -Wall -Wno-pointer-sign -Wno-format-security -Wno-stringop-truncation \
         -DAMIDEFRAG_HOST=1 -I . -I inc -I ../src
GTK_CFLAGS ?= $(shell pkg-config --cflags gtk+-3.0)
GTK_LIBS   ?= $(shell pkg-config --libs gtk+-3.0)

all: amidefrag

amidefrag: $(SRCS) amiga_compat.h
	gcc $(CFLAGS) -o amidefrag $(SRCS)

gui: amidefrag-gui
amidefrag-gui: $(GUISRCS) amiga_compat.h
	gcc $(CFLAGS) $(GTK_CFLAGS) -o amidefrag-gui $(GUISRCS) $(GTK_LIBS)

clean:
	rm -f amidefrag amidefrag-gui
