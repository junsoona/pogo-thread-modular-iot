# Linux OTBR RCP firmware (`rcp/`)

This is a separate, independent ESP-IDF project: an ESP32-H2 flashed as an
OpenThread **Radio Co-Processor (RCP)**, connected to a Linux PC running
OTBR (OpenThread Border Router) over one USB cable. It is based on ESP-IDF
v6.1's official example, `$IDF_PATH/examples/openthread/ot_rcp`.

**Scope of this pass**: RCP firmware + this document + build verification
only. Nothing here joins the existing Power Base to the OTBR's Thread
network, and no Matter Bridge, deep-sleep, or POGO 5V work is done -- see
"What this does NOT do yet" below.

## 1. Overall structure

```
rcp/
  CMakeLists.txt        project(pogo_rcp) -- standard ESP-IDF project boilerplate
  partitions.csv         nvs + phy_init + factory app (copied verbatim from upstream)
  sdkconfig.defaults      OpenThread Radio-Only device, USB Serial/JTAG RCP transport,
                          all logging disabled -- see section 2 and section 4 below
  main/
    CMakeLists.txt        idf_component_register(..., PRIV_REQUIRES openthread nvs_flash
                          esp_event vfs console) -- see this file's own comment for why
                          these are listed explicitly instead of via an
                          idf_component.yml (item 7 of the original request)
    esp_ot_config.h        copied verbatim from upstream (unmodified, original SPDX
                          header intact) -- selects the USB host_connection_mode
                          branch since CONFIG_OPENTHREAD_RCP_UART/_SPI are both off
    esp_ot_rcp.c            app_main() itself is UNCHANGED from upstream (starts
                          OpenThread with the RCP config from esp_ot_config.h); the
                          upstream external-coexistence and IEEE 802.15.4
                          debug-console integrations were removed -- see that file's
                          own header comment
```

This project does **not** use `EXTRA_COMPONENT_DIRS` and does **not** touch
`components/` -- it shares nothing with `module`/`power_base`/`upper`/`lower`.
It is entirely standalone, exactly like how upstream's `ot_rcp` example is
its own project.

`rcp/` has no `idf_component.yml` anywhere (neither at the project root nor
under `main/`): upstream's example needs one only to pull in three
managed/local example-helper components (`ot_examples_common`,
`cmd_ieee802154_debug`, `esp-qa/coexist-cmd`) for FTD/CLI network-forming
helpers, IEEE 802.15.4 PHY debug commands, and external-coexistence GPIO
commands -- none of which this project enables (`CONFIG_OPENTHREAD_CLI=n`,
`CONFIG_IEEE802154_DEBUG` never set, `CONFIG_ESP_COEX_EXTERNAL_COEXIST_ENABLE`
never set). The one thing `ot_examples_common` actually did that THIS
project still needs -- pulling the in-tree `openthread`/`esp_openthread`
component chain into the build at all -- is declared directly via
`main/CMakeLists.txt`'s own `PRIV_REQUIRES` instead, so no managed
component of any kind is required to build `rcp/`.

## 2. Why USB Serial/JTAG RCP (not UART)

- **One cable.** The ESP32-H2 SuperMini's own USB port doubles as both the
  flashing/programming interface AND, once this firmware boots, the Spinel
  RCP link to the Linux host -- no separate UART-to-USB adapter, no extra
  wiring between the board and the host.
- **No UART GPIO wiring.** `CONFIG_OPENTHREAD_RCP_UART` is off; this project
  never defines `OPENTHREAD_UART_RX_PIN`/`OPENTHREAD_UART_TX_PIN` and has no
  Kconfig item for manually choosing UART RCP pins (upstream's
  `main/Kconfig.projbuild`, which only exists to configure THAT, was not
  carried into this project at all -- see section 1).
- Matches this repository's own POGO modules' physical reality: the RCP
  board sits next to a Linux PC on a bench/rack, not daisy-chained over
  POGO pins like `module`/`power_base` are to each other.

