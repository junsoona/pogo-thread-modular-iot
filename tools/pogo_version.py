#!/usr/bin/env python3
"""
POGO version and build-identity tool (docs/versioning.md).

One implementation of everything that connects a source tree to a built artifact:

  versions      the product versions of the firmware repository's components, read from the ONE
                definition file, versions.json (never guessed, never written by this tool);
  fingerprint   a deterministic hash of a component's build inputs -- its sources AND the build
                tooling in the repository that its build runs (COMPONENT_SOURCES names them one by
                one; this file is among them): sorted relative paths and contents, untracked files
                included, generated files and output excluded -- so that two different uncommitted
                trees on the same HEAD are two different builds;
  identity      commit (full SHA), clean/dirty, fingerprint, build id; "unknown" where it cannot
                be known (a tree without Git and without verifiable SOURCE_INFO.json). Clean/dirty
                is decided over exactly the files of the fingerprint;
  stamp         the C header a build compiles in (written only when its content changes, so an
                incremental build picks up new metadata and nothing rebuilds for nothing);
  manifest      a machine-checkable description of a directory of built files (relative path,
                size, SHA-256) with the metadata above, and its verification against the files
                AND against the primary binary itself, read again every time: version, build id,
                kind, commit, fingerprint, dirty, variant, config id (the embedded
                POGO_BUILD_INFO string), the ESP-IDF app description, and the target the image or
                executable header is for. --expect-* is checked on top of that;
  record        a template for an install / hardware-test record (public part and private part).

What it never does: it never writes into the source tree (except with an explicit --out path
given by the caller), never changes the Git index, never bumps a version, never deploys, and
never reads a secret configuration image.

Usage (from anywhere; paths may contain spaces):
  tools/pogo_version.py versions [--json]
  tools/pogo_version.py check                      # versions.json is well formed
  tools/pogo_version.py identity --component power_base [--json]
  tools/pogo_version.py fingerprint --component power_base
  tools/pogo_version.py stamp --component power_base --out <file.h> [--variant ble] [...]
  tools/pogo_version.py source-info --out <SOURCE_INFO.json>     # for a source bundle without .git
  tools/pogo_version.py manifest-create --dir <artifact dir> --component power_base ... files...
  tools/pogo_version.py manifest-verify --dir <artifact dir> [--expect-component X] [--release]
                                        [--match-current-tree]   # also: is it a build of this tree?
  tools/pogo_version.py appdesc <app.bin>          # what an ESP-IDF image says about itself
  tools/pogo_version.py record-template --out <dir>
  tools/pogo_version.py build-firmware --component power_base --variant ble --out <NEW dir> [--release]
  tools/pogo_version.py legacy-inventory --dir <old build dirs' parent> [--out <file.md>]
"""
import argparse
import datetime
import fnmatch
import hashlib
import json
import os
import re
import struct
import subprocess
import sys

sys.dont_write_bytecode = True

TOOL_DIR = os.path.dirname(os.path.abspath(__file__))
DEFAULT_ROOT = os.path.dirname(TOOL_DIR)

VERSIONS_FILE = "versions.json"
VERSIONS_SCHEMA = 1
MANIFEST_SCHEMA = 1
SOURCE_INFO_FILE = "SOURCE_INFO.json"
SOURCE_INFO_SCHEMA = 1
FINGERPRINT_DOMAIN = b"pogo-source-fingerprint-v1\0"
UNKNOWN = "unknown"

SEMVER = re.compile(r"^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$")
SHA40 = re.compile(r"^[0-9a-f]{40}$")
HEX64 = re.compile(r"^[0-9a-f]{64}$")

# ESP-IDF esp_app_desc_t (components/esp_app_format/include/esp_app_desc.h, checked against v6.1):
#   magic_word(4) secure_version(4) reserv1(8) version[32] project_name[32] time[16] date[16]
#   idf_ver[32] app_elf_sha256[32] ...; it is the first thing in the first segment, i.e. at
#   offset 24 (image header) + 8 (segment header) of the .bin.
APP_DESC_OFFSET = 32
APP_DESC_MAGIC = 0xABCD5432
APP_DESC_VERSION_LEN = 32  # including the terminating NUL: 31 usable characters

# The build inputs of each component, relative to the repository root: everything in the
# repository that its build reads -- what a compiler or code generator reads, AND the build
# tooling that decides how it is compiled or what the build is told about itself. Each entry is a
# directory or one file, named explicitly; tools/ as a whole is not an input (tests, generators of
# configuration images and host stubs build nothing here).
#
#   tools/cmake/pogo_version.cmake  included by the three ESP-IDF projects' CMakeLists.txt
#                                   (PROJECT_VER, the stamp target; it can add compile options)
#   tools/pogo_version.py           this file: it writes the identity header every build compiles
#                                   in, and picks the sdkconfig overlays of a variant
#   tools/build_matter_bridge.sh    the bridge's GN build (args, generated files)
#
# The fingerprint, the dirty flag and the before/after comparison of a build all use exactly this
# set (source_files / is_input). tools/test_pogo_version.py checks that whatever the real build
# files name is covered here.
_IDENTITY_TOOL = "tools/pogo_version.py"
_SHARED_CMAKE = "tools/cmake/pogo_version.cmake"
COMPONENT_SOURCES = {
    "power_base": ["power_base", "components", VERSIONS_FILE, _SHARED_CMAKE, _IDENTITY_TOOL],
    "module": ["module", "components", VERSIONS_FILE, _SHARED_CMAKE, _IDENTITY_TOOL],
    # The RCP shares no component with the other projects (tools/test_rcp_config.py).
    "rcp": ["rcp", VERSIONS_FILE, _SHARED_CMAKE, _IDENTITY_TOOL],
    # The bridge compiles six of the components (BUILD.gn, the CMake host builds); the directory
    # is an input as a whole, which also keeps the headers the manifest's compatibility facts are
    # read from inside its fingerprint.
    "linux_bridge": ["linux/matter_bridge", "components", "tools/build_matter_bridge.sh", VERSIONS_FILE, _IDENTITY_TOOL],
}
# The chip a firmware component is built for. The bridge has no fixed target: its manifest says
# what the executable's own header says (linux-arm64, ...).
COMPONENT_TARGETS = {"power_base": "esp32h2", "module": "esp32h2", "rcp": "esp32h2"}
# esp_chip_id_t (bootloader_support/include/esp_app_format.h, checked against ESP-IDF v6.1): the
# 16-bit chip id at offset 12 of an application image's header.
IMAGE_CHIP_ID_OFFSET = 12
ESP_CHIP_IDS = {0x0000: "esp32", 0x0002: "esp32s2", 0x0005: "esp32c3", 0x0009: "esp32s3", 0x000C: "esp32c2", 0x000D: "esp32c6",
                0x0010: "esp32h2", 0x0012: "esp32p4", 0x0014: "esp32c61", 0x0017: "esp32c5", 0x0019: "esp32h21", 0x001C: "esp32h4",
                0x0020: "esp32s31"}
ELF_MACHINES = {183: "arm64", 62: "x86_64", 40: "arm", 243: "riscv", 3: "x86"}
MACHO_CPUS = {0x0100000C: "arm64", 0x01000007: "x86_64"}
COMPONENT_PROJECT_NAMES = {"power_base": "power_base", "module": "pogo_module", "rcp": "pogo_rcp"}

# Never part of a fingerprint: build output, local configuration, generated files, logs, caches
# and the metadata this tool itself produces (which would otherwise feed back into the hash).
EXCLUDED_DIR_NAMES = {"build", "managed_components", "__pycache__", ".git", ".vscode", "out", "generated", ".build", "DerivedData"}
EXCLUDED_REL_DIRS = {"linux/matter_bridge/app/third_party/connectedhomeip"}
EXCLUDED_FILE_PATTERNS = [
    "sdkconfig", "sdkconfig.old", ".DS_Store", "*.pyc", "*.bin", "*.elf", "*.map", "*.log", "*.orig", "*.rej",
    "pogo-matter-bridge.zap", "pogo_build_info_gen.h", "manifest.json", SOURCE_INFO_FILE,
]


class ToolError(Exception):
    pass


def fail(message):
    raise ToolError(message)


