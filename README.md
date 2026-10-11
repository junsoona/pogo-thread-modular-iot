# POGO 기반 모듈형 Thread 센서 시스템

> 공개용 기록: 보드 식별값·계정·포트·주소는 예시로 바꿨습니다. 명령 실행 시 자신의 실제 값으로 바꾸세요. 원본 근거는 비공개로 보존했습니다.


ESP32-H2 센서 모듈을 POGO 커넥터로 이어 붙이면 각 모듈이 자신의 위치를 판단하고,
Thread 네트워크에 가입해 측정값을 전원 모듈로 보내는 졸업작품 프로젝트다. 센서별로
펌웨어를 따로 만들지 않고 `module/`의 공통 바이너리와 NVS 설정 이미지를 사용한다.

현재 구성은 다음과 같다.

- `power_base/`: POGO 체인의 시작점, 프로비저닝, 토폴로지 관리, 센서 데이터 집계
- `module/`: SHT45, BH1750, SGP40, SGP41, PMS5003 공통 센서 모듈 펌웨어
- `rcp/`: Linux OTBR에 연결하는 ESP32-H2 USB RCP 펌웨어
- `linux/matter_bridge/`: 전원 모듈의 데이터를 받아 Matter 엔드포인트로 공개하는 Linux 앱
- `upper/`, `lower/`: 초기 개발 구조를 보존한 회귀 시험용 프로젝트

## 현재 확인된 범위

실물에서 확인한 주요 경로는 아래와 같다. 세부 근거와 아직 확인하지 않은 항목은
[`docs/validation-status.md`](docs/validation-status.md)에 계속 정리한다.

- Power Base → 센서 모듈 1대 연결, 프로비저닝, Thread 가입, 데이터 수집
- Power Base → INTERMEDIATE → LEAF의 POGO depth 2 체인
- 체결 해제와 재연결 후 토폴로지 및 데이터 전송 자동 복구
- SHT45, BH1750, SGP40, SGP41, PMS5003 센서 데이터 수신
- Linux OTBR의 Active Dataset을 사용한 Power Base 가입
- Linux Matter Bridge에서 온도, 습도, 조도, PM1/PM2.5/PM10 공개
- BH1750 조도값과 PMS5003 Air Quality/Identify 클러스터를 `chip-tool`로 확인
- RCP USB Spinel 보강 적용 후 진단 빌드 1시간, 운영 빌드 1시간 연속 시험에서 재시작 0회

현재 제한도 분명하다.

- POGO 5V 전원 회로는 아직 없다. 각 보드는 USB로 따로 전원을 공급한다.
- Deep-sleep과 자동 Light-sleep은 구현하지 않았다. Matter에서 항상 온라인 상태를
  유지하고 POGO UART 프레임을 놓치지 않는 쪽을 우선했다.
- SGP40/SGP41의 Gas Index는 농도 단위가 아니므로 Matter 농도 클러스터로 변환하지 않는다.
- POGO depth 2는 확인했지만, 실제 시험에서 Thread 무선 경로는 INTERMEDIATE를 거치지
  않고 OTBR 또는 Power Base에 직접 붙었다. 이는 정상적인 OpenThread 라우팅 결과다.
- Matter 인증과 Apple Home/Google Home의 모든 표시 방식은 이 프로젝트의 검증 범위가 아니다.

## 문서

