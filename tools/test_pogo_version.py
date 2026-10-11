#!/usr/bin/env python3
"""
Tests for tools/pogo_version.py (docs/versioning.md). Everything runs in temporary directories
with made-up trees and a throw-away Git repository; the real repository is only read (and it is
checked that reading it changes nothing).

  python3 tools/test_pogo_version.py
"""
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
TOOLS = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)
import pogo_version as pv  # noqa: E402

failures = 0
checks = 0


def check(cond, message):
    global failures, checks
    checks += 1
    if cond:
        print("ok:   " + message)
    else:
        print("FAIL: " + message)
        failures += 1


def tool(root, *args):
    p = subprocess.run([sys.executable, os.path.join(TOOLS, "pogo_version.py"), "--root", root] + list(args),
                       capture_output=True, text=True)
    return p.returncode, p.stdout, p.stderr


def git(root, *args):
    env = dict(os.environ, GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@example.invalid", GIT_COMMITTER_NAME="t",
               GIT_COMMITTER_EMAIL="t@example.invalid", GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_SYSTEM=os.devnull)
    return subprocess.run(["git", "-C", root] + list(args), capture_output=True, text=True, env=env, check=True).stdout


def write(root, rel, data):
    path = os.path.join(root, *rel.split("/"))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb" if isinstance(data, bytes) else "w") as f:
        f.write(data)
    return path


VERSIONS = {"schema": 1, "components": {n: {"version": "0.1.0"} for n in ("power_base", "module", "rcp", "linux_bridge")}}

PROTO_H = '#define PB_BLE_PROTO_VERSION 1u\n#define PB_BLE_CAP_MGMT_V1 "mgmt_v1"\n#define PB_BLE_MSG_STATUS_REQ 0x01u\n'
ENROLL_H = "#define PB_ENROLL_RECORD_V3_LEN 200\n"
MGMT_H = "#define PB_MGMT_PROTO_VERSION 1u\n"


def make_tree(root, versions=None):
    """A miniature of the firmware repository; the directory name has a space in it."""
    write(root, "versions.json", json.dumps(versions or VERSIONS))
    write(root, "power_base/main/app_main.c", "int main(void) { return 0; }\n")
    write(root, "power_base/CMakeLists.txt", "project(power_base)\n")
    write(root, "module/main/app_main.c", "int module(void) { return 1; }\n")
    write(root, "rcp/main/rcp.c", "int rcp(void) { return 2; }\n")
    write(root, "components/pogo_build_info/pogo_build_info.c", "/* info */\n")
    write(root, "components/power_base_ble_enroll/include/power_base_ble_enroll/pb_ble_proto.h", PROTO_H)
    write(root, "components/power_base_enrollment/include/power_base_enrollment/power_base_enrollment.h", ENROLL_H)
    write(root, "components/power_base_mgmt/include/power_base_mgmt/pb_mgmt_proto.h", MGMT_H)
    write(root, "linux/matter_bridge/app/api/src/x.cpp", "int x;\n")
    write(root, "docs/readme.md", "docs are not sources\n")
    write(root, "tools/helper.py", "# tools are not sources\n")
    write(root, "tools/pogo_version.py", "# the build-identity tool (stand-in)\n")
    write(root, "tools/test_pogo_version.py", "# its test (stand-in): not a build input\n")
    write(root, "tools/cmake/pogo_version.cmake", "# shared CMake of the firmware projects (stand-in)\n")
    write(root, "tools/build_matter_bridge.sh", "# the bridge's build script (stand-in)\n")
    write(root, "linux/matter_bridge/app/BUILD.gn", "# gn\n")
    write(root, ".gitignore", "**/build/\n**/sdkconfig\n*.bin\n")


ESP32H2, ESP32C6 = 0x0010, 0x000D  # esp_chip_id_t


def fake_image(path, version, project, build_info, chip=ESP32H2):
    """An ESP-IDF application image as far as this tool reads one: the image header with its chip
    id (offset 12), the app description at offset 32 and the build-info string somewhere after."""
    desc = struct.pack("<II8s32s32s16s16s32s32s", pv.APP_DESC_MAGIC, 0, b"", version.encode(), project.encode(),
                       b"12:00:00", b"Oct  6 2026", b"v6.1", b"\0" * 32)
    data = b"\xe9" + b"\0" * 11 + struct.pack("<H", chip) + b"\0" * 18 + desc + b"\0" * 64 + build_info.encode() + b"\0" * 16
    with open(path, "wb") as f:
        f.write(data)


def tree_digest(root):
    """Names, sizes, mtimes and contents of everything under root (the .git directory included)."""
    h = hashlib.sha256()
    for current, dirs, files in os.walk(root):
        dirs.sort()
        for name in sorted(files):
            full = os.path.join(current, name)
            st = os.lstat(full)
            h.update(os.path.relpath(full, root).encode() + b"\0" + str(st.st_size).encode() + b"\0" + str(st.st_mtime_ns).encode() + b"\0")
            if not os.path.islink(full):
                with open(full, "rb") as f:
                    h.update(f.read())
    return h.hexdigest()


def main():
    base = tempfile.mkdtemp(prefix="pogo version test ")
    try:
        run(base)
        run_build_inputs(os.path.join(base, "inputs"))
        run_manifest_against_binary(os.path.join(base, "binary"))
    finally:
        shutil.rmtree(base, ignore_errors=True)
    print("\n%d checks, %d failed" % (checks, failures))
    print("ALL PASS" if failures == 0 else "FAILED")
    return 1 if failures else 0


