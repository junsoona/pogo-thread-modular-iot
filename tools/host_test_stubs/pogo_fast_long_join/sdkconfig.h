/*
 * tools/host_test_stubs/pogo_fast/sdkconfig.h with one difference: a Thread
 * join timeout far longer than a whole provisioning sequence, so
 * tools/test_module_upstream_regeneration.c can tell "the parent gave up its
 * wait for JOIN_RESULT because the upstream generation moved on" from "the
 * wait simply timed out".
 */
#pragma once

#include "../pogo_fast/sdkconfig.h"

#undef CONFIG_POGO_JOIN_TIMEOUT_MS
#define CONFIG_POGO_JOIN_TIMEOUT_MS 3000