- [`PROJECT-NOTES.md`](PROJECT-NOTES.md): 프로젝트 목표와 설계 원칙
- [`docs/architecture.md`](docs/architecture.md): 전체 구조와 부팅 흐름
- [`docs/pogo-interface.md`](docs/pogo-interface.md): POGO 신호, GPIO, 체결 감지
- [`docs/module-config.md`](docs/module-config.md): 센서 모듈 설정 이미지 형식
- [`docs/state-machine.md`](docs/state-machine.md): POGO·프로비저닝·Thread 상태 머신
- [`docs/pogo-lifecycle.md`](docs/pogo-lifecycle.md): 모듈 자동 등록·삭제와 offline 구분
- [`docs/power-base.md`](docs/power-base.md): 전원 모듈, 토폴로지, Dataset, 전원 회로 계획
- [`docs/power-base-management.md`](docs/power-base-management.md): Power Base 일반 등록과 삭제·등록 해제(관리 자격증명, 통신 규약, 상태 전이)
- [`docs/otbr-rcp.md`](docs/otbr-rcp.md): RCP 빌드, OTBR 연결, USB 복구 설계
- [`docs/matter-bridge.md`](docs/matter-bridge.md): Linux Matter Bridge 구조와 검증 방법
- [`docs/validation-status.md`](docs/validation-status.md): 기능별 시험 상태
- [`docs/versioning.md`](docs/versioning.md): 구성요소 버전, 빌드 식별 정보, manifest, 설치 기록 ([`CHANGELOG.md`](CHANGELOG.md), [`docs/release-records/`](docs/release-records/README.md))

## 개발 환경

- ESP-IDF v6.1
- 대상 MCU: ESP32-H2
- Matter SDK: connectedhomeip v1.6.0.0
- Linux OTBR: `ot-br-posix`

저장소 경로에 공백이나 한글이 있어도 소스는 그대로 둘 수 있다. ESP-IDF 빌드 디렉터리는
경로 문제를 피하기 위해 홈 디렉터리 아래의 ASCII 경로를 사용한다.

```bash
cd "$HOME/pogo-thread-modular-iot"
source ~/.espressif/v6.1/esp-idf/export.sh

idf.py -C module     -B ~/esp-builds/pogo_module     set-target esp32h2
idf.py -C power_base -B ~/esp-builds/pogo_power_base set-target esp32h2
idf.py -C rcp        -B ~/esp-builds/pogo_rcp        set-target esp32h2

idf.py -C module     -B ~/esp-builds/pogo_module     build
idf.py -C power_base -B ~/esp-builds/pogo_power_base build
idf.py -C rcp        -B ~/esp-builds/pogo_rcp        build
```

`upper/`와 `lower/` 회귀 빌드가 필요하면 같은 방식으로 별도 빌드 디렉터리를 지정한다.

```bash
idf.py -C upper -B ~/esp-builds/pogo_upper set-target esp32h2 build
idf.py -C lower -B ~/esp-builds/pogo_lower set-target esp32h2 build
```

## 설정 이미지 만들기

센서 종류와 GPIO 배치는 앱 바이너리가 아니라 `cfg` NVS 파티션에 저장한다. 일반 센서
설정과 Power Base 설정을 각각 생성한다.

```bash
source ~/.espressif/v6.1/esp-idf/export.sh

bash tools/gen_config.sh build/cfg_images
bash tools/gen_power_base_config.sh build/cfg_images
```

주요 출력 파일은 다음과 같다.

```text
build/cfg_images/power_base_config.bin
build/cfg_images/prototype_sht45_config.bin
build/cfg_images/prototype_bh1750_config.bin
build/cfg_images/prototype_sgp40_config.bin
build/cfg_images/prototype_sgp41_config.bin
build/cfg_images/prototype_pms5003_config.bin
```

Linux OTBR의 Dataset과 Matter Bridge 목적지를 함께 넣을 때는 별도 생성기를 사용한다.
Dataset 파일에는 네트워크 키가 들어 있으므로 저장소 밖에 두고 권한을 `0600`으로 유지한다.

```bash
bash tools/gen_power_base_bridge_config.sh \
  "$OTBR_MLEID" \
  5684 \
  build/cfg_images \
  "$HOME/otbr-active-dataset.hex"
```

생성 결과는 `build/cfg_images/power_base_otbr_bridge_config.bin`이다.

## GPIO 배치

