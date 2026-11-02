# Host (Linux) AmiDefrag: the CLI (plain gcc, no deps) and the GTK3 window
# (needs libgtk-3-dev).  Both run the same core on image files and raw disks.
CORE = carr volume fsmap ffs_map pfs_map pfs_core pfs_defrag sfs_map ffs_core ffs_defrag sfs_defrag blockdev clib build
CORESRC = $(addprefix ../src/,$(addsuffix .c,$(CORE)))
SRCS = ../src/cli.c $(CORESRC) amiga_shim.c host_rdargs.c host_locale.c host_platform.c host_main.c
GUISRCS = $(CORESRC) amiga_shim.c host_locale.c host_platform.c gui_gtk.c
STRINGS_H = ../src/amidefrag_strings.h
CFLAGS = -O2 -Wall -Wno-pointer-sign -Wno-format-security -Wno-stringop-truncation \
         -DAMIDEFRAG_HOST=1 -I . -I inc -I ../src
GTK_CFLAGS ?= $(shell pkg-config --cflags gtk+-3.0)
GTK_LIBS   ?= $(shell pkg-config --libs gtk+-3.0)

all: amidefrag

$(STRINGS_H): ../catalogs/AmiDefrag.cd ../support/gencat.py
	python3 ../support/gencat.py header ../catalogs/AmiDefrag.cd $(STRINGS_H)

amidefrag: $(SRCS) amiga_compat.h $(STRINGS_H)
	gcc $(CFLAGS) -o amidefrag $(SRCS)

gui: amidefrag-gui
amidefrag-gui: $(GUISRCS) amiga_compat.h $(STRINGS_H)
	gcc $(CFLAGS) $(GTK_CFLAGS) -o amidefrag-gui $(GUISRCS) $(GTK_LIBS)

clean:
	rm -f amidefrag amidefrag-gui runidx_test

# randomized check of the PFS3 bitmap range setter and free-run index
check-index: ../test/runidx_test.c ../src/pfs_core.c ../src/carr.c
	gcc -O1 -g -Wall -Wno-pointer-sign -Wno-format-security -Wno-stringop-truncation -DAMIDEFRAG_HOST=1 -I . -I inc -I ../src -o runidx_test ../test/runidx_test.c amiga_shim.c ../src/clib.c ../src/blockdev.c
	./runidx_test