# ---------------------------------------------------------------- versions

def load_versions(root):
    path = os.path.join(root, VERSIONS_FILE)
    try:
        with open(path, encoding="utf-8") as f:
            data = json.load(f)
    except OSError as e:
        fail("cannot read %s: %s" % (path, e))
    except ValueError as e:
        fail("%s is not JSON: %s" % (path, e))
    if not isinstance(data, dict) or data.get("schema") != VERSIONS_SCHEMA:
        fail("%s: schema must be %d" % (path, VERSIONS_SCHEMA))
    components = data.get("components")
    if not isinstance(components, dict):
        fail("%s: 'components' must be an object" % path)
    if set(components) != set(COMPONENT_SOURCES):
        fail("%s: components must be exactly %s (found %s)" % (path, sorted(COMPONENT_SOURCES), sorted(components)))
    out = {}
    for name, entry in components.items():
        version = entry.get("version") if isinstance(entry, dict) else None
        if not isinstance(version, str) or not SEMVER.match(version):
            fail("%s: %s.version must be MAJOR.MINOR.PATCH with no suffix (found %r)" % (path, name, version))
        if len(version) >= APP_DESC_VERSION_LEN:
            fail("%s: %s.version does not fit the %d-byte app description field" % (path, name, APP_DESC_VERSION_LEN))
        out[name] = version
    return out


# ---------------------------------------------------------------- fingerprint

def _excluded_file(name):
    return any(fnmatch.fnmatchcase(name, pattern) for pattern in EXCLUDED_FILE_PATTERNS)


def _excluded(rel, is_dir=False):
    """Whether a path (relative to the root, POSIX) is never a build input, whatever scope it is
    in: inside an excluded directory, or a file with an excluded name. A symbolic link counts as a
    file, also when it points at a directory."""
    parts = rel.split("/")
    if any(part in EXCLUDED_DIR_NAMES for part in (parts if is_dir else parts[:-1])):
        return True
    if any(rel == prefix or rel.startswith(prefix + "/") for prefix in EXCLUDED_REL_DIRS):
        return True
    return not is_dir and _excluded_file(parts[-1])


def is_input(component, rel):
    """Whether the file `rel` (relative to the root, POSIX; it need not exist any more) is one of
    the component's build inputs. This is the one definition: source_files() lists exactly the
    existing files it accepts, and the dirty flag asks it about every path Git reports."""
    for scope in COMPONENT_SOURCES[component]:
        if rel == scope or rel.startswith(scope + "/"):
            return not _excluded(rel)
    return False


def source_files(root, component):
    """Sorted list of (relative POSIX path, absolute path) of the component's build inputs that
    exist in this tree."""
    if component not in COMPONENT_SOURCES:
        fail("unknown component %r (known: %s)" % (component, ", ".join(sorted(COMPONENT_SOURCES))))
    found = {}
    for scope in COMPONENT_SOURCES[component]:
        top = os.path.join(root, *scope.split("/"))
        if os.path.islink(top) or os.path.isfile(top):
            if not _excluded(scope):
                found[scope] = top
            continue
        if not os.path.isdir(top) or _excluded(scope, is_dir=True):
            continue  # an input that does not exist in this tree contributes nothing (and is dirty in Git)
        for current, dirs, files in os.walk(top, followlinks=False):
            rel_dir = os.path.relpath(current, root).replace(os.sep, "/")
            keep = []
            for d in dirs:
                rel = rel_dir + "/" + d
                if os.path.islink(os.path.join(current, d)):
                    files.append(d)  # recorded as a link, never followed
                elif not _excluded(rel, is_dir=True):
                    keep.append(d)
            dirs[:] = keep
            for name in files:
                rel = rel_dir + "/" + name
                if not _excluded(rel):
                    found[rel] = os.path.join(current, name)
    return sorted(found.items(), key=lambda item: item[0].encode("utf-8"))


def fingerprint(root, component, files=None):
    """SHA-256 over the component's build inputs: domain, component name, then for every file in
    the byte order of its relative path  'F' path NUL size NUL content  (or 'L' path NUL target NUL
    for a symbolic link). No timestamps, no permissions, no absolute paths, no Git."""
    h = hashlib.sha256()
    h.update(FINGERPRINT_DOMAIN)
    h.update(component.encode("utf-8") + b"\0")
    count = 0
    for rel, full in (source_files(root, component) if files is None else files):
        encoded = rel.encode("utf-8")
        if os.path.islink(full):
            h.update(b"L" + encoded + b"\0" + os.readlink(full).encode("utf-8") + b"\0")
        else:
            size = os.path.getsize(full)
            h.update(b"F" + encoded + b"\0" + str(size).encode("ascii") + b"\0")
            with open(full, "rb") as f:
                read = 0
                while True:
                    chunk = f.read(1 << 20)
                    if not chunk:
                        break
                    read += len(chunk)
                    h.update(chunk)
            if read != size:
                fail("%s changed while it was being read" % rel)
        count += 1
    if count == 0:
        fail("component %s has no source files under %s" % (component, root))
    return h.hexdigest()


# ---------------------------------------------------------------- git

def _git(root, *args):
    """Read-only Git queries. GIT_OPTIONAL_LOCKS=0 keeps even `status` from touching the index."""
    env = dict(os.environ)
    env["GIT_OPTIONAL_LOCKS"] = "0"
    try:
        p = subprocess.run(["git", "-C", root] + list(args), capture_output=True, text=True, env=env)
    except OSError:
        return None
    return p.stdout if p.returncode == 0 else None


def git_state(root, component, files=None):
    """(commit, dirty) from Git, or None when `root` is not the top of a Git work tree.

    Dirty means: the component's build inputs are not exactly what HEAD has. It is decided over
    the same set of files as the fingerprint -- a path Git reports counts when is_input() accepts
    it, whether it is modified, staged, deleted, renamed, untracked or ignored by Git. (A file
    that Git ignores but the build reads is in the fingerprint, so the tree is not the commit.)"""
    top = _git(root, "rev-parse", "--show-toplevel")
    if top is None or os.path.realpath(top.strip()) != os.path.realpath(root):
        return None
    commit = _git(root, "rev-parse", "--verify", "HEAD")
    if commit is None or not SHA40.match(commit.strip()):
        return None
    status = _git(root, "status", "--porcelain=v1", "-z", "--untracked-files=all", "--ignored=matching")
    if status is None:
        return None
    existing = None
    entries = status.split("\0")
    i = 0
    while i < len(entries):
        entry = entries[i]
        i += 1
        if len(entry) < 4:
            continue
        code, paths = entry[:2], [entry[3:]]
        if "R" in code or "C" in code:  # "XY new NUL old NUL"
            paths.append(entries[i] if i < len(entries) else "")
            i += 1
        for path in paths:
            if path.endswith("/"):
                # A directory Git lists as one entry (ignored as a whole): dirty when any existing
                # input lies inside it.
                if existing is None:
                    existing = [rel for rel, _ in (source_files(root, component) if files is None else files)]
                if any(rel.startswith(path) for rel in existing):
                    return commit.strip(), True
            elif path and is_input(component, path):
                return commit.strip(), True
    return commit.strip(), False


def identity(root, component):
    """Everything that identifies the source of one component. Never invents a commit."""
    versions = load_versions(root)
    files = source_files(root, component)
    fp = fingerprint(root, component, files)
    info = {
        "component": component,
        "version": versions[component],
        "source_fingerprint": fp,
        "commit": UNKNOWN,
        "dirty": UNKNOWN,
        "origin": UNKNOWN,
    }
    state = git_state(root, component, files)
    if state is not None:
        info["commit"], info["dirty"] = state
        info["origin"] = "git"
    else:
        # A source bundle: its origin is kept only when the bundle says what it was made from AND
        # these very build inputs (sources and build tooling) still hash to what it recorded.
        recorded = _read_source_info(root)
        entry = (recorded or {}).get("components", {}).get(component)
        if entry and entry.get("source_fingerprint") == fp and SHA40.match(str(entry.get("commit", ""))) \
                and isinstance(entry.get("dirty"), bool):
            info["commit"] = entry["commit"]
            info["dirty"] = entry["dirty"]
            info["origin"] = "source-info-verified"
    info["build_id"] = build_id(info["commit"], info["dirty"], fp)
    return info


