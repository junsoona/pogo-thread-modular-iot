#!/bin/bash
#
# Builds and runs the two host reproductions behind the Power Base's "release
# receipt over Thread" (docs/power-base-management.md, section 4), against the
# very sources ESP-IDF builds the firmware from:
#
#   tools/ot_host_repro/pb_release_session_nexus.cpp
#       the real OpenThread core ($IDF_PATH/components/openthread/openthread),
#       two instances on OpenThread's own in-process "Nexus" simulation;
#   tools/ot_host_repro/pb_volatile_store_psa.c
#       the real PSA key store ($IDF_PATH/components/mbedtls/mbedtls/tf-psa-crypto);
#
# both with components/power_base_ot_store/pb_volatile_store.c -- the file the
# firmware links -- in front of the stores, and the second also with
# pb_stored_network.c, which reads and erases the stores past it. A linker's
# --wrap, which the firmware uses, does not exist on every host: here the same
# two names per function come from compiling the platform's file under __real_
# names.
#
#   IDF_PATH=~/.espressif/v6.1/esp-idf \
#       tools/test_power_base_release_session_openthread.sh <work directory>
#
# <work directory> is created when missing and holds everything this script
# writes (two out-of-tree builds and the test programs); an existing one is
# reused, which makes a second run quick. Nothing is written into the ESP-IDF
# tree or into this repository (the two CMake builds run Python scripts of that
# tree; PYTHONDONTWRITEBYTECODE keeps their bytecode caches out of it). Needs
# cmake, ninja, a C/C++ compiler and python3. The first run compiles
# OpenThread: a few minutes.
#
# What this is not: the ESP-IDF settings port, NVS, the radio, a board. See the
# two programs' own header comments for what each leaves out.
set -euo pipefail

if [ "$#" -ne 1 ]; then
    echo "usage: IDF_PATH=<esp-idf> $0 <work directory>" >&2
    exit 2
fi
if [ -z "${IDF_PATH:-}" ] || [ ! -d "$IDF_PATH/components/openthread/openthread/tests/nexus" ]; then
    echo "IDF_PATH must name an ESP-IDF tree with its OpenThread submodule (and OpenThread's tests/nexus)" >&2
    exit 2
fi

export PYTHONDONTWRITEBYTECODE=1

