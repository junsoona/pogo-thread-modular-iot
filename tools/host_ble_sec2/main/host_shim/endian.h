/* macOS only: the two byte-order helpers ESP-IDF's security2.c takes from
 * <endian.h> on its other hosts. */
#pragma once
#include <libkern/OSByteOrder.h>
#define htobe32(x) OSSwapHostToBigInt32(x)
#define be32toh(x) OSSwapBigToHostInt32(x)
