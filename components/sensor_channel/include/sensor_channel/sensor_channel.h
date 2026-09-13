/*
 * Common sensor-sample wire format, transported over the Thread data plane
 * (CoAP, see components/sensor_transport) -- NOT the POGO provisioning
 * protocol (components/pogo_proto), which is left alone (see docs/state-machine.md,
 * section 6 of the follow-up spec: "기존 POGO provisioning protocol은 가능한
 * 호환성 유지").
 *
 * A network layer (sensor_transport / CoAP) never needs to know any
 * per-sensor struct: it only ever moves an opaque, versioned, length-prefixed
 * byte buffer produced by sensor_channel_pack() and parsed back by
 * sensor_channel_unpack(). Every channel type this firmware currently knows
 * about is listed once, here -- module_config.sensor_type_bitmap (see
 * components/module_config/include/module_config/module_config.h) uses the
 * exact same bit positions as SENSOR_CHANNEL_* below, so "this permanently
 * paired sensor produces channel X" is expressed with one enum, not two.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SENSOR_CHANNEL_PAYLOAD_VERSION 1u

/*
 * Bit position in module_config.sensor_type_bitmap (u16) AND the wire
 * "type" byte of one channel entry (see sensor_channel_entry_t below) --
 * the two are deliberately the same enum, never two separate numberings.
 * Room for 16 channel types total (bitmap width); 10 are defined so far.
 * Adding SGP40/41/PMS5003 support (section 10/11 of the follow-up spec)
 * only ever means adding a new value here, never renumbering an existing one
 * (a shipped module's sensor_type_bitmap must keep meaning what it meant
 * when it was flashed).
 */
typedef enum {
    SENSOR_CHANNEL_ILLUMINANCE = 0,  /* BH1750 */
    SENSOR_CHANNEL_TEMPERATURE = 1,  /* SHT45; also SGP40/41 compensation input */
    SENSOR_CHANNEL_HUMIDITY    = 2,  /* SHT45; also SGP40/41 compensation input */
    SENSOR_CHANNEL_VOC_RAW     = 3,  /* SGP40, SGP41 */
    SENSOR_CHANNEL_VOC_INDEX   = 4,  /* SGP40, SGP41 (Sensirion Gas Index Algorithm) */
    SENSOR_CHANNEL_NOX_RAW     = 5,  /* SGP41 only */
    SENSOR_CHANNEL_NOX_INDEX   = 6,  /* SGP41 only (Sensirion Gas Index Algorithm) */
    SENSOR_CHANNEL_PM1_0       = 7,  /* PMS5003 */
    SENSOR_CHANNEL_PM2_5       = 8,  /* PMS5003 */
    SENSOR_CHANNEL_PM10        = 9,  /* PMS5003 */
    SENSOR_CHANNEL_TYPE_COUNT,
} sensor_channel_type_t;

/* Room for every channel type above in one sample, plus headroom -- see
 * SENSOR_CHANNEL_TYPE_COUNT. Also the array size of sensor_measurement_t.channels
 * in sensor_provider.h. */
#define SENSOR_CHANNEL_MAX_COUNT SENSOR_CHANNEL_TYPE_COUNT

/*
 * Every wire value is a signed 32-bit fixed-point integer; this says what
 * the integer units mean so a receiver never has to hardcode "channel X is
 * always milli-units" per channel type.
 */
typedef enum {
    SENSOR_SCALE_NONE  = 0, /* raw integer, driver-defined units (e.g. VOC/NOx raw ticks) */
    SENSOR_SCALE_MILLI = 1, /* value = real_value * 1000 (e.g. milli-lux, milli-degC, milli-%RH) */
    SENSOR_SCALE_MICRO = 2, /* value = real_value * 1000000 */
    SENSOR_SCALE_UNIT  = 3, /* value is already whole units (e.g. ug/m3, Gas Index 0..500) */
} sensor_scale_t;

/*
 * Per-channel measurement status. Values 0..4 mirror sensor_status_t
 * (sensor_provider.h) exactly on purpose (one status vocabulary, not two);
 * CRC_ERROR/RANGE_ERROR/CONDITIONING extend it for drivers that need to
 * distinguish those from a generic DRIVER_ERROR (SHT4x/SGP4x CRC checks,
 * SGP4x warm-up). Kept as a plain uint8_t on the wire, not the C enum type,
 * so a future value here never breaks pack/unpack of an old firmware's frame.
 */
