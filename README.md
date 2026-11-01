# AmiDefrag

A disk defragmenter for AmigaOS with the classic Windows-defrag block map,
built so that the disk is **never left in a dangerous state**.

> Status: **v0.5, all three engines in testing.** AmiDefrag analyses FFS,
> PFS3 and SFS volumes and draws their block map, defragments **OFS/FFS**
> and **PFS3** volumes with the per-file single-sector commit design
> below, and defragments **SFS** volumes by driving the SmartFilesystem
> handler's own step-by-step defragmenter. Tested on images and under
> emulation; treat the engines as beta on real disks and keep backups.

## Download

**[Latest build](https://github.com/ChuckyGang/AmiDefrag/releases/latest)** — the
Amiga executable with icon in an LHA archive, plus the native Linux CLI
(`amidefrag-linux-x86_64`) and the Linux GTK window
(`amidefrag-gui-linux-x86_64`, needs the GTK3 runtime that every desktop
has). Click **Assets** on that page to see the files. Every push to `main` replaces
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
* Defragments OFS/FFS and PFS3 volumes (see below). Nothing but the move
  engines can write: the block device layer refuses writes until an
  engine arms them after the volume is inhibited and re-checked.
* Defragments SFS volumes through the handler (see below); AmiDefrag
  itself writes nothing there, it only sends packets and follows the
  reported moves on the map.

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
  more than one run are touched; free space is not consolidated yet on
  FFS (the PFS3 engine's room-making pass is not ported). The boot volume
  is refused in this version.
  Verified by `test/ffsfuzz.py`: random xdftool images across all DOS
  types, crash and torn-sector injection after random writes, then
  validator + rerun, with every file's md5 checked and the bitmap
  cross-checked; and by a byte-identical differential run of the m68k
  binary under vamos against the Linux build.
* **SFS** (implemented, `src/sfs_defrag.c`) — the SmartFilesystem handler
  contains its own transaction-safe defragmenter, driven in single steps
  through the `ACTION_SFS_DEFRAGMENT_INIT` / `ACTION_SFS_DEFRAGMENT_STEP`
  packets (fs/packets.h in the SFS sources, handler 1.83 or newer). Each
  step is one transaction of the handler, so the volume is consistent at
  every moment and stays mounted and usable during the run; the step
  reply lists what moved and the map follows live, then a fresh analysis
  gives the final picture. The handler also packs files together to
  consolidate free space, so it moves more than the fragmented files and
  gives no percentage. Moving SFS blocks from outside the handler would
  be *less* safe, not more: the metadata lives in B-trees with no single
  commit write, and the handler's log format is not published. Because
  the engine talks to a running handler, SFS stays map-only on image
  files (Linux build, `IMAGE=`). Verified with `test/sfs_handler_defrag.py`,
  which drives the real handler under amifuse on an image, replays every
  reported move on the bitmap and checks it against the handler's bitmap
  and every file's md5 afterwards.
* **PFS3** (implemented, `src/pfs_defrag.c`) — a PFS3 file is a chain of
  anodes, one per contiguous run, and its directory entry names the first
  anode. The engine inhibits the volume and, per file, copies the data to
  the smallest free run that fits, verifies it, journals the old chain,
  marks the new run used in the data bitmap, then rewrites the *first*
  anode in place to describe the whole file: a 512-byte sector write,
  the commit, read back and compared (one anode in every 512 bytes of an
  anode block straddles a sector boundary; those take two writes, the
  sector with the block number first, and the recovery decides by the
  block number). Afterwards the other anodes of the old chain are cleared
  (a free anode is all zeros, as the handler writes it) and the old runs
  freed. Every write before the commit only marks free blocks used and
  every write after it only releases blocks nothing references, so a
  crash at any sector boundary leaves a volume the handler reads
  correctly, with at worst a few leaked blocks that the journal entry
  reclaims on the next run (it finishes a committed move or undoes an
  uncommitted one, after checking that no file reaches the blocks it is
  about to free). The rootblock, the reserved bitmap and all index,
  super, directory and deldir blocks are never written; pfs3aio does not
  check block datestamps, so the rewritten sectors keep theirs. Volumes
  with cross-linked blocks or a bitmap that disagrees with the chains are
  refused before anything is written.
  Files with no free run large enough get room made for them in a second
  pass: the largest free hole is grown by moving the runs next to it
  aside, one at a time, each with the same commit (rewriting one anode to
  point at the new place; a run moved aside keeps its place in its file's
  chain). Runs land outside the area the hole can grow into, so nothing
  moves twice, and smaller files go first so the holes they leave merge
  for the larger ones. Runs that fit nowhere are not split, so a nearly
  full disk can still end with a few fragmented files; those are
  reported as skipped. Verified by `test/pfsfuzz.py`:
  images filled through the real pfs3aio handler (amifuse handler
  bridge), every file re-read through the handler after clean runs and
  after crash / torn-sector replays aimed at the metadata writes, zero
  leaked blocks required; and by `test/pfs_bigcheck.py` on the 1 GB
  torture image.

## Build

```
make                 # Amiga binary out/AmiDefrag (+ .info), Bartman toolchain
make linux           # native Linux CLI host/amidefrag (image mode, plain gcc)
make linux-gui       # native Linux GTK3 window host/amidefrag-gui (needs libgtk-3-dev)
make testdisk        # test/test.hdf: FFS + PFS3 + SFS partitions with files
python3 test/ffsfuzz.py 10   # FFS engine fuzz loop with crash replay (needs xdftool)
test/make_bigfrag.py --fs ffs   # 1 GB torture image (also pfs3 / sfs), see below
test/sfs_handler_defrag.py IMG --partition FRG2   # SFS engine check through amifuse (see file)
python3 test/pfsfuzz.py 10              # PFS3 engine fuzz loop through the real pfs3aio (needs amifuse)
test/pfs_bigcheck.py IMG --partition FRG1   # PFS3 engine on a copy of a big image, handler-verified
```

Toolchain selection, paths and the Bebbo alternative are the same as in
AmiPart (see the Makefile header). The same `src/*.c` compile on both
targets; `host/` holds the thin POSIX shim, the sidecar journal and the
GTK window; `src/gui.c` is the only Amiga-only file.

## Use

GUI: run `AmiDefrag` from the Shell without arguments or from Workbench.
Pick a volume, press **Analyze**. **Stop** ends a scan at once and a
defragmentation after the file in flight has been committed, so the
volume is consistent; the rest can be done in a later run. In the CLI,
Ctrl-C does the same.

CLI (Amiga and Linux share the template):

```
AmiDefrag DH0:                      analyse a mounted volume
AmiDefrag DH0: MAP WIDTH=72         ... with an ASCII block map
AmiDefrag LIST                      list mounted partitions
AmiDefrag IMAGE=disk.hdf LIST       partitions of an image (Linux: IMAGE=/path)
AmiDefrag IMAGE=disk.hdf PART=DH1 MAP DEBUG
AmiDefrag DH1: FILES                list the fragmented files (PFS3; all files with DEBUG)
AmiDefrag DH1: DRYRUN               what DEFRAG would move, nothing written (FFS, PFS3)
AmiDefrag DH1: DEFRAG               defragment (asks for YES unless FORCE)
```

On an SFS volume DEFRAG drives the handler's defragmenter, so there is no
DRYRUN preview there and the volume must be mounted.

On a mounted Amiga volume AmiDefrag creates the journal file
`AmiDefrag.journal` in the root directory itself (2 KB on FFS, 128 KB on
PFS3: it records the old chain of the file in flight, up to about 10,000
pieces) and removes it afterwards. With `IMAGE=` on the Amiga there is no
handler to create it, so it must exist in the image first (`xdftool img
write journal.bin AmiDefrag.journal`).

### Linux

```
amidefrag IMAGE=disk.hdf LIST                     partitions of an image
amidefrag IMAGE=disk.hdf PART=DH0 MAP             text map
amidefrag IMAGE=disk.hdf PART=DH0 DRYRUN | DEFRAG the engine (FFS, PFS3)
sudo amidefrag IMAGE=/dev/sdb LIST                a real disk (CF card, SD...)
amidefrag-gui [disk.hdf [PART]]                   the window; PART analyses at once
sudo amidefrag-gui /dev/sdb                       the window on a real disk
```

The window is the Amiga one on GTK3: partition list, Analyze, Rescan,
Defragment, Stop, About, the live block map and the status line, driven
by the same engines through the same callbacks. Open takes an image file;
a raw disk is given on the command line and needs root. FFS and PFS3 are
defragmented; SFS is map only on Linux, because its engine drives the
SmartFilesystem handler through DOS packets and there is no handler here.

On Linux the run's journal is a **sidecar file**, since nothing here can
create a file inside the filesystem: `<image>.AmiDefrag-journal-<start
block>` next to the image, or `/var/tmp/amidefrag-<disk>-<start
block>.journal` for a raw disk. It is created when a live run starts and
removed when the run ends cleanly; one left behind means an interrupted
run, and the next run on the same partition reads it to finish or undo
that file's move. Nothing else may have the image or disk open or mounted
during a run. `AMIDEFRAG_JOURNAL_IN_VOLUME=1` selects the Amiga behaviour
(journal file in the volume) instead; the test harnesses use it.

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
pfs3aio, SmartFilesystem from `~/UAE/WB323/L`), writes one big filler,
then fills the disk with alternating small fillers and real small files
(up to 32 KB), deletes the fillers so the free space becomes a sieve of
1-16 KB holes plus one large run, and copies the larger real files into
the holes (`/mnt/share/Amiga/Demos`, `MODULES` and then `~/UAE/WB323` by
default, `--src` to change). The handler's own allocator does the
fragmenting, so the layout is what a real Amiga would produce; nothing
has to be deleted afterwards. The emulated handlers manage roughly 2 MB/s
and 30-80 small files a second, so a 1 GB image takes 30-90 minutes.
The images are plain RDB hardfiles for Amiberry (attach as IDE, not
uaehf, so the embedded PFS3/SFS handler loads) or for `dd` onto a
scratch card.

amifuse notes: the mount runs through `test/amifuse_patched.py`, which
raises amifuse's per-packet reply budget (pfs3aio is still committing
after tens of thousands of deletes when the next packet arrives, and
stock amifuse reports EIO). amifuse cannot list directories on a PFS3
volume from a cold start, so leftovers of an interrupted PFS3 run must be
deleted on the Amiga; `--cleanup-only` handles FFS and SFS.

