# Module config

## Fixed vs. dynamic, and why they're in separate NVS partitions

| Fixed (this document)                          | Dynamic (`role_manager`/`child_service`/`parent_service`/OpenThread) |
|-------------------------------------------------|------------------------------------------------------------------------|
| driver_id, sensor_type_bitmap                   | ROOT / INTERMEDIATE / LEAF                                              |
| I2C/SPI bus, GPIOs, address/clock, calibration   | Leader / Router / Child(SED) Thread role                                |
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

- `blob` (binary, 90 bytes, big-endian multi-byte fields -- see the exact
  byte offsets in `module_config.c`'s header comment)
- `crc` (u16): CRC16/CCITT-FALSE over `blob`, using the same algorithm as
  `components/pogo_proto/pogo_crc16.c`.

Both must be present, `blob` must be exactly 90 bytes, the CRC must match,
and the unpacked `schema_version` must equal `MODULE_CONFIG_SCHEMA_VERSION`
(currently 1). Beyond that, `validate_config()` (same file) additionally
rejects a structurally-valid-but-nonsensical config:

- `bus_type` other than I2C(0)/SPI(1).
- `measurement_timeout_ms == 0`.
- an enabled port (`uart_num >= 0`) with `tx_gpio`/`rx_gpio` unset.
- UPSTREAM and DOWNSTREAM both enabled on the same `uart_num`.
- any GPIO number outside `[-1, 31]` (`-1` = "not present on this board").
- the same GPIO number assigned to more than one function (across both
  ports' TX/RX/DETECT_IN/DETECT_OUT/BASE_ID and the active bus's I2C/SPI
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
`configs/module_config.example.json`), packs the 90-byte blob, computes its
CRC16, and calls the ESP-IDF NVS partition generator to emit a flashable
`.bin`:

```bash
source ~/.espressif/v6.1/esp-idf/export.sh   # provides IDF_PATH + the nvs_partition_gen.py tool

python3 tools/pack_module_config.py configs/dev_root.json build/cfg_images/dev_root_config.bin
python3 tools/pack_module_config.py configs/dev_leaf.json build/cfg_images/dev_leaf_config.bin

# or, both at once:
bash tools/gen_config.sh build/cfg_images
```

Each run prints the `dev_mode`, `driver_id`, and computed CRC it packed, so a
mismatch against what you expect is visible immediately rather than only
showing up as a boot-time `CONFIG_ERROR` later.

The output size (`0x3000` = 12288 bytes by default) must match the `cfg`
partition's size in `module/partitions.csv` -- `pack_module_config.py`
takes `--size` if you ever change that.

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
