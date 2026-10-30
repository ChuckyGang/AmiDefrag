# Host (Linux) AmiDefrag - image-file mode, read-only. Plain gcc, no deps.
CORE = cli volume fsmap ffs_map pfs_map sfs_map ffs_core ffs_defrag blockdev clib build
SRCS = $(addprefix ../src/,$(addsuffix .c,$(CORE))) \
       amiga_shim.c host_rdargs.c host_platform.c host_main.c
amidefrag: $(SRCS) amiga_compat.h
	gcc -O2 -Wall -Wno-pointer-sign -Wno-format-security -Wno-stringop-truncation \
	    -DAMIDEFRAG_HOST=1 -I . -I inc -I ../src -o amidefrag $(SRCS)
clean:
	rm -f amidefrag