### 센서 모듈 공통 POGO 포트

| 기능 | UPSTREAM | DOWNSTREAM |
|---|---:|---:|
| UART | UART0 | UART1 |
| TX | GPIO4 | GPIO10 |
| RX | GPIO5 | GPIO11 |
| DETECT_IN | GPIO12 | GPIO23 |
| DETECT_OUT | GPIO2 | GPIO3 |
| BASE_ID | GPIO14 | 사용 안 함 |

Power Base의 포트는 센서 모듈의 DOWNSTREAM과 같은 핀을 쓴다.

### Power Base에서 첫 모듈로 연결

| Power Base | 첫 모듈 UPSTREAM |
|---|---|
| GPIO10 TX | GPIO5 RX |
| GPIO11 RX | GPIO4 TX |
| GPIO3 DETECT_OUT | GPIO12 DETECT_IN |
| GPIO23 DETECT_IN | GPIO2 DETECT_OUT |
| GND | GND |
| BASE_ID 접점 GND | GPIO14 BASE_ID |

### 중간 모듈에서 다음 모듈로 연결

| 앞 모듈 DOWNSTREAM | 뒤 모듈 UPSTREAM |
|---|---|
| GPIO10 TX | GPIO5 RX |
| GPIO11 RX | GPIO4 TX |
| GPIO3 DETECT_OUT | GPIO12 DETECT_IN |
| GPIO23 DETECT_IN | GPIO2 DETECT_OUT |
| GND | GND |

DETECT 신호는 open-drain active-low로 동작한다. 두 보드의 GND를 반드시 공통으로 묶는다.
현재 POGO 커넥터로 5V를 보내지 않으므로 각 보드에 USB 전원을 따로 공급한다.

### 센서 연결

I2C 센서는 공통으로 SDA=GPIO0, SCL=GPIO1을 사용한다.

| 센서 | 버스 | 주소/설정 |
|---|---|---|
| SHT45 | I2C | `0x44` |
| BH1750 | I2C | `0x23` 기본, ADDR 핀 상태 확인 |
| SGP40 | I2C | `0x59` |
| SGP41 | I2C | `0x59` |
| PMS5003 | UART1 | MCU TX=GPIO0, MCU RX=GPIO1, 9600 8N1 |

PMS5003은 terminal LEAF 전용 설정이라 DOWNSTREAM POGO 포트를 사용하지 않는다.

## 플래시

먼저 실제 포트를 확인한다.

```bash
ls /dev/cu.usbmodem*
```

앱과 부트로더, 파티션 테이블을 플래시한다.

```bash
BASE_PORT=/dev/cu.usbmodemXXXX
MIDDLE_PORT=/dev/cu.usbmodemXXXX
LEAF_PORT=/dev/cu.usbmodemXXXX

idf.py -C power_base -B ~/esp-builds/pogo_power_base -p "$BASE_PORT" flash
idf.py -C module     -B ~/esp-builds/pogo_module     -p "$MIDDLE_PORT" flash
idf.py -C module     -B ~/esp-builds/pogo_module     -p "$LEAF_PORT" flash
```

각 보드의 `cfg` 이미지를 `0x187000`에 기록한다. 아래 예시는 SHT45 중간 모듈과
PMS5003 말단 모듈 조합이다.

```bash
python -m esptool --chip esp32h2 -p "$BASE_PORT" write-flash \
  0x187000 build/cfg_images/power_base_config.bin

python -m esptool --chip esp32h2 -p "$MIDDLE_PORT" write-flash \
  0x187000 build/cfg_images/prototype_sht45_config.bin

python -m esptool --chip esp32h2 -p "$LEAF_PORT" write-flash \
  0x187000 build/cfg_images/prototype_pms5003_config.bin
```

OTBR와 Matter Bridge를 사용할 때는 Power Base에 bridge 설정 이미지를 기록한다.

