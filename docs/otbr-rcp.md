# ESP32-H2 USB RCP와 Linux OTBR

`rcp/`는 Linux OTBR용 ESP32-H2 Radio Co-Processor 펌웨어다. USB Serial/JTAG를
Spinel 전송에 사용하며 센서 모듈, Power Base, 공용 `components/`와 독립된 ESP-IDF
프로젝트다.

RCP는 Thread Dataset을 소유하지 않는다. Dataset과 Thread 네트워크 상태는 Linux의
`otbr-agent`가 관리한다.

## 구성

```text
rcp/
  CMakeLists.txt
  partitions.csv
  partitions_diag.csv
  sdkconfig.defaults
  sdkconfig.diag
  main/
    CMakeLists.txt
    esp_ot_config.h
    esp_ot_rcp.c
    esp_ot_uart_send_wrap.c
    esp_openthread_platform_process_wrap.c
    rcp_uart_tx_*.c
    rcp_usb_rx_*.c
```

공식 ESP-IDF v6.1 `ot_rcp` 예제의 핵심 구성을 사용한다. 외부 managed component 없이
ESP-IDF 내장 `openthread`, `nvs_flash`, `esp_event`, `vfs`, `console`을
CMake dependency로 직접 선언했다.

주요 설정:

- target: ESP32-H2
- `CONFIG_OPENTHREAD_RADIO=y`
- `CONFIG_OPENTHREAD_RCP_USB_SERIAL_JTAG=y`
- UART/SPI RCP transport 비활성
- OpenThread CLI와 Spinel debug console 비활성
- 일반 로그 비활성

ESP32-H2의 USB 포트는 순수 Spinel byte stream으로 사용한다. RCP가 정상 동작해도
`idf.py monitor`에 사람이 읽을 로그가 나오지 않는 것이 정상이다.

## 빌드

```bash
cd "/Users/junsoona/no-icloud/main project_1"
source ~/.espressif/v6.1/esp-idf/export.sh

idf.py -C rcp -B ~/esp-builds/pogo_rcp set-target esp32h2
idf.py -C rcp -B ~/esp-builds/pogo_rcp build
```

정적 설정 검사는 ESP-IDF 없이 실행할 수 있다.

```bash
python3 tools/test_rcp_config.py
```

## 플래시

RCP port는 실행 중인 `otbr-agent`가 점유한다. Linux에서 agent를 먼저 중지하고 보드를
Mac으로 옮겨 플래시하거나, 같은 Linux 호스트에서 agent를 중지한 상태로 플래시한다.

Mac:

```bash
source ~/.espressif/v6.1/esp-idf/export.sh
RCP_PORT=/dev/cu.usbmodem1401

idf.py -C rcp -B ~/esp-builds/pogo_rcp \
  -p "$RCP_PORT" flash
```

따옴표는 ASCII 작은따옴표나 큰따옴표를 사용한다. 문서 편집기에서 들어간 `’` 문자를
사용하면 shell이 닫는 따옴표로 인식하지 못해 `quote>` 상태가 된다.

Linux에서 다시 시작:

```bash
sudo systemctl start otbr-agent
sudo systemctl status otbr-agent --no-pager -l
sudo ot-ctl state
ip link show wpan0
```

## 안정적인 device path

`/dev/ttyACM0`는 연결 순서에 따라 바뀔 수 있으므로 systemd와 OTBR 설정에는
`/dev/serial/by-id/` 경로를 사용한다.

```bash
ls -l /dev/serial/by-id/
```

현재 사용한 보드의 예:

```text
/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_74:4D:BD:65:CF:DF-if00
```

`/etc/default/otbr-agent`의 예:

```bash
OTBR_AGENT_OPTS="-I wpan0 -B wlp5s0 spinel+hdlc+uart:///dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_74:4D:BD:65:CF:DF-if00"
```

backbone interface 이름 `wlp5s0`은 실제 Linux PC의 interface에 맞춘다.

## OTBR 상태 확인

```bash
sudo systemctl show otbr-agent \
  -p ActiveState -p SubState -p MainPID -p NRestarts

sudo ot-ctl state
sudo ot-ctl partitionid
sudo ot-ctl router table
sudo ot-ctl child table

ip link show wpan0
ip -6 addr show dev wpan0
```

`ot-ctl`의 `Connection refused`와 사라진 `wpan0`은 agent가 실행 중이 아니거나 RCP
연결이 깨졌다는 뜻이다. 서비스가 `active (exited)`인데 실제 `otbr-agent` 프로세스가
없다면 SysV wrapper가 성공으로 끝났을 뿐이므로 process와 journal을 함께 확인한다.