def build_id(commit, dirty, fp):
    """<commit12>[+dirty].<fingerprint12> -- at most 31 characters, so it fits a 32-byte field.
    'unknown' in place of the commit when it is not known."""
    head = commit[:12] if SHA40.match(commit or "") else UNKNOWN
    if dirty is True:
        head += "+dirty"
    elif dirty is not False:
        head += "+?" if head != UNKNOWN else ""
    return head + "." + fp[:12]


def _read_source_info(root):
    path = os.path.join(root, SOURCE_INFO_FILE)
    try:
        with open(path, encoding="utf-8") as f:
            data = json.load(f)
    except (OSError, ValueError):
        return None
    if not isinstance(data, dict) or data.get("schema") != SOURCE_INFO_SCHEMA or not isinstance(data.get("components"), dict):
        return None
    return data


# ---------------------------------------------------------------- build kind

def build_kind(info, release):
    """'release' only for a clean tree with a known commit, asked for explicitly; else
    'development'. A Release compiler configuration is not what makes a build a release."""
    clean = info["dirty"] is False and SHA40.match(info["commit"] or "") is not None
    if release:
        if not clean:
            fail("release mode needs a clean tree with a known commit: %s is %s (commit %s). "
                 "Commit the work first, or build without --release (a development build)."
                 % (info["component"], "dirty" if info["dirty"] is True else "of unknown state", info["commit"]))
        return "release"
    return "development"


# ---------------------------------------------------------------- stamp

def _c_string(value):
    return '"' + str(value).replace("\\", "\\\\").replace('"', '\\"') + '"'


def stamp_text(info, kind, variant, config_id):
    dirty = {True: "1", False: "0"}.get(info["dirty"], "-1")
    summary = "POGO_BUILD_INFO{component=%s;version=%s;build=%s;kind=%s;variant=%s;commit=%s;dirty=%s;fingerprint=%s;config=%s}" % (
        info["component"], info["version"], info["build_id"], kind, variant, info["commit"],
        {True: "true", False: "false"}.get(info["dirty"], UNKNOWN), info["source_fingerprint"], config_id)
    lines = [
        "/* Generated by tools/pogo_version.py stamp. Do not edit, do not commit. */",
        "#pragma once",
        "#define POGO_BUILD_COMPONENT " + _c_string(info["component"]),
        "#define POGO_BUILD_VERSION " + _c_string(info["version"]),
        "#define POGO_BUILD_ID " + _c_string(info["build_id"]),
        "#define POGO_BUILD_KIND " + _c_string(kind),
        "#define POGO_BUILD_VARIANT " + _c_string(variant),
        "#define POGO_BUILD_COMMIT " + _c_string(info["commit"]),
        "#define POGO_BUILD_DIRTY " + dirty + " /* 1 dirty, 0 clean, -1 unknown */",
        "#define POGO_BUILD_SOURCE_FINGERPRINT " + _c_string(info["source_fingerprint"]),
        "#define POGO_BUILD_CONFIG_ID " + _c_string(config_id),
        "#define POGO_BUILD_INFO_STRING " + _c_string(summary),
        "",
    ]
    return "\n".join(lines)


def write_if_changed(path, text):
    try:
        with open(path, encoding="utf-8") as f:
            if f.read() == text:
                return False
    except OSError:
        pass
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        f.write(text)
    os.replace(tmp, path)
    return True


def sdkconfig_identity(sdkconfig_path, component=None):
    """(variant, config id) from a generated ESP-IDF sdkconfig: which POGO build configuration it
    is, and a hash of the whole file. The file holds no secret of this project (no setup code,
    PSKd, Dataset or key is ever put into Kconfig); only its hash and a few flags are used."""
    if not sdkconfig_path or not os.path.isfile(sdkconfig_path):
        return "unknown", UNKNOWN
    with open(sdkconfig_path, "rb") as f:
        raw = f.read()
    text = raw.decode("utf-8", "replace")

    def on(name):
        return re.search(r"(?m)^%s=y$" % re.escape(name), text) is not None

    gpio = re.search(r"(?m)^CONFIG_POWER_BASE_REENROLL_BUTTON_GPIO=(\d+)$", text)
    if component == "rcp":
        # Both RCP builds log nothing (the USB port is the Spinel link). The diagnostic one is the
        # one that keeps a core dump in flash (rcp/sdkconfig.diag).
        return ("diag" if on("CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH") else "default"), "sdkconfig-sha256:" + hashlib.sha256(raw).hexdigest()
    if on("CONFIG_POWER_BASE_REENROLL_BUTTON"):
        variant = "ble+reenroll_button_gpio" + (gpio.group(1) if gpio else "?")
    elif on("CONFIG_POWER_BASE_BLE_ENROLLMENT"):
        variant = "ble"
    elif on("CONFIG_POWER_BASE_BENCH_JOINER_CONSOLE"):
        variant = "bench"
    else:
        variant = "default"
    return variant, "sdkconfig-sha256:" + hashlib.sha256(raw).hexdigest()


# ---------------------------------------------------------------- binaries

def read_app_desc(path):
    """What an ESP-IDF application image says about itself."""
    with open(path, "rb") as f:
        head = f.read(APP_DESC_OFFSET + 256)
    if len(head) < APP_DESC_OFFSET + 256:
        fail("%s is too short to be an application image" % path)
    magic, = struct.unpack_from("<I", head, APP_DESC_OFFSET)
    if magic != APP_DESC_MAGIC:
        fail("%s has no ESP-IDF application description (not an app image)" % path)

    def text(offset, length):
        return head[APP_DESC_OFFSET + offset:APP_DESC_OFFSET + offset + length].split(b"\0", 1)[0].decode("utf-8", "replace")

    return {"version": text(16, 32), "project_name": text(48, 32), "idf_ver": text(112, 32)}


def read_image_kind(path):
    """What kind of binary a file is, as far as its own header says: an ESP-IDF application image
    with its chip, an ELF executable with its machine, or a Mach-O one. `target` is the name this
    tool uses for it. None when the header is none of these.

    An ELF header names an architecture, not an operating system: "linux-<arch>" is this
    repository's name for the bridge's executables, and the header proves only the <arch> part."""
    with open(path, "rb") as f:
        head = f.read(APP_DESC_OFFSET + 8)
    if len(head) >= APP_DESC_OFFSET + 4 and head[0] == 0xE9 and struct.unpack_from("<I", head, APP_DESC_OFFSET)[0] == APP_DESC_MAGIC:
        chip, = struct.unpack_from("<H", head, IMAGE_CHIP_ID_OFFSET)
        return {"format": "esp-idf-app", "chip_id": "0x%04x" % chip, "target": ESP_CHIP_IDS.get(chip, "chip-0x%04x" % chip)}
    if head[:4] == b"\x7fELF" and len(head) >= 20:
        machine, = struct.unpack_from("<H" if head[5] == 1 else ">H", head, 18)
        arch = ELF_MACHINES.get(machine, "machine-%d" % machine)
        return {"format": "elf", "arch": arch, "target": "linux-" + arch}
    if head[:4] == b"\xcf\xfa\xed\xfe" and len(head) >= 8:
        cpu, = struct.unpack_from("<I", head, 4)
        arch = MACHO_CPUS.get(cpu, "cpu-0x%08x" % cpu)
        return {"format": "mach-o", "arch": arch, "target": "macos-" + arch}
    if head[:4] in (b"\xca\xfe\xba\xbe", b"\xca\xfe\xba\xbf"):
        return {"format": "mach-o", "arch": "universal", "target": "macos-universal"}
    return None


