/*
 * What this firmware image is: component, product version and build identity
 * (docs/versioning.md). The values come from the repository's versions.json
 * and from tools/pogo_version.py at build time; nothing here is typed in.
 *
 * The product version is also in ESP-IDF's application description
 * (PROJECT_VER, see tools/cmake/pogo_version.cmake). The two must agree, and
 * tools/pogo_version.py manifest-create checks that they do in the built
 * image before it describes it.
 *
 * None of this is a protocol: no BLE message, CoAP payload, NVS record or
 * Matter attribute carries these strings, and nothing decides what a peer
 * supports by comparing them. Capabilities are negotiated where they always
 * were.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* "POGO_BUILD_INFO{component=...;version=...;build=...;kind=...;variant=...;
 * commit=...;dirty=...;fingerprint=...;config=...}" -- one string, kept in
 * the image so that it can be read back out of the .bin. */
const char *pogo_build_info_string(void);

/* One log line, once, early in app_main(): component, version, build id,
 * build kind and variant. Prints nothing in a build whose log level is NONE
 * (the RCP: its USB port is the Spinel link). */
void pogo_build_info_log(void);

#ifdef __cplusplus
}
#endif