```bash
pgrep -af otbr-agent
sudo journalctl -u otbr-agent -b --no-pager -n 150
```

ModemManager가 RCP를 건드리지 않도록 해당 USB device에 ignore rule을 적용하는 편이 좋다.
포트 소유자는 정상 상태에서 `otbr-agent` 하나뿐이어야 한다.

```bash
RCP=/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_74:4D:BD:65:CF:DF-if00
TTY="$(readlink -f "$RCP")"

sudo fuser -v "$TTY"
sudo lsof "$TTY"
```

## Active Dataset 백업

Thread 네트워크가 정상 상태일 때 Linux host에서 Dataset을 저장한다.

```bash
umask 077
sudo ot-ctl dataset active -x | sed '/Done/d' > "$HOME/otbr-active-dataset.hex"
test -s "$HOME/otbr-active-dataset.hex"
ls -l "$HOME/otbr-active-dataset.hex"
```

Dataset 파일에는 Network Key가 들어 있다. 화면이나 Git에 내용을 출력하지 않고,
저장소 밖에서 `0600`으로 보관한다.

Mac에서 Power Base 설정을 만들려면 안전한 경로로 복사한다.

```bash
scp junsoo-na@<LINUX_IP>:/home/junsoo-na/otbr-active-dataset.hex \
  "$HOME/otbr-active-dataset.hex"
chmod 600 "$HOME/otbr-active-dataset.hex"
```

## Power Base를 OTBR Dataset에 가입시키기

Power Base bridge 설정 이미지를 만든다.

```bash
cd "/Users/junsoona/no-icloud/main project_1"
source ~/.espressif/v6.1/esp-idf/export.sh

OTBR_MLEID='<OTBR_MESH_LOCAL_EID>'

bash tools/gen_power_base_bridge_config.sh \
  "$OTBR_MLEID" \
  5684 \
  build/cfg_images \
  "$HOME/otbr-active-dataset.hex"
```

출력:

```text
build/cfg_images/power_base_otbr_bridge_config.bin
```

처음 STANDALONE에서 전환하는 보드는 이전 동적 Dataset을 한 번 지운 뒤 cfg를 기록한다.

```bash
python -m esptool --chip esp32h2 -p "$BASE_PORT" erase-region 0x9000 0x6000

python -m esptool --chip esp32h2 -p "$BASE_PORT" write-flash \
  0x187000 build/cfg_images/power_base_otbr_bridge_config.bin
```

정상 로그에는 Dataset 내용 대신 mode와 길이만 나온다.

```text
Thread dataset mode: EXTERNAL_OTBR (... bytes; contents hidden)
external OTBR: Active Dataset accepted (...) -- joining existing Thread network
power-base FTD: attached, otDeviceRole=...
```

OTBR와 Power Base의 `partitionid`가 일치하는지 확인한다.

## USB Spinel 장애 분석

초기 실물 시험에서는 OTBR journal에 다음 순서가 반복됐다.

```text
P-HdlcIntface-: Error decoding hdlc frame: Parse
P-RadioSpinel-: Unexpected RCP reset: RESET_FAULT
HandleRcpUnexpectedReset()
otbr-agent.service: Failed with result 'exit-code'
```

USB disconnect는 kernel journal에 없었고, 같은 `ttyACM`을 점유한 다른 process도 없었다.
RCP 진단 coredump에서는 `ot_main` task가 `esp_openthread_launch_mainloop()` 반환 뒤
`assert(false)`에 도달한 경로를 확인했다.

coredump에 `errno` 값은 보존되지 않았다. 따라서 다음 둘을 구분한다.

- **coredump로 확인**: OpenThread mainloop가 반환했고 RCP task가 assert로 종료
- **소스 분석으로 확인**: USB RX read의 `EIO`가 `ESP_FAIL`로 전파되면 같은 경로에 도달 가능
- **실물 시험으로 확인**: TX/RX 보강을 함께 적용한 뒤 2시간 동안 같은 오류가 재발하지 않음

## TX 보강

ESP-IDF v6.1의 `otPlatUartSend()`는 `write()`를 한 번 호출하고 결과와 관계없이
`otPlatUartSendDone()`을 호출한다. 상위 `NcpSend()`는 반환값을 사용하지 않는다.

프로젝트에서는 linker `--wrap=otPlatUartSend`로 다음 동작을 적용한다.

