// Shared CHECK macro for this library's host tests -- same style as
// linux/matter_bridge/ingest/tests and linux/matter_bridge/app/core/tests
// (see that directory's own test_check.hpp), so all three test suites in
// this repo read the same way.
#pragma once

#include <cstdio>

#define API_CHECK(cond, msg)                                                                                        \
    do {                                                                                                             \
        if (!(cond)) {                                                                                              \
            std::printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);                                             \
            (*failures)++;                                                                                          \
        } else {                                                                                                    \
            std::printf("ok:   %s\n", msg);                                                                         \
        }                                                                                                           \
    } while (0)
