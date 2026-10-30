# AmiDefrag

A disk defragmenter for AmigaOS with the classic Windows-defrag block map,
built so that the disk is **never left in a dangerous state**.

> Status: **v0.2, FFS engine in testing.** AmiDefrag analyses FFS, PFS3 and
> SFS volumes and draws their block map, and can defragment **OFS/FFS**
> volumes (`DOS\0`-`DOS\7`) with the per-file commit design below. PFS3 and
> SFS are map-only so far. Tested on images and under emulation; treat the
> engine as beta on real disks and keep backups.

## Download

**[Latest build](https://github.com/ChuckyGang/AmiDefrag/releases/latest)** — the
Amiga executable with icon in an LHA archive, plus the native Linux CLI.
Click **Assets** on that page to see the files. Every push to `main` replaces
the rolling `proto` prerelease and keeps a dated copy.

**Director:** John Hertell — john(at)hertell.nu
**Code:** Claude Code (Anthropic)

---

## Why another defragmenter

Every classic Amiga defragmenter (ReOrg, DiskOptimizer, ...) read the whole
disk structure into RAM, computed a complete new layout and wrote it all
back. The volume was inconsistent for the entire run; a power cut or Guru
in the middle meant a dead partition.

Windows never did that. It moves one file at a time through a filesystem
call, and the filesystem commits each move atomically, so the volume is
valid at every instant. AmiDefrag follows that model.

## What it does today

* Lists the partitions in the DOS device list (name, volume, filesystem,
  size), or the partitions in an RDB disk image.
* Builds a per-block map of a volume by reading the filesystem's own
  structures: bitmap, directories, file headers / anode chains / extent
  tree. Classes: free, data, fragmented data, system, reserved, unknown.
* Shows it as a grid (GUI) or as an ASCII map (CLI), with file counts,
  fragmented-file counts and the worst file's piece count.
* Defragments OFS/FFS volumes (see below). Nothing but the FFS engine can
  write: the block device layer refuses writes until the engine arms them
  after the volume is inhibited and its root block re-checked.

Supported: OFS/FFS `DOS\0`-`DOS\7` (512 and 1024-byte blocks, dircache
volumes, hard links skipped as headers only), PFS3 `PFS\x`/`PDS\x`
(reserved area, anode chains, super-index volumes), SFS `SFS\0` (the
`SFS\2` extent layout is unverified and flagged in the status line).

## Safety design

* **FFS** (implemented, `src/ffs_defrag.c`) — the handler has no move
  API, so the engine inhibits the volume and works per file: it picks the
  smallest free run that fits, copies the data there (OFS next-pointers
  patched), reads every block back, writes the new extension blocks,
  saves the new header image in a journal file, and then rewrites the file
  header in place. That single sector write is the commit; the header
  never moves, so hard links, directory caches and locks are untouched.
  The root block carries bitmap-flag = invalid from the first write to the
  last, exactly as FFS does for its own writes. A crash at any point leaves
  either the old or the new copy referenced, never both, so the FFS
  validator can only find leaked blocks, which it frees. A torn header or
  root write is repaired from the journal on the next run (the root is
  found again by scanning for the journal file's header). Only files in
  more than one run are touched; free space is not consolidated yet. The
  boot volume is refused in this version.
  Verified by `test/ffsfuzz.py`: random xdftool images across all DOS
  types, crash and torn-sector injection after random writes, then
  validator + rerun, with every file's md5 checked and the bitmap
  cross-checked; and by a byte-identical differential run of the m68k
  binary under vamos against the Linux build.
* **SFS** — the SmartFilesystem handler contains its own transaction-safe
  defragmenter, driven in single steps through the `ACTION_SFS_DEFRAGMENT_
  INIT` / `ACTION_SFS_DEFRAGMENT_STEP` packets (fs/packets.h in the SFS
  sources). Each step is one atomic move committed through the handler's
  log; the packet reports what moved so the map can follow live. Moving
  SFS blocks from outside the handler would be *less* safe, not more.
* **PFS3** — all metadata lives in a reserved area with its own
  copy-on-write commit scheme. The safe route is a move-extent packet in
  the handler itself (pfs3aio is open source); until then the only
  handler-safe option is copying a file and letting PFS3 re-place it.

## Build

```
make                 # Amiga binary out/AmiDefrag (+ .info), Bartman toolchain
make linux           # native Linux CLI host/amidefrag (image mode, plain gcc)
make testdisk        # test/test.hdf: FFS + PFS3 + SFS partitions with files
python3 test/ffsfuzz.py 10   # FFS engine fuzz loop with crash replay (needs xdftool)
test/make_bigfrag.py --fs ffs   # 1 GB torture image (also pfs3 / sfs), see below
```

Toolchain selection, paths and the Bebbo alternative are the same as in
AmiPart (see the Makefile header). The same `src/*.c` compile on both
targets; `host/` holds the thin POSIX shim and `gui.c` is the only
Amiga-only file. A Linux GUI frontend over the same map is planned.

## Use

GUI: run `AmiDefrag` from the Shell without arguments or from Workbench.
Pick a volume, press **Analyze**.

CLI (Amiga and Linux share the template):

```
AmiDefrag DH0:                      analyse a mounted volume
AmiDefrag DH0: MAP WIDTH=72         ... with an ASCII block map
AmiDefrag LIST                      list mounted partitions
AmiDefrag IMAGE=disk.hdf LIST       partitions of an image (Linux: IMAGE=/path)
AmiDefrag IMAGE=disk.hdf PART=DH1 MAP DEBUG
AmiDefrag DH1: DRYRUN               what DEFRAG would move, nothing written
AmiDefrag DH1: DEFRAG               defragment (asks for YES unless FORCE)
```

On an image file (Linux, or `IMAGE=` on the Amiga) there is no handler to
create the journal, so a 2 KB file named `AmiDefrag.journal` must exist in
the root directory first (`xdftool img write journal.bin AmiDefrag.journal`).
On a mounted volume AmiDefrag creates and removes it itself.

## Debug

Open the folder in VS Code with the amiga-debug extension and press F5:
`.vscode/launch.json` boots an A1200 with `test/test.hdf` attached, so the
three test partitions show up in the volume list. `make testdisk` builds
that image with AmiPart's host CLI, xdftool and (when present) amifuse
with the real pfs3aio / SmartFilesystem handlers.

### Big fragmented test images

`test/make_bigfrag.py --fs ffs|pfs3|sfs [--size-mb 1024]` builds an RDB
image with one partition (`FRG0`/`FRG1`/`FRG2`, handler embedded in the
RDB) whose files are shredded on purpose. It mounts the partition
read-write through amifuse with the *real* handler (FastFileSystem 3.2,
pfs3aio, SmartFilesystem from `~/UAE/WB323/L`), fills the disk with
alternating tiny filler files, deletes every other one so the free space
becomes a sieve of 1-16 KB holes, copies real files into the holes
(`/mnt/share/Amiga/Demos`, `MODULES` and then `~/UAE/WB323` by default, `--src` to
change), then deletes the remaining fillers. Result: every file larger
than a hole is in many pieces, free space is riddled with small gaps,
and one large free run is left at the end. The emulated handlers manage
roughly 2 MB/s and 30-80 small files a second, so a 1 GB image takes from
about an hour (FFS) to several hours (PFS3, SFS); run the three in
parallel. The images are plain RDB hardfiles for Amiberry (attach as
IDE, not uaehf, so the embedded PFS3/SFS handler loads) or for `dd` onto
a scratch card.

## Layout

```
src/        shared core: blockdev (read-only I/O), volume (DosList / RDB),
            fsmap (map model), ffs_map / pfs_map / sfs_map (parsers),
            cli (ReadArgs mode), gui (GadTools window), main
host/       Linux build: amiga_compat.h, amiga_shim.c, host_rdargs.c, host_main.c
support/    Bartman runtime support, icon generator, LHA packer
test/       make_testdisk.sh, make_bigfrag.py, ffsfuzz.py (images are generated, not committed)
```

## References

* SFS 1.279 sources: `fs/blockstructure.h`, `bitmap.h`, `adminspaces.h`,
  `btreenodes.h`, `packets.h` (defragment packets).
* pfs3aio sources: `blocks.h`, `allocation.c`, `anodes.c`.
* AmigaDOS FFS block layout (ROM Kernel Manual, adflib/amitools).

## License

MIT, see LICENSE. Experimental software: keep a full backup of any volume you defragment.
