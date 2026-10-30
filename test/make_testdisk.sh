#!/usr/bin/env bash
# Build test/test.hdf: a 48 MB RDB disk with three partitions
#   DH0  FFS  (DOS\3)  cyl   2-31   fragmented on purpose (xdftool)
#   DH1  PFS3 (PDS\3)  cyl  32-61   files written by the real pfs3aio handler
#   DH2  SFS  (SFS\0)  cyl  62-94   files written by the real SmartFilesystem
# Used by the F5 debug launch (.vscode/launch.json mounts it as a hard
# drive of the emulated A1200) and by the host CLI
# (host/amidefrag IMAGE=test/test.hdf LIST).
#
# Needs: AmiPart's host CLI (partitioning + internal formatter), amitools'
# xdftool (FFS content), and optionally amifuse + the pfs3aio /
# SmartFilesystem handler binaries for PFS3/SFS content (those two stay
# empty-but-valid when amifuse or a handler is missing).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$HERE/test.hdf
WORK=$HERE/work
AMIPART=${AMIPART:-$HERE/../../AmiPart/host/amipart}
PFS3=${PFS3_HANDLER:-$(ls ~/UAE/*/L/pfs3aio 2>/dev/null | head -1 || true)}
SFS=${SFS_HANDLER:-$(ls ~/UAE/*/L/SmartFilesystem 2>/dev/null | head -1 || true)}

[ -x "$AMIPART" ] || { echo "AmiPart host binary not found at $AMIPART (make linux in AmiPart, or AMIPART=...)"; exit 1; }
command -v xdftool >/dev/null || { echo "xdftool (pipx install amitools) missing"; exit 1; }
mkdir -p "$WORK"
rm -f "$OUT"

# 16 heads x 63 sectors = 1008 blocks (504 KB) per cylinder, 96 cylinders.
dd if=/dev/zero of="$OUT" bs=516096 count=96 status=none
cat > "$WORK/part.script" <<SCRIPT
OPEN FILE $OUT
INIT NEW
ADDPART NAME=DH0 LOW=2  HIGH=31 TYPE=DOS3 VOLNAME=FFSTest BOOTABLE
ADDPART NAME=DH1 LOW=32 HIGH=61 TYPE=PDS3 VOLNAME=PFSTest
ADDPART NAME=DH2 LOW=62 HIGH=94 TYPE=SFS0 VOLNAME=SFSTest
WRITE
CLOSE
SCRIPT
"$AMIPART" SCRIPT "$WORK/part.script" FORCE
"$AMIPART" IMAGE="$OUT" INFO | sed -n '1,20p'

# --- DH0: FFS via xdftool on a stand-alone image of identical geometry,
# then copied into place.  Same block count -> same root block position.
FFS=$WORK/ffs_part.hdf
rm -f "$FFS"
xdftool "$FFS" create chs=30,16,63 + format FFSTest ffs >/dev/null
mkfile() { head -c "$2" /dev/urandom > "$WORK/$1"; }
mkfile s1.bin 40000; mkfile s2.bin 70000; mkfile m1.bin 300000; mkfile m2.bin 450000; mkfile big.bin 2500000
cmds=""
for i in 1 2 3 4 5 6; do cmds="$cmds + write $WORK/m1.bin fill_$i + write $WORK/s1.bin small_$i"; done
xdftool "$FFS" ${cmds# + } >/dev/null
xdftool "$FFS" delete fill_1 + delete fill_3 + delete fill_5 >/dev/null
xdftool "$FFS" write "$WORK/big.bin" fragmented_big + makedir docs + write "$WORK/m2.bin" docs/report + write "$WORK/s2.bin" docs/notes >/dev/null
xdftool "$FFS" delete small_2 + delete small_4 >/dev/null
xdftool "$FFS" write "$WORK/m2.bin" fragmented_medium >/dev/null
dd if="$FFS" of="$OUT" bs=516096 seek=2 conv=notrunc status=none
echo "DH0 (FFS) populated:"; xdftool "$FFS" list | tail -n +2 | head -20

# --- DH1 / DH2: real handlers through amifuse (write-only API, so these
# volumes are valid and populated but not deliberately fragmented).
if command -v amifuse >/dev/null; then
  for spec in "DH1:$PFS3" "DH2:$SFS"; do
    part=${spec%%:*}; drv=${spec#*:}
    if [ -n "$drv" ] && [ -f "$drv" ]; then
      for f in s1.bin s2.bin m1.bin m2.bin big.bin; do
        amifuse write "$OUT" --partition "$part" --driver "$drv" --file "${f%.bin}" --in "$WORK/$f" >/dev/null 2>&1 || echo "  ($part: write $f failed)"
      done
      echo "$part populated with $(basename "$drv"):"; amifuse ls "$OUT" --partition "$part" --driver "$drv" 2>/dev/null | head -8 || true
    else
      echo "$part left empty (no handler binary found; set PFS3_HANDLER / SFS_HANDLER)"
    fi
  done
else
  echo "amifuse not installed: DH1/DH2 left empty but formatted"
fi
echo "wrote $OUT"