`CONFIG_OPENTHREAD_RCP_USB_SERIAL_JTAG=y` is a `choice` inside OpenThread's
own Kconfig (`OPENTHREAD_RCP_TRANSPORT`, mutually exclusive with UART/SPI --
see `$IDF_PATH/components/openthread/Kconfig`); its own `depends on` clause
additionally requires `CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG=y` and
`!CONFIG_OPENTHREAD_CONSOLE_TYPE_USB_SERIAL_JTAG`. ESP32-H2 already defaults
the first to "y" (any `SOC_USB_SERIAL_JTAG_SUPPORTED` chip whose PRIMARY
console isn't itself USB Serial/JTAG), and the second is automatically
satisfied because `CONFIG_OPENTHREAD_CONSOLE_ENABLE=n` disables that whole
choice -- but `rcp/sdkconfig.defaults` sets
`CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG=y` explicitly anyway, so this
dependency is a visible, checked-in fact rather than an implicit default
that could silently change in a future ESP-IDF release.

### Why ordinary logs never corrupt the Spinel stream

The USB Serial/JTAG port is a SINGLE endpoint. Nothing on this chip
separates "Spinel bytes" from "a stray `ESP_LOGI` line" at the hardware
level -- the separation here is that **no text ever gets written at all**:

```
CONFIG_LOG_DEFAULT_LEVEL_NONE=y
CONFIG_OPENTHREAD_LOG_LEVEL_NONE=y
CONFIG_BOOTLOADER_LOG_LEVEL_ERROR=y      # bootloader itself stays terse
CONFIG_ESP_SYSTEM_PANIC_SILENT_REBOOT=y  # a panic reboots silently, no dump
CONFIG_OPENTHREAD_RCP_SPINEL_CONSOLE=n   # no Spinel-tunneled console commands either
CONFIG_OPENTHREAD_CONSOLE_ENABLE=n
CONFIG_OPENTHREAD_CLI=n
```

This is upstream's own official recipe for RCP firmware (see
`sdkconfig.defaults`'s "Configurations for optimizing the size of ot_rcp
firmware" section), carried into this project unchanged. **Because of this,
do not expect ordinary text output from `idf.py monitor` while this
firmware is running as an RCP** -- see section 6.

## 3. Build and flash commands

Build only (this pass's own verification; see the end of this document for
the exact commands actually run):

```bash
source ~/.espressif/v6.1/esp-idf/export.sh
idf.py -C rcp -B ~/esp-builds/pogo_rcp set-target esp32h2
idf.py -C rcp -B ~/esp-builds/pogo_rcp fullclean
idf.py -C rcp -B ~/esp-builds/pogo_rcp build
```

The resulting binary is at `~/esp-builds/pogo_rcp/pogo_rcp.bin` (plus
`bootloader/bootloader.bin` and `partition_table/partition-table.bin` in the
same build directory) -- **not flashed by this pass**.

To actually flash a real board (NOT run by this pass -- for the next real
hardware session):

```bash
source ~/.espressif/v6.1/esp-idf/export.sh
idf.py -C rcp -B ~/esp-builds/pogo_rcp -p <PORT> flash
```

`<PORT>` is the board's USB Serial/JTAG device -- see section 4 for how to
find a stable path for it on Linux. **If OTBR is already running against
this same device, stop it first** (see section 5) -- `esptool`/`idf.py
flash` cannot open a serial port OTBR already holds open.

Optional convenience wrapper: `tools/build_rcp.sh` runs the same
set-target/fullclean/build sequence with the port and build directory as
arguments (never hardcoded) and fails loudly if the ESP-IDF environment
(`$IDF_PATH`/`idf.py`) is not already sourced -- see that script's own
header comment.

## 4. Finding a stable device path on Linux (`/dev/serial/by-id/`)

A raw `/dev/ttyACM0`-style path can change across reboots or when multiple
USB-serial devices are plugged in. Linux's udev already creates a stable,
descriptive symlink for every USB serial device under
`/dev/serial/by-id/`, keyed by the device's own USB vendor/product/serial
strings -- an ESP32-H2's built-in USB Serial/JTAG controller shows up there
as something like:

```
/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_<...>-if00
```

To find it:

```bash
ls -l /dev/serial/by-id/
```

Use that full `by-id/...` path (not the raw `/dev/ttyACM*`/`/dev/ttyUSB*`
name) for both `idf.py -p <PORT> flash` and OTBR's own RCP device
configuration (e.g. `otbr-agent`'s `-I` / `RADIO_URL` argument,
`spinel+hdlc+uart://<by-id path>`) -- this is also what keeps OTBR pointed
at the right board if it is ever unplugged/replugged into a different
`/dev/ttyACM*` number, or if a second board is later plugged in alongside
it.

## 5. OTBR owns the RCP port -- stop it before flashing

Once OTBR's `otbr-agent` opens the RCP's USB Serial/JTAG device, it holds
that port exclusively for as long as it runs. Flashing new firmware to
that same board while OTBR is still running will fail (the port is busy) --
**stop the OTBR service first**:

```bash
sudo systemctl stop otbr-agent   # or whatever your OTBR install's service/process is named
```

then flash (section 3), then restart OTBR once the new firmware is
confirmed running (section 6). This project does not install or configure
OTBR itself -- that remains a manual step on the Linux host, outside this
repository.

## 6. What to expect from `idf.py monitor` (and what NOT to expect)

**Do not expect ordinary text log output from `idf.py monitor` while this
firmware is running as an RCP.** Every log source is disabled (section 2) --
`idf.py monitor` on a running RCP will show, at most, the ROM bootloader's
very first few lines (which happen before any of this firmware's own log
suppression takes effect) and then go silent, because from that point on
the USB endpoint is carrying raw Spinel protocol bytes, not text.

**This is expected, not a hang or a crash.** Whether the RCP firmware is
actually working correctly is judged entirely from the Linux side, via
OTBR/Spinel communication -- for example:

- `otbr-agent` logs (via `journalctl -u otbr-agent` or wherever it logs)
  showing a successful RCP reset/version handshake, not a Spinel timeout.
- `ot-ctl state` (or the equivalent OTBR CLI/REST call) returning a real
  Thread role once a network is formed, rather than erroring out.
- `ot-ctl` / `wpantund`-style RCP version query commands successfully
  reading the RCP's reported version string over Spinel.

None of that Linux-side verification was run in this pass -- see the final
report for exactly what WAS verified (a clean host build only).

## 7. What this does NOT do yet

- **The existing Power Base is not joined to this RCP's/OTBR's Thread
  network.** `power_base/main/app_main.c` still forms or resumes its OWN
  Active Dataset independently (see `docs/power-base.md`) -- flashing and
  running this RCP + OTBR does not, by itself, make the existing
  module/power_base sensor chain part of the same Thread network as OTBR.
  The two are simply unrelated Thread networks (different Datasets/PAN
  IDs/Network Keys) until a future pass explicitly connects them.
- No Matter Bridge, deep-sleep, or POGO 5V work was done in this pass
  (unchanged from `docs/validation-status.md`'s "Explicitly out of scope"
  table).
- No real hardware was flashed, and OTBR itself was not installed or
  configured, by this pass -- see the final report for exactly what host
  build verification WAS run.

## 8. Next step: an external-Dataset mode for the Power Base

To actually bring the existing sensor chain onto OTBR's Thread network, a
**future** pass needs to teach `power_base/main/app_main.c` a new boot mode
that, instead of forming/resuming its own Dataset, safely accepts an
Active Dataset TLV blob that OTBR itself generated (`ot-ctl dataset active
-x`, or the equivalent OTBR REST/D-Bus call) and commits THAT as its own
Active Dataset before joining. This is not implemented yet. Sketch of what
that future work needs to decide (not started here):

- How the TLV blob actually reaches the Power Base (NVS pre-provisioning
  before first boot? A new POGO/serial provisioning step? A config-image
  field alongside the existing `configs/power_base.json`?) -- undecided.
- Validating the injected TLV the same way `thread_manager_validate_dataset_tlvs()`
  already validates a TLV blob handed down through a POGO `SET_DATASET`
  frame (`components/thread_manager`), so a malformed/incomplete external
  Dataset is rejected instead of bricking the join, exactly like the
  existing module<->module path already refuses a bad TLV.
- Whether the Power Base still ever falls back to forming its own network
  (today's behavior) when no external Dataset is configured, for continuity
  with every existing real-hardware result in `docs/validation-status.md`.

**No real Network Key or other Dataset secret is hardcoded or committed
anywhere in this repository** -- this section is a plan, not an
implementation, and none of `rcp/`'s own files contain Thread credentials
of any kind (an RCP is Radio Only -- it has no Dataset of its own to leak;
see section 2's `CONFIG_OPENTHREAD_RADIO=y`).