## Layout

```
src/        shared core: blockdev (read-only I/O), volume (DosList / RDB),
            fsmap (map model), ffs_map / pfs_map / sfs_map (parsers),
            ffs_core + ffs_defrag (FFS engine), pfs_core + pfs_defrag (PFS3
            engine), sfs_defrag (SFS via handler),
            cli (ReadArgs mode), gui (GadTools window), main
host/       Linux build: amiga_compat.h, amiga_shim.c, host_rdargs.c, host_main.c,
            host_platform.c (sidecar journal), gui_gtk.c (GTK3 window)
support/    Bartman runtime support, icon generator, LHA packer
test/       make_testdisk.sh, make_bigfrag.py, ffsfuzz.py, pfsfuzz.py,
            pfs_bigcheck.py, sfs_handler_defrag.py
            (images are generated, not committed)
```

## References

* SFS 1.279 sources: `fs/blockstructure.h`, `bitmap.h`, `adminspaces.h`,
  `btreenodes.h`, `packets.h` (defragment packets).
* pfs3aio sources: `blocks.h`, `allocation.c`, `anodes.c`, `update.c`
  (commit order), `directory.c` (deldir keeps anodes, frees blocks).
* AmigaDOS FFS block layout (ROM Kernel Manual, adflib/amitools).

## License

MIT, see LICENSE. Experimental software: keep a full backup of any volume you defragment.