def target_problem(component, target, image):
    """Why `target` is not a target this component's binary `image` (read_image_kind) can be said
    to be built for; None when it is consistent with what the header shows."""
    if image is None:
        return "its header is neither an ESP-IDF application image nor an ELF or Mach-O executable, so no target can be read from it"
    if component in COMPONENT_TARGETS:
        if image["format"] != "esp-idf-app":
            return "it is not an ESP-IDF application image (%s), which %s must be" % (image["format"], component)
        if image["target"] != COMPONENT_TARGETS[component]:
            return "its image header is for %s (chip id %s); %s is built for %s" % (image["target"], image["chip_id"], component, COMPONENT_TARGETS[component])
        if target != image["target"]:
            return "its image header is for %s (chip id %s), not %r" % (image["target"], image["chip_id"], target)
        return None
    if image["format"] == "esp-idf-app":
        return "it is an ESP-IDF application image, which %s is not" % component
    if target == image["target"] or (isinstance(target, str) and target.startswith(image["target"] + "-")):
        return None
    if target == "linux" and image["format"] == "elf":
        return None  # the architecture is simply not stated
    return "its header is %s for %s (%s), not %r" % (image["format"].upper() if image["format"] == "elf" else "Mach-O", image["arch"], image["target"], target)


def read_binary(path, component):
    """Everything a built binary says about itself: the embedded POGO_BUILD_INFO fields, for a
    firmware component the ESP-IDF application description, and what its header is."""
    embedded = read_embedded_build_info(path)
    if embedded is None:
        fail("%s carries no POGO_BUILD_INFO string: it was not built with the version stamp" % os.path.basename(path))
    says = {"build_info": embedded}
    if component in COMPONENT_PROJECT_NAMES:
        says["app_description"] = read_app_desc(path)
    image = read_image_kind(path)
    if image is not None:
        says["image"] = image
    return says


BUILD_INFO_RE = re.compile(rb"POGO_BUILD_INFO\{([ -z|~]{1,400})\}")


def read_embedded_build_info(path):
    """The POGO_BUILD_INFO{...} string a binary was compiled with; None when it has none.
    Fails when it has more than one different string (two builds mixed in one file)."""
    with open(path, "rb") as f:
        data = f.read()
    found = {m.group(1) for m in BUILD_INFO_RE.finditer(data)}
    if not found:
        return None
    if len(found) > 1:
        fail("%s contains %d different POGO_BUILD_INFO strings" % (path, len(found)))
    fields = {}
    for part in found.pop().decode("ascii").split(";"):
        key, _, value = part.partition("=")
        fields[key] = value
    return fields


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


# ---------------------------------------------------------------- compatibility facts

def compatibility(root):
    """Protocol and schema facts read out of the sources a build was made from -- never derived
    from a product version. A fact that cannot be found is an error, not a blank."""

    def read(rel):
        with open(os.path.join(root, *rel.split("/")), encoding="utf-8") as f:
            return f.read()

    def one(text, pattern, what):
        m = re.search(pattern, text)
        if not m:
            fail("compatibility: cannot find %s in the sources" % what)
        return m.group(1)

    proto = read("components/power_base_ble_enroll/include/power_base_ble_enroll/pb_ble_proto.h")
    enroll = read("components/power_base_enrollment/include/power_base_enrollment/power_base_enrollment.h")
    mgmt = read("components/power_base_mgmt/include/power_base_mgmt/pb_mgmt_proto.h")
    out = {
        "ble_enroll_proto": int(one(proto, r"#define PB_BLE_PROTO_VERSION (\d+)u", "PB_BLE_PROTO_VERSION")),
        "ble_capabilities": sorted(re.findall(r'#define PB_BLE_CAP_\w+ "(\w+)"', proto)),
        "ble_messages": sorted(set(re.findall(r"#define PB_BLE_MSG_\w+_REQ (0x[0-9a-fA-F]{2})u", proto))),
        "enrollment_record_versions": [1, 2, 3] if "PB_ENROLL_RECORD_V3_LEN" in enroll else [1, 2],
        "mgmt_proto": int(one(mgmt, r"#define PB_MGMT_PROTO_VERSION (\d+)u?", "PB_MGMT_PROTO_VERSION")),
        # The management protocol grew by message types and capability bits without a new version
        # number (HELLO, 2026-10-08): what a build speaks is these two lists, not the number alone.
        "mgmt_messages": sorted(set(re.findall(r"PB_MGMT_TYPE_\w+ = (0x[0-9a-fA-F]{2})", mgmt))),
        "mgmt_capabilities": sorted(name.lower() for name in re.findall(r"#define PB_MGMT_((?:BOARD|HUB)_CAP_\w+) 0x", mgmt)),
        "http_api": "pogohome-api-v1",
    }
    reenroll_path = os.path.join(root, "components", "power_base_reenroll", "include", "power_base_reenroll", "pb_reenroll_store.h")
    if os.path.isfile(reenroll_path):
        with open(reenroll_path, encoding="utf-8") as f:
            out["reenroll_journal_format"] = int(one(f.read(), r"#define PB_REENROLL_FORMAT_VERSION (\d+)u", "PB_REENROLL_FORMAT_VERSION"))
    return out


# ---------------------------------------------------------------- manifest

SECRET_NAME_HINTS = ["setup-code", "setup_code", "pskd", "dataset", "network_key", "networkkey", "mgmt_key", "token",
                     "cfg_images", "_config.bin", "credential", "secret", ".pem", ".key", "sdkconfig"]


def _looks_secret(rel):
    low = rel.lower()
    return any(hint in low for hint in SECRET_NAME_HINTS)


def manifest_create(args):
    root = args.root
    directory = os.path.abspath(args.dir)
    if not os.path.isdir(directory):
        fail("%s is not a directory" % directory)
    info = identity(root, args.component)
    kind = build_kind(info, args.release)
    if args.fingerprint_before and args.fingerprint_before != info["source_fingerprint"]:
        fail("the sources of %s changed while it was being built (fingerprint before %s, after %s): "
             "this output is not a build of any one source tree and gets no manifest"
             % (args.component, args.fingerprint_before[:12], info["source_fingerprint"][:12]))
    if not args.files:
        fail("no files given")
    files = []
    primary = None
    for rel in args.files:
        rel = rel.replace(os.sep, "/")
        full = os.path.join(directory, *rel.split("/"))
        if os.path.isabs(rel) or ".." in rel.split("/"):
            fail("file paths are relative to --dir: %s" % rel)
        if not os.path.isfile(full) or os.path.islink(full):
            fail("missing file: %s" % rel)
        if _looks_secret(rel) and not args.allow_sensitive_names:
            fail("%s looks like a configuration or secret file; a public manifest never lists one "
                 "(not even its hash). Keep it in the private record instead." % rel)
        files.append({"path": rel, "size": os.path.getsize(full), "sha256": sha256_file(full)})
        if primary is None and rel == args.primary:
            primary = full
    if not args.primary:
        fail("--primary is required: a manifest is about one binary, and it is that binary's own word that is checked")
    if primary is None:
        fail("--primary %s is not among the files" % args.primary)

    # Variant, config id and target are the binary's, never a default: what the caller says must
    # agree with it, and what the caller does not say is taken from it.
    embedded = _check_binary(primary, info, kind, args.component, args.variant, args.config_id)
    variant = embedded["build_info"].get("variant")
    config_id = embedded["build_info"].get("config")
    if not variant or not config_id:
        fail("%s does not say its variant and config id" % args.primary)
    image = embedded.get("image")
    target = args.target or (image or {}).get("target")
    problem = target_problem(args.component, target, image)
    if problem:
        fail("%s cannot be described as built for %r: %s" % (args.primary, target, problem))
    manifest = {
        "manifest_schema": MANIFEST_SCHEMA,
        "type": "pogo-build-manifest",
        "created_utc": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "component": info["component"],
        "version": info["version"],
        "build_id": info["build_id"],
        "build_kind": kind,
        "source": {
            "commit": info["commit"],
            "dirty": info["dirty"],
            "fingerprint": info["source_fingerprint"],
            "origin": info["origin"],
            "inputs_unchanged_during_build": bool(args.fingerprint_before),
        },
        "build": {
            "configuration": args.configuration,
            "variant": variant,
            "target": target,
            "config_id": config_id,
            "toolchain": _parse_pairs(args.toolchain),
        },
        "binary_says": embedded,
        "compatibility": compatibility(root),
        "primary": args.primary,
        "files": files,
        "status": {
            "deployed": False,
            "hardware_verified": False,
            "note": "A manifest describes files. It is not a deployment and not a hardware test result.",
        },
    }
    out = os.path.join(directory, "manifest.json")
    with open(out, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2, sort_keys=True)
        f.write("\n")
    print("manifest written: %s (%s %s, %s, build %s, %d file(s))" % (out, info["component"], info["version"], kind, info["build_id"], len(files)))
    return 0


