#!/usr/bin/env python3
"""Checks, in a LINKED Power Base image and in the ESP-IDF sources it was built from, what
components/power_base_ot_store takes for granted (docs/power-base-management.md, section 4).

The component stands in front of OpenThread's two persistent stores by link-time wrapping: the
firmware is linked with -Wl,--wrap=<function> for nine functions (power_base/main/CMakeLists.txt).
That only protects anything if, in the image that will run,

  image    every call of one of the nine platform functions comes from the component's own
           __wrap_ function for it (or from the one file that reads and erases the flash on
           purpose, pb_stored_network.c), and the __wrap_ functions are what OpenThread and the
           PSA key store actually call; the firmware's erase reaches that file's flash erase, and
           whatever hands out a release receipt asks that file's store check first;

  sources  the numbers pb_stored_network.c repeats from OpenThread and from ESP-IDF are still
           the ones in that ESP-IDF: the settings namespace, the offset and the list of
           OpenThread's persistent PSA keys, key references being enabled, the PSA key store
           filing a key under its key id, and the one key OpenThread makes for itself at every
           start being the Network Key's id and nothing else.

    tools/check_power_base_ot_store.py --elf <build>/power_base.elf [--idf $IDF_PATH]
                                       [--objdump riscv32-esp-elf-objdump]

Reads only. Exit 0 = all checks passed, 1 = a check failed, 2 = it could not be run.
This is a check of the image and the sources on this machine. It does not run the image.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys

WRAPPED = [
    "otPlatSettingsGet",
    "otPlatSettingsSet",
    "otPlatSettingsAdd",
    "otPlatSettingsDelete",
    "otPlatSettingsWipe",
    "psa_its_get_info",
    "psa_its_get",
    "psa_its_set",
    "psa_its_remove",
]

# Besides __wrap_<f>, who may refer to the platform's own <f>: the read-back and the erase of the
# flash, which must NOT go through the switch (components/power_base_ot_store/pb_stored_network.c;
# every function of that file is named pb_stored_network_*, or is its static
# network_key_entry_is_this_boots, and the compiler may put one inside another). Only these three:
# nothing writes key storage past the switch, and the one stored value that is read past it (the
# entry under the Network Key's id, to compare it with the write that was seen) is compared inside
# the component and goes nowhere.
DIRECT_PREFIX = "pb_stored_network_"
DIRECT_STATICS = ("network_key_entry_is_this_boots",)
DIRECT_FUNCTIONS = {"psa_its_get_info", "psa_its_remove", "psa_its_get"}


def is_direct(name):
    return name.startswith(DIRECT_PREFIX) or name.startswith(DIRECT_STATICS)

# Functions of the component that call the PUBLIC name on purpose (the check in engage()): in a
# wrapped image that reference is to __wrap_<f>, like everybody else's.
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


class Report:
    def __init__(self):
        self.failures = 0

    def check(self, ok, what, detail=""):
        print(("ok:   " if ok else "FAIL: ") + what + (f" [{detail}]" if detail and not ok else ""))
        if not ok:
            self.failures += 1
        return ok


def disassemble(objdump, elf):
    try:
        out = subprocess.run([objdump, "-d", "--no-show-raw-insn", elf], check=True, capture_output=True, text=True)
    except (OSError, subprocess.CalledProcessError) as exc:
        print(f"cannot disassemble {elf} with {objdump}: {exc}", file=sys.stderr)
        sys.exit(2)
    return out.stdout


def references(disassembly):
    """{referenced symbol: {function that refers to it}} for every exact `<symbol>` annotation
    of an instruction (a call, a jump, or an address taken), and the set of defined functions."""
    refs = {}
    defined = set()
    current = None
    label = re.compile(r"^[0-9a-f]+ <([^>]+)>:$")
    target = re.compile(r"<([^>+]+)>")
    for line in disassembly.splitlines():
        m = label.match(line)
        if m:
            current = m.group(1)
            defined.add(current)
            continue
        if current is None or ":" not in line:
            continue
        for name in target.findall(line.split(":", 1)[1]):
            refs.setdefault(name, set()).add(current)
    return refs, defined


def call_leads_to_call(disassembly, function, first, second):
    """In `function`'s code: is the one call of `first` followed, on the path the code takes from
    there, by a call of `second` with no other call and no branch in between? Unconditional jumps
    inside the function are followed (a compiler is free to put a block out of line and jump back).
    Returns (True, "") or (False, why)."""
    label = re.compile(r"^[0-9a-f]+ <([^>]+)>:$")
    insn = re.compile(r"^\s*([0-9a-f]+):\s+(\S+)\s*(.*)$")
    code, inside = [], False
    for line in disassembly.splitlines():
        m = label.match(line)
        if m:
            inside = m.group(1) == function
            continue
        if inside:
            m = insn.match(line)
            if m:
                code.append((int(m.group(1), 16), m.group(2), m.group(3)))
    if not code:
        return False, "no code for " + function
    index = {addr: i for i, (addr, _, _) in enumerate(code)}

    def called(i):
        _, mnemonic, operands = code[i]
        if mnemonic not in ("jal", "jalr", "call"):
            return None
        m = re.search(r"<([^>+]+)>", operands)
        return m.group(1) if m else "?"

    calls_first = [i for i in range(len(code)) if called(i) == first]
    if len(calls_first) != 1:
        return False, "%d call(s) of %s" % (len(calls_first), first)
    i = calls_first[0] + 1
    for _ in range(64):
        if i >= len(code):
            return False, "ran off the end of " + function
        addr, mnemonic, operands = code[i]
        name = called(i)
        if name is not None:
            return (True, "") if name == second else (False, "the next call is " + name)
        if mnemonic == "j":
            m = re.match(r"([0-9a-f]+)\b", operands)
            if not m or int(m.group(1), 16) not in index:
                return False, "a jump out of " + function
            i = index[int(m.group(1), 16)]
            continue
        if mnemonic.startswith("b") or mnemonic in ("ret", "jr", "c.jr", "mret"):
            return False, "a %s before any call" % mnemonic
        i += 1
    return False, "no call within 64 instructions"


def check_image(report, elf, objdump):
    disassembly = disassemble(objdump, elf)
    refs, defined = references(disassembly)
    for f in WRAPPED:
        wrap = "__wrap_" + f
        if not report.check(f in defined and wrap in defined, f"{f}: the platform's function and {wrap} are both linked"):
            continue
        others = sorted(c for c in refs.get(f, set()) - {wrap, f} if not (f in DIRECT_FUNCTIONS and is_direct(c)))
        report.check(not others, f"{f}: no caller reaches the platform's function past the switch",
                     "referred to by " + ", ".join(others[:6]))
        callers = sorted(refs.get(wrap, set()) - {wrap})
        outside = [c for c in callers if not c.startswith("pb_volatile_store_") and not c.startswith("__wrap_")]
        report.check(bool(outside), f"{f}: {wrap} is what the rest of the image calls",
                     "no caller outside the component")
        if outside:
            print(f"      called by: {', '.join(outside[:4])}{' ...' if len(outside) > 4 else ''}")
    report.check("pb_volatile_store_engage" in defined and
                 {"__wrap_otPlatSettingsGet", "__wrap_psa_its_get_info"} <= {
                     name for name, users in refs.items() if "pb_volatile_store_engage" in users},
                 "pb_volatile_store_engage(): its two checking reads resolve to the switch")
    def direct(f):
        return sorted(c for c in refs.get(f, set()) if is_direct(c))

    def users(name, prefix=False):
        return {c for target, callers in refs.items() if (target.startswith(name) if prefix else target == name)
                for c in callers}

    report.check(bool(direct("psa_its_get_info")),
                 "pb_stored_network.c: reads the platform's key storage itself (the read-back)")
    report.check(bool(direct("psa_its_remove")),
                 "pb_stored_network.c: removes from the platform's key storage itself (the erase past the switch)")
    # The firmware's erase: OpenThread's own, and -- once the switch is on -- the flash's.
    erase_callees = {t for t, callers in refs.items() if "pb_network_store_erase" in callers}
    report.check("pb_network_store_erase" in defined and "pb_volatile_store_is_engaged" in erase_callees and
                 any(t.startswith("pb_stored_network_") and "erase" in t for t in erase_callees),
                 "pb_network_store_erase(): asks whether the switch is on, and erases the flash itself then")
    # The origin of what lies under the Network Key's id: every write of key storage is the PSA key
    # store's own and comes through the switch (where it is watched), a released board erases and
    # reads back before OpenThread starts, and the store check compares the entry with that write.
    writers = sorted(refs.get("__wrap_psa_its_set", set()) - {"__wrap_psa_its_set"})
    report.check(bool(writers) and all(w.startswith(("psa_", "mbedtls_psa_")) for w in writers),
                 "key storage is written by the PSA key store only (every psa_its_set is one of its own)",
                 ", ".join(writers[:6]))
    removers = sorted(refs.get("__wrap_psa_its_remove", set()) - {"__wrap_psa_its_remove"})
    report.check(bool(removers) and all(w.startswith(("psa_", "mbedtls_psa_")) for w in removers),
                 "and removed, through the switch, by the PSA key store only", ", ".join(removers[:6]))
    report.check(bool(users("pb_volatile_store_its_watch_start")) and
                 all(is_direct(u) for u in users("pb_volatile_store_its_watch_start")) and
                 bool(direct("psa_its_get")) and bool(users("pb_volatile_store_its_watch_matches")),
                 "pb_stored_network.c: watches the Network Key's id from a read-back that found both stores empty, and "
                 "compares the stored entry with the write it saw")
    prepare = "pb_mgmt_runtime_prepare_released_start"
    prepare_callees = {t for t, callers in refs.items() if prepare in callers}
    report.check(prepare in defined and "pb_mgmt_released_prestart" in prepare_callees and
                 "pb_mgmt_released_prestart" in defined and
                 bool(users("pb_stored_network_erase") | users("pb_stored_network_settings_erase")),
                 "a released board's stores are erased and read back by pb_mgmt_runtime_prepare_released_start()")
    ok, why = call_leads_to_call(disassembly, "app_main", prepare, "thread_manager_init")
    report.check(ok, "app_main: from that call the code goes straight on to thread_manager_init() -- the erase "
                     "comes before OpenThread exists", why)
    # Every boot: a final record is checked against the stores (enrollment_mgmt.c).
    report.check("pb_mgmt_client_check_final_release" in defined and
                 "pb_mgmt_runtime_finish_pending_release" in defined and
                 bool(users("pb_mgmt_client_check_final_release")) and bool(users("pb_stored_network_holds_no_network")),
                 "the boot path checks a final record against the stores (pb_mgmt_client_check_final_release)")
    # The release receipt over BLE, in the images that have BLE enrollment.
    if "release_receipt_source" in defined:
        source_callees = {t for t, callers in refs.items() if "release_receipt_source" in callers}
        check_fn = sorted(t for t in source_callees if t.startswith("receipt_no_network_stored"))
        report.check("pb_mgmt_receipt_load" in source_callees and bool(check_fn) and
                     any("pb_network_store_holds_no_network" in
                         {t for t, callers in refs.items() if fn in callers} for fn in check_fn),
                     "BLE: the receipt source hands pb_mgmt_receipt_load() a check that reads the stores")
        others = sorted(users("pb_mgmt_receipt_load") - {"release_receipt_source", "pb_mgmt_receipt_load"})
        report.check(not others, "BLE: nothing else in the image hands out a receipt", ", ".join(others[:6]))
    else:
        report.check("pb_mgmt_receipt_load" not in defined or not users("pb_mgmt_receipt_load"),
                     "(no BLE enrollment in this image: nothing hands out a receipt)")


def read(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read()


def check_sources(report, idf):
    ot = os.path.join(idf, "components", "openthread")
    mbed = os.path.join(idf, "components", "mbedtls")
    ours = read(os.path.join(ROOT, "components", "power_base_ot_store", "pb_stored_network.c"))

    def ours_define(name):
        m = re.search(r"#define\s+" + name + r"\s+(\S+)", ours)
        return m.group(1) if m else None

    settings = read(os.path.join(ot, "src", "port", "esp_openthread_settings.c"))
    m = re.search(r'#define\s+OT_NAMESPACE\s+"([^"]+)"', settings)
    report.check(m is not None and f'"{m.group(1)}"' == ours_define("OT_SETTINGS_NVS_NAMESPACE"),
                 "the NVS namespace of OpenThread's settings is the one pb_stored_network.c reads")
    bodies = re.split(r"\n(?=\w[\w\s\*]*\botPlatSettings\w+\()", settings)
    crossing = [b.split("(")[0].split()[-1] for b in bodies[1:]
                if len(re.findall(r"\botPlatSettings(?:Get|Set|Add|Delete|Wipe)\s*\(", b)) > 1]
    report.check(not crossing, "ESP-IDF's settings functions do not call one another (such a call would not be wrapped)",
                 ", ".join(crossing))

    config = read(os.path.join(ot, "private_include", "openthread-core-esp32x-ftd-config.h"))
    report.check(re.search(r"#define\s+OPENTHREAD_CONFIG_PLATFORM_KEY_REFERENCES_ENABLE\s+1\b", config) is not None,
                 "ESP-IDF builds OpenThread (FTD) with key references: the Network Key is a persistent PSA key")

    platform = read(os.path.join(ot, "openthread", "src", "core", "config", "platform.h"))
    m = re.search(r"#define\s+OPENTHREAD_CONFIG_PSA_ITS_NVM_OFFSET\s+(0x[0-9a-fA-F]+)", platform)
    overridden = "OPENTHREAD_CONFIG_PSA_ITS_NVM_OFFSET" in config
    offset = ours_define("OT_PSA_KEY_REF_OFFSET")
    report.check(m is not None and not overridden and offset is not None and
                 int(m.group(1), 16) == int(offset.rstrip("u"), 16),
                 "OpenThread's persistent key ids start at the offset pb_stored_network.c uses")

    storage = read(os.path.join(ot, "openthread", "src", "core", "crypto", "storage.hpp"))
    m = re.search(r"enum Type : uint8_t\s*\{(.*?)\}", storage, re.S)
    types = sorted(int(v) for v in re.findall(r"=\s*(\d+)", m.group(1))) if m else []
    first, last = ours_define("OT_PSA_KEY_REF_FIRST"), ours_define("OT_PSA_KEY_REF_LAST")
    report.check(bool(types) and first is not None and last is not None and
                 types == list(range(int(first.rstrip("u")), int(last.rstrip("u")) + 1)),
                 "OpenThread's persistent key types are exactly the range pb_stored_network.c asks for",
                 f"OpenThread has {types}")
    storage_cpp = read(os.path.join(ot, "openthread", "src", "core", "crypto", "storage.cpp"))
    m = re.search(r"void Storage::KeyRefManager::DestroyPersistentKeys\(void\)\s*\{(.*?)\n\}", storage_cpp, re.S)
    report.check(m is not None and len(re.findall(r"DestroyKey\(KeyRefFor\(k\w+\)\)", m.group(1))) == len(types),
                 "and the erase (DestroyPersistentKeys) destroys every one of them")

    # The one key OpenThread makes for itself at every start (pb_stored_network_holds_no_network()).
    own = ours_define("OT_PSA_KEY_REF_NETWORK_KEY")
    m = re.search(r"\bkNetworkKey\s*=\s*(\d+)", storage)
    report.check(m is not None and own is not None and int(m.group(1)) == int(own.rstrip("u")),
                 "the Network Key's key type is the one pb_stored_network.c lets a later boot find")
    key_manager = read(os.path.join(ot, "openthread", "src", "core", "thread", "key_manager.cpp"))
    m = re.search(r"void KeyManager::Init\(void\)\s*\{(.*?)\n\}", key_manager, re.S)
    init_body = m.group(1) if m else ""
    report.check(len(re.findall(r"\bStore\w+\(", init_body)) == 1 and
                 re.search(r"StoreNetworkKey\(networkKey,\s*(/\*.*?\*/\s*)?false\)", init_body) is not None,
                 "at a start OpenThread stores one key of its own -- a random Network Key, only when none is stored")
    m = re.search(r"void KeyManager::StoreNetworkKey\(.*?\)\s*\{(.*?)\n\}", key_manager, re.S)
    report.check(m is not None and "KeyRefFor(Crypto::Storage::KeyRefManager::kNetworkKey)" in m.group(1) and
                 len(re.findall(r"ImportKey\(", m.group(1))) == 1,
                 "and it stores it under the Network Key's id, nowhere else")
    store_body = m.group(1) if m else ""
    report.check(re.search(r"if \(!aOverWriteExisting\)\s*\{.*?HasKey\(keyRef\).*?ExitNow\(\);", store_body, re.S) is not None,
                 "a key that IS stored under that id is left alone by a start (so it may be a network's)")
    report.check(store_body.find("DestroyKey(keyRef)") != -1 and
                 store_body.find("DestroyKey(keyRef)") < store_body.find("ImportKey("),
                 "and whoever replaces it destroys it first (StoreNetworkKey: DestroyKey, then ImportKey)")
    psa_storage = read(os.path.join(mbed, "mbedtls", "tf-psa-crypto", "core", "psa_crypto_storage.c"))
    m2 = re.search(r"static psa_status_t psa_crypto_storage_store\(.*?\)\s*\{(.*?)\n\}", psa_storage, re.S)
    body2 = m2.group(1) if m2 else ""
    report.check(len(re.findall(r"\bpsa_its_set\(", body2)) == 1 and
                 re.search(r"psa_is_key_present_in_storage\(key\)\s*==\s*1\)\s*\{\s*return PSA_ERROR_ALREADY_EXISTS;", body2) is not None and
                 len(re.findall(r"\bpsa_its_set\(", psa_storage)) == 1,
                 "the PSA key store writes a persistent key with exactly one psa_its_set, and refuses to write over "
                 "an existing one")
    instance = read(os.path.join(ot, "openthread", "src", "core", "instance", "instance.cpp"))
    m = re.search(r"void Instance::AfterInit\(void\)\s*\{(.*?)\n\}", instance, re.S)
    after_init = m.group(1) if m else ""
    report.check("Get<KeyManager>().Init();" in after_init and
                 not re.search(r"\.(Save|Store|Add|Delete|Wipe)\w*\(", after_init),
                 "a start reads the settings (Settings::Init, Mle::Restore) and saves nothing itself")

    key_store = read(os.path.join(mbed, "mbedtls", "tf-psa-crypto", "core", "psa_crypto_storage.c"))
    esp_config = read(os.path.join(mbed, "port", "include", "mbedtls", "esp_config.h"))
    report.check("return key;" in key_store.split("psa_its_identifier_of_slot", 1)[1].split("}", 1)[0] and
                 not re.search(r"^\s*#define\s+MBEDTLS_PSA_CRYPTO_KEY_ID_ENCODES_OWNER", esp_config, re.M),
                 "the PSA key store files a persistent key under its key id")
    its = read(os.path.join(mbed, "port", "psa_crypto_storage", "esp_psa_its.c"))
    report.check(re.search(r'#define\s+PSA_ITS_NVS_NAMESPACE\s+"psa_its"', its) is not None and
                 "nvs_open(PSA_ITS_NVS_NAMESPACE" in its,
                 "ESP-IDF's PSA storage is the NVS namespace \"psa_its\", read from flash on every call")


def find_objdump(explicit):
    if explicit:
        return explicit
    for name in ("riscv32-esp-elf-objdump",):
        found = shutil.which(name)
        if found:
            return found
    print("riscv32-esp-elf-objdump is not on PATH (source the ESP-IDF export script, or pass --objdump)", file=sys.stderr)
    sys.exit(2)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--elf", help="the linked Power Base image (power_base.elf)")
    parser.add_argument("--idf", default=os.environ.get("IDF_PATH"), help="the ESP-IDF the image is built from")
    parser.add_argument("--objdump")
    args = parser.parse_args()
    if not args.elf and not args.idf:
        parser.error("nothing to check: give --elf and/or --idf")

    report = Report()
    if args.elf:
        if not os.path.isfile(args.elf):
            print(f"no such image: {args.elf}", file=sys.stderr)
            return 2
        print(f"-- image: {args.elf} --")
        check_image(report, args.elf, find_objdump(args.objdump))
    if args.idf:
        if not os.path.isdir(os.path.join(args.idf, "components", "openthread")):
            print(f"not an ESP-IDF tree: {args.idf}", file=sys.stderr)
            return 2
        print(f"-- sources: {args.idf} --")
        check_sources(report, args.idf)
    print(f"\n{'PASS' if report.failures == 0 else 'FAIL'}: {report.failures} failure(s)")
    return 0 if report.failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
