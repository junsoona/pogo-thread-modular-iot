#!/bin/bash
#
# Builds and runs the host reproduction behind "the same hub at another
# Thread address" (docs/power-base-management.md, section 12) against the
# very OpenThread sources ESP-IDF builds the firmware from:
#
#   tools/ot_host_repro/pb_locate_nexus.cpp
#       the real OpenThread core ($IDF_PATH/components/openthread/openthread),
#       four instances (fifteen in its last run: a network with more routers
#       than one round asks) on OpenThread's own in-process "Nexus" simulation, with
#       the board's real management client (components/power_base_mgmt), its
#       real enrollment record code (components/power_base_enrollment, on the
#       NVS simulator) and the firmware's real hint collection
#       (power_base/main/enrollment_locate_hints.c).
#
#   IDF_PATH=~/.espressif/v6.1/esp-idf \
#       tools/test_power_base_locate_openthread.sh <work directory>
#
# <work directory> is created when missing and holds everything this script
# writes (the out-of-tree OpenThread build and the test program); an existing
# one is reused, which makes a second run quick -- also one made by
# tools/test_power_base_release_session_openthread.sh, whose OpenThread build
# is the same. Nothing is written into the ESP-IDF tree or into this
# repository. Needs cmake, ninja, a C/C++ compiler and python3. The first run
# compiles OpenThread: a few minutes.
#
# What this is not: the ESP-IDF port, NVS, the radio, a board, and Linux. See
# the program's own header comment.
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
arch=()
if [ "$(uname -s)" = "Darwin" ]; then
    arch=(-arch "$(uname -m)")
fi

echo "== ESP-IDF: $IDF_PATH"
echo "== OpenThread: $(GIT_OPTIONAL_LOCKS=0 git -C "$ot" rev-parse HEAD 2>/dev/null || echo unknown)"
echo "== work directory: $work"

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

out="$work/pogo_locate"
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

stubs="$repo/tools/host_test_stubs"
inc=(-I"$stubs/nvs_sim" -I"$stubs" -I"$repo/components/power_base_enrollment/include"
     -I"$repo/components/power_base_mgmt/include" -I"$repo/power_base/main")
cflags=(-std=c11 -Wall -Wextra -g "${arch[@]}")
# The board's code, as the firmware compiles it (C), and the one file of
# power_base/main that only speaks OpenThread's public API.
cc "${cflags[@]}" "${inc[@]}" -c "$repo/components/power_base_mgmt/pb_mgmt_proto.c" -o "$out/pb_mgmt_proto.o"
cc "${cflags[@]}" "${inc[@]}" -c "$repo/components/power_base_mgmt/pb_mgmt_client.c" -o "$out/pb_mgmt_client.o"
cc "${cflags[@]}" "${inc[@]}" -c "$repo/components/power_base_enrollment/power_base_enrollment.c" -o "$out/power_base_enrollment.o"
cc "${cflags[@]}" "${inc[@]}" -c "$stubs/nvs_sim/nvs_sim.c" -o "$out/nvs_sim.o"
# OpenThread's real headers first: the host stubs directory has stand-ins under
# the same names (for the tests that run without OpenThread).
cc "${cflags[@]}" -I"$ot/include" "${inc[@]}" -c "$repo/power_base/main/enrollment_locate_hints.c" -o "$out/enrollment_locate_hints.o"
cc "${cflags[@]}" "${inc[@]}" -c "$repo/tools/ot_host_repro/pb_locate_glue.c" -o "$out/pb_locate_glue.o"
c++ "${cxxflags[@]}" "${inc[@]}" -c "$repo/tools/ot_host_repro/pb_locate_nexus.cpp" -o "$out/pb_locate_nexus.o"
(cd "$nexus" && c++ -g "${arch[@]}" "$out/pb_locate_nexus.o" "$out/pb_mgmt_proto.o" "$out/pb_mgmt_client.o" \
    "$out/power_base_enrollment.o" "$out/nvs_sim.o" "$out/enrollment_locate_hints.o" "$out/pb_locate_glue.o" \
    -o "$out/pb_locate_nexus" \
    src/cli/libopenthread-cli-ftd.a tests/nexus/libot-nexus-platform.a src/core/libopenthread-ftd.a \
    tests/nexus/libot-nexus-platform.a third_party/mbedtls/repo/library/libmbedtls.a \
    third_party/mbedtls/repo/library/libmbedcrypto.a src/core/libopenthread-ftd.a third_party/tcplp/libtcplp-ftd.a \
    src/core/libopenthread-ftd.a third_party/mbedtls/repo/library/libmbedx509.a \
    third_party/mbedtls/repo/library/libmbedcrypto.a third_party/mbedtls/repo/3rdparty/everest/libeverest.a \
    third_party/mbedtls/repo/3rdparty/p256-m/libp256m.a 2> "$work/ot_nexus.locate.link.log")

echo
echo "== OpenThread (Nexus): finding the hub again"
# OpenThread's own log lines start with "<node id>", the board code's with a
# log level; what this prints are the checks.
set +e
"$out/pb_locate_nexus" 2>&1 | grep -v '^<[0-9]' | tee "$out/pb_locate_nexus.log"
status=${PIPESTATUS[0]}
set -e
echo
if [ "$status" -eq 0 ]; then
    echo "PASS: the reproduction"
    exit 0
fi
echo "FAIL: exit $status"
exit 1