def _parse_pairs(pairs):
    out = {}
    for pair in pairs or []:
        key, sep, value = pair.partition("=")
        if not sep or not key:
            fail("expected key=value, got %r" % pair)
        out[key] = value
    return out


def _dirty_text(dirty):
    return {True: "true", False: "false"}.get(dirty, UNKNOWN)


def _check_binary(path, info, kind, component, variant, config_id=None):
    """The binary must say what the manifest is about to say. Returns what it says."""
    says = read_binary(path, component)
    embedded = says["build_info"]
    expected = {
        "component": component, "version": info["version"], "build": info["build_id"], "kind": kind,
        "commit": info["commit"], "fingerprint": info["source_fingerprint"], "dirty": _dirty_text(info["dirty"]),
    }
    if variant:
        expected["variant"] = variant
    if config_id:
        expected["config"] = config_id
    for key, value in expected.items():
        if embedded.get(key) != value:
            fail("%s says %s=%s but this tree and request say %s=%s -- the binary is not a build of these "
                 "sources in this mode (stale incremental build, another configuration, or the sources changed since)"
                 % (os.path.basename(path), key, embedded.get(key), key, value))
    if component in COMPONENT_PROJECT_NAMES:
        desc = says["app_description"]
        if desc["version"] != info["version"]:
            fail("%s: app description version is %r, versions.json says %r" % (os.path.basename(path), desc["version"], info["version"]))
        if desc["project_name"] != COMPONENT_PROJECT_NAMES[component]:
            fail("%s: app description project is %r, expected %r for %s"
                 % (os.path.basename(path), desc["project_name"], COMPONENT_PROJECT_NAMES[component], component))
    return says


# What a manifest of schema 1 must contain, and in what form. (key path, check, what it must be)
_ORIGINS = ("git", "source-info-verified", UNKNOWN)


def _nonempty_str(v):
    return isinstance(v, str) and v != ""


def _manifest_form_problems(m):
    """Missing or malformed fields. A manifest that fails here is not checked any further."""
    problems = []

    def need(cond, message):
        if not cond:
            problems.append(message)

    def section(name):
        value = m.get(name)
        need(isinstance(value, dict), "%s is missing or not an object" % name)
        return value if isinstance(value, dict) else {}

    need(m.get("manifest_schema") == MANIFEST_SCHEMA and not isinstance(m.get("manifest_schema"), bool),
         "manifest_schema is %r, this tool knows %d" % (m.get("manifest_schema"), MANIFEST_SCHEMA))
    need(m.get("type") == "pogo-build-manifest", "type is not pogo-build-manifest")
    need(m.get("component") in COMPONENT_SOURCES, "unknown component %r" % (m.get("component"),))
    need(isinstance(m.get("version"), str) and SEMVER.match(m["version"]) is not None, "version %r is not MAJOR.MINOR.PATCH" % (m.get("version"),))
    need(_nonempty_str(m.get("build_id")), "build_id is missing or not a string")
    need(m.get("build_kind") in ("development", "release"), "build_kind %r is neither development nor release" % (m.get("build_kind"),))
    need(_nonempty_str(m.get("created_utc")), "created_utc is missing or not a string")
    need(_nonempty_str(m.get("primary")), "primary is missing: a manifest must name the binary it is about")

    source = section("source")
    commit, fp, dirty = source.get("commit"), source.get("fingerprint"), source.get("dirty")
    need(commit == UNKNOWN or (isinstance(commit, str) and SHA40.match(commit) is not None), "source.commit %r is neither a full SHA nor 'unknown'" % (commit,))
    need(isinstance(fp, str) and HEX64.match(fp) is not None, "source.fingerprint is not a SHA-256")
    need(dirty is True or dirty is False or dirty == UNKNOWN, "source.dirty %r is not true, false or 'unknown'" % (dirty,))
    need(source.get("origin") in _ORIGINS, "source.origin %r is not one of %s" % (source.get("origin"), ", ".join(_ORIGINS)))
    need(isinstance(source.get("inputs_unchanged_during_build"), bool), "source.inputs_unchanged_during_build is missing or not a boolean")

    build = section("build")
    for key in ("configuration", "variant", "target", "config_id"):
        need(_nonempty_str(build.get(key)), "build.%s is missing or not a non-empty string" % key)
    toolchain = build.get("toolchain")
    need(isinstance(toolchain, dict) and all(_nonempty_str(k) and isinstance(v, str) for k, v in toolchain.items()),
         "build.toolchain is missing or not an object of strings")

    says = section("binary_says")
    need(isinstance(says.get("build_info"), dict), "binary_says.build_info is missing or not an object")
    need(isinstance(m.get("compatibility"), dict), "compatibility is missing or not an object")
    status = section("status")
    need(status.get("deployed") is False and status.get("hardware_verified") is False,
         "a build manifest never says deployed or hardware-verified (that is what an install record is for)")

    files = m.get("files")
    need(isinstance(files, list) and len(files) > 0, "files is missing, empty or not a list")
    for entry in files if isinstance(files, list) else []:
        ok = (isinstance(entry, dict) and _nonempty_str(entry.get("path")) and isinstance(entry.get("size"), int)
              and not isinstance(entry.get("size"), bool) and entry["size"] >= 0 and isinstance(entry.get("sha256"), str)
              and HEX64.match(entry["sha256"]) is not None)
        need(ok, "a file entry is not {path, size, sha256}: %r" % (entry.get("path") if isinstance(entry, dict) else entry,))
    return problems


