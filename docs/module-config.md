# Module config

**This document covers `module/`'s `module_config` (the sensor-pairing
config). The dedicated power-base project (`power_base/`) has its own,
separate, much smaller `power_base_config` (no sensor fields at all) -- see
[power-base.md](power-base.md) section 14 for its schema and
`tools/pack_power_base_config.py` for its generator. It reuses this
document's `gpio_profile.c` GPIO-usability policy directly rather than
duplicating it.**

## Fixed vs. dynamic, and why they're in separate NVS partitions

| Fixed (this document)                          | Dynamic (`role_manager`/`child_service`/`parent_service`/OpenThread) |
|-------------------------------------------------|------------------------------------------------------------------------|
| driver_id, sensor_type_bitmap                   | ROOT / INTERMEDIATE / LEAF                                              |
| I2C/SPI/UART bus, GPIOs, address/clock, calibration | Leader / Router / Child(SED) Thread role                             |
| measurement_timeout_ms                          | Parent/child relationship, assigned UUID, slot/depth                   |
| UPSTREAM/DOWNSTREAM port GPIOs + UART settings   | Thread Active Dataset, polling period, session/retry state             |

The fixed side never changes for a given ESP32-H2 across its whole life --
it is decided once, at manufacture/assembly, when a specific sensor gets
wired to a specific ESP32-H2 (section 2 of the implementation spec: no
hot-swap, no driver auto-detection, no per-boot re-discovery). The dynamic
side is rewritten every time the module gets docked somewhere new. Keeping
them in different NVS **partitions** (not just different namespaces in one)
means an operation that legitimately needs to nuke all dynamic state (e.g. a
factory-reset-style "forget this network") can erase the `"nvs"` partition
alone without touching, or needing to re-flash, the sensor pairing.

```
nvs   partition  (module/partitions.csv)  <- dynamic: OpenThread's own
                                              dataset copy + role_manager/
                                              child_service/parent_service
                                              state (namespace "child_dyn").
cfg   partition                            <- fixed: module_config
                                              (namespace "module_cfg") AND,
                                              separately, the dev-board
                                              override (namespace
                                              "dev_cfg") -- see
                                              pogo-interface.md.
```

## The `module_config` blob wire layout

`module_config_load()` (`components/module_config/module_config.c`) reads
exactly two NVS values from the `"cfg"` partition, namespace `"module_cfg"`:

