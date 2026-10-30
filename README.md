# AmiDefrag

A disk defragmenter for AmigaOS with the classic Windows-defrag block map,
built so that the disk is **never left in a dangerous state**.

> Status: **read-only preview (v0.1).** AmiDefrag analyses FFS, PFS3 and SFS
> volumes and draws their block map. It does not move a single block yet.
> The move engines come next, one filesystem at a time, each with its own
> crash-safety argument (see "Safety design" below).

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
* Everything is read-only. There is no write entry point in the block
  device layer at all in this version.

Supported: OFS/FFS `DOS\0`-`DOS\7` (512 and 1024-byte blocks, dircache
volumes, hard links skipped as headers only), PFS3 `PFS\x`/`PDS\x`
(reserved area, anode chains, super-index volumes), SFS `SFS\0` (the
`SFS\2` extent layout is unverified and flagged in the status line).

## Safety design for the move engines (next steps)

* **FFS** — the handler has no move API, so the engine will inhibit the
  volume and work per file: allocate new contiguous blocks, copy, read
  back, then rewrite the file header (one sector, atomic at the device
  level), then free the old blocks. The bitmap-valid flag is cleared for
  the duration, so a crash only ever means leaked blocks, repaired by the
  FFS validator. Headers of files with hard links are not moved.
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
```

## Debug

Open the folder in VS Code with the amiga-debug extension and press F5:
`.vscode/launch.json` boots an A1200 with `test/test.hdf` attached, so the
three test partitions show up in the volume list. `make testdisk` builds
that image with AmiPart's host CLI, xdftool and (when present) amifuse
with the real pfs3aio / SmartFilesystem handlers.

## Layout

```
src/        shared core: blockdev (read-only I/O), volume (DosList / RDB),
            fsmap (map model), ffs_map / pfs_map / sfs_map (parsers),
            cli (ReadArgs mode), gui (GadTools window), main
host/       Linux build: amiga_compat.h, amiga_shim.c, host_rdargs.c, host_main.c
support/    Bartman runtime support, icon generator, LHA packer
test/       make_testdisk.sh (test.hdf is generated, not committed)
```

## References

* SFS 1.279 sources: `fs/blockstructure.h`, `bitmap.h`, `adminspaces.h`,
  `btreenodes.h`, `packets.h` (defragment packets).
* pfs3aio sources: `blocks.h`, `allocation.c`, `anodes.c`.
* AmigaDOS FFS block layout (ROM Kernel Manual, adflib/amitools).
