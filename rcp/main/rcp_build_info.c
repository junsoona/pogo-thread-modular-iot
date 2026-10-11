/*
 * What this RCP image is: component, product version and build identity
 * (docs/versioning.md). One string, compiled in from a header that
 * tools/pogo_version.py writes into the build directory at every build
 * (main/CMakeLists.txt).
 *
 * Nothing in the firmware reads or prints it. The production RCP builds with
 * the log level NONE because its USB port carries raw Spinel bytes
 * (docs/otbr-rcp.md), and this file does not change that: there is no log
 * call, no console and no Spinel property here. The string is found in the
 * .bin by its "POGO_BUILD_INFO{" prefix (tools/pogo_version.py appdesc).
 *
 * The RCP has its own copy of these few lines instead of using
 * components/pogo_build_info on purpose: this project shares no component
 * with the other firmware projects (tools/test_rcp_config.py).
 */
#include "pogo_build_info_gen.h"

static const char k_build_info[] = POGO_BUILD_INFO_STRING;

/* Kept in the image by `-u pogo_build_info_string` (main/CMakeLists.txt). */
const char *pogo_build_info_string(void);
const char *pogo_build_info_string(void)
{
    return k_build_info;
}
