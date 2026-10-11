#include "pogo_build_info/pogo_build_info.h"

#include "esp_log.h"
#include "pogo_build_info_gen.h" /* generated into the build directory; see CMakeLists.txt */

static const char *TAG = "pogo_build";

/* In the image whether or not anything prints it (CMakeLists.txt keeps
 * pogo_build_info_string() from being garbage-collected at link time):
 * tools/pogo_version.py finds it in the .bin by its "POGO_BUILD_INFO{"
 * prefix. */
static const char k_build_info[] = POGO_BUILD_INFO_STRING;

const char *pogo_build_info_string(void)
{
    return k_build_info;
}

void pogo_build_info_log(void)
{
    /* "development" unless the build was made in release mode from a clean,
     * known commit. A dirty tree says so in the build id (+dirty). */
    ESP_LOGI(TAG, "POGO %s %s build %s (%s, %s)", POGO_BUILD_COMPONENT, POGO_BUILD_VERSION, POGO_BUILD_ID, POGO_BUILD_KIND,
             POGO_BUILD_VARIANT);
}