def run(base):
    root = os.path.join(base, "repo with space")
    os.makedirs(root)
    make_tree(root)

    print("\n== versions.json: one definition, strict format ==")
    check(pv.load_versions(root) == {n: "0.1.0" for n in VERSIONS["components"]}, "four components, each with its own version")
    for bad, why in (("0.1", "two parts"), ("0.1.0-dev", "a suffix"), ("0.1.0+abc", "build metadata"), ("v0.1.0", "a prefix"),
                     ("01.1.0", "a leading zero"), ("0.1.0 ", "trailing space"), (1, "a number")):
        broken = json.loads(json.dumps(VERSIONS))
        broken["components"]["rcp"]["version"] = bad
        write(root, "versions.json", json.dumps(broken))
        rc, _, err = tool(root, "check")
        check(rc == 2 and "rcp.version" in err, "a version with %s is refused" % why)
    for mutate, why in ((lambda d: d["components"].pop("rcp"), "a missing component"),
                        (lambda d: d["components"].update(extra={"version": "0.1.0"}), "an unknown component"),
                        (lambda d: d.update(schema=2), "another schema")):
        broken = json.loads(json.dumps(VERSIONS))
        mutate(broken)
        write(root, "versions.json", json.dumps(broken))
        check(tool(root, "check")[0] == 2, "%s is refused" % why)
    independent = json.loads(json.dumps(VERSIONS))
    independent["components"]["power_base"]["version"] = "0.2.0"
    write(root, "versions.json", json.dumps(independent))
    v = pv.load_versions(root)
    check(v["power_base"] == "0.2.0" and v["module"] == "0.1.0", "one component's version moves without the others")
    write(root, "versions.json", json.dumps(VERSIONS))
    check(len("0.1.0") < pv.APP_DESC_VERSION_LEN and pv.APP_DESC_VERSION_LEN == 32, "the version fits ESP-IDF's 32-byte app description field")

    print("\n== fingerprint: content and relative path, nothing else ==")
    fp = pv.fingerprint(root, "power_base")
    check(fp == pv.fingerprint(root, "power_base") and len(fp) == 64, "the same tree hashes the same")
    copy = os.path.join(base, "elsewhere", "another name")
    shutil.copytree(root, copy)
    for current, _, files in os.walk(copy):
        for name in files:
            os.utime(os.path.join(current, name), (1, 1))
    check(pv.fingerprint(copy, "power_base") == fp, "a copy in another directory with other timestamps hashes the same")
    check(pv.fingerprint(root, "module") != fp and pv.fingerprint(root, "rcp") != fp, "each component has its own fingerprint")

    module_before = pv.fingerprint(root, "module")
    rcp_before = pv.fingerprint(root, "rcp")
    write(root, "power_base/main/app_main.c", "int main(void) { return 1; }\n")
    changed = pv.fingerprint(root, "power_base")
    check(changed != fp, "changing a source changes the fingerprint")
    check(pv.fingerprint(root, "module") == module_before, "a Power Base source does not change the module's fingerprint")
    write(root, "power_base/main/app_main.c", "int main(void) { return 0; }\n")
    check(pv.fingerprint(root, "power_base") == fp, "changing it back restores the fingerprint")
    write(root, "power_base/main/new_untracked.c", "int n;\n")
    with_new = pv.fingerprint(root, "power_base")
    check(with_new != fp, "a new (untracked) source changes the fingerprint")
    os.rename(os.path.join(root, "power_base/main/new_untracked.c"), os.path.join(root, "power_base/main/renamed.c"))
    check(pv.fingerprint(root, "power_base") not in (fp, with_new), "the same content under another name is another fingerprint")
    os.remove(os.path.join(root, "power_base/main/renamed.c"))
    write(root, "components/shared/shared.c", "int s;\n")
    check(pv.fingerprint(root, "power_base") != fp and pv.fingerprint(root, "module") != module_before,
          "a shared component source changes every component built from it")
    check(pv.fingerprint(root, "rcp") == rcp_before, "and not one that is not built from it")
    write(root, "components/pogo_build_info/pogo_build_info.c", "/* info, edited */\n")
    check(pv.fingerprint(root, "rcp") == rcp_before, "the RCP is built from rcp/ alone: not even the build-info component is part of it")
    write(root, "components/pogo_build_info/pogo_build_info.c", "/* info */\n")
    os.remove(os.path.join(root, "components/shared/shared.c"))
    os.rmdir(os.path.join(root, "components/shared"))

    for rel, why in (("power_base/build/x.o", "a build directory"), ("power_base/sdkconfig", "the local sdkconfig"),
                     ("power_base/main/out.bin", "a binary"), ("power_base/main/run.log", "a log"),
                     ("power_base/main/__pycache__/x.pyc", "a Python cache"), ("components/pogo_build_info/generated/pogo_build_info_gen.h", "the generated stamp"),
                     ("power_base/.DS_Store", "Finder metadata"), ("docs/new.md", "documentation"), ("tools/new_tool.py", "a tool"),
                     ("power_base/manifest.json", "a manifest"), ("SOURCE_INFO.json", "the bundle's own metadata")):
        path = write(root, rel, "noise\n")
        check(pv.fingerprint(root, "power_base") == fp, "%s does not change the fingerprint" % why)
        os.remove(path)
    for d in ("power_base/build", "power_base/main/__pycache__", "components/pogo_build_info/generated"):
        os.rmdir(os.path.join(root, d))
    write(root, "versions.json", json.dumps(independent))
    check(pv.fingerprint(root, "power_base") != fp, "the version definition is part of what is built")
    write(root, "versions.json", json.dumps(VERSIONS))
    os.symlink("app_main.c", os.path.join(root, "power_base/main/link.c"))
    linked = pv.fingerprint(root, "power_base")
    os.remove(os.path.join(root, "power_base/main/link.c"))
    os.symlink("other.c", os.path.join(root, "power_base/main/link.c"))
    check(linked != fp and pv.fingerprint(root, "power_base") != linked, "a symbolic link counts by its target, not by what it points at")
    os.remove(os.path.join(root, "power_base/main/link.c"))
    rc, out, _ = tool(root, "fingerprint", "--component", "power_base")
    check(rc == 0 and out.strip() == fp, "the command line gives the same value (root with a space in its path)")
    check(tool(root, "fingerprint", "--component", "nope")[0] == 2, "an unknown component is an error")

    print("\n== no Git: unknown, never an invented commit ==")
    info = pv.identity(root, "power_base")
    check(info["commit"] == "unknown" and info["dirty"] == "unknown" and info["origin"] == "unknown", "a tree without Git has an unknown commit and state")
    check(info["build_id"] == "unknown." + fp[:12] and info["source_fingerprint"] == fp, "its build id still tells its sources apart")
    rc, _, err = tool(root, "stamp", "--component", "power_base", "--out", os.path.join(base, "h.h"), "--release")
    check(rc == 2 and "release mode needs a clean tree" in err, "it cannot be a release build")

    print("\n== Git: commit, clean, dirty, untracked ==")
    git(root, "init", "-q")
    git(root, "add", "-A")
    git(root, "commit", "-q", "-m", "one")
    head = git(root, "rev-parse", "HEAD").strip()
    clean = pv.identity(root, "power_base")
    check(clean["commit"] == head and len(head) == 40, "the full commit SHA")
    check(clean["dirty"] is False and clean["origin"] == "git", "a committed tree is clean")
    check(clean["source_fingerprint"] == fp, "committing does not change the fingerprint")
    check(clean["build_id"] == head[:12] + "." + fp[:12] and len(clean["build_id"]) <= 31, "build id: <commit12>.<fingerprint12>")

    write(root, "power_base/main/app_main.c", "int main(void) { return 7; }\n")
    dirty_a = pv.identity(root, "power_base")
    check(dirty_a["dirty"] is True and dirty_a["commit"] == head, "a modified source: dirty, same HEAD")
    check(dirty_a["build_id"] == head[:12] + "+dirty." + dirty_a["source_fingerprint"][:12] and len(dirty_a["build_id"]) <= 31,
          "build id says +dirty and still fits 31 characters")
    write(root, "power_base/main/app_main.c", "int main(void) { return 8; }\n")
    dirty_b = pv.identity(root, "power_base")
    check(dirty_b["commit"] == dirty_a["commit"] and dirty_b["build_id"] != dirty_a["build_id"],
          "two different uncommitted trees on the same HEAD are two different builds")
    check(pv.identity(root, "module")["dirty"] is False, "another component's sources are still clean")
    git(root, "checkout", "-q", "--", "power_base/main/app_main.c")
    write(root, "power_base/main/untracked.c", "int u;\n")
    untracked = pv.identity(root, "power_base")
    check(untracked["dirty"] is True and untracked["source_fingerprint"] != fp, "an untracked source: dirty, and in the fingerprint")
    os.remove(os.path.join(root, "power_base/main/untracked.c"))
    write(root, "docs/extra.md", "x\n")
    write(root, "power_base/build/junk.o", "x\n")
    check(pv.identity(root, "power_base")["dirty"] is False, "untracked documentation and ignored build output do not make a component dirty")
    os.remove(os.path.join(root, "docs/extra.md"))
    shutil.rmtree(os.path.join(root, "power_base/build"))
    git(root, "add", "-A")
    staged_before = git(root, "diff", "--cached", "--name-only")
    write(root, "module/main/app_main.c", "int module(void) { return 3; }\n")
    git(root, "add", "module/main/app_main.c")
    check(pv.identity(root, "module")["dirty"] is True, "a staged, uncommitted change is dirty too")
    git(root, "reset", "-q", "HEAD", "--", "module/main/app_main.c")
    git(root, "checkout", "-q", "--", "module/main/app_main.c")
    check(staged_before == "", "(the test tree is back to clean)")

    print("\n== release mode needs a clean, known tree ==")
    header = os.path.join(base, "out dir", "pogo_build_info_gen.h")
    rc, out, _ = tool(root, "stamp", "--component", "power_base", "--out", header, "--variant", "ble", "--release")
    text = open(header).read()
    check(rc == 0 and '#define POGO_BUILD_KIND "release"' in text and "#define POGO_BUILD_DIRTY 0" in text, "a clean tree can be stamped as a release")
    check('#define POGO_BUILD_VERSION "0.1.0"' in text and ('#define POGO_BUILD_COMMIT "%s"' % head) in text and
          '#define POGO_BUILD_VARIANT "ble"' in text, "the stamp carries version, full commit and variant")
    write(root, "power_base/main/app_main.c", "int main(void) { return 9; }\n")
    rc, _, err = tool(root, "stamp", "--component", "power_base", "--out", header, "--release")
    check(rc == 2 and "dirty" in err and open(header).read() == text, "a dirty tree is refused in release mode and the stamp is left alone")

    print("\n== the stamp follows the sources (incremental builds) ==")
    rc, _, _ = tool(root, "stamp", "--component", "power_base", "--out", header, "--variant", "ble")
    dev = open(header).read()
    check(rc == 0 and '#define POGO_BUILD_KIND "development"' in dev and "#define POGO_BUILD_DIRTY 1" in dev and "+dirty." in dev,
          "the same dirty tree is a development build")
    mtime = os.stat(header).st_mtime_ns
    rc, out, _ = tool(root, "stamp", "--component", "power_base", "--out", header, "--variant", "ble")
    check(rc == 0 and os.stat(header).st_mtime_ns == mtime and "[unchanged]" in out, "stamping again with nothing changed does not rewrite the header")
    write(root, "power_base/main/app_main.c", "int main(void) { return 10; }\n")
    tool(root, "stamp", "--component", "power_base", "--out", header, "--variant", "ble")
    check(open(header).read() != dev, "after a source change the next stamp is new: no metadata of the earlier sources stays")
    git(root, "checkout", "-q", "--", "power_base/main/app_main.c")
    tool(root, "stamp", "--component", "power_base", "--out", header, "--variant", "ble")
    back = open(header).read()
    check("#define POGO_BUILD_DIRTY 0" in back and "+dirty" not in back, "and back to clean when the tree is clean again")
    sdk = write(base, "sdk/sdkconfig", "CONFIG_POWER_BASE_BLE_ENROLLMENT=y\nCONFIG_POWER_BASE_REENROLL_BUTTON=y\nCONFIG_POWER_BASE_REENROLL_BUTTON_GPIO=9\n")
    check(pv.sdkconfig_identity(sdk)[0] == "ble+reenroll_button_gpio9", "variant from the sdkconfig: button trial")
    write(base, "sdk/sdkconfig", "CONFIG_POWER_BASE_BLE_ENROLLMENT=y\n# CONFIG_POWER_BASE_REENROLL_BUTTON is not set\n")
    v_ble, id_ble = pv.sdkconfig_identity(sdk)
    write(base, "sdk/sdkconfig", "CONFIG_POWER_BASE_BENCH_JOINER_CONSOLE=y\n")
    v_bench, id_bench = pv.sdkconfig_identity(sdk)
    write(base, "sdk/sdkconfig", "CONFIG_OTHER=y\n")
    check((v_ble, v_bench, pv.sdkconfig_identity(sdk)[0]) == ("ble", "bench", "default") and id_ble != id_bench and id_ble.startswith("sdkconfig-sha256:"),
          "variant ble / bench / default, and a config id that is a hash (no setting is copied out)")
    # Both RCP builds log nothing; only the diagnostic one keeps a core dump in flash.
    write(base, "sdk/sdkconfig", "CONFIG_LOG_DEFAULT_LEVEL_NONE=y\n# CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH is not set\n")
    rcp_default = pv.sdkconfig_identity(sdk, "rcp")[0]
    write(base, "sdk/sdkconfig", "CONFIG_LOG_DEFAULT_LEVEL_NONE=y\nCONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y\n")
    check((rcp_default, pv.sdkconfig_identity(sdk, "rcp")[0]) == ("default", "diag"), "RCP variant default / diag, told apart by the core dump setting")

    print("\n== a source bundle without Git ==")
    bundle = os.path.join(base, "bundle")
    shutil.copytree(root, bundle, ignore=shutil.ignore_patterns(".git"))
    b = pv.identity(bundle, "power_base")
    check(b["commit"] == "unknown" and b["source_fingerprint"] == fp, "a bundle without metadata: commit unknown, same fingerprint")
    rc, _, err = tool(root, "source-info", "--out", os.path.join(root, "SOURCE_INFO.json"))
    check(rc == 2 and not os.path.exists(os.path.join(root, "SOURCE_INFO.json")), "source-info refuses to write into the source tree")
    rc, _, _ = tool(root, "source-info", "--out", os.path.join(bundle, "SOURCE_INFO.json"))
    b = pv.identity(bundle, "power_base")
    check(rc == 0 and b["commit"] == head and b["dirty"] is False and b["origin"] == "source-info-verified",
          "with SOURCE_INFO.json that matches its sources the bundle keeps its origin")
    check(b["build_id"] == clean["build_id"], "and has the build id of the tree it was made from")
    write(bundle, "power_base/main/app_main.c", "int main(void) { return 99; }\n")
    b = pv.identity(bundle, "power_base")
    check(b["commit"] == "unknown" and b["origin"] == "unknown", "a bundle whose sources were changed afterwards loses it: unknown, not the old commit")
    check(pv.identity(bundle, "module")["commit"] == head, "(a component whose sources still match keeps it)")
    info_path = os.path.join(bundle, "SOURCE_INFO.json")
    data = json.load(open(info_path))
    data["components"]["module"]["commit"] = "not-a-sha"
    json.dump(data, open(info_path, "w"))
    check(pv.identity(bundle, "module")["commit"] == "unknown", "metadata with something that is not a commit is not trusted")
    open(info_path, "w").write("{broken")
    check(pv.identity(bundle, "rcp")["commit"] == "unknown", "unreadable metadata: unknown")

    print("\n== manifest: files, metadata, and what the binary itself says ==")
    art = os.path.join(base, "artifacts", "pb ble")
    os.makedirs(art)
    tool(root, "stamp", "--component", "power_base", "--out", header, "--variant", "ble")
    info_string = [line for line in open(header).read().splitlines() if "POGO_BUILD_INFO_STRING" in line][0].split('"')[1]
    fake_image(os.path.join(art, "power_base.bin"), "0.1.0", "power_base", info_string)
    write(art, "bootloader/bootloader.img", b"\x01\x02\x03")
    args = ["manifest-create", "--dir", art, "--component", "power_base", "--configuration", "esp-idf", "--variant", "ble",
            "--primary", "power_base.bin", "--fingerprint-before", fp, "--toolchain", "esp-idf=v6.1", "power_base.bin", "bootloader/bootloader.img"]
    rc, out, err = tool(root, *args)
    manifest_path = os.path.join(art, "manifest.json")
    check(rc == 0 and os.path.isfile(manifest_path), "a manifest is created for a build that matches its sources")
    m = json.load(open(manifest_path))
    check(m["manifest_schema"] == 1 and m["component"] == "power_base" and m["version"] == "0.1.0" and m["build_kind"] == "development",
          "it has its own schema version, the component, the version and the build kind")
    check(m["source"] == {"commit": head, "dirty": False, "fingerprint": fp, "origin": "git", "inputs_unchanged_during_build": True},
          "the full commit, clean/dirty, fingerprint, and that the inputs did not change")
    check([f["path"] for f in m["files"]] == ["power_base.bin", "bootloader/bootloader.img"] and
          m["files"][1] == {"path": "bootloader/bootloader.img", "size": 3, "sha256": hashlib.sha256(b"\x01\x02\x03").hexdigest()},
          "every file with its relative path, size and SHA-256")
    check(m["build"]["target"] == "esp32h2" and m["build"]["variant"] == "ble" and m["build"]["toolchain"] == {"esp-idf": "v6.1"}, "target, variant and toolchain")
    check(m["compatibility"]["ble_enroll_proto"] == 1 and m["compatibility"]["ble_capabilities"] == ["mgmt_v1"] and
          m["compatibility"]["enrollment_record_versions"] == [1, 2, 3], "protocol and record facts, read from the sources")
    check(m["binary_says"]["app_description"]["version"] == "0.1.0" and m["binary_says"]["build_info"]["build"] == clean["build_id"],
          "what the binary itself says is recorded")
    check(m["status"] == {"deployed": False, "hardware_verified": False, "note": m["status"]["note"]}, "a manifest is neither a deployment nor a hardware result")
    check(not any(k in json.dumps(m).lower() for k in ("pskd", "setup_code", "network_key")), "nothing secret-shaped is in it")
    rc, out, _ = tool(root, "manifest-verify", "--dir", art, "--expect-component", "power_base", "--expect-target", "esp32h2", "--expect-variant", "ble")
    check(rc == 0 and "manifest verified" in out, "it verifies")

    def verify_fails(why, *extra, needle=None):
        rc, out, _ = tool(root, "manifest-verify", "--dir", art, *extra)
        check(rc == 1 and (needle is None or needle in out), why)

    verify_fails("expecting another component fails", "--expect-component", "module", needle="expected 'module'")
    verify_fails("expecting another target fails", "--expect-target", "linux", needle="target is")
    verify_fails("expecting another variant fails", "--expect-variant", "default")
    verify_fails("expecting another version fails", "--expect-version", "0.2.0")
    verify_fails("asking for a release fails for a development build", "--release", needle="not a release")
    original = open(manifest_path).read()
    boot = os.path.join(art, "bootloader", "bootloader.img")
    os.rename(boot, boot + ".gone")
    verify_fails("a missing file fails", needle="missing file")
    os.rename(boot + ".gone", boot)
    open(boot, "wb").write(b"\x01\x02\x04")
    verify_fails("a changed file of the same size fails", needle="SHA-256 differs")
    open(boot, "wb").write(b"\x01\x02\x03\x04")
    verify_fails("a file of another size fails", needle="size of")
    open(boot, "wb").write(b"\x01\x02\x03")
    for mutate, why in ((lambda d: d.update(version="0.9.0"), "a version that is not the binary's"),
                        (lambda d: d.update(build_id="0123456789ab.0123456789ab"), "a build id that does not follow from its source"),
                        (lambda d: d["source"].update(commit="1234"), "a commit that is not a full SHA"),
                        (lambda d: d["source"].update(fingerprint="zz"), "a fingerprint that is not a hash"),
                        (lambda d: d.update(component="rcp"), "another component than the binary's"),
                        (lambda d: d.update(manifest_schema=2), "an unknown manifest schema"),
                        (lambda d: d["status"].update(hardware_verified=True), "a manifest that claims hardware verification"),
                        (lambda d: d["status"].update(deployed=True), "a manifest that claims deployment"),
                        (lambda d: d.update(build_kind="release"), "a development build relabelled as a release"),
                        (lambda d: d["files"].append({"path": "../outside", "size": 1, "sha256": "0" * 64}), "a path outside the directory"),
                        (lambda d: d.update(primary=None), "a firmware manifest without its primary binary")):
        d = json.loads(original)
        mutate(d)
        open(manifest_path, "w").write(json.dumps(d))
        verify_fails("%s fails" % why)
    open(manifest_path, "w").write("{")
    verify_fails_rc = tool(root, "manifest-verify", "--dir", art)[0]
    check(verify_fails_rc == 2, "a manifest that is not JSON fails")
    os.remove(manifest_path)
    check(tool(root, "manifest-verify", "--dir", art)[0] == 2, "no manifest fails")

    print("\n== a manifest is refused when the build is not what the tree says ==")

    def create_fails(why, mutate_args=None, needle=None):
        a = list(args) if mutate_args is None else mutate_args(list(args))
        rc, _, err = tool(root, *a)
        check(rc == 2 and not os.path.exists(manifest_path) and (needle is None or needle in err), why)

    create_fails("the sources changed during the build (fingerprint before != after)",
                 lambda a: [x if x != fp else "0" * 64 for x in a], "changed while it was being built")
    fake_image(os.path.join(art, "power_base.bin"), "0.0.9", "power_base", info_string)
    create_fails("the binary's app description has another version than versions.json", needle="app description version")
    fake_image(os.path.join(art, "power_base.bin"), "0.1.0", "pogo_module", info_string)
    create_fails("the binary is another project", needle="app description project")
    fake_image(os.path.join(art, "power_base.bin"), "0.1.0", "power_base", "no stamp here")
    create_fails("the binary has no build stamp", needle="no POGO_BUILD_INFO")
    fake_image(os.path.join(art, "power_base.bin"), "0.1.0", "power_base", info_string.replace(clean["build_id"], "000000000000.000000000000"))
    create_fails("the binary was built from other sources (stale incremental build)", needle="not a build of these sources")
    fake_image(os.path.join(art, "power_base.bin"), "0.1.0", "power_base", info_string + "\0\0" + info_string.replace("variant=ble", "variant=bench"))
    create_fails("the binary carries two different stamps", needle="different POGO_BUILD_INFO")
    fake_image(os.path.join(art, "power_base.bin"), "0.1.0", "power_base", info_string)
    create_fails("a listed file is missing", lambda a: a + ["nope.bin"], "missing file")
    create_fails("a release manifest for a development binary", lambda a: a + ["--release"] if False else a[:1] + ["--release"] + a[1:], "kind=development")
    write(art, "power_base_await_config.bin", b"cfg")
    create_fails("a configuration image is never listed", lambda a: a + ["power_base_await_config.bin"], "private record")
    write(art, "board.setup-code", b"x")
    create_fails("nor a setup code file", lambda a: a + ["board.setup-code"], "private record")
    write(root, "power_base/main/app_main.c", "int main(void) { return 11; }\n")
    create_fails("the tree changed after the build: the old binary gets no manifest", lambda a: [x for x in a if x not in ("--fingerprint-before", fp)],
                 "not a build of these sources")
    git(root, "checkout", "-q", "--", "power_base/main/app_main.c")
    rc, _, _ = tool(root, *args)
    check(rc == 0, "(and with everything in order it is created again)")

    print("\n== reading a real-shaped image ==")
    rc, out, _ = tool(root, "appdesc", os.path.join(art, "power_base.bin"))
    said = json.loads(out)
    check(rc == 0 and said["app_description"] == {"version": "0.1.0", "project_name": "power_base", "idf_ver": "v6.1"} and
          said["build_info"]["component"] == "power_base", "appdesc prints what an image says about itself")
    write(art, "not_an_image.bin", b"\0" * 400)
    check(tool(root, "appdesc", os.path.join(art, "not_an_image.bin"))[0] == 2, "a file without the app description magic is refused")

    print("\n== record templates ==")
    records = os.path.join(base, "records")
    rc, _, _ = tool(root, "record-template", "--out", records)
    public = open(os.path.join(records, "install-record.public.md")).read()
    private = open(os.path.join(records, "install-record.private.md")).read()
    check(rc == 0 and "Roll back to" in public and "Not verified in this session" in public and "alias" in public,
          "the public record has the artifact SHAs, the combination, results, what was not verified and the roll-back")
    check("PRIVATE" in private and "never commit" in private, "addresses, accounts and board identities go into a separate private record")
    check(tool(root, "record-template", "--out", records)[0] == 2, "an existing record is not overwritten")

    print("\n== the tool changes nothing it reads ==")
    git(root, "add", "-A")
    before = tree_digest(root)
    index_before = open(os.path.join(root, ".git", "index"), "rb").read()
    for command in (["versions"], ["check"], ["identity", "--component", "power_base"], ["fingerprint", "--component", "linux_bridge"],
                    ["files", "--component", "rcp"], ["compat"], ["stamp", "--component", "module", "--out", os.path.join(base, "elsewhere.h")],
                    ["manifest-verify", "--dir", art]):
        tool(root, *command)
    check(tree_digest(root) == before, "sources, .git and timestamps are byte for byte what they were after every read command")
    check(open(os.path.join(root, ".git", "index"), "rb").read() == index_before, "the Git index is untouched")
    art_before = tree_digest(art)
    tool(root, "manifest-verify", "--dir", art)
    check(tree_digest(art) == art_before, "verifying changes nothing in the artifact directory")

    print("\n== the real repository ==")
    if os.path.isfile(os.path.join(REPO, "versions.json")):
        versions = pv.load_versions(REPO)
        check(set(versions) == {"power_base", "module", "rcp", "linux_bridge"}, "the real versions.json names the four components")
        status_before = subprocess.run(["git", "-C", REPO, "status", "--porcelain=v1", "--untracked-files=all"], capture_output=True, text=True,
                                       env=dict(os.environ, GIT_OPTIONAL_LOCKS="0")).stdout
        index_path = os.path.join(REPO, ".git", "index")
        index_before = open(index_path, "rb").read() if os.path.isfile(index_path) else b""
        first = {c: pv.identity(REPO, c) for c in versions}
        second = {c: pv.identity(REPO, c) for c in versions}
        check(first == second, "its identities are reproducible")
        check(all(len(i["build_id"]) <= 31 for i in first.values()), "every build id fits 31 characters")
        check(all(i["commit"] == "unknown" or len(i["commit"]) == 40 for i in first.values()), "every commit is a full SHA or unknown")
        comp = pv.compatibility(REPO)
        check(comp["ble_enroll_proto"] == 1 and "mgmt_v1" in comp["ble_capabilities"] and comp["mgmt_proto"] == 1,
              "the compatibility facts are found in the real sources")
        check(comp["mgmt_messages"] == ["0x01", "0x02", "0x03", "0x05", "0x06", "0x81", "0x82", "0x83", "0x85", "0x86"] and
              comp["mgmt_capabilities"] == ["board_cap_endpoint_locate", "board_cap_thread_receipt",
                                            "hub_cap_endpoint_locate", "hub_cap_thread_receipt"],
              "and with them what the management protocol speaks beyond its version number")
        status_after = subprocess.run(["git", "-C", REPO, "status", "--porcelain=v1", "--untracked-files=all"], capture_output=True, text=True,
                                       env=dict(os.environ, GIT_OPTIONAL_LOCKS="0")).stdout
        index_after = open(index_path, "rb").read() if os.path.isfile(index_path) else b""
        check(status_before == status_after and index_before == index_after, "reading the real repository changed neither its status nor its index")
        files = [rel for rel, _ in pv.source_files(REPO, "power_base")]
        check(not any("/build/" in f or f.endswith("sdkconfig") or f.endswith(".bin") or f.startswith("docs/") for f in files),
              "no build output, sdkconfig, binary or document is in the Power Base fingerprint")
        check(sorted(f for f in files if f.startswith("tools/")) == ["tools/cmake/pogo_version.cmake", "tools/pogo_version.py"],
              "of tools/, exactly the two files its build runs are (review finding of 2026-10-06: they used to be left out)")
        check("power_base/main/app_main.c" in files and "versions.json" in files and "power_base/sdkconfig.defaults" in files,
              "the sources, the version definition and the checked-in configuration defaults are")
    else:
        print("(no versions.json next to this test: real-repository checks skipped)")