```bash
python -m esptool --chip esp32h2 -p "$BASE_PORT" write-flash \
  0x187000 build/cfg_images/power_base_otbr_bridge_config.bin
```

STANDALONE에서 EXTERNAL_OTBR 모드로 처음 전환하는 보드는 동적 Dataset이 남지 않도록
한 번만 아래 영역을 지운다. 기존 보드 상태를 알고 있을 때만 실행한다.

```bash
python -m esptool --chip esp32h2 -p "$BASE_PORT" erase-region 0x9000 0x6000
```

## 모니터

각 보드는 별도 터미널에서 확인한다.

```bash
idf.py -C power_base -B ~/esp-builds/pogo_power_base -p "$BASE_PORT" monitor
idf.py -C module     -B ~/esp-builds/pogo_module     -p "$MIDDLE_PORT" monitor
idf.py -C module     -B ~/esp-builds/pogo_module     -p "$LEAF_PORT" monitor
```

정상 체결에서는 `PEER_READY`, 역할 결정, `JOIN_RESULT_ACK`, topology 등록, sample 저장이
차례로 나타난다. 세 보드 로그를 파일로 저장했다면 다음 도구로 기본 조건을 검사한다.

```bash
python3 tools/validate_three_tier_logs.py \
  --base base.log \
  --middle middle.log \
  --leaf leaf.log
```

이 검사는 POGO 물리 토폴로지 depth를 확인한다. Thread 패킷이 실제로 중간 모듈을 무선
릴레이했는지는 별도 RF 시험이나 패킷 수준 관찰이 필요하다.

## Linux OTBR와 Matter Bridge

RCP 빌드와 OTBR 설치·운영 절차는 [`docs/otbr-rcp.md`](docs/otbr-rcp.md)에 정리했다.
Matter Bridge는 Linux 호스트에서 connectedhomeip v1.6.0.0 환경을 활성화한 뒤 빌드한다.

```bash
cd ~/pogo-thread-modular-iot
export CHIP_ROOT="$HOME/connectedhomeip"

PW_ACTIVATE_SKIP_CHECKS='' \
  tools/build_matter_bridge.sh "$HOME/pogo-builds/pogo-matter-bridge"
```

systemd 서비스로 설치한 환경에서는 상태와 로그를 다음처럼 확인한다.

```bash
sudo systemctl status otbr-agent pogo-matter-bridge --no-pager -l
sudo ot-ctl state
ip link show wpan0

sudo journalctl -u pogo-matter-bridge.service -f -o cat
```

RCP 포트는 OTBR가 독점한다. RCP를 다시 플래시할 때는 먼저 `otbr-agent`를 중지하고,
플래시가 끝난 뒤 다시 시작한다.

## 테스트

`tools/` 아래의 C 테스트는 실제 production 소스를 host compiler에 링크하는 방식으로
작성했다. Python 테스트는 설정 이미지, 문서 규칙, 소스 간 상수 정합성을 검사한다.
Matter Bridge의 SDK 비의존 테스트는 아래 명령으로 한 번에 실행한다.

```bash
tools/test_matter_bridge.sh
```

펌웨어 변경 후에는 최소한 영향을 받는 host test, 해당 ESP-IDF 프로젝트 빌드,
`git diff --check`를 실행한다. 실물 검증 결과는 host test 결과와 구분해
[`docs/validation-status.md`](docs/validation-status.md)에 기록한다.

## Git 작업 원칙

- Dataset 파일, NVS 비밀 이미지, 실물 로그는 커밋하지 않는다.
- `secrets/`, `*otbr-active-dataset.hex`, 로컬 빌드 디렉터리는 `.gitignore`로 제외한다.
- 펌웨어와 `cfg` 이미지가 함께 바뀌었는지 구분해서 기록한다.
- 실물 시험을 하지 않은 변경은 문서에서 “구현 완료”와 “실물 확인 완료”를 나눠 쓴다.
