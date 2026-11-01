#!/usr/bin/env python3
"""ffsfuzz.py - FFS move engine fuzz loop (Linux host, not Amiga).

Each iteration builds a random OFS/FFS image with xdftool (random geometry
and DOS type), fragments it with a write/delete/write pattern, drops the
AmiDefrag.journal file in, records the md5 of every file, then:

  1. runs  host/amidefrag IMAGE=.. DEFRAG FORCE  and checks every file reads
     back identical, that no fragmented file remains except the ones the
     engine reported as skipped for space, and that the bitmap is
     consistent (no unreferenced used blocks, no referenced free blocks);
  2. replays the SAME starting image several times with a simulated crash
     after a random write (AMIDEFRAG_CRASH_AFTER=n, and the torn-sector
     variant n't'), then does what a real system would do: run AmiDefrag
     again (journal repair of a torn header), let "the validator" rebuild
     the bitmap (validate() below, a Python stand-in for FFS's), run
     AmiDefrag once more, and checks the files and bitmap as in 1.

Requirements: amitools (xdftool) on PATH, host/amidefrag built.
Usage: python3 ffsfuzz.py [iterations] [base_seed]
Failed iterations keep their images as work/fuzz/fail_*.hdf.
"""
import hashlib, os, random, struct, subprocess, sys, shutil

HERE = os.path.dirname(os.path.abspath(__file__))
HOST = os.path.join(HERE, "..", "host", "amidefrag")
WORK = os.path.join(HERE, "work", "fuzz")
DOSTYPES = ["ofs", "ffs", "ofs+intl", "ffs+intl", "ffs+intl+dircache", "ofs+intl+dircache"]


def sh(cmd, env=None, check=True):
    e = dict(os.environ)
    # the Amiga keeps the journal in the volume: test that path unless the
    # caller asks for the host's sidecar journal (AMIDEFRAG_SIDECAR=1)
    if not os.environ.get("AMIDEFRAG_SIDECAR"): e.setdefault("AMIDEFRAG_JOURNAL_IN_VOLUME", "1")
    if env: e.update(env)
    r = subprocess.run(cmd, shell=True, capture_output=True, text=True, env=e)
    if check and r.returncode != 0:
        raise RuntimeError(f"command failed ({r.returncode}): {cmd}\n{r.stdout}\n{r.stderr}")
    return r


def md5(path):
    with open(path, "rb") as f:
        return hashlib.md5(f.read()).hexdigest()


# ---------------------------------------------------------------- image ---

