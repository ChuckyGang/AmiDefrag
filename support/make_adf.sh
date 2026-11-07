#!/bin/sh
# make_adf.sh <AmiDefrag binary> <output.adf>
# Bootable OFS floppy image (xdftool from amitools) that runs AmiDefrag
# from the Shell: the way to defragment the boot volume without live
# mode, since nothing on the hard disk is open then.
set -e
BIN="$1"; OUT="$2"; HERE="$(dirname "$0")"
[ -f "$BIN" ] || { echo "make_adf.sh: $BIN not found" >&2; exit 1; }
rm -f "$OUT"
xdftool "$OUT" create + format "AmiDefrag" ofs + boot install
xdftool "$OUT" makedir S + write "$HERE/adf/Startup-Sequence" S/Startup-Sequence \
             + write "$BIN" AmiDefrag + write "$HERE/../README.md" README.md
xdftool "$OUT" list
