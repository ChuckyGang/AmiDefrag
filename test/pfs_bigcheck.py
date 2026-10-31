#!/usr/bin/env python3
"""pfs_bigcheck.py - run the PFS3 engine on a copy of a big image and verify.

  1. copies IMAGE to WORKDIR (unless --in-place), writes the journal file
     through the real pfs3aio handler (amifuse handler bridge),
  2. lists every file with  host/amidefrag ... FILES DEBUG  and takes the
     md5 of all fragmented files plus --sample random others through the
     handler,
  3. runs  host/amidefrag IMAGE=.. PART=.. DEFRAG FORCE  (timed),
  4. takes the md5s again through the handler, compares, re-analyses the
     map (fragmented left must equal the skipped count, unknown must be
     0) and lets the handler create and delete a file.

Usage: pfs_bigcheck.py IMAGE --partition FRG1 [--sample 500] [--workdir DIR]
Reuses the bridge worker and helpers of pfsfuzz.py.
"""
import argparse, os, random, shutil, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import pfsfuzz as pf


def list_files(img, part):
    # Amiga names are Latin-1; names outside ASCII are left out of the
    # md5 sample (the bridge's name handling is not known to be 8-bit clean)
    r = subprocess.run([pf.HOST, f"IMAGE={img}", f"PART={part}", "FILES", "DEBUG"], capture_output=True)
    files = []
    for line in r.stdout.decode("latin-1").splitlines():
        parts = line.split(None, 2)
        if len(parts) == 3 and parts[0].isdigit() and parts[1].isdigit():
            path = parts[2].split("  [anode ")[0]          # DEBUG appends "  [anode N first B]"
            if path.isascii():
                files.append((int(parts[0]), int(parts[1]), path))
    return files


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("--partition", required=True)
    ap.add_argument("--sample", type=int, default=500)
    ap.add_argument("--workdir", default=os.path.join(HERE, "work", "pfsbig"))
    ap.add_argument("--in-place", action="store_true")
    ap.add_argument("--seed", type=int, default=1)
    a = ap.parse_args()
    pf.PART = a.partition
    os.makedirs(a.workdir, exist_ok=True)
    img = os.path.abspath(a.image)
    if not a.in_place:
        dst = os.path.join(a.workdir, os.path.basename(img))
        print(f"copying {img} -> {dst}", flush=True)
        shutil.copy(img, dst)
        img = dst
    t0 = time.time()
    log = lambda m: print(f"[{time.time() - t0:7.1f}s] {m}", flush=True)

    frag0, unk0, note0, out0 = pf.analyse(img)
    log(f"before: fragmented {frag0}, unknown {unk0} {note0}")
    files = list_files(img, a.partition)
    frag_paths = [p for pieces, blocks, p in files if pieces > 1 and p != "AmiDefrag.journal"]
    others = [p for pieces, blocks, p in files if pieces <= 1 and p != "AmiDefrag.journal"]
    rng = random.Random(a.seed)
    sample = frag_paths + rng.sample(others, min(a.sample, len(others)))
    log(f"{len(files)} files listed, {len(frag_paths)} fragmented, checking {len(sample)}")

    res = pf.bridge(img, [["write", "AmiDefrag.journal", 128 * 1024, "zero"], ["md5", sample], ["flush"]])
    md5_0 = res["md5"]
    missing = [p for p in sample if md5_0.get(p, "MISSING").startswith(("MISSING", "SHORT"))]
    log(f"journal written, md5 before taken ({len(missing)} unreadable: {missing[:5]})")

    t1 = time.time()
    rc, writes, moved, skipped, errors, out, _ = pf.run_defrag(img)
    log(f"defrag: rc {rc}, {writes} writes, moved {moved}, skipped {skipped}, errors {errors} in {time.time() - t1:.0f} s")
    print(out[-1500:])

    res = pf.bridge(img, [["md5", sample], ["write", "post_run_probe", 4096, "probe"], ["delete", "post_run_probe"], ["flush"]])
    md5_1 = res["md5"]
    changed = [p for p in sample if md5_0.get(p) != md5_1.get(p)]
    log(f"md5 after: {len(changed)} changed: {changed[:10]}; handler write probe {'ok' if res['ok'] else 'FAILED'}")
    frag1, unk1, note1, out1 = pf.analyse(img)
    # the journal file itself may be fragmented; it is never moved
    frag1_files = len([p for pieces, blocks, p in list_files(img, a.partition) if pieces > 1 and p != "AmiDefrag.journal"])
    log(f"after: fragmented {frag1} ({frag1_files} without the journal, skipped {skipped}), unknown {unk1} {note1}")
    print(out1)
    ok = rc in (0, 5) and not changed and res["ok"] and unk1 == 0 and frag1_files == skipped and errors == 0
    print("RESULT:", "OK" if ok else "FAILED")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
