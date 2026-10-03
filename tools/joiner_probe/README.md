# Spare Power Base Joiner probe

This separate ESP-IDF image tests the ESP32-H2 Joiner against the existing
Linux OTBR. It does **not** run the Power Base application, send heartbeats,
or register a Base in the Linux API. Never flash it onto the operating Base.

Build with ESP-IDF v6.1:

```bash
source ~/.espressif/v6.1/esp-idf/export.sh
IDF_COMPONENT_MANAGER=0 idf.py -C tools/joiner_probe -B /tmp/pogo_joiner_probe build
```

For a physical trial, first identify the **spare** board's serial port and
confirm it is not the operating Base. Flashing replaces that board's app.
Back up its flash before changing it. In the Mac Terminal (outside a restricted
agent sandbox), use:

```bash
SPARE_PORT=/dev/cu.usbmodem101  # recheck the port on your Mac
python -m esptool --chip esp32h2 -p "$SPARE_PORT" flash-id
mkdir -p "$HOME/pogo-spare-backups"
chmod 700 "$HOME/pogo-spare-backups"
umask 077
# Replace FLASH_BYTES with the detected flash size in bytes, e.g. 0x200000
# for a 2 MB chip. The backup may contain private Thread credentials.
python -m esptool --chip esp32h2 -p "$SPARE_PORT" read-flash 0 FLASH_BYTES \
    "$HOME/pogo-spare-backups/spare-before-joiner.bin"
```

After checking the backup size, use `idf.py -C tools/joiner_probe -B
/tmp/pogo_joiner_probe -p "$SPARE_PORT" flash monitor`. The `-B` directory
must be the one used for this probe's build.

On the spare board's CLI, run `ot factoryreset` to clear any old test
Dataset (the board reboots), then check `ot help` includes `joiner` and run
`ot eui64`. On the Linux OTBR, use the following commands with this exact
EUI-64, a fresh temporary PSKd and a 120-second window:

```text
sudo ot-ctl commissioner start
sudo ot-ctl commissioner joiner add <SPARE_EUI64> <ONE_TIME_PSKD> 120
```

On the spare board, run `ot ifconfig up`, then `ot joiner start <ONE_TIME_PSKD>`.
Wait for `Join success`, then run `ot thread start` and `ot state`. The role
should become `child` or `router`. On the OTBR, remove the entry with
`sudo ot-ctl commissioner joiner remove <SPARE_EUI64>`. Do not paste the PSKd,
Dataset, or Network Key into project files or chat. If the OTBR reports that
the Commissioner command is unsupported, stop and inspect its build/config;
do not use wildcard Joiner authorization as a workaround.

This proves radio/commissioning feasibility only. The integrated path -- the
real `power_base` firmware in `pb_thread:mode=2` taking the PSKd on its
console (`pb_join start`) and then sending heartbeats -- is the bench build
described in `docs/power-base.md` ("앱 등록 대기 상태와 Joiner 가입"). The future production path
must authenticate the app request, bind it to a chosen hub and EUI-64, expire
the window, persist successful state safely, and wait for the Linux Bridge's
heartbeat before telling the app that registration succeeded.