FIRMWARE = ("power_base", "module", "rcp")
ALL = FIRMWARE + ("linux_bridge",)


def ids(root, components=ALL):
    return {c: pv.identity(root, c) for c in components}


def release_refused(root, component, out_dir):
    rc, _, err = tool(root, "stamp", "--component", component, "--out", os.path.join(out_dir, component + " release.h"), "--release")
    return rc == 2 and "release mode needs a clean tree" in err


def run_build_inputs(base):
    """Review finding 1 (2026-10-06): what a build reads from the repository besides the sources
    -- the shared CMake file, the identity tool, the bridge's build script -- is part of the
    identity, per component, and the fingerprint, the dirty flag and the before/after comparison
    of a build all look at the same set of files."""
    root = os.path.join(base, "repo with space")
    os.makedirs(root)
    make_tree(root)
    write(root, ".gitignore", "**/build/\n**/sdkconfig\n*.bin\n*.local.h\n__pycache__/\n")
    write(root, "power_base/notes.log", "a tracked log\n")
    git(root, "init", "-q")
    git(root, "add", "-A", "-f")
    git(root, "commit", "-q", "-m", "one")
    head = git(root, "rev-parse", "HEAD").strip()
    clean = ids(root)
    out = os.path.join(base, "out")
    os.makedirs(out)

    print("\n== build inputs: the shared CMake file ==")
    check(all(i["dirty"] is False and i["commit"] == head for i in clean.values()), "(the committed tree is clean for all four components)")
    cmake = "tools/cmake/pogo_version.cmake"
    original = open(os.path.join(root, cmake)).read()
    write(root, cmake, original + "add_compile_definitions(POGO_REVIEW_PROBE=1)\n")
    after = ids(root)
    for c in FIRMWARE:
        check(after[c]["source_fingerprint"] != clean[c]["source_fingerprint"] and after[c]["build_id"] != clean[c]["build_id"],
              "%s: a change to the shared CMake file alone changes its fingerprint and build id" % c)
        check(after[c]["dirty"] is True, "%s: and makes it dirty" % c)
        check(release_refused(root, c, out), "%s: a release is refused while that build input is uncommitted" % c)
    check(after["linux_bridge"] == clean["linux_bridge"], "linux_bridge does not use that file: its identity is unchanged (inputs are per component)")
    rc, listing, _ = tool(root, "files", "--component", "rcp")
    check(rc == 0 and cmake in listing.split("\n"), "the file is listed among the RCP's inputs")
    write(root, cmake, original)
    check(ids(root) == clean, "(restored: the clean identities again)")

    print("\n== build inputs: the identity tool and the bridge's build script ==")
    tool_rel = "tools/pogo_version.py"
    tool_original = open(os.path.join(root, tool_rel)).read()
    write(root, tool_rel, tool_original + "# changed\n")
    after = ids(root)
    check(all(after[c]["source_fingerprint"] != clean[c]["source_fingerprint"] and after[c]["dirty"] is True for c in ALL),
          "a change to the tool that writes the build identity changes all four components and makes them dirty")
    check(all(release_refused(root, c, out) for c in ALL), "and none of them can be a release")
    write(root, tool_rel, tool_original)
    script = "tools/build_matter_bridge.sh"
    script_original = open(os.path.join(root, script)).read()
    write(root, script, script_original + "# changed\n")
    after = ids(root)
    check(after["linux_bridge"]["build_id"] != clean["linux_bridge"]["build_id"] and after["linux_bridge"]["dirty"] is True,
          "the bridge's build script is an input of linux_bridge")
    check(all(after[c] == clean[c] for c in FIRMWARE), "and of no firmware component")
    write(root, script, script_original)
    for rel, why in (("tools/helper.py", "another tool"), ("tools/test_pogo_version.py", "the tool's test"), ("docs/readme.md", "a document")):
        write(root, rel, "changed\n")
    check(ids(root) == clean, "tools that no build runs, tests and documents are not inputs: nothing changes and nothing is dirty")
    git(root, "checkout", "-q", "--", ".")

    print("\n== build inputs: in a tree that is dirty already ==")
    write(root, "power_base/main/app_main.c", "int main(void) { return 21; }\n")
    dirty_a = pv.identity(root, "power_base")
    write(root, cmake, original + "# one more uncommitted change, to a build input only\n")
    dirty_b = pv.identity(root, "power_base")
    check(dirty_a["dirty"] is True and dirty_b["dirty"] is True and dirty_b["commit"] == dirty_a["commit"], "(dirty before and after, same HEAD)")
    check(dirty_b["source_fingerprint"] != dirty_a["source_fingerprint"] and dirty_b["build_id"] != dirty_a["build_id"],
          "changing only a build input in an already dirty tree gives another fingerprint and build id")
    git(root, "checkout", "-q", "--", ".")

    print("\n== build inputs: removed, ignored, excluded ==")
    os.remove(os.path.join(root, cmake))
    gone = ids(root, FIRMWARE)
    check(all(gone[c]["source_fingerprint"] != clean[c]["source_fingerprint"] for c in FIRMWARE), "a removed build input changes the fingerprint")
    check(all(gone[c]["dirty"] is True for c in FIRMWARE), "and is dirty (the dirty flag looks at the same files as the fingerprint)")
    git(root, "checkout", "-q", "--", ".")
    write(root, "components/shared/tuning.local.h", "#define LOCAL 1\n")  # ignored by Git, read by the compiler
    ignored = pv.identity(root, "power_base")
    check(ignored["source_fingerprint"] != clean["power_base"]["source_fingerprint"], "(a file Git ignores but the build reads is in the fingerprint)")
    check(ignored["dirty"] is True and release_refused(root, "power_base", out),
          "so it is dirty too: a fingerprint that is not the commit's is never called clean")
    shutil.rmtree(os.path.join(root, "components/shared"))
    write(root, "power_base/notes.log", "a tracked log, changed\n")  # tracked, but excluded from the inputs by its name
    logged = pv.identity(root, "power_base")
    check(logged["source_fingerprint"] == clean["power_base"]["source_fingerprint"] and logged["dirty"] is False,
          "a tracked file that is not an input (a log) changes neither the fingerprint nor the dirty flag")
    git(root, "checkout", "-q", "--", ".")
    for rel in ("power_base/build/junk.o", "tools/__pycache__/pogo_version.cpython-311.pyc", "tools/cmake/build/CMakeCache.txt",
                "components/pogo_build_info/generated/pogo_build_info_gen.h", "linux/matter_bridge/app/api/generated/pogo_build_info_gen.h",
                "linux/matter_bridge/app/out/host/pogo_matter_bridge", "power_base/sdkconfig", "rcp/manifest.json", "SOURCE_INFO.json",
                "module/main/app.bin", "power_base/managed_components/x/x.c"):
        write(root, rel, "output\n")
    check(ids(root) == clean, "build output, caches, generated headers, manifests and local sdkconfig change nothing and are not dirty")
    for rel in ("power_base/build", "tools/__pycache__", "tools/cmake/build", "components/pogo_build_info/generated", "linux/matter_bridge/app/api/generated",
                "linux/matter_bridge/app/out", "power_base/managed_components"):
        shutil.rmtree(os.path.join(root, rel))
    for rel in ("power_base/sdkconfig", "rcp/manifest.json", "SOURCE_INFO.json", "module/main/app.bin"):
        os.remove(os.path.join(root, rel))
    check(ids(root) == clean, "(clean again)")

    print("\n== build inputs: a source bundle without Git ==")
    info_path = os.path.join(base, "SOURCE_INFO.json")
    tool(root, "source-info", "--out", info_path)
    bundle = os.path.join(base, "bundle")
    shutil.copytree(root, bundle, ignore=shutil.ignore_patterns(".git"))
    shutil.copyfile(info_path, os.path.join(bundle, "SOURCE_INFO.json"))
    kept = ids(bundle)
    check(all(kept[c]["commit"] == head and kept[c]["origin"] == "source-info-verified" and kept[c]["build_id"] == clean[c]["build_id"] for c in ALL),
          "(a bundle with matching SOURCE_INFO.json keeps its origin)")
    write(bundle, cmake, original + "add_compile_definitions(POGO_REVIEW_PROBE=1)\n")
    edited = ids(bundle)
    check(all(edited[c]["commit"] == "unknown" and edited[c]["origin"] == "unknown" and edited[c]["build_id"].startswith("unknown.") for c in FIRMWARE),
          "after a build input was changed in the bundle, SOURCE_INFO.json is no longer a valid baseline: commit unknown")
    check(edited["linux_bridge"]["commit"] == head, "(a component that does not use that input keeps its origin)")
    write(bundle, cmake, original)
    write(bundle, tool_rel, tool_original + "# changed\n")
    check(all(i["commit"] == "unknown" for i in ids(bundle).values()), "the same for a change to the identity tool: unknown for every component")

    print("\n== build inputs: changed while a build was running ==")
    art = os.path.join(base, "artifact rcp")
    os.makedirs(art)
    header = os.path.join(out, "rcp.h")
    tool(root, "stamp", "--component", "rcp", "--out", header, "--variant", "default")
    info_string = [line for line in open(header).read().splitlines() if "POGO_BUILD_INFO_STRING" in line][0].split('"')[1]
    fake_image(os.path.join(art, "pogo_rcp.bin"), "0.1.0", "pogo_rcp", info_string)
    before = clean["rcp"]["source_fingerprint"]
    args = ["manifest-create", "--dir", art, "--component", "rcp", "--configuration", "esp-idf", "--variant", "default",
            "--primary", "pogo_rcp.bin", "--fingerprint-before", before, "pogo_rcp.bin"]
    write(root, cmake, original + "add_compile_definitions(POGO_REVIEW_PROBE=1)\n")  # ... while the build ran
    rc, _, err = tool(root, *args)
    check(rc == 2 and not os.path.exists(os.path.join(art, "manifest.json")), "a build input changed during the build: no manifest")
    check("changed while it was being built" in err, "and the reason given is that the inputs changed")
    write(root, cmake, original)
    rc, _, _ = tool(root, *args)
    check(rc == 0, "(with the inputs unchanged the manifest is written)")

    print("\n== build inputs: what the real build files name is covered ==")
    if os.path.isfile(os.path.join(REPO, "versions.json")):
        def named(build_files, pattern):
            found = set()
            for rel in build_files:
                for line in open(os.path.join(REPO, *rel.split("/")), encoding="utf-8"):
                    if not line.lstrip().startswith("#"):
                        found.update(re.findall(pattern, line))
            return found

        firmware_files = {c: [c + "/CMakeLists.txt", c + "/main/CMakeLists.txt"] for c in FIRMWARE}
        for c in FIRMWARE:
            listed = set(tool(REPO, "files", "--component", c)[1].split("\n"))
            wanted = {"tools/" + t for t in named(firmware_files[c], r"\.\./tools/([\w./-]+)")} | {"tools/pogo_version.py"}
            check(wanted and wanted <= listed, "%s: %s named by its CMake files are among its inputs" % (c, ", ".join(sorted(wanted))))
            uses_components = bool(named(firmware_files[c], r"(EXTRA_COMPONENT_DIRS)"))
            check(uses_components == any(f.startswith("components/") for f in listed),
                  "%s: components/ is an input exactly when the project adds it (%s)" % (c, "it does" if uses_components else "it does not"))
        bridge_files = ["linux/matter_bridge/app/BUILD.gn", "linux/matter_bridge/app/api/CMakeLists.txt", "linux/matter_bridge/app/core/CMakeLists.txt",
                        "linux/matter_bridge/ingest/CMakeLists.txt", "tools/build_matter_bridge.sh"]
        listed = set(tool(REPO, "files", "--component", "linux_bridge")[1].split("\n"))
        components = named(bridge_files, r"components/(\w+)")
        check(components and all(any(f.startswith("components/%s/" % name) for f in listed) for name in components),
              "linux_bridge: the %d components its build files name are among its inputs" % len(components))
        check({"tools/build_matter_bridge.sh", "tools/pogo_version.py", "versions.json"} <= listed, "linux_bridge: its build script, the identity tool and the version file too")
        everything = []
        for top in sorted(os.listdir(REPO)):
            if top == ".git":
                continue
            if not os.path.isdir(os.path.join(REPO, top)) or os.path.islink(os.path.join(REPO, top)):
                everything.append(top)
                continue
            for current, dirs, names in os.walk(os.path.join(REPO, top), followlinks=False):
                rel_dir = os.path.relpath(current, REPO).replace(os.sep, "/")
                everything += [rel_dir + "/" + n for n in names + [d for d in dirs if os.path.islink(os.path.join(current, d))]]
        check(all({rel for rel, _ in pv.source_files(REPO, c)} == {rel for rel in everything if pv.is_input(c, rel)} for c in ALL),
              "over all %d files of the work tree, the list that is fingerprinted and the predicate the dirty flag uses select the same files" % len(everything))
        check(not any(f.startswith("tools/") and f not in ("tools/build_matter_bridge.sh", "tools/pogo_version.py", "tools/cmake/pogo_version.cmake")
                      for c in ALL for f in tool(REPO, "files", "--component", c)[1].split("\n")),
              "and no other file of tools/ is an input of anything (tests, generators of configuration images, host stubs)")
    else:
        print("(no versions.json next to this test: real-repository checks skipped)")