- partial write는 남은 offset부터 계속 전송
- `EIO`, `EAGAIN`, `EWOULDBLOCK`, 0-byte write는 재시도
- 각 write 뒤 stock 구현과 같이 USB TX FIFO flush
- 2ms `vTaskDelay()`로 양보
- 프레임당 전체 예산 200ms
- 전체 buffer가 끝났을 때만 `otPlatUartSendDone()` 호출
- 예산 소진이나 영구 오류는 `esp_restart()`로 복구하고 상위에 성공처럼 반환하지 않음
- Spinel stream에 텍스트 로그를 쓰지 않음

host test:

회귀 테스트는 `tools/test_rcp_uart_tx_retry.c`가 production retry 코드를 host I/O simulation에
직접 연결한다. 검증 범위에는 첫 성공, EIO/EAGAIN, partial write, 0-byte, timeout, 영구 오류,
write/flush 순서가 포함된다.

## RX 보강

`esp_openthread_uart_process()`의 read 실패가 `ESP_FAIL`로 올라오면
`esp_openthread_platform_process()`와 mainloop가 반환할 수 있다.

`--wrap=esp_openthread_platform_process`에서 다음 정책을 적용한다.

- 실제 함수를 부르기 전에 `errno=0`
- 결과가 `ESP_FAIL`이고 `errno==EIO`일 때만 transient 처리
- 2ms 간격, 총 200ms까지 다시 호출
- 성공하면 정상 반환
- 다른 오류는 원래 결과를 그대로 반환
- EIO가 예산 끝까지 지속되면 로그 없이 `esp_restart()`

다른 workflow의 오류를 숨기지 않도록 `ESP_FAIL + EIO` 조합만 재시도한다.

host test는 성공, transient EIO 복구, timeout 뒤 noreturn 복구, non-EIO 보존을 실제 wrapper
소스에 링크해 확인한다.

## 진단 RCP

진단 빌드는 flash coredump 파티션을 사용하고 Spinel USB에는 panic text를 출력하지 않는다.

```bash
source ~/.espressif/v6.1/esp-idf/export.sh

idf.py -C rcp \
  -B ~/esp-builds/pogo_rcp_diag \
  -DSDKCONFIG="$HOME/esp-builds/pogo_rcp_diag/sdkconfig" \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.diag" \
  set-target esp32h2

idf.py -C rcp \
  -B ~/esp-builds/pogo_rcp_diag \
  -DSDKCONFIG="$HOME/esp-builds/pogo_rcp_diag/sdkconfig" \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.diag" \
  build
```

플래시:

```bash
idf.py -C rcp -B ~/esp-builds/pogo_rcp_diag \
  -p "$RCP_PORT" flash
```

coredump 확인:

```bash
idf.py -C rcp -B ~/esp-builds/pogo_rcp_diag \
  -p "$RCP_PORT" coredump-info
```

운영 빌드와 진단 빌드는 서로 다른 build directory와 sdkconfig를 사용한다.

## 실물 연속 시험

전체 체인:

```text
Linux OTBR + ESP32-H2 RCP
Power Base
SHT45 INTERMEDIATE
PMS5003 LEAF
```

한 시간 시험 명령:

```bash
sudo bash -c '
START="$(date -Is)"
BEFORE="$(systemctl show otbr-agent -p NRestarts --value)"

echo "start=$START before=$BEFORE"
sleep 3600

AFTER="$(systemctl show otbr-agent -p NRestarts --value)"
echo "before=$BEFORE after=$AFTER"

journalctl -u otbr-agent --since "$START" --no-pager |
grep -Ei "P-HdlcIntface|RESET_FAULT|RESET_SOFTWARE|Unexpected RCP reset|HandleRcpUnexpectedReset|Failed with result" || true
'
```

기록된 결과:

- 진단 빌드 1시간: `NRestarts 425 → 425`, 대상 오류 0회
- 운영 빌드 1시간: `NRestarts 0 → 0`, 대상 오류 0회

이 결과는 TX와 RX 보강을 함께 적용한 조합을 검증한다. 각 수정의 단독 효과를 분리한
시험은 아니다.

## 자동 복구

systemd의 `Restart=` 설정으로 `otbr-agent`가 실패하면 다시 시작하고,
Matter Bridge service는 `PartOf=otbr-agent.service`로 함께 재시작한다. 실제 시험에서
RCP/OTBR 재시작 뒤 `wpan0`, Matter Bridge, Apple Home, Google Home 값이 자동으로
복구됐고 Matter fabric/KVS는 유지됐다.

USB 보강 이후에도 systemd 자동 복구는 방어 수단으로 유지한다.