- `blob` (binary, 90-byte schema v1 or 104-byte schema v2, big-endian -- see the exact
  byte offsets in `module_config.c`'s header comment)
- `crc` (u16): CRC16/CCITT-FALSE over `blob`, using the same algorithm as
  `components/pogo_proto/pogo_crc16.c`.

Both must be present, the length must match its schema, and the CRC must
match. Schema v1 remains accepted for existing I2C/SPI images; schema v2
adds `uart_tx_gpio`, `uart_rx_gpio`, `uart_baud_rate`, `uart_mode`, and
`uart_warmup_ms`. Beyond that, `validate_config()` additionally
rejects a structurally-valid-but-nonsensical config:

- `bus_type` other than I2C(0)/SPI(1)/UART(2).
- `measurement_timeout_ms == 0`.
- an enabled port (`uart_num >= 0`) with `tx_gpio`/`rx_gpio` unset.
- UPSTREAM and DOWNSTREAM both enabled on the same `uart_num`.
- any GPIO number outside `[-1, 31]` (`-1` = "not present on this board").
- the same GPIO number assigned to more than one function (across both
  ports' TX/RX/DETECT_IN/DETECT_OUT/BASE_ID and the active bus's I2C/SPI/UART
  pins).
- `detect_out_active_low = false` on an enabled `detect_out_gpio` --
  `pogo_port`'s DETECT_OUT drive logic is hardcoded open-drain active-low
  (see pogo-interface.md), so a config claiming active-high would silently
  behave as active-low anyway, which is worse than refusing to boot.

`tools/pack_module_config.py` checks the same `measurement_timeout_ms`,
`bus_type`, and `detect_out_active_low` conditions (plus the mock-driver-vs-
`dev_mode` check below) at **generation** time too, so a bad `configs/*.json`
is caught before a `.bin` is ever produced, not only after flashing it.

**Any** failure of any of the checks above logs one `CONFIG_ERROR: ...` line
and the caller (`module/main/app_main.c`) halts before starting the sensor,
POGO pairing, or any Thread activity -- there is
no partial-config fallback and no "try anyway."

The separate `"dev_cfg"` namespace (same partition) holds one `dev_mode` u8
key (`module_config_load_dev_mode()`), defaulting to `AUTO` if the whole
namespace is absent -- which is always true for a real production image,
since only the dev config images below ever write it.

## Generating a config image

`tools/pack_module_config.py` takes a human-editable JSON file (see
`configs/dev_root.json`, `configs/dev_leaf.json`, and the illustrative
`configs/module_config.example.json`), packs the schema-specific blob, computes its
CRC16, and calls the ESP-IDF NVS partition generator to emit a flashable
`.bin`:

```bash
source ~/.espressif/v6.1/esp-idf/export.sh   # provides IDF_PATH + the nvs_partition_gen.py tool

python3 tools/pack_module_config.py configs/dev_root.json build/cfg_images/dev_root_config.bin
python3 tools/pack_module_config.py configs/dev_leaf.json build/cfg_images/dev_leaf_config.bin
python3 tools/pack_module_config.py configs/prototype_pms5003.json build/cfg_images/prototype_pms5003_config.bin

# or, both at once:
bash tools/gen_config.sh build/cfg_images
```

`prototype_pms5003.json` is the terminal-only exception to the shared
two-port I2C prototype profile. It uses schema v2, UART1/GPIO0/1 at 9600
baud in PASSIVE mode, a 30-second warm-up marker, and a fully disabled
downstream port. The packer and runtime loader both reject a PMS5003 config
that violates any of those ownership rules.

Each run prints the `dev_mode`, `driver_id`, and computed CRC it packed, so a
mismatch against what you expect is visible immediately rather than only
showing up as a boot-time `CONFIG_ERROR` later.

The output size (`0x3000` = 12288 bytes by default) must match the `cfg`
partition's size in `module/partitions.csv` -- `pack_module_config.py`
takes `--size` if you ever change that.

## ESP32-H2 GPIO validation (section 1 of the follow-up spec)

`validate_config()` now also enforces a chip-specific GPIO profile, since
`module_config` is only ever used by the `module` project and that project
only ever targets ESP32-H2 (and, per the follow-up spec, the ESP32-H2
SuperMini dev boards ARE the final prototype hardware -- no separate PCB is
planned):

- Every GPIO actually assigned must be `-1` or in `[0, 27]`.
- No GPIO may be one of the boot strapping pins (8, 9, 25), the pins not
  bonded out on the ESP32-H2 SuperMini/SiP package at all (6, 7), the
  internal SPI flash pins (15-21), or the USB Serial/JTAG pins (26, 27) --
  see docs/pogo-interface.md's confirmed GPIO profile for the reasoning and
  the corrections this forced (UPSTREAM `DETECT_IN` moved off GPIO13, the
  SuperMini's onboard LED pin, to GPIO6, then off GPIO6 -- not actually
  bonded out on this package -- to GPIO2, and (this session) off GPIO2 to
  GPIO12, an ESP32-H2 RTC GPIO channel usable as a future terminal-leaf
  Deep-sleep wake source -- `DETECT_OUT` took the vacated GPIO2). GPIO2-5
  are ordinary,
  allowed GPIOs under this firmware's USB-Serial/JTAG console configuration
  (NOT strapping pins); GPIO13 is not rejected by this chip-level check at
  all (its avoidance is a board-level LED-conflict decision, made in
  `configs/prototype_*.json`, not a chip-level rule). The exact
  classification lives in `components/module_config/gpio_profile.c`
  (`gpio_profile_reserved_reason()`), mirrored bit-for-bit by
  `tools/pack_module_config.py`'s `STRAPPING_GPIOS`/`NOT_BONDED_OUT_GPIOS`/
  `FLASH_GPIOS`/`USB_GPIOS` and checked for drift by
  `tools/test_gpio_profile_parity.py`.
- An enabled port's `uart_num` must be exactly 0 or 1 -- ESP32-H2 has only
  two UART controllers, and both are permanently committed to the two POGO
  ports.
- `downstream_port.base_id_gpio` must always be `-1` (BASE_ID is
  UPSTREAM-only), regardless of `dev_mode`.
- **Only when `dev_mode == AUTO`**: an enabled port must also have both
  `detect_in_gpio` and `detect_out_gpio` set, and `upstream_port.base_id_gpio`
  must be set -- real hardware cannot derive ROOT/INTERMEDIATE/LEAF without
  them. `configs/dev_root.json`/`configs/dev_leaf.json` (`dev_mode` !=
  `AUTO`) are exempt from this specific check and keep validating exactly as
  before -- see docs/pogo-interface.md's "development compat mode" section.

Every failure logs one `CONFIG_ERROR: <field>=<value> ...` line naming the
specific field and value, e.g.
`CONFIG_ERROR: upstream_port.detect_in_gpio=6 is reserved on ESP32-H2 (not bonded out on ESP32-H2 SuperMini/SiP package)`.

## driver_id, sensor_type_bitmap, and the common registries

Section 2 of the follow-up spec asks that `driver_id` and the channel
bitmap live in one explicit common header each, never repeated as bare
numbers per config file:

- `driver_id` values are `#define`d once in
  `components/sensor_provider/include/sensor_provider/sensor_provider.h`
  (`SENSOR_DRIVER_ID_MOCK`/`BH1750`/`SHT45`/`SGP40`/`SGP41`/`PMS5003`, each a
  4-byte ASCII tag like the pre-existing `MOCK`).
- Channel bit positions (shared by `module_config.sensor_type_bitmap` and
  the per-sample wire format's channel `type` byte -- see the next section)
  are the `sensor_channel_type_t` enum in
  `components/sensor_channel/include/sensor_channel/sensor_channel.h`.

`tools/pack_module_config.py`'s `DRIVER_IDS`/`CHANNEL_BITS` dicts mirror
these two headers bit-for-bit (with a "must match" comment, the same pattern
already used for `MOCK_DRIVER_ID`) and let a `configs/*.json` file reference
either the mnemonic name or the raw numeric value:

```json
"driver_id": "SHT45",
"sensor_type_bitmap": ["TEMPERATURE", "HUMIDITY"],
```

resolves to exactly the same bytes as the old numeric style still used by
`configs/dev_root.json`/`configs/dev_leaf.json` (`"driver_id": "0x4D4F434B"`,
`"sensor_type_bitmap": 1`) -- both forms remain valid, so the pre-existing
dev configs needed no change.

## Prototype configs (real AUTO-mode hardware)

`configs/prototype_sht45.json`, `configs/prototype_bh1750.json`,
`configs/prototype_sgp40.json`, and `configs/prototype_sgp41.json` are the
four `dev_mode=AUTO` configs for ordinary I2C sensor boards on the confirmed
GPIO profile (see docs/pogo-interface.md) -- all four share the identical
`upstream_port`/`downstream_port` block; only `driver_id`, `i2c_address`,
`sensor_type_bitmap`, `measurement_timeout_ms`, and `calibration` differ.
`tools/test_prototype_port_blocks_match.py` checks this mechanically for all
four (byte-for-byte port comparison, plus driver_id/bitmap/address decoded
back out of the actual packed NVS blob, plus a parity check that this
document's driver_id/channel tables below never drift from
`tools/pack_module_config.py`'s `DRIVER_IDS`/`CHANNEL_BITS` dicts or the C
headers they mirror).

| | SHT45 | BH1750 | SGP40 | SGP41 |
|---|---|---|---|---|
| `driver_id` | `SHT45` (`0x53483435`) | `BH1750` (`0x42483137`) | `SGP40` (`0x53473430`) | `SGP41` (`0x53473431`) |
| `i2c_address` | `0x44` | `0x23` (ADDR low/floating -- confirm the actual breakout's strap) | `0x59` (fixed, no ADDR strap) | `0x59` (fixed, same address family as SGP40 -- a board never pairs both) |
| `sensor_type_bitmap` | `["TEMPERATURE", "HUMIDITY"]` | `["ILLUMINANCE"]` | `["VOC_RAW", "VOC_INDEX"]` | `["VOC_RAW", "VOC_INDEX", "NOX_RAW", "NOX_INDEX"]` |
| `measurement_timeout_ms` | 200 | 300 | 200 | 200 |

SGP40/SGP41 notes (real hardware for these two boards is still **not**
wired/tested -- see docs/validation-status.md):

- `measurement_timeout_ms=200` is chosen the same way as SHT45's: it must
  stay comfortably above the driver's own hardware measurement ceiling
  (`components/sgp40/sgp40.c`'s `SGP40_MEASURE_MS_MAX`=30ms;
  `components/sgp41/sgp41.c`'s `SGP41_CONDITIONING_MS_MAX`/
  `SGP41_MEASURE_MS_MAX`=50ms each) so a normal reading never gets
  misreported as `SENSOR_STATUS_TIMEOUT`, while still being short enough to
  abandon a genuinely wedged sensor without stalling `child_service`'s
  `SENSOR_HELLO` handler for long. Both drivers separately floor the
  *sampling* period (not this per-read timeout) at 1000ms via
  `get_min_polling_period_ms()`, since the Sensirion Gas Index Algorithm is
  defined for ~1 Hz sampling -- see `tools/test_effective_polling_period.c`.
- SGP41's first ~10s (`SGP41_CONDITIONING_WINDOW_MS`) after init report only
  the two VOC channels, tagged `SENSOR_CHANNEL_STATUS_CONDITIONING` --
  `sensor_type_bitmap` still lists all four channels (the driver's steady-
  state capability set), matching the same convention PMS5003's 30s warm-up
  already uses. `tools/test_conditioning_provisioning.c` already covers
  `parent_service.c` accepting a `CONDITIONING` sensor_status during
  provisioning; this is unchanged for SGP40/SGP41.
- Both drivers always compensate against Sensirion's own fixed documented
  default (50%RH / 25degC) -- neither reads this board's real ambient
  humidity/temperature, and neither borrows another module's SHT45 reading
  over the wire. **Do not mistake VOC_INDEX/NOX_INDEX values from these
  boards for humidity/temperature-corrected readings** -- both drivers log
  this loudly (`ESP_LOGW`) at every init.

```bash
python3 tools/pack_module_config.py configs/prototype_sht45.json build/cfg_images/prototype_sht45_config.bin
python3 tools/pack_module_config.py configs/prototype_bh1750.json build/cfg_images/prototype_bh1750_config.bin
python3 tools/pack_module_config.py configs/prototype_sgp40.json build/cfg_images/prototype_sgp40_config.bin
python3 tools/pack_module_config.py configs/prototype_sgp41.json build/cfg_images/prototype_sgp41_config.bin
# or, along with dev_root/dev_leaf/prototype_pms5003, all at once:
bash tools/gen_config.sh build/cfg_images
```

## driver_id and the mock sensor

`driver_id` selects exactly one compiled-in `sensor_provider` driver (see
`components/sensor_provider/sensor_provider.h`) via a fixed registry lookup
-- an unrecognized `driver_id` is `CONFIG_ERROR`, never "probe every driver
and see which responds" (section 11).

`SENSOR_DRIVER_ID_MOCK` (`0x4D4F434B`, ASCII `"MOCK"`) selects the
development-only mock driver (`components/sensor_provider/mock_sensor_provider.c`):
fixed `sensor_type_bitmap=0x0001`, fixed 120ms simulated measurement delay,
configurable `measurement_timeout_ms`. It always logs
`mock sensor_provider ACTIVE (development only, ...)` at init.

`module/main/app_main.c` refuses to start
(`CONFIG_ERROR: mock sensor_provider selected with dev_mode=AUTO`) if
`driver_id == SENSOR_DRIVER_ID_MOCK` while `dev_mode == AUTO` -- i.e. mock
can only ever run alongside an explicit `DEV_ROOT`/`DEV_LEAF` override,
never silently in what looks like a production image.

## Flashing a config image (no app re-flash needed)

`pogo_module.bin` never changes between boards; only the `cfg` partition
image does. Given the offsets in `module/partitions.csv` (`cfg` at
`0x187000`, from the build in this repo):

```bash
python -m esptool --chip esp32h2 -p /dev/tty.usbmodemXXXXXX \
    write-flash 0x187000 build/cfg_images/dev_root_config.bin
```

(Re-run `idf.py partition-table` or check the printed offsets if you ever
resize a partition -- don't hardcode `0x187000` blindly if `partitions.csv`
changes.)
