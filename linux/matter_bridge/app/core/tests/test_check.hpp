// Shared CHECK macro for this core library's host tests -- mirrors the
// style already used by linux/matter_bridge/ingest/tests (see that
// directory's test_registry_and_session.cpp) so both test suites read the
// same way. Each Run*Tests() function owns its own `failures` counter and
// passes a pointer to it, rather than a single shared global, so the three
// test files here can run in any order without interfering with each
// other's counts.
#pragma once

#include <cstdio>

#define MB_CHECK(cond, msg)                                                                                          \
    do {                                                                                                             \
        if (!(cond)) {                                                                                               \
            std::printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);                                             \
            (*failures)++;                                                                                          \
        } else {                                                                                                     \
            std::printf("ok:   %s\n", msg);                                                                         \
        }                                                                                                            \
    } while (0)