def manifest_verify(args):
    """Checks a manifest in three separate steps, each against something real:
      1. its form (required fields);
      2. the files it lists (presence, size, SHA-256) and the PRIMARY BINARY ITSELF, read again
         from the file: every field of the manifest that the binary also states must agree with
         it. What the manifest records under binary_says is never taken as evidence;
      3. what the caller expects (--expect-*, --release, --match-current-tree) -- on top of 2,
         never instead of it.
    What cannot be read from the files is reported as not verified."""
    directory = os.path.abspath(args.dir)
    path = os.path.join(directory, "manifest.json")
    try:
        with open(path, encoding="utf-8") as f:
            m = json.load(f)
    except OSError as e:
        fail("no manifest: %s" % e)
    except ValueError as e:
        fail("manifest is not JSON: %s" % e)
    if not isinstance(m, dict):
        fail("manifest is not an object")

    problems = _manifest_form_problems(m)
    if problems:
        for p in problems:
            print("FAIL: " + p)
        print("manifest NOT verified: %s (%d problem(s) with its form; nothing else was checked)" % (path, len(problems)))
        return 1

    def need(cond, message):
        if not cond:
            problems.append(message)

    component, version, kind = m["component"], m["version"], m["build_kind"]
    source, build = m["source"], m["build"]
    commit, fp, dirty = source["commit"], source["fingerprint"], source["dirty"]
    need(m["build_id"] == build_id(commit, dirty, fp), "build_id %r does not follow from commit, dirty and fingerprint" % m["build_id"])
    if kind == "release":
        need(dirty is False and commit != UNKNOWN, "a release manifest needs a clean tree and a known commit")
        need(source["inputs_unchanged_during_build"] is True, "a release manifest needs proof that the inputs did not change during the build")

    # -- the files
    listed = set()
    for entry in m["files"]:
        rel = entry["path"]
        if os.path.isabs(rel) or ".." in rel.split("/") or rel in listed:
            problems.append("bad or repeated path %r" % rel)
            continue
        listed.add(rel)
        full = os.path.join(directory, *rel.split("/"))
        if not os.path.isfile(full) or os.path.islink(full):
            problems.append("missing file: %s" % rel)
            continue
        if os.path.getsize(full) != entry["size"]:
            problems.append("size of %s is %d, manifest says %r" % (rel, os.path.getsize(full), entry["size"]))
        elif sha256_file(full) != entry["sha256"]:
            problems.append("contents of %s do not match the manifest (SHA-256 differs)" % rel)

    # -- the primary binary, read again
    primary = m["primary"]
    checked, target_note = [], None
    need(primary in listed, "primary %r is not among the files" % primary)
    full = os.path.join(directory, *primary.split("/"))
    if primary in listed and os.path.isfile(full) and not os.path.islink(full):
        try:
            real = read_binary(full, component)
            embedded = real["build_info"]
            for key, value, name in (("component", component, "component"), ("version", version, "version"), ("build", m["build_id"], "build_id"),
                                     ("kind", kind, "build_kind"), ("commit", commit, "source.commit"), ("fingerprint", fp, "source.fingerprint"),
                                     ("dirty", _dirty_text(dirty), "source.dirty"), ("variant", build["variant"], "build.variant"),
                                     ("config", build["config_id"], "build.config_id")):
                need(embedded.get(key) == value, "the binary says %s=%s, the manifest's %s says %s" % (key, embedded.get(key), name, value))
                checked.append(name)
            if "app_description" in real:
                desc = real["app_description"]
                need(desc["version"] == version, "the binary's app description version is %r, the manifest says %r" % (desc["version"], version))
                need(desc["project_name"] == COMPONENT_PROJECT_NAMES[component], "the binary is project %r, not %s" % (desc["project_name"], component))
                checked.append("app description (version, project)")
                if "esp-idf" in build["toolchain"]:
                    need(build["toolchain"]["esp-idf"] == desc["idf_ver"],
                         "the binary was built with ESP-IDF %r, the manifest's toolchain says %r" % (desc["idf_ver"], build["toolchain"]["esp-idf"]))
                    checked.append("build.toolchain.esp-idf")
            image = real.get("image")
            problem = target_problem(component, build["target"], image)
            need(problem is None, "build.target %r does not fit the binary: %s" % (build["target"], problem))
            if image is not None:
                if image["format"] == "esp-idf-app":
                    target_note = "build.target %s (image header, chip id %s)" % (image["target"], image["chip_id"])
                elif build["target"] == "linux":
                    target_note = "build.target as far as the header goes (ELF; the architecture, %s, is not stated in the manifest)" % image["arch"]
                else:
                    target_note = "build.target as far as the header goes (%s executable for %s; the operating system is not provable from it)" % (
                        "an ELF" if image["format"] == "elf" else "a Mach-O", image["arch"])
            # The manifest's own record of the binary is not evidence. But a record that is not
            # true of the binary is not a manifest this tool wrote for it.
            for key, recorded in m["binary_says"].items():
                need(recorded == real.get(key), "binary_says.%s in the manifest is not what the binary says (%r, the binary: %r)" % (key, recorded, real.get(key)))
        except ToolError as e:
            problems.append(str(e))

    # -- what the caller expects, on top of the above
    if args.expect_component:
        need(component == args.expect_component, "component is %r, expected %r" % (component, args.expect_component))
    if args.expect_target:
        need(build["target"] == args.expect_target, "target is %r, expected %r" % (build["target"], args.expect_target))
    if args.expect_variant:
        need(build["variant"] == args.expect_variant, "variant is %r, expected %r" % (build["variant"], args.expect_variant))
    if args.expect_version:
        need(version == args.expect_version, "version is %r, expected %r" % (version, args.expect_version))
    if args.release:
        need(kind == "release", "this is a %s build, not a release" % kind)
    # A separate question from the artifact's own integrity, answered separately.
    tree_note, tree_differs = "not compared (--match-current-tree asks that separate question)", False
    if getattr(args, "match_current_tree", False):
        now = identity(args.root, component)
        if (now["source_fingerprint"], now["commit"], now["dirty"]) == (fp, commit, dirty):
            tree_note = "this artifact is a build of the current tree (%s)" % now["build_id"]
        else:
            tree_differs = True
            tree_note = "this artifact is NOT a build of the current tree (the tree is %s, the artifact %s)" % (now["build_id"], m["build_id"])

    if problems:
        for p in problems:
            print("FAIL: " + p)
        print("manifest NOT verified: %s (%d problem(s))" % (path, len(problems)))
        return 1
    print("manifest verified: %s -- %s %s, %s, build %s, %d file(s). This says the files are the build it describes; "
          "it does not say they were installed or tested." % (path, component, version, kind, m["build_id"], len(listed)))
    print("  read again from %s and equal to the manifest: %s" % (primary, ", ".join(checked + ([target_note] if target_note else []))))
    unverified = ["build.configuration", "build.toolchain" + (" (other than esp-idf)" if "build.toolchain.esp-idf" in checked else ""),
                  "compatibility", "created_utc", "source.origin", "source.inputs_unchanged_during_build"]
    print("  not verified (cannot be read from the files; the manifest's own word): " + ", ".join(unverified))
    print("  current tree: " + tree_note)
    if tree_differs:
        print("NOT the current tree: the artifact is intact and is what its manifest says, but it was built from other inputs than this tree has now")
        return 1
    return 0


# ---------------------------------------------------------------- records

PUBLIC_RECORD_TEMPLATE = """# Install / hardware test record (public part)

Record schema: 1. One file per installation or test session. Nothing in this file may identify a
board, an address, an account or a secret; those go into the private part.

- Date (local, with zone):
- Recorded by:
- Private part: <file name in the private records directory, or "none">

## What was installed

| Component | Version | Build id | Build kind | Manifest SHA-256 | Primary artifact SHA-256 | Target (alias only) |
|---|---|---|---|---|---|---|
| app (iOS / Mac) | | | | | | |
| power_base | | | | | | |
| module | | | | | | |
| rcp | | | | | | |
| linux_bridge | | | | | | |

Rows that were not installed in this session: write "not changed" and the build that was already
there if it is known, or "unknown (not managed by this scheme)" -- never a guess.

## What was tested

| Step | Result (pass / fail / not run) | Evidence (log file, screenshot) |
|---|---|---|
| | | |

## Not verified in this session

-

## Roll back to

| Component | Previous artifact SHA-256 | Where it is kept |
|---|---|---|
| | | |
"""

PRIVATE_RECORD_TEMPLATE = """# Install / hardware test record (PRIVATE part -- never commit, never publish)

Belongs to the public record: <file name>

- Target alias -> what it really is (board EUI-64, serial port, host name, address, account):
- Configuration images used (path and SHA-256; never their contents):
- Anything else that identifies the installation:

Never write a setup code, PSKd, Thread Dataset, Network Key, management key or token here either:
record where it is kept, not what it is.
"""


def record_template(args):
    out = os.path.abspath(args.out)
    os.makedirs(out, exist_ok=True)
    for name, text in (("install-record.public.md", PUBLIC_RECORD_TEMPLATE), ("install-record.private.md", PRIVATE_RECORD_TEMPLATE)):
        path = os.path.join(out, name)
        if os.path.exists(path):
            fail("%s exists already; not overwritten" % path)
        with open(path, "w", encoding="utf-8") as f:
            f.write(text)
        print("written: " + path)
    return 0


# ---------------------------------------------------------------- firmware build

# The build configurations that exist, by the sdkconfig overlays they are made of (relative to the
# project directory) and by what the generated sdkconfig must then say they are.
FIRMWARE_PROJECTS = {"power_base": "power_base", "module": "module", "rcp": "rcp"}
FIRMWARE_VARIANTS = {
    "power_base": {
        "default": (["sdkconfig.defaults"], "default"),
        "bench": (["sdkconfig.defaults", "sdkconfig.bench_joiner"], "bench"),
        "ble": (["sdkconfig.defaults", "sdkconfig.ble_enrollment"], "ble"),
        "ble_reenroll_gpio9": (["sdkconfig.defaults", "sdkconfig.ble_enrollment", "sdkconfig.reenroll_button_gpio9"], "ble+reenroll_button_gpio9"),
    },
    "module": {"default": (["sdkconfig.defaults"], "default")},
    "rcp": {"default": (["sdkconfig.defaults"], "default"), "diag": (["sdkconfig.defaults", "sdkconfig.diag"], "diag")},
}
BUILD_MARKER = ".pogo-build-output"