def build_image(img, rng):
    """Random fragmented image; returns {amiga_path: md5}."""
    dostype = rng.choice(DOSTYPES)
    while True:
        cyl, heads, secs = rng.randint(10, 60), rng.choice([2, 4, 8, 16]), rng.choice([11, 17, 22, 32, 63])
        nblocks = cyl * heads * secs
        if os.path.exists(img): os.remove(img)
        # create and format in two invocations: chained, xdftool writes the
        # root of some geometries to the wrong block (its own list fails too).
        # Some geometries also make xdftool place a bitmap page past the end
        # of the image (file grows by a block): regenerate those.
        sh(f'xdftool "{img}" create chs={cyl},{heads},{secs}')
        sh(f'xdftool "{img}" format FUZZ {dostype}')
        if os.path.getsize(img) == nblocks * 512:
            break
    budget = int(nblocks * 512 * rng.uniform(0.25, 0.6))
    cap = max(4000, nblocks * 512 // 8)          # keep single files small vs the disk
    files = {}
    tmp = os.path.join(WORK, "f.bin")
    dirs = [""]
    for d in range(rng.randint(0, 3)):
        name = f"dir{d}"
        sh(f'xdftool "{img}" makedir {name}')
        dirs.append(name + "/")
    n = 0
    used = 0
    names = []
    while used < budget and n < 40:
        size = min(cap, rng.choice([rng.randint(1, 3000), rng.randint(3000, 60000), rng.randint(60000, 400000)]))
        if used + size > budget: break
        with open(tmp, "wb") as f:
            f.write(os.urandom(size))
        path = rng.choice(dirs) + f"f{n}"
        sh(f'xdftool "{img}" write "{tmp}" "{path}"')
        files[path] = md5(tmp)
        names.append(path)
        used += size
        n += 1
    # delete some, write more into the holes -> fragmentation
    for path in rng.sample(names, k=max(1, len(names) // 3)):
        sh(f'xdftool "{img}" delete "{path}"')
        del files[path]
    extra = 0
    while extra < 4:
        size = min(cap, rng.randint(20000, 500000))
        with open(tmp, "wb") as f:
            f.write(os.urandom(size))
        path = rng.choice(dirs) + f"g{extra}"
        r = sh(f'xdftool "{img}" write "{tmp}" "{path}"', check=False)
        if r.returncode != 0: break            # disk full
        files[path] = md5(tmp)
        extra += 1
    with open(tmp, "wb") as f:
        f.write(b"\0" * 2048)
    sh(f'xdftool "{img}" write "{tmp}" AmiDefrag.journal')
    return files, dostype, nblocks


def check_files(img, files):
    out = os.path.join(WORK, "out.bin")
    bad = []
    for path, h in files.items():
        if os.path.exists(out): os.remove(out)
        r = sh(f'xdftool "{img}" read "{path}" "{out}"', check=False)
        if r.returncode != 0 or not os.path.exists(out) or md5(out) != h:
            bad.append(path)
    return bad


def analyse(img):
    """Parse the map output: (fragmented, unknown, note, bitmap_valid)."""
    r = sh(f'"{HOST}" IMAGE="{img}"', check=False)
    frag = unknown = None
    note = ""
    valid = "Bitmap flag: valid" in r.stdout
    for line in r.stdout.splitlines():
        if line.startswith("Files:"):
            frag = int(line.split("fragmented:")[1].split()[0])
        if "unknown" in line and "blocks" in line and line.strip().startswith("?"):
            unknown = int(line.split()[2])
        if line.startswith("Note:"):
            note = line
    return frag, unknown, note, valid, r.stdout


def run_defrag(img, env=None):
    r = sh(f'"{HOST}" IMAGE="{img}" DEFRAG FORCE', env=env, check=False)
    writes = skipped = moved = 0
    for line in r.stdout.splitlines():
        if line.startswith("Device I/O:"):
            writes = int(line.split("read,")[1].split()[0])
        if line.startswith("Done:"):
            moved = int(line.split("fragmented,")[1].split()[0])
            skipped = int(line.split("blocks),")[1].split()[0])
    return r.returncode, writes, moved, skipped, r.stdout + r.stderr


# ------------------------------------------------------- python validator ---

def be(b, i):
    return struct.unpack_from(">I", b, i * 4)[0]


def checksum_fix(blk, at):
    b = bytearray(blk)
    struct.pack_into(">I", b, at * 4, 0)
    s = sum(struct.unpack_from(">%dI" % (len(b) // 4), b, 0)) & 0xFFFFFFFF
    struct.pack_into(">I", b, at * 4, (-s) & 0xFFFFFFFF)
    return bytes(b)


def validate(img):
    """What FFS's validator does: rebuild the bitmap from the tree, then
    set the valid flag.  Leaked blocks become free."""
    with open(img, "r+b") as f:
        data = bytearray(f.read())
        bs = 512
        nl = bs // 4
        nblocks = len(data) // bs
        reserved = 2
        blk = lambda k: data[k * bs:(k + 1) * bs]
        def is_root(k):
            if k < reserved or k >= nblocks: return False
            b = blk(k)
            return be(b, 0) == 2 and struct.unpack_from(">i", b, (nl - 1) * 4)[0] == 1 and \
                   (sum(struct.unpack_from(">%dI" % nl, b, 0)) & 0xFFFFFFFF) == 0
        root = (nblocks - 1 + reserved) // 2
        if not is_root(root):
            root = be(blk(0), 2)                    # boot block pointer, as the parser does
        if not is_root(root):
            raise RuntimeError("validator: no root block")
        used = set()
        dirs = [root]
        seen = set()
        while dirs:
            d = dirs.pop()          # pushed once each (header marked in its chain)
            used.add(d)
            b = blk(d)
            ext = be(b, nl - 2)          # dircache chain for dos\4/5 dirs
            while ext and ext < nblocks and be(blk(ext), 0) == 33 and ext not in used:
                used.add(ext)
                ext = be(blk(ext), 4)
            for slot in range(nl - 56):
                k = be(b, 6 + slot)
                while k and k < nblocks and k not in seen:
                    seen.add(k)
                    used.add(k)
                    h = blk(k)
                    sec = struct.unpack_from(">i", h, (nl - 1) * 4)[0]
                    if sec == 2:
                        dirs.append(k)
                    elif sec == -3:
                        cur = h
                        while True:
                            for i in range(be(cur, 2)):
                                used.add(be(cur, nl - 51 - i))
                            e = be(cur, nl - 2)
                            if not e: break
                            used.add(e)
                            cur = blk(e)
                    k = be(h, nl - 4)
        # bitmap pages (their keys stay where they are)
        rb = blk(root)
        bpb = (nl - 1) * 32
        pages = (nblocks - reserved + bpb - 1) // bpb
        keys = [be(rb, nl - 49 + p) for p in range(min(pages, 25))]
        ext = be(rb, nl - 24)
        while ext and len(keys) < pages:
            used.add(ext)
            eb = blk(ext)
            for j in range(nl - 1):
                if len(keys) < pages: keys.append(be(eb, j))
            ext = be(eb, nl - 1)
        used.update(keys)                     # all pages first: a page built early must
                                              # already see the later pages' keys as used
        for p, key in enumerate(keys):
            page = bytearray(bs)
            for j in range(1, nl):
                w = 0
                for bit in range(32):
                    k = reserved + p * bpb + (j - 1) * 32 + bit
                    if k < nblocks and k not in used:
                        w |= 1 << bit
                struct.pack_into(">I", page, j * 4, w)
            page = checksum_fix(bytes(page), 0)
            data[key * bs:(key + 1) * bs] = page
        rbn = bytearray(rb)
        struct.pack_into(">I", rbn, (nl - 50) * 4, 0xFFFFFFFF)
        data[root * bs:(root + 1) * bs] = checksum_fix(bytes(rbn), 5)
        f.seek(0)
        f.write(data)


# ----------------------------------------------------------------- loop ---

def fail(msg, img, tag):
    keep = os.path.join(WORK, f"fail_{tag}.hdf")
    shutil.copy(img, keep)
    print(f"  FAIL: {msg}  (image kept as {keep})")
    return 1


def final_checks(img, files, skipped, tag):
    bad = check_files(img, files)
    if bad:
        return fail(f"content changed: {bad}", img, tag)
    frag, unknown, note, valid, out = analyse(img)
    if frag is None or unknown is None:
        return fail("map failed:\n" + out, img, tag)
    if frag != skipped:
        return fail(f"{frag} fragmented files remain, engine skipped {skipped}", img, tag)
    if unknown != 0 or note or not valid:
        return fail(f"bitmap inconsistent: unknown={unknown} {note} valid={valid}", img, tag)
    return 0


def iteration(it, seed):
    rng = random.Random(seed)
    img = os.path.join(WORK, "fuzz.hdf")
    pristine = os.path.join(WORK, "pristine.hdf")
    files, dostype, nblocks = build_image(img, rng)
    frag0, unknown0, note0, valid0, _ = analyse(img)
    print(f"[{it}] seed {seed}: {dostype}, {nblocks} blocks, {len(files)} files, {frag0} fragmented")
    if unknown0 or note0:
        return fail(f"fresh xdftool image already inconsistent: unknown={unknown0} {note0}", img, f"it{it}_fresh")
    shutil.copy(img, pristine)

    rc, writes, moved, skipped, out = run_defrag(img)
    if rc not in (0, 5):
        return fail(f"defrag rc={rc}\n{out}", img, f"it{it}_rc")
    print(f"      clean run: moved {moved}, skipped {skipped}, {writes} writes")
    if final_checks(img, files, skipped, f"it{it}_clean"):
        return 1

    # crash replays on the pristine image
    if writes > 0:
        points = sorted(set([0, 1, writes - 1] + [rng.randrange(writes) for _ in range(3)]))
        for n in points:
            for torn in (False, True):
                shutil.copy(pristine, img)
                env = {"AMIDEFRAG_CRASH_AFTER": f"{n}{'t' if torn else ''}"}
                rc, _, _, _, out = run_defrag(img, env)
                tag = f"it{it}_crash{n}{'t' if torn else ''}"
                if rc != 99:
                    return fail(f"crash injection did not fire (rc={rc})\n{out}", img, tag)
                # what a real system does next
                rc2, _, _, _, out2 = run_defrag(img)         # journal repair, then "needs validation" or finish
                if rc2 not in (0, 5):
                    return fail(f"post-crash run rc={rc2}\n{out2}", img, tag)
                # validate first when the flag is invalid: a bitmap page torn by
                # the crash makes xdftool refuse the volume, while FFS itself
                # would rebuild the bitmap at mount and read the files fine
                _, _, _, valid, _ = analyse(img)
                if not valid:
                    validate(img)
                    _, unk, note, valid, out_v = analyse(img)
                    if unk or note or not valid:
                        return fail(f"harness validator left the bitmap inconsistent: unknown={unk} {note}\n{out_v}", img, tag)
                bad = check_files(img, files)
                if bad:
                    return fail(f"content lost after crash at write {n}{'t' if torn else ''}: {bad}\n{out}\n{out2}", img, tag)
                rc3, _, _, skipped3, out3 = run_defrag(img)
                if rc3 not in (0, 5):
                    return fail(f"rerun after validation rc={rc3}\n{out3}", img, tag)
                if final_checks(img, files, skipped3, tag):
                    return 1
        print(f"      crash replays at writes {points} (plain + torn): all recovered")
    return 0


def main():
    iters = int(sys.argv[1]) if len(sys.argv) > 1 else 5
    base = int(sys.argv[2]) if len(sys.argv) > 2 else random.randrange(1 << 30)
    os.makedirs(WORK, exist_ok=True)
    fails = 0
    for it in range(iters):
        fails += iteration(it, base + it)
    print(f"{iters} iterations, {fails} failed (base seed {base})")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
