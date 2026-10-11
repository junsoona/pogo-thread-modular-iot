#!/usr/bin/env python3
"""
Runs the REAL tools/gen_power_base_setup_qr.swift (compiled here, into a
temporary directory) and reads every PNG it writes back with two QR readers
that share no code with it (tools/power_base_setup_qr_check.swift: Vision and
Core Image's detector), against tools/power_base_setup_qr_vectors.json -- the
vector file the app's parser is tested with as well.

  - every valid vector: the PNG holds exactly one QR code that says exactly
    the vector's 53-byte string (the string comes from the vector file, not
    from the tool); upper-case EUI-64 input is written lower case;
  - the drawing: 8-bit gray PNG without alpha, pure black and white, every
    module the same whole number of pixels, at least four light modules on
    every side;
  - the secret: the PNG is mode 0600 in a directory of mode 0700, the setup
    code and the QR string appear in no output and not as text in the file,
    nothing is overwritten, nothing is written inside a Git work tree, and a
    refused or failed run leaves nothing behind;
  - the setup-code file: read only when it is a regular file of mode 0600
    (no symbolic link, no FIFO, no directory) holding exactly a canonical
    code and at most one line feed; its content is never echoed;
  - every EUI-64 and file content the vector file lists as refused is
    refused; the setup code is never accepted on the command line;
  - the tool is not a firmware build input (tools/pogo_version.py files).

Every EUI-64 and setup code here is made up and public. No real setup-code
file is opened, no board is touched, nothing is flashed.

macOS only (the tool uses Core Image, ImageIO and Vision). Needs the Xcode
command line tools (xcrun swiftc); no ESP-IDF environment.
    python3 tools/test_gen_power_base_setup_qr.py
With POGO_APP_REPO=<app repository> it also checks that the app's copy of the
vector file is this repository's file, byte for byte.
"""
import json
import os
import platform
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
import zlib

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS_DIR = os.path.join(ROOT_DIR, "tools")
GEN_SOURCE = os.path.join(TOOLS_DIR, "gen_power_base_setup_qr.swift")
CHECK_SOURCE = os.path.join(TOOLS_DIR, "power_base_setup_qr_check.swift")
VECTORS = os.path.join(TOOLS_DIR, "power_base_setup_qr_vectors.json")
APP_VECTORS = "PogoHome/Packages/PowerBaseBLEKit/Tests/PowerBaseBLEKitTests/Support/power-base-setup-qr-vectors.json"

failures = 0


def check(cond, msg):
    global failures
    print(("ok:   " if cond else "FAIL: ") + msg)
    if not cond:
        failures += 1


def mode_of(path):
    return stat.S_IMODE(os.lstat(path).st_mode)


def png_chunks(data):
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    chunks, offset = [], 8
    while offset < len(data):
        length, kind = struct.unpack(">I4s", data[offset:offset + 8])
        body = data[offset + 8:offset + 8 + length]
        crc, = struct.unpack(">I", data[offset + 8 + length:offset + 12 + length])
        assert zlib.crc32(kind + body) == crc
        chunks.append((kind.decode("ascii"), body))
        offset += 12 + length
    return chunks


def main():
    if platform.system() != "Darwin":
        print("skipped: the QR generator is a macOS tool (Core Image, ImageIO, Vision)")
        return 0
    with open(VECTORS, "rb") as f:
        vectors_bytes = f.read()
    vectors = json.loads(vectors_bytes)
    check(vectors["format"] == "POGO-PB" and vectors["format_version"] == 1 and vectors["payload_bytes"] == 53,
          "the vector file is for POGO-PB version 1, 53 bytes")

    work = tempfile.mkdtemp(prefix="pb-setup-qr-test-")
    os.chmod(work, 0o700)
    try:
        run_all(work, vectors)
    finally:
        shutil.rmtree(work, ignore_errors=True)

    app_repo = os.environ.get("POGO_APP_REPO")
    if app_repo:
        with open(os.path.join(app_repo, APP_VECTORS), "rb") as f:
            check(f.read() == vectors_bytes, "the app repository's vector file is this file, byte for byte")
    else:
        print("note: POGO_APP_REPO not set -- the app's copy of the vector file was not compared")

    for component in ("power_base", "module", "rcp", "linux_bridge"):
        listed = subprocess.run([sys.executable, os.path.join(TOOLS_DIR, "pogo_version.py"), "files", "--component", component],
                                capture_output=True, text=True, env={**os.environ, "PYTHONDONTWRITEBYTECODE": "1"})
        check(listed.returncode == 0 and "setup_qr" not in listed.stdout,
              f"the QR tool, its test and its vectors are not build inputs of {component}")

    print()
    print("ALL PASSED" if failures == 0 else f"{failures} FAILURE(S)")
    return 1 if failures else 0