def cmd_build_firmware(args):
    """Builds one firmware configuration into a NEW directory, checks that what was built is what
    the tree and versions.json say, and describes it with a manifest. It flashes nothing."""
    root, component = args.root, args.component
    if component not in FIRMWARE_PROJECTS:
        fail("build-firmware builds %s (not %s)" % (", ".join(sorted(FIRMWARE_PROJECTS)), component))
    variants = FIRMWARE_VARIANTS[component]
    if args.variant not in variants:
        fail("%s has the variants %s (not %s)" % (component, ", ".join(sorted(variants)), args.variant))
    overlays, expected_variant = variants[args.variant]
    idf_path = os.environ.get("IDF_PATH")
    if not idf_path or not os.path.isdir(idf_path):
        fail("IDF_PATH is not set: source the ESP-IDF export script first")
    out = os.path.abspath(args.out)
    if os.path.exists(out) and (not os.path.isdir(out) or os.listdir(out)):
        # Never build over anything that is already there: an earlier trial image, a backup, a
        # release directory or an older output of this tool.
        fail("%s exists and is not empty: give a new output directory" % out)
    real_root = os.path.realpath(root)
    real_out = os.path.join(os.path.realpath(os.path.dirname(out)), os.path.basename(out))
    if os.path.commonpath([real_out, real_root]) == real_root:
        fail("the output directory must be outside the repository")

    before = identity(root, component)
    kind = build_kind(before, args.release)  # a dirty tree is refused here in release mode
    os.makedirs(out)
    with open(os.path.join(out, BUILD_MARKER), "w", encoding="utf-8") as f:
        f.write("made by tools/pogo_version.py build-firmware; safe to delete as a whole\n")
    build_dir = os.path.join(out, "build")
    project_dir = os.path.join(root, FIRMWARE_PROJECTS[component])
    command = ["idf.py", "-C", project_dir, "-B", build_dir, "-D", "SDKCONFIG=" + os.path.join(build_dir, "sdkconfig"),
               "-D", "IDF_TARGET=" + COMPONENT_TARGETS[component], "-D", "SDKCONFIG_DEFAULTS=" + ";".join(overlays)]
    command += ["-D", "POGO_RELEASE=" + ("1" if args.release else "0"), "build"]
    env = dict(os.environ)
    env["IDF_COMPONENT_MANAGER"] = "0"
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    log_path = os.path.join(out, "build.log")
    print("building %s (%s, %s) into %s ..." % (component, args.variant, kind, out), flush=True)
    with open(log_path, "w", encoding="utf-8") as log:
        log.write("$ " + " ".join(command) + "\n")
        log.flush()
        rc = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, env=env).returncode
    if rc != 0:
        fail("the build failed (exit %d); see %s. No manifest was written." % (rc, log_path))

    after = identity(root, component)
    if after["source_fingerprint"] != before["source_fingerprint"] or after["commit"] != before["commit"] or after["dirty"] != before["dirty"]:
        fail("the sources of %s changed while it was being built (before %s, after %s). The output in %s is not a build "
             "of any one source tree: no manifest was written and it must not be used as a release or test candidate."
             % (component, before["build_id"], after["build_id"], out))

    sdkconfig = os.path.join(build_dir, "sdkconfig")
    detected, config_id = sdkconfig_identity(sdkconfig, component)
    if detected != expected_variant:
        fail("asked for the %s variant, but the generated sdkconfig is a %r build" % (args.variant, detected))
    project_name = COMPONENT_PROJECT_NAMES[component]
    artifact = os.path.join(out, "artifact")
    wanted = [(project_name + ".bin", project_name + ".bin"), ("bootloader/bootloader.bin", "bootloader/bootloader.bin"),
              ("partition_table/partition-table.bin", "partition_table/partition-table.bin"), ("flash_args", "flash_args")]
    import shutil
    files = []
    for source, rel in wanted:
        full = os.path.join(build_dir, *source.split("/"))
        if not os.path.isfile(full):
            fail("the build did not produce %s" % source)
        target = os.path.join(artifact, *rel.split("/"))
        os.makedirs(os.path.dirname(target), exist_ok=True)
        shutil.copyfile(full, target)
        files.append(rel)

    # The ESP-IDF version is the one the image itself was built with (its application
    # description); the directory's own version is only recorded when it says something else.
    built_with = read_app_desc(os.path.join(artifact, project_name + ".bin"))["idf_ver"]
    toolchain = ["esp-idf=" + built_with]
    if _idf_version(idf_path) not in (built_with, UNKNOWN):
        toolchain.append("esp-idf-path-version=" + _idf_version(idf_path))
    try:
        with open(os.path.join(build_dir, "project_description.json"), encoding="utf-8") as f:
            compiler = json.load(f).get("c_compiler", "")
        if compiler and os.path.isfile(compiler):
            version = subprocess.run([compiler, "-dumpfullversion"], capture_output=True, text=True).stdout.strip()
            toolchain.append("c_compiler=%s %s" % (os.path.basename(compiler), version))
    except (OSError, ValueError):
        pass
    toolchain.append("python=%d.%d.%d" % sys.version_info[:3])

    class ManifestArgs:
        pass

    m = ManifestArgs()
    m.root, m.dir, m.component, m.release = root, artifact, component, args.release
    m.configuration, m.variant, m.target = "esp-idf", expected_variant, COMPONENT_TARGETS[component]
    m.config_id, m.toolchain, m.primary = config_id, toolchain, project_name + ".bin"
    m.fingerprint_before, m.allow_sensitive_names, m.files = before["source_fingerprint"], False, files
    manifest_create(m)

    class VerifyArgs:
        pass

    v = VerifyArgs()
    v.root, v.match_current_tree = root, True  # a build that was just made is a build of this tree
    v.dir, v.expect_component, v.expect_target, v.expect_variant = artifact, component, COMPONENT_TARGETS[component], expected_variant
    v.expect_version, v.release = before["version"], args.release
    if manifest_verify(v) != 0:
        fail("the manifest that was just written does not verify")
    print("%s build of %s %s (%s): %s" % (kind, component, before["version"], expected_variant, artifact))
    if kind != "release":
        print("NOTE: a development build%s. It is not a release candidate." % (" from a dirty tree" if before["dirty"] is True else ""))
    return 0


def _idf_version(idf_path):
    for name in ("version.txt",):
        try:
            with open(os.path.join(idf_path, name), encoding="utf-8") as f:
                return f.read().strip()
        except OSError:
            pass
    described = _git(idf_path, "describe", "--tags", "--always")
    return described.strip() if described else UNKNOWN


# ---------------------------------------------------------------- legacy artifacts

LEGACY_SKIP_HINTS = ["config", "cfg", "backup", "dataset", "secret", "nvs", "otbr", "await", "bridge_config", "ble_config"]


def cmd_legacy_inventory(args):
    """Lists application images that were built before this scheme existed, with what they say
    about themselves (ESP-IDF's app description as it was then: a `git describe` string) and
    their real SHA-256. Read-only. Only *.bin files directly inside each build directory are
    looked at. A file whose name suggests a configuration image, a Dataset or a backup is never
    opened; of any other, only the header is read, and it is hashed only when that header is an
    ESP-IDF application description."""
    base = os.path.abspath(args.dir)
    if not os.path.isdir(base):
        fail("%s is not a directory" % base)
    rows = []
    for entry in sorted(os.listdir(base)):
        directory = os.path.join(base, entry)
        if not os.path.isdir(directory) or os.path.isfile(os.path.join(directory, BUILD_MARKER)) or \
                os.path.isfile(os.path.join(os.path.dirname(directory), entry, "artifact", "manifest.json")):
            continue
        for name in sorted(os.listdir(directory)):
            full = os.path.join(directory, name)
            low = name.lower()
            if not name.endswith(".bin") or not os.path.isfile(full) or os.path.islink(full) or any(h in low for h in LEGACY_SKIP_HINTS):
                continue
            try:
                desc = read_app_desc(full)
            except ToolError:
                continue  # not an application image
            embedded = None
            try:
                embedded = read_embedded_build_info(full)
            except ToolError:
                pass
            if embedded is not None:
                continue  # built with the version stamp: described by its own manifest, not legacy
            rows.append((entry, name, os.path.getsize(full), sha256_file(full), desc["project_name"], desc["version"], desc["idf_ver"]))
    lines = ["| Directory | File | Size | SHA-256 | Project (app description) | Version string as built | ESP-IDF |", "|---|---|---:|---|---|---|---|"]
    for row in rows:
        lines.append("| `%s` | `%s` | %d | `%s` | %s | `%s` | %s |" % row)
    text = "\n".join(lines) + "\n"
    if args.out:
        out = os.path.abspath(args.out)
        if os.path.exists(out):
            fail("%s exists already; not overwritten" % out)
        with open(out, "w", encoding="utf-8") as f:
            f.write(text)
        print("written: %s (%d image(s))" % (out, len(rows)))
    else:
        sys.stdout.write(text)
    return 0