repo="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$1"
work="$(cd "$1" && pwd)"
case "$work/" in
    "$repo"/*) echo "the work directory must be outside this repository" >&2; exit 2 ;;
    "$IDF_PATH"/*) echo "the work directory must be outside the ESP-IDF tree" >&2; exit 2 ;;
esac

ot="$IDF_PATH/components/openthread/openthread"
psa="$IDF_PATH/components/mbedtls/mbedtls/tf-psa-crypto"
store="$repo/components/power_base_ot_store"
stubs="$repo/tools/host_test_stubs/power_base_ot_store"
arch=()
if [ "$(uname -s)" = "Darwin" ]; then
    arch=(-arch "$(uname -m)")
fi

echo "== ESP-IDF: $IDF_PATH"
echo "== OpenThread: $(GIT_OPTIONAL_LOCKS=0 git -C "$ot" rev-parse HEAD 2>/dev/null || echo unknown)"
echo "== work directory: $work"

# ---------------------------------------------------------------- OpenThread

nexus="$work/ot_nexus"
if [ ! -f "$nexus/build.ninja" ]; then
    cmake -S "$ot" -B "$nexus" -GNinja -DOT_PLATFORM=nexus -DOT_COMPILE_WARNING_AS_ERROR=OFF -DOT_THREAD_VERSION=1.4 \
        -DOT_APP_CLI=OFF -DOT_APP_NCP=OFF -DOT_APP_RCP=OFF -DOT_15_4=ON -DOT_NEXUS_BUILD_TESTS=ON \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        -DOT_PROJECT_CONFIG="$ot/tests/nexus/openthread-core-nexus-config.h" > "$work/ot_nexus.configure.log" 2>&1
fi
# One of OpenThread's own Nexus tests: building it builds every library this
# program needs, and its compile command is the one to compile with.
ninja -C "$nexus" nexus_form_join > "$work/ot_nexus.build.log" 2>&1

out="$work/pogo"
mkdir -p "$out"
flags_file="$out/nexus_cxxflags.txt"
python3 - "$nexus" > "$flags_file" <<'PY'
import json, shlex, sys
for entry in json.load(open(sys.argv[1] + "/compile_commands.json")):
    if entry["file"].endswith("tests/nexus/test_form_join.cpp"):
        args, keep, i = shlex.split(entry["command"]), [], 1
        while i < len(args):
            if args[i] == "-o":
                i += 2
            elif args[i] == "-c" or args[i].endswith("test_form_join.cpp"):
                i += 1
            else:
                keep.append(args[i])
                i += 1
        print("\n".join(keep))
        break
else:
    sys.exit("no compile command for OpenThread's test_form_join.cpp")
PY
cxxflags=()
while IFS= read -r flag; do
    cxxflags+=("$flag")
done < "$flags_file"

# OpenThread's own settings platform file, unchanged, under the names --wrap
# gives the wrapped functions.
c++ "${cxxflags[@]}" \
    -DotPlatSettingsGet=__real_otPlatSettingsGet -DotPlatSettingsSet=__real_otPlatSettingsSet \
    -DotPlatSettingsAdd=__real_otPlatSettingsAdd -DotPlatSettingsDelete=__real_otPlatSettingsDelete \
    -DotPlatSettingsWipe=__real_otPlatSettingsWipe \
    -c "$ot/tests/nexus/platform/nexus_settings.cpp" -o "$out/nexus_settings_real.o"
cc -std=c11 -Wall -Wextra -g "${arch[@]}" -DPB_VOLATILE_STORE_TEST_HOOKS -I"$store/include" -I"$ot/include" -I"$stubs/psa" \
    -c "$store/pb_volatile_store.c" -o "$out/pb_volatile_store_ot.o"
c++ "${cxxflags[@]}" -I"$store/include" -I"$stubs/psa" \
    -c "$repo/tools/ot_host_repro/pb_release_session_nexus.cpp" -o "$out/pb_release_session_nexus.o"
(cd "$nexus" && c++ -g "${arch[@]}" "$out/pb_release_session_nexus.o" "$out/pb_volatile_store_ot.o" "$out/nexus_settings_real.o" \
    -o "$out/pb_release_session_nexus" \
    src/cli/libopenthread-cli-ftd.a tests/nexus/libot-nexus-platform.a src/core/libopenthread-ftd.a \
    tests/nexus/libot-nexus-platform.a third_party/mbedtls/repo/library/libmbedtls.a \
    third_party/mbedtls/repo/library/libmbedcrypto.a src/core/libopenthread-ftd.a third_party/tcplp/libtcplp-ftd.a \
    src/core/libopenthread-ftd.a third_party/mbedtls/repo/library/libmbedx509.a \
    third_party/mbedtls/repo/library/libmbedcrypto.a third_party/mbedtls/repo/3rdparty/everest/libeverest.a \
    third_party/mbedtls/repo/3rdparty/p256-m/libp256m.a 2> "$work/ot_nexus.link.log")

echo
echo "== OpenThread (Nexus): the release session"
# OpenThread's own log lines start with "<node id>"; what this prints are the
# checks.
set +e
"$out/pb_release_session_nexus" 2>&1 | grep -v '^<[0-9]' | tee "$out/pb_release_session_nexus.log"
nexus_status=${PIPESTATUS[0]}
set -e

# ---------------------------------------------------------------- the PSA key store

psa_build="$work/tf_psa_crypto"
shim="$work/tf_psa_crypto_shim"
mkdir -p "$shim/mbedtls"
# ESP-IDF's copy includes two headers under names its own port supplies
# (components/mbedtls/port/include/mbedtls); for a host build they are these.
printf '#include "mbedtls/private/bignum.h"\n' > "$shim/mbedtls/bignum.h"
printf '#include "mbedtls/private/ecp.h"\n' > "$shim/mbedtls/ecp.h"
if [ ! -f "$psa_build/build.ninja" ]; then
    cmake -S "$psa" -B "$psa_build" -GNinja -DENABLE_TESTING=OFF -DENABLE_PROGRAMS=OFF -DGEN_FILES=OFF \
        -DTF_PSA_CRYPTO_FATAL_WARNINGS=OFF -DCMAKE_C_FLAGS="-I$shim" -DCMAKE_BUILD_TYPE=Debug \
        > "$work/tf_psa_crypto.configure.log" 2>&1
fi
ninja -C "$psa_build" > "$work/tf_psa_crypto.build.log" 2>&1

inc=(-I"$shim" -I"$psa/drivers/builtin/include" -I"$psa_build/include" -I"$psa/include" -I"$psa/core" -I"$psa/dispatch"
     -I"$psa/extras" -I"$psa/platform" -I"$psa/utilities")
# TF-PSA-Crypto's own file storage, unchanged, under the __real_ names.
cc -g "${arch[@]}" "${inc[@]}" \
    -Dpsa_its_get_info=__real_psa_its_get_info -Dpsa_its_get=__real_psa_its_get \
    -Dpsa_its_set=__real_psa_its_set -Dpsa_its_remove=__real_psa_its_remove \
    -c "$psa/core/psa_its_file.c" -o "$out/psa_its_file_real.o"
cc -std=c11 -Wall -Wextra -g "${arch[@]}" -DPB_VOLATILE_STORE_TEST_HOOKS -I"$store/include" -I"$stubs/ot" "${inc[@]}" \
    -c "$store/pb_volatile_store.c" -o "$out/pb_volatile_store_psa_gate.o"
# The read-back and the erase of the stores themselves (the firmware's file),
# its settings half on the functional NVS simulator.
nvs_stubs=(-I"$repo/tools/host_test_stubs/nvs_sim" -I"$repo/tools/host_test_stubs")
cc -std=c11 -Wall -Wextra -g "${arch[@]}" -DPB_VOLATILE_STORE_TEST_HOOKS -I"$store/include" "${inc[@]}" "${nvs_stubs[@]}" \
    -c "$store/pb_stored_network.c" -o "$out/pb_stored_network_psa.o"
cc -std=c11 -Wall -Wextra -g "${arch[@]}" "${nvs_stubs[@]}" \
    -c "$repo/tools/host_test_stubs/nvs_sim/nvs_sim.c" -o "$out/nvs_sim.o"
cc -std=c11 -Wall -Wextra -g "${arch[@]}" -I"$store/include" -I"$stubs/ot" "${inc[@]}" "${nvs_stubs[@]}" \
    -c "$repo/tools/ot_host_repro/pb_volatile_store_psa.c" -o "$out/pb_volatile_store_psa.o"
libs=("$psa_build/core/libtfpsacrypto.a" "$psa_build/drivers/builtin/libbuiltin.a" "$psa_build/drivers/everest/libeverest.a"
      "$psa_build/drivers/p256-m/libp256-m.a" "$psa_build/drivers/pqcp/libpqcp.a" "$psa_build/platform/libplatform.a"
      "$psa_build/utilities/libutilities.a" "$psa_build/extras/libextras.a")
cc -g "${arch[@]}" "$out/pb_volatile_store_psa.o" "$out/pb_volatile_store_psa_gate.o" "$out/pb_stored_network_psa.o" \
    "$out/nvs_sim.o" "$out/psa_its_file_real.o" \
    -o "$out/pb_volatile_store_psa" "${libs[@]}" "${libs[@]}" 2> "$work/tf_psa_crypto.link.log"

echo
echo "== PSA key store (TF-PSA-Crypto): the switch in front of its storage"
# The program keeps its "flash" as files in the current directory and wants an
# empty one.
run="$out/psa_run.$$"
mkdir "$run"
set +e
(cd "$run" && "$out/pb_volatile_store_psa") 2>&1 | tee "$out/pb_volatile_store_psa.log"
psa_status=${PIPESTATUS[0]}
set -e
rmdir "$run" 2>/dev/null || { echo "the program left files in its storage directory: $run" >&2; psa_status=1; }

echo
if [ "$nexus_status" -eq 0 ] && [ "$psa_status" -eq 0 ]; then
    echo "PASS: both reproductions"
    exit 0
fi
echo "FAIL: OpenThread=$nexus_status PSA=$psa_status"
exit 1