typedef enum {
    SENSOR_CHANNEL_STATUS_OK              = 0,
    SENSOR_CHANNEL_STATUS_NOT_INITIALIZED = 1,
    SENSOR_CHANNEL_STATUS_TIMEOUT         = 2,
    SENSOR_CHANNEL_STATUS_BUS_ERROR       = 3,
    SENSOR_CHANNEL_STATUS_DRIVER_ERROR    = 4,
    SENSOR_CHANNEL_STATUS_CRC_ERROR       = 5,
    SENSOR_CHANNEL_STATUS_RANGE_ERROR     = 6,
    SENSOR_CHANNEL_STATUS_CONDITIONING    = 7, /* SGP4x warm-up: value present but not yet trustworthy */
} sensor_channel_status_t;

typedef struct {
    uint8_t type;   /* one of sensor_channel_type_t */
    uint8_t scale;  /* one of sensor_scale_t */
    uint8_t status; /* one of sensor_channel_status_t */
    int32_t value;  /* signed 32-bit fixed-point; meaning given by (type, scale) */
} sensor_channel_reading_t;

#define SENSOR_UUID_LEN 8

/*
 * One full sample: this module's permanently-paired sensor's reading(s) at
 * one point in time. sensor_channel_pack()/sensor_channel_unpack() are the
 * ONLY code that knows the wire byte layout -- callers (sensor_transport,
 * sensor_service) never memcpy this struct onto the wire directly (section 6:
 * "C struct 메모리를 그대로 송신하지 마라").
 */
typedef struct {
    uint8_t  payload_version;               /* SENSOR_CHANNEL_PAYLOAD_VERSION */
    uint8_t  module_uuid[SENSOR_UUID_LEN];  /* this module's provisioning UUID, or a MAC-derived
                                                pseudo-UUID if never provisioned (ROOT) -- see
                                                sensor_service_get_module_uuid() */
    uint32_t sample_seq;                    /* per-module monotonically increasing counter */
    uint32_t uptime_ms;                     /* monotonic time since this module's boot */
    uint16_t sensor_bitmap;                 /* this module's fixed sensor_type_bitmap */
    uint8_t  channel_count;
    sensor_channel_reading_t channels[SENSOR_CHANNEL_MAX_COUNT];
} sensor_sample_t;

/*
 * Wire layout (all multi-byte integers big-endian):
 *   0   1  payload_version
 *   1   8  module_uuid
 *   9   4  sample_seq
 *   13  4  uptime_ms
 *   17  2  sensor_bitmap
 *   19  1  channel_count (N)
 *   20  N*7  channel entries: type(1) scale(1) status(1) value(4, i32 BE)
 * = 20 + 7*N bytes, N in [0, SENSOR_CHANNEL_MAX_COUNT].
 */
#define SENSOR_CHANNEL_HEADER_LEN 20u
#define SENSOR_CHANNEL_ENTRY_LEN  7u
#define SENSOR_CHANNEL_MAX_WIRE_LEN (SENSOR_CHANNEL_HEADER_LEN + SENSOR_CHANNEL_ENTRY_LEN * SENSOR_CHANNEL_MAX_COUNT)

/*
 * Serializes `in` into `out` (capacity out_size). Returns the number of
 * bytes written, or 0 if: out_size is too small, channel_count exceeds
 * SENSOR_CHANNEL_MAX_COUNT, any channel's `type` is >= SENSOR_CHANNEL_TYPE_COUNT,
 * or two channels share the same `type` (a sample never reports the same
 * channel twice -- section 6: reject "duplicate" entries, not just duplicate
 * whole samples, which is the CoAP layer's (UUID,sample_seq) job -- see
 * components/sensor_transport).
 */
size_t sensor_channel_pack(const sensor_sample_t *in, uint8_t *out, size_t out_size);

/*
 * Parses `data`/`len` into `out`. Returns false for: a length that doesn't
 * exactly match SENSOR_CHANNEL_HEADER_LEN + 7*channel_count for the
 * channel_count byte actually present (no trailing garbage, no truncation),
 * an unsupported payload_version, channel_count > SENSOR_CHANNEL_MAX_COUNT,
 * an unknown channel `type` (>= SENSOR_CHANNEL_TYPE_COUNT), or a duplicate
 * channel `type` within the same sample.
 */
bool sensor_channel_unpack(const uint8_t *data, uint16_t len, sensor_sample_t *out);

#ifdef __cplusplus
}
#endif