# ---------------------------------------------------------------- commands

def cmd_versions(args):
    versions = load_versions(args.root)
    if args.json:
        print(json.dumps(versions, sort_keys=True))
    elif args.component:
        if args.component not in versions:
            fail("unknown component %r" % args.component)
        print(versions[args.component])
    else:
        for name in sorted(versions):
            print("%-13s %s" % (name, versions[name]))
    return 0


def cmd_check(args):
    versions = load_versions(args.root)
    print("versions.json: ok (%s)" % ", ".join("%s %s" % (k, versions[k]) for k in sorted(versions)))
    return 0


def cmd_identity(args):
    info = identity(args.root, args.component)
    if args.json:
        print(json.dumps(info, sort_keys=True))
    else:
        for key in ("component", "version", "build_id", "commit", "dirty", "source_fingerprint", "origin"):
            print("%-19s %s" % (key, info[key]))
    return 0


def cmd_fingerprint(args):
    print(fingerprint(args.root, args.component))
    return 0


def cmd_files(args):
    for rel, _ in source_files(args.root, args.component):
        print(rel)
    return 0


def cmd_stamp(args):
    info = identity(args.root, args.component)
    kind = build_kind(info, args.release)
    variant, config_id = args.variant, args.config_id
    if args.sdkconfig:
        detected, detected_id = sdkconfig_identity(args.sdkconfig, args.component)
        variant = variant or detected
        config_id = config_id or detected_id
    text = stamp_text(info, kind, variant or "default", config_id or UNKNOWN)
    changed = write_if_changed(args.out, text)
    if not args.quiet:
        print("%s: %s %s build %s (%s, %s)%s" % (args.out, info["component"], info["version"], info["build_id"], kind,
                                                 variant or "default", "" if changed else " [unchanged]"))
    return 0


def cmd_source_info(args):
    if git_state(args.root, "power_base") is None:
        fail("source-info is written from a Git work tree (it records what a bundle was made from)")
    data = {"schema": SOURCE_INFO_SCHEMA, "note": "Kept at the top of a source bundle without .git. The tool trusts it only "
            "while the bundle's sources still hash to the fingerprints below.", "components": {}}
    for name in sorted(COMPONENT_SOURCES):
        info = identity(args.root, name)
        data["components"][name] = {"commit": info["commit"], "dirty": info["dirty"], "source_fingerprint": info["source_fingerprint"]}
    out = os.path.abspath(args.out)
    real_out = os.path.join(os.path.realpath(os.path.dirname(out)), os.path.basename(out))
    if os.path.commonpath([real_out, os.path.realpath(args.root)]) == os.path.realpath(args.root) and not args.allow_in_tree:
        fail("refusing to write %s inside the source tree; give a path outside it (the bundle's top directory)" % out)
    with open(out, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2, sort_keys=True)
        f.write("\n")
    print("written: " + out)
    return 0


def cmd_appdesc(args):
    desc = read_app_desc(args.image)
    embedded = read_embedded_build_info(args.image)
    print(json.dumps({"app_description": desc, "build_info": embedded, "image": read_image_kind(args.image)}, indent=2, sort_keys=True))
    return 0


def cmd_compat(args):
    print(json.dumps(compatibility(args.root), indent=2, sort_keys=True))
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description="POGO version and build-identity tool (docs/versioning.md)")
    parser.add_argument("--root", default=DEFAULT_ROOT, help="repository (or source bundle) root; default: this tool's repository")
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("versions", help="print the product versions from versions.json")
    p.add_argument("--json", action="store_true")
    p.add_argument("--component")
    p.set_defaults(func=cmd_versions)

    p = sub.add_parser("check", help="check versions.json")
    p.set_defaults(func=cmd_check)

    for name, func, text in (("identity", cmd_identity, "version, commit, dirty, fingerprint and build id of a component"),
                             ("fingerprint", cmd_fingerprint, "the source fingerprint of a component"),
                             ("files", cmd_files, "the files a component's fingerprint covers")):
        p = sub.add_parser(name, help=text)
        p.add_argument("--component", required=True)
        if name == "identity":
            p.add_argument("--json", action="store_true")
        p.set_defaults(func=func)

    p = sub.add_parser("stamp", help="write the build-info header (only when it changes)")
    p.add_argument("--component", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--variant")
    p.add_argument("--config-id")
    p.add_argument("--sdkconfig", help="the build's generated sdkconfig: gives the variant and the config id")
    p.add_argument("--release", action="store_true", help="a release build: refuses a dirty or unknown tree")
    p.add_argument("--quiet", action="store_true")
    p.set_defaults(func=cmd_stamp)

    p = sub.add_parser("source-info", help="write SOURCE_INFO.json for a source bundle without .git")
    p.add_argument("--out", required=True)
    p.add_argument("--allow-in-tree", action="store_true")
    p.set_defaults(func=cmd_source_info)

    p = sub.add_parser("manifest-create", help="describe a directory of built files")
    p.add_argument("--dir", required=True)
    p.add_argument("--component", required=True)
    p.add_argument("--configuration", required=True, help="compiler configuration, e.g. Debug, Release, esp-idf-default")
    p.add_argument("--variant", help="must be the binary's own variant; default: the binary's")
    p.add_argument("--target", help="must fit the binary's header; default: what the header says")
    p.add_argument("--config-id", help="must be the binary's own config id; default: the binary's")
    p.add_argument("--toolchain", action="append", metavar="KEY=VALUE")
    p.add_argument("--primary", help="required: the binary whose own word the manifest is checked against")
    p.add_argument("--fingerprint-before", help="the source fingerprint taken before the build started")
    p.add_argument("--release", action="store_true")
    p.add_argument("--allow-sensitive-names", action="store_true", help=argparse.SUPPRESS)
    p.add_argument("files", nargs="+")
    p.set_defaults(func=manifest_create)

    p = sub.add_parser("manifest-verify", help="check a manifest against its files and its binary")
    p.add_argument("--dir", required=True)
    p.add_argument("--expect-component")
    p.add_argument("--expect-target")
    p.add_argument("--expect-variant")
    p.add_argument("--expect-version")
    p.add_argument("--release", action="store_true", help="also require a release build")
    p.add_argument("--match-current-tree", action="store_true",
                   help="also require that the artifact is a build of the current tree (a separate question from its own integrity)")
    p.set_defaults(func=manifest_verify)

    p = sub.add_parser("appdesc", help="what an ESP-IDF image says about itself")
    p.add_argument("image")
    p.set_defaults(func=cmd_appdesc)

    p = sub.add_parser("compat", help="protocol and schema facts read from the sources")
    p.set_defaults(func=cmd_compat)

    p = sub.add_parser("record-template", help="write the install / hardware-test record templates")
    p.add_argument("--out", required=True)
    p.set_defaults(func=record_template)

    p = sub.add_parser("build-firmware", help="build one firmware configuration into a NEW directory, check it and describe it")
    p.add_argument("--component", required=True, choices=sorted(FIRMWARE_PROJECTS))
    p.add_argument("--variant", default="default")
    p.add_argument("--out", required=True, help="a directory that does not exist yet (or is empty), outside the repository")
    p.add_argument("--release", action="store_true", help="release mode: needs a clean tree and a known commit")
    p.set_defaults(func=cmd_build_firmware)

    p = sub.add_parser("legacy-inventory", help="list application images built before this scheme (read-only)")
    p.add_argument("--dir", required=True)
    p.add_argument("--out")
    p.set_defaults(func=cmd_legacy_inventory)

    args = parser.parse_args(argv)
    args.root = os.path.abspath(args.root)
    try:
        return args.func(args)
    except ToolError as e:
        print("pogo_version: " + str(e), file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