def elf_header(machine):
    """A 64-bit little-endian ELF header as far as the tool reads one (e_machine at offset 18)."""
    return b"\x7fELF\x02\x01\x01\x00" + b"\0" * 8 + struct.pack("<HH", 2, machine) + b"\0" * 44


def run_manifest_against_binary(base):
    """Review finding 2 (2026-10-06): a manifest is checked against what the primary binary really
    says -- variant, configuration id and dirty state included -- and --expect-* comes on top of
    that, never instead of it."""
    root = os.path.join(base, "repo with space")
    os.makedirs(root)
    make_tree(root)
    git(root, "init", "-q")
    git(root, "add", "-A")
    git(root, "commit", "-q", "-m", "one")
    fp = pv.fingerprint(root, "power_base")
    config = "sdkconfig-sha256:" + "ab" * 32
    header = os.path.join(base, "pb.h")
    tool(root, "stamp", "--component", "power_base", "--out", header, "--variant", "ble", "--config-id", config)
    info_string = [line for line in open(header).read().splitlines() if "POGO_BUILD_INFO_STRING" in line][0].split('"')[1]
    art = os.path.join(base, "pb ble artifact")
    os.makedirs(art)
    fake_image(os.path.join(art, "power_base.bin"), "0.1.0", "power_base", info_string)
    write(art, "flash_args", "--flash_mode dio\n")
    create = ["manifest-create", "--dir", art, "--component", "power_base", "--configuration", "esp-idf", "--variant", "ble", "--config-id", config,
              "--primary", "power_base.bin", "--fingerprint-before", fp, "--toolchain", "esp-idf=v6.1", "power_base.bin", "flash_args"]
    manifest_path = os.path.join(art, "manifest.json")

    print("\n== manifest against the real binary: a normal artifact ==")
    rc, _, err = tool(root, *create)
    check(rc == 0, "a manifest is created" + ("" if rc == 0 else ": " + err.strip()))
    rc, out, _ = tool(root, "manifest-verify", "--dir", art)
    check(rc == 0 and "manifest verified" in out, "it verifies")
    rc, out, _ = tool(root, "manifest-verify", "--dir", art, "--expect-component", "power_base", "--expect-target", "esp32h2", "--expect-variant", "ble",
                      "--expect-version", "0.1.0")
    check(rc == 0, "also with everything the caller may expect")
    original = open(manifest_path).read()

    def verify_edited(mutate, *extra):
        d = json.loads(original)
        mutate(d)
        open(manifest_path, "w").write(json.dumps(d))
        rc, out, err = tool(root, "manifest-verify", "--dir", art, *extra)
        open(manifest_path, "w").write(original)
        return rc, out + err

    print("\n== manifest against the real binary: a plain BLE image labelled as the BOOT-button build ==")
    boot = "ble+reenroll_button_gpio9"
    rc, out = verify_edited(lambda d: d["build"].update(variant=boot))
    check(rc == 1, "a manifest that calls the plain BLE image '%s' is refused" % boot)
    check("variant" in out and "ble" in out, "because the binary itself says variant=ble")
    rc, out = verify_edited(lambda d: d["build"].update(variant=boot), "--expect-variant", boot)
    check(rc == 1, "and asking for exactly that wrong variant with --expect-variant does not make it pass")
    rc, out = verify_edited(lambda d: d["build"].update(variant=boot, config_id="unknown"), "--expect-variant", boot)
    check(rc == 1, "the reviewed case as a whole (variant relabelled, config id blanked, wrong variant expected) is refused")
    rc, out = verify_edited(lambda d: (d["build"].update(variant=boot), d["binary_says"]["build_info"].update(variant=boot)), "--expect-variant", boot)
    check(rc == 1, "editing the manifest's own record of what the binary says does not help: the binary is read again")

    print("\n== manifest against the real binary: every corresponding field ==")
    for mutate, why in ((lambda d: d["build"].update(config_id="unknown"), "a blanked config id"),
                        (lambda d: d["build"].update(config_id="sdkconfig-sha256:" + "cd" * 32), "another config id"),
                        (lambda d: d["binary_says"]["build_info"].update(variant="bench"), "a recorded binary statement that is not the binary's (variant)"),
                        (lambda d: d["binary_says"]["build_info"].update(config="unknown"), "a recorded binary statement that is not the binary's (config)"),
                        (lambda d: d["binary_says"]["app_description"].update(project_name="pogo_module"), "a recorded app description that is not the binary's"),
                        (lambda d: d["build"].update(target="esp32c6"), "another target than the image header's")):
        check(verify_edited(mutate)[0] == 1, "%s is refused" % why)
    check(verify_edited(lambda d: d["build"].update(target="esp32c6"), "--expect-target", "esp32c6")[0] == 1,
          "and a wrong target is refused even when the caller expects that very target")
    check(verify_edited(lambda d: d["build"].update(target="linux"))[0] == 1, "a firmware image labelled as a Linux build is refused")

    print("\n== manifest: required fields and their form ==")
    for path_ in (("build",), ("build", "variant"), ("build", "config_id"), ("build", "target"), ("build", "configuration"), ("build", "toolchain"),
                  ("source",), ("source", "origin"), ("source", "inputs_unchanged_during_build"), ("source", "dirty"), ("binary_says",),
                  ("binary_says", "build_info"), ("compatibility",), ("created_utc",), ("status",), ("primary",), ("files",), ("build_id",), ("build_kind",)):
        def drop(d, path_=path_):
            for key in path_[:-1]:
                d = d[key]
            del d[path_[-1]]
        check(verify_edited(drop)[0] == 1, "a manifest without %s is refused" % ".".join(path_))
    for mutate, why in ((lambda d: d["build"].update(variant=5), "a variant that is not a string"),
                        (lambda d: d["build"].update(variant=""), "an empty variant"),
                        (lambda d: d["build"].update(target=["esp32h2"]), "a target that is a list"),
                        (lambda d: d["build"].update(toolchain="gcc"), "a toolchain that is not an object"),
                        (lambda d: d["build"].update(config_id=None), "a null config id"),
                        (lambda d: d.update(build="esp32h2"), "a build section that is a string"),
                        (lambda d: d.update(source=[]), "a source section that is a list"),
                        (lambda d: d["source"].update(origin="somewhere"), "an unknown origin"),
                        (lambda d: d["source"].update(inputs_unchanged_during_build="yes"), "an inputs flag that is not a boolean"),
                        (lambda d: d.update(version=1), "a version that is a number"),
                        (lambda d: d.update(files={}), "files that are not a list"),
                        (lambda d: d["files"][0].pop("sha256"), "a file entry without its hash"),
                        (lambda d: d["files"][0].update(size="12"), "a file size that is a string"),
                        (lambda d: d["files"][1].update(sha256="xyz"), "a hash that is not a SHA-256"),
                        (lambda d: d.update(binary_says="power_base 0.1.0"), "a binary statement that is not an object"),
                        (lambda d: d.update(created_utc=20261006), "a creation time that is not a string"),
                        (lambda d: d.update(primary="flash_args"), "a primary that is not the binary")):
        check(verify_edited(mutate)[0] == 1, "%s is refused" % why)
    check(tool(root, "manifest-verify", "--dir", art)[0] == 0, "(the unedited manifest still verifies)")

    print("\n== manifest: the earlier checks still hold ==")
    image = os.path.join(art, "power_base.bin")
    data = open(image, "rb").read()
    open(image, "wb").write(data[:-1] + b"\x01")
    check(tool(root, "manifest-verify", "--dir", art)[0] == 1, "a changed image is refused")
    open(image, "wb").write(data)
    os.rename(os.path.join(art, "flash_args"), os.path.join(art, "flash_args.gone"))
    check(tool(root, "manifest-verify", "--dir", art)[0] == 1, "a missing file is refused")
    os.rename(os.path.join(art, "flash_args.gone"), os.path.join(art, "flash_args"))
    for extra, why in ((("--expect-component", "module"), "component"), (("--expect-target", "esp32c6"), "target"), (("--expect-variant", "bench"), "variant"),
                       (("--expect-version", "0.2.0"), "version"), (("--release",), "build kind")):
        check(tool(root, "manifest-verify", "--dir", art, *extra)[0] == 1, "an artifact that is not the %s the caller expects is refused" % why)

    print("\n== manifest: a binary whose variant, config or target is not the request's gets none ==")
    os.remove(manifest_path)

    def create_refused(args, why, needle=None):
        rc, _, err = tool(root, *args)
        check(rc == 2 and not os.path.exists(manifest_path) and (needle is None or needle in err), why)

    create_refused([boot if a == "ble" else a for a in create], "a manifest for another variant than the binary's is not written", "variant")
    create_refused(["sdkconfig-sha256:" + "cd" * 32 if a == config else a for a in create], "nor for another config id", "config")
    create_refused(create + ["--target", "esp32c6"], "nor for another target than the image header's", "esp32")
    fake_image(image, "0.1.0", "power_base", info_string, chip=ESP32C6)
    create_refused(create, "an image built for another chip than the component's gets no manifest", "esp32c6")
    fake_image(image, "0.1.0", "power_base", info_string)
    rc, _, _ = tool(root, *[a for a in create if a not in ("--variant", "ble", "--config-id", config)])
    m = json.load(open(manifest_path)) if rc == 0 else {"build": {}}
    check(rc == 0 and m["build"].get("variant") == "ble" and m["build"].get("config_id") == config and m["build"].get("target") == "esp32h2",
          "when the caller does not say, variant, config id and target are taken from the binary, not from a default")

    print("\n== manifest: the same artifact, another question -- is it a build of the current tree? ==")
    rc, out, _ = tool(root, "manifest-verify", "--dir", art, "--match-current-tree")
    check(rc == 0 and "current tree" in out, "a fresh artifact is a build of the current tree")
    write(root, "tools/cmake/pogo_version.cmake", "# changed after the build\n")
    rc, out, _ = tool(root, "manifest-verify", "--dir", art)
    check(rc == 0, "after the tree moved on the artifact is still what its manifest says (its own integrity)")
    rc, out, _ = tool(root, "manifest-verify", "--dir", art, "--match-current-tree")
    check(rc == 1 and "current tree" in out, "but it is no longer a build of the current tree, and --match-current-tree says so")
    git(root, "checkout", "-q", "--", ".")

    print("\n== manifest: an executable that is not an ESP-IDF image (the bridge) ==")
    hub = os.path.join(base, "bridge artifact")
    os.makedirs(hub)
    header = os.path.join(base, "bridge.h")
    tool(root, "stamp", "--component", "linux_bridge", "--out", header, "--variant", "gn-linux")
    bridge_string = [line for line in open(header).read().splitlines() if "POGO_BUILD_INFO_STRING" in line][0].split('"')[1]
    open(os.path.join(hub, "pogo_matter_bridge"), "wb").write(elf_header(183) + b"\0" * 64 + bridge_string.encode() + b"\0")  # EM_AARCH64
    bridge_create = ["manifest-create", "--dir", hub, "--component", "linux_bridge", "--configuration", "gn", "--primary", "pogo_matter_bridge", "pogo_matter_bridge"]
    rc, _, err = tool(root, *bridge_create)
    m = json.load(open(os.path.join(hub, "manifest.json"))) if rc == 0 else {"build": {}}
    check(rc == 0 and m["build"].get("target") == "linux-arm64" and m["build"].get("variant") == "gn-linux",
          "an ELF executable for AArch64: target linux-arm64 and the variant, both read from the binary")
    rc, out, _ = tool(root, "manifest-verify", "--dir", hub, "--expect-component", "linux_bridge")
    check(rc == 0, "it verifies")
    check("not verified" in out or "not checked" in out, "and the report names what a manifest check cannot establish")
    bridge_manifest = os.path.join(hub, "manifest.json")
    bridge_original = open(bridge_manifest).read() if os.path.isfile(bridge_manifest) else None
    for target, why in (("linux-x86_64", "another architecture"), ("esp32h2", "a chip"), ("macos-arm64", "another executable format")):
        refused = False
        if bridge_original is not None:
            d = json.loads(bridge_original)
            d["build"]["target"] = target
            open(bridge_manifest, "w").write(json.dumps(d))
            refused = tool(root, "manifest-verify", "--dir", hub)[0] == 1
        check(refused, "labelled as %s (%s) it is refused" % (target, why))
    if bridge_original is not None:
        os.remove(bridge_manifest)
    rc, _, err = tool(root, *(bridge_create[:-1] + ["--target", "linux-x86_64", "pogo_matter_bridge"]))
    check(rc == 2 and not os.path.exists(os.path.join(hub, "manifest.json")), "a manifest that would call the AArch64 executable x86_64 is not written")
    rc, _, _ = tool(root, *[a for a in bridge_create if a not in ("--primary",)][:-1])
    check(rc == 2, "a manifest must name the binary it is about (no --primary: refused)")


if __name__ == "__main__":
    sys.exit(main())