def run_all(work, vectors):
    bin_dir = os.path.join(work, "bin")
    os.mkdir(bin_dir, 0o700)
    gen = os.path.join(bin_dir, "gen_power_base_setup_qr")
    checker = os.path.join(bin_dir, "power_base_setup_qr_check")
    for source, binary in ((GEN_SOURCE, gen), (CHECK_SOURCE, checker)):
        built = subprocess.run(["xcrun", "swiftc", "-O", source, "-o", binary], capture_output=True, text=True)
        if built.returncode != 0:
            sys.stderr.write(built.stdout + built.stderr)
            sys.exit(f"could not compile {os.path.basename(source)}")
    check(os.path.isfile(gen) and os.path.isfile(checker), "the generator and the independent reader compile (no package, no library)")

    secrets_dir = os.path.join(work, "codes")
    os.mkdir(secrets_dir, 0o700)
    counter = [0]

    def code_file(content, mode=0o600):
        counter[0] += 1
        path = os.path.join(secrets_dir, f"case{counter[0]}.setup-code")
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, "wb") as f:
            f.write(content.encode("utf-8"))
        os.chmod(path, mode)
        return path

    def generate(eui64, code_path, out, extra_env=None):
        env = {k: v for k, v in os.environ.items() if not k.startswith("PB_") and not k.startswith("POGO_")}
        env.update(extra_env or {})
        return subprocess.run([gen, "--eui64", eui64, "--setup-code-file", code_path, "--out", out],
                              capture_output=True, text=True, env=env)

    def read_back(png_path, payload):
        expect = os.path.join(secrets_dir, "expected-payload")
        fd = os.open(expect, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, "wb") as f:
            f.write(payload.encode("ascii"))
        result = subprocess.run([checker, "--png", png_path, "--expect-payload-file", expect], capture_output=True, text=True)
        os.remove(expect)
        if result.returncode != 0:
            return {}
        return json.loads(result.stdout)

    good = vectors["valid"][0]
    secrets = [v["setup_code"] for v in vectors["valid"]] + [v["payload"] for v in vectors["valid"]]

    def shows_no_secret(text, also=()):
        return not any(secret in text for secret in list(secrets) + list(also))

    # --- every valid vector, read back by two independent readers --------
    out_dir = os.path.join(work, "qr")  # does not exist: the tool creates it
    first_png = None
    for index, vector in enumerate(vectors["valid"]):
        name = vector["name"]
        # The prepare tool writes "<code>\n"; a file without the line feed is the same code.
        path = code_file(vector["setup_code"] + ("\n" if index % 2 == 0 else ""))
        eui_typed = vector["eui64"].upper() if index % 2 == 0 else vector["eui64"]
        out = os.path.join(out_dir, f"{name}.png")
        result = generate(eui_typed, path, out)
        check(result.returncode == 0 and os.path.isfile(out), f"[{name}] a QR code PNG is written")
        if result.returncode != 0:
            sys.stderr.write(result.stderr)
            continue
        first_png = first_png or out
        check(mode_of(out) == 0o600, f"[{name}] the PNG is mode 0600")
        check(mode_of(out_dir) == 0o700, f"[{name}] the directory the tool created is mode 0700")
        check(shows_no_secret(result.stdout + result.stderr), f"[{name}] neither the setup code nor the QR string is printed")
        check(vector["eui64"] in result.stdout and "POGO-PB version 1" in result.stdout, f"[{name}] the output names the board and the format version")
        facts = read_back(out, vector["payload"])
        check(facts.get("vision_symbols") == 1 and facts.get("vision_match") is True,
              f"[{name}] Vision reads exactly one QR code, and it says the vector's string")
        check(facts.get("coreimage_symbols") == 1 and facts.get("coreimage_match") is True,
              f"[{name}] Core Image's detector reads the same")
        check(facts.get("pixel_values") == [0, 255] and facts.get("color_model") == "gray" and facts.get("has_alpha") is False
              and facts.get("bits_per_pixel") == 8, f"[{name}] 8-bit gray, no alpha, pure black on pure white")
        scale = facts.get("module_pixels")
        check(isinstance(scale, int) and scale >= 8 and facts.get("modules_are_flat_squares") is True and facts.get("symbol_is_square") is True,
              f"[{name}] every module is the same whole number of pixels ({scale})")
        quiet = facts.get("quiet_zone_modules", {})
        check(len(quiet) == 4 and all(value >= 4 and value == int(value) for value in quiet.values()),
              f"[{name}] at least four light modules on every side ({sorted(set(quiet.values()))})")
        check((facts.get("symbol_modules", 0) - 17) % 4 == 0 and facts.get("width") == facts.get("height"),
              f"[{name}] a square image of a {facts.get('symbol_modules')}-module symbol")
        with open(out, "rb") as f:
            data = f.read()
        kinds = [kind for kind, _ in png_chunks(data)]
        check(not any(kind in ("tEXt", "iTXt", "zTXt", "eXIf") for kind in kinds) and kinds[0] == "IHDR" and kinds[-1] == "IEND",
              f"[{name}] the PNG carries no text or EXIF chunk ({' '.join(sorted(set(kinds)))})")
        check(vector["setup_code"].encode() not in data and vector["payload"].encode() not in data and vector["eui64"].encode() not in data,
              f"[{name}] the code is in the file as a picture only, not as text")
        # A reader is not fooled into a match: the same picture does not say another vector's string.
        other = vectors["valid"][(index + 1) % len(vectors["valid"])]
        check(read_back(out, other["payload"]).get("vision_match") is False, f"[{name}] ... and does not say another board's string")

    listing = sorted(os.listdir(out_dir)) if os.path.isdir(out_dir) else []
    check(listing == sorted(v["name"] + ".png" for v in vectors["valid"]), "nothing but the finished PNGs is left in the output directory")

    # --- the same input gives the same picture ---------------------------
    again = os.path.join(out_dir, "again.png")
    result = generate(good["eui64"], code_file(good["setup_code"] + "\n"), again)
    with open(os.path.join(out_dir, good["name"] + ".png"), "rb") as a, open(again, "rb") as b:
        check(result.returncode == 0 and a.read() == b.read(), "the same board and code give the same PNG, byte for byte")
    os.remove(again)

    # --- refusals: nothing written, nothing echoed ------------------------
    def refused(result, out, what, status=1, also=()):
        check(result.returncode == status, f"{what}: refused (exit {result.returncode})")
        check(not os.path.lexists(out), f"{what}: no PNG is written")
        check(shows_no_secret(result.stdout + result.stderr, also), f"{what}: the refusal shows no code")

    def leftovers():
        return sorted(os.listdir(out_dir))

    before = leftovers()
    good_file = code_file(good["setup_code"] + "\n")
    good_stat = os.lstat(good_file)

    for case in vectors["generator_rejects"]["eui64"]:
        out = os.path.join(out_dir, "refused.png")
        refused(generate(case["value"], good_file, out), out, f"EUI-64 {case['name']}")

    for case in vectors["generator_rejects"]["setup_code_file_content"]:
        out = os.path.join(out_dir, "refused.png")
        content = case["content"]
        # What the file held must not come back in the refusal (whole, or its first line).
        echoes = [part for part in (content.strip(), content.strip().lower(), content.split("\n")[0]) if len(part) >= 8]
        refused(generate(good["eui64"], code_file(content), out), out, f"setup-code file {case['name']}", also=echoes)

    out = os.path.join(out_dir, "refused.png")
    for mode in (0o644, 0o640, 0o604, 0o660):
        refused(generate(good["eui64"], code_file(good["setup_code"] + "\n", mode), out), out, f"setup-code file of mode {oct(mode)}")
    link = os.path.join(secrets_dir, "link.setup-code")
    os.symlink(good_file, link)
    refused(generate(good["eui64"], link, out), out, "setup-code file that is a symbolic link to a good one")
    refused(generate(good["eui64"], secrets_dir, out), out, "setup-code file that is a directory")
    fifo = os.path.join(secrets_dir, "fifo.setup-code")
    os.mkfifo(fifo, 0o600)
    refused(generate(good["eui64"], fifo, out), out, "setup-code file that is a FIFO (and the tool does not hang)")
    refused(generate(good["eui64"], os.path.join(secrets_dir, "missing.setup-code"), out), out, "setup-code file that does not exist")

    # A Git work tree made here: nothing is created in this repository for the test.
    tree = os.path.join(work, "worktree")
    os.makedirs(os.path.join(tree, ".git"))
    os.makedirs(os.path.join(tree, "sub"), mode=0o700)
    os.chmod(tree, 0o700)
    inside_code = os.path.join(tree, "sub", "inside.setup-code")
    shutil.copy(good_file, inside_code)
    os.chmod(inside_code, 0o600)
    refused(generate(good["eui64"], inside_code, out), out, "setup-code file inside a Git work tree")
    for target in (os.path.join(tree, "sub", "qr.png"), os.path.join(tree, "new", "deeper", "qr.png")):
        refused(generate(good["eui64"], good_file, target), target, f"--out inside a Git work tree ({os.path.relpath(target, tree)})")
    check(not os.path.exists(os.path.join(tree, "new")), "no directory is created inside a work tree on the way to refusing")
    # This repository itself: decided before anything is created.
    repo_target = os.path.join(ROOT_DIR, "build", "pb-setup-qr-test-must-not-exist", "qr.png")
    refused(generate(good["eui64"], good_file, repo_target), repo_target, "--out inside this repository")
    check(not os.path.exists(os.path.dirname(repo_target)), "nothing is created inside this repository")
    linked_tree = os.path.join(work, "looks-outside")
    os.symlink(os.path.join(tree, "sub"), linked_tree)
    target = os.path.join(linked_tree, "qr.png")
    refused(generate(good["eui64"], good_file, target), target, "--out through a symbolic link into a work tree")

    existing = os.path.join(out_dir, good["name"] + ".png")
    with open(existing, "rb") as f:
        kept = f.read()
    result = generate(vectors["valid"][1]["eui64"], code_file(vectors["valid"][1]["setup_code"] + "\n"), existing)
    with open(existing, "rb") as f:
        check(result.returncode == 1 and f.read() == kept and mode_of(existing) == 0o600,
              "an existing PNG is never overwritten (another board's code is refused, the file is unchanged)")
    dangling = os.path.join(out_dir, "dangling.png")
    os.symlink(os.path.join(work, "nowhere.png"), dangling)
    result = generate(good["eui64"], good_file, dangling)
    check(result.returncode == 1 and os.path.islink(dangling) and not os.path.exists(os.path.join(work, "nowhere.png")),
          "--out that is a symbolic link is refused and not written through")
    os.remove(dangling)

    open_dir = os.path.join(work, "open-dir")
    os.mkdir(open_dir, 0o755)
    os.chmod(open_dir, 0o755)
    target = os.path.join(open_dir, "qr.png")
    refused(generate(good["eui64"], good_file, target), target, "--out in an existing directory of mode 0755")
    check(mode_of(open_dir) == 0o755, "... whose mode the tool does not change")
    closed = os.path.join(work, "closed-dir")
    os.mkdir(closed, 0o700)
    dir_link = os.path.join(work, "dir-link")
    os.symlink(closed, dir_link)
    target = os.path.join(dir_link, "qr.png")
    refused(generate(good["eui64"], good_file, target), target, "--out whose directory is a symbolic link")
    check(os.listdir(closed) == [], "... and nothing is written into the directory it points to")
    target = os.path.join(good_file, "qr.png")
    refused(generate(good["eui64"], good_file, target), target, "--out whose directory is a file")
    target = os.path.join(out_dir, "qr.txt")
    refused(generate(good["eui64"], good_file, target), target, "--out that is not a .png")

    # A failure after the directory was created: the name is too long for the file system.
    fresh = os.path.join(work, "fresh", "deeper")
    target = os.path.join(fresh, "a" * 250 + ".png")
    refused(generate(good["eui64"], good_file, target), target, "--out with a name the file system refuses")
    check(not os.path.exists(os.path.join(work, "fresh")), "the directories created for a run that failed are removed again")

    # The setup code is not an argument, and not an environment variable.
    typed = subprocess.run([gen, "--eui64", good["eui64"], "--setup-code", good["setup_code"], "--out", out], capture_output=True, text=True)
    refused(typed, out, "the setup code given as an argument", status=2)
    typed = subprocess.run([gen, "--eui64", good["eui64"], good["setup_code"], "--out", out], capture_output=True, text=True)
    refused(typed, out, "the setup code given as a bare argument", status=2)
    from_env = subprocess.run([gen, "--eui64", good["eui64"], "--out", out], capture_output=True, text=True,
                              env={**os.environ, "PB_SETUP_CODE": good["setup_code"], "SETUP_CODE": good["setup_code"]})
    refused(from_env, out, "the setup code given in the environment (the file is still required)", status=2)

    check(leftovers() == before, "no refusal left a partial file in the output directory")
    after = os.lstat(good_file)
    with open(good_file, "rb") as f:
        check(f.read() == (good["setup_code"] + "\n").encode() and stat.S_IMODE(after.st_mode) == 0o600
              and after.st_mtime_ns == good_stat.st_mtime_ns, "the setup-code file is as it was: the tool only reads it")

if __name__ == "__main__":
    sys.exit(main())
