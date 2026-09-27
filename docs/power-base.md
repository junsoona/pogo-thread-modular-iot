# Power Base 설계

`power_base/`는 센서 모듈과 별도로 빌드하는 ESP32-H2 프로젝트다. POGO 체인의 시작점,
downstream 프로비저닝, 토폴로지 등록, 센서 데이터 집계, Linux 전송을 담당한다.

Power Base라는 이름은 애플리케이션 역할을 뜻한다. Thread 네트워크의 Leader를 고정한다는
뜻이 아니다. OpenThread는 상황에 따라 Power Base를 CHILD, ROUTER, LEADER 중 하나로
선택할 수 있고, 세 상태에서 같은 서비스를 실행한다.

## 구성

Power Base가 직접 사용하는 주요 컴포넌트는 다음과 같다.

- `power_base_config`: POGO 포트와 전원 제어 설정
- `power_base_thread_config`: STANDALONE/EXTERNAL_OTBR Dataset 설정
- `power_base_bridge_config`: Linux bridge 목적지 설정
- `thread_manager`: always-on FTD 설정과 Thread 시작
- `pogo_port`: 첫 센서 모듈과의 물리 연결
- `parent_service`: downstream 모듈 프로비저닝
- `power_base_transport`: CoAP `/s`, `/t` 수신
- `topology_registry`: UUID 기반 노드 상태 저장
- `sensor_root_aggregator`: 최신 센서 샘플 저장과 중복 제거
- `linux_bridge_exporter`: Linux OTBR 호스트로 북향 전송

Power Base에는 센서 드라이버와 `child_service`가 필요하지 않다.

## POGO 연결

Power Base의 한 포트는 센서 모듈의 DOWNSTREAM과 같은 핀을 사용한다.

| Power Base | 첫 센서 모듈 UPSTREAM | 용도 |
|---|---|---|
| GPIO10 TX | GPIO5 RX | UART |
| GPIO11 RX | GPIO4 TX | UART |
| GPIO3 DETECT_OUT | GPIO12 DETECT_IN | Power Base가 연결 상태 알림 |
| GPIO23 DETECT_IN | GPIO2 DETECT_OUT | 센서 모듈이 연결 상태 알림 |
| GND | GND | 공통 기준 |
| BASE_ID GND 접점 | GPIO14 | 첫 모듈이 Power Base 연결을 판정 |

DETECT 신호는 open-drain active-low다. GPIO3은 출력 전용이므로 Deep-sleep wake 핀일
필요가 없다. 센서 모듈의 GPIO12는 UPSTREAM DETECT_IN이며 RTC GPIO 조건을 만족하지만,
현재 펌웨어에는 Deep-sleep 진입 코드가 없다.

## 부팅 순서

`power_base/main/app_main.c`의 순서는 다음과 같다.

1. reset reason을 기록한다.
2. NVS와 고정 `power_base_cfg`를 읽는다.
3. `pb_thread` 설정을 읽어 Dataset 출처를 결정한다.
4. efuse 기반 영구 UUID를 만든다.
5. OpenThread를 초기화한다.
6. Dataset 모드에 따라 Thread를 시작한다.
7. attach supervisor task를 한 번 만든 뒤 `app_main()`은 반환한다.
8. supervisor는 200ms 간격으로 역할을 확인한다.
9. CHILD, ROUTER, LEADER 중 하나가 되면 서비스를 한 번만 시작하고 자신을 삭제한다.

30초 안에 attach되지 않아도 영구 정지하지 않는다. 30초 뒤 첫 경고를 남기고 이후에는
최대 60초 간격으로 경고하면서 계속 기다린다. DETACHED/DISABLED 상태에서는
provisioning을 허용하지 않으며, attach가 회복되면 다음 확인부터 다시 허용한다.

서비스 시작 함수는 아래 항목을 각각 한 번만 만든다.

- Power Base CoAP transport
- `parent_service`
- POGO downstream port와 task
- topology stale sweep task
- bridge exporter 설정이 있으면 exporter task

치명적인 설정 오류, OpenThread API 실패, task/port 생성 실패는 부팅을 중단한다. 느린 attach는
치명적인 오류로 보지 않는다.

## Thread 역할

Power Base는 always-on FTD로 시작한다.

- `rx-on-when-idle=true`
- router eligible
- Thread 역할은 OpenThread가 결정
- CHILD, ROUTER, LEADER에서 서비스 가능
- DETACHED/DISABLED에서는 provisioning과 collector 제공 중지

`pb_provisioning_allowed()`와 `pb_get_collector()`는 호출할 때마다 현재 역할을 읽는다.
역할을 별도로 캐시하거나 watcher task를 추가하지 않았다.

## 프로비저닝 순서

Power Base의 `parent_service`는 첫 센서 모듈에 다음 순서로 정보를 보낸다.

1. `PEER_HELLO`
2. `UUID_REQ`
3. `ASSIGN_SLOT`
4. `ASSIGN_PARENT_INFO`
5. `SENSOR_INFO_REQ`
6. `POLL_CONFIG`
7. `SET_COLLECTOR`
8. `SET_DATASET`
9. `JOIN_RESULT` 확인과 ACK

센서가 `SENSOR_STATUS_CONDITIONING`을 반환해도 PMS5003/SGP41의 정상 예열 과정으로
보고 프로비저닝을 계속한다. 다른 non-zero 상태는 오류로 처리한다.

응답 timeout은 제한된 횟수만 재시도한다. 세션이 끝나도 peer가 연결된 상태라면 idle ping
주기 뒤 새 프로비저닝 세션을 시작할 수 있다.

## Collector endpoint

센서 데이터와 topology 보고는 현재 Thread Leader 주소가 아니라 Power Base의 collector
endpoint로 보낸다. endpoint는 POGO `SET_COLLECTOR` 메시지로 전달한다.

wire payload는 다음 30바이트다.

```text
power_base_uuid(8) | IPv6 address(16) | port(2) | generation(4)
```

Power Base는 자신의 Mesh-Local EID와 CoAP port 5683을 제공한다. INTERMEDIATE는 받은
endpoint를 downstream LEAF에 그대로 전달한다. 센서 모듈은 새 프로비저닝 시작,
DETACHED, upstream 오류에서 캐시를 지운다.

이 구조 덕분에 Power Base의 `otDeviceRole`이 바뀌어도 센서의 목적지는 바뀌지 않는다.

## 센서 데이터와 topology

Power Base는 CoAP resource 두 개를 연다.

- `/s`: 센서 sample
- `/t`: topology report

sample을 받기 전에 해당 module UUID가 topology registry에 online 상태로 등록되어 있어야
한다. 등록이 없으면 4.04, 부모 상태가 맞지 않거나 bitmap/세대가 맞지 않으면 4.12를
반환한다. 저장소가 busy/full이면 5.03을 반환한다.

aggregator 결과와 CoAP 코드는 다음처럼 연결한다.

| aggregator 결과 | CoAP | bridge export |
|---|---|---|
| STORED | 2.04 | 실행 |
| DUPLICATE | 2.03 | 실행하지 않음 |
| BUSY/FULL | 5.03 | 실행하지 않음 |
| INVALID | 4.xx | 실행하지 않음 |

`stored`는 Power Base의 RAM aggregator에 저장됐다는 뜻이다. 플래시 영구 저장을 의미하지
않는다.

## Topology registry

registry key는 module UUID다. 각 entry에는 다음 정보가 있다.

- module UUID와 parent UUID
- depth
- 논리 역할
- sensor bitmap
- online/downstream 상태
- generation과 sequence
- 마지막 보고 시각

depth 1 노드는 Power Base에 직접 연결된다. depth 2 이상 보고는 부모 UUID가 존재하고,
부모 depth가 하나 작으며, 부모가 online이고 downstream이 연결된 경우에만 적용한다.

Power Base의 POGO 링크가 분리되면 depth 1 노드와 그 subtree를 offline으로 바꾼다.
분리된 자식의 늦은 heartbeat나 sample이 도착해도 부모가 복구되기 전에는 다시 online으로
살아날 수 없다. 재연결 뒤에는 부모부터 다시 등록하고 자식이 이어서 등록한다.

stale sweep는 일정 시간 보고가 없는 노드를 offline으로 바꾼다. 다른 UUID의 상태는
건드리지 않는다.

## Dataset 모드

### STANDALONE

기존 동작이다. Power Base가 저장된 Active Dataset을 재사용하거나 새 Dataset으로 Thread
네트워크를 시작한다.

### EXTERNAL_OTBR

Linux OTBR가 소유한 Active Dataset TLV를 `pb_thread` NVS namespace에서 읽고 기존
Thread 네트워크에 가입한다. 이 모드에서 Power Base는 새 Dataset을 만들지 않는다.

설정이 잘못됐을 때 STANDALONE으로 조용히 돌아가지 않는다. 잘못된 mode, 비어 있는 TLV,
길이 초과, Dataset 검증 실패는 CONFIG_ERROR로 부팅을 중단한다. Dataset 내용은 로그에
출력하지 않고 mode와 길이만 남긴다.

센서 모듈에는 두 모드 모두 기존 `SET_DATASET` 단계로 같은 Dataset을 전달한다.

## NVS 설정

`cfg` 파티션 안에 namespace를 나눠 사용한다.

| namespace | 내용 |
|---|---|
| `power_base_cfg` | POGO 포트, timeout, 전원 제어 |
| `pb_thread` | Dataset mode와 external Active TLV |
| `pb_bridge` | Linux host IPv6 주소와 port |

기존 `power_base_cfg` blob은 44바이트 schema v1을 유지한다. `pb_thread` namespace가
없으면 STANDALONE으로 동작해 기존 이미지와 호환된다. `pb_bridge`가 없거나
`enabled=0`이면 exporter만 비활성화하고 나머지 기능은 정상 부팅한다.

bridge가 enabled인데 주소가 unspecified, multicast, link-local이거나 port가 0이면 exporter를
비활성화하고 명확한 설정 오류를 남긴다. POGO와 Thread 자체는 계속 동작한다.

설정 이미지는 다음 명령으로 만든다.

```bash
source ~/.espressif/v6.1/esp-idf/export.sh

bash tools/gen_power_base_config.sh build/cfg_images

bash tools/gen_power_base_otbr_config.sh \
  "$HOME/otbr-active-dataset.hex" \
  build/cfg_images

bash tools/gen_power_base_bridge_config.sh \
  "$OTBR_MLEID" \
  5684 \
  build/cfg_images \
  "$HOME/otbr-active-dataset.hex"
```

마지막 명령의 출력은 Dataset 비밀을 포함하므로 파일 권한을 `0600`으로 유지한다.

## Linux exporter

exporter의 callback은 OpenThread 처리 경로에서 대기하지 않는다.

- TOPOLOGY: bounded queue에 0-tick enqueue
- SAMPLE: UUID별 pending 표시와 최신값 coalescing
- HEARTBEAT: exporter task에서 생성

SAMPLE은 기본적으로 UUID당 1000ms에 한 번의 논리 publish만 시작한다. 한 publish 안의
CoAP retry는 이 횟수에 포함하지 않는다. retry attempt 직전에 aggregator에서 최신 sample을
다시 읽기 때문에 오래된 sequence를 재전송하지 않는다.

TOPOLOGY는 sample 제한과 별도이며 즉시 전송한다. queue에 오래 머문 이벤트도 전송 직전에
현재 registry entry를 다시 읽는다. 주기적인 full resync가 topology와 최신 sample을 복구한다.

Linux 응답 처리:

- 2.xx: 성공
- 4.xx: 명시적인 거부이므로 재시도하지 않음
- timeout, OpenThread 오류, 5.xx: 초기 1회 + 최대 2회 재시도
- 모든 transient retry 소진: pending으로 되돌려 다음 허용 시점에 재시도

Linux가 꺼져 있어도 Power Base의 `/s`, `/t` ACK와 내부 aggregator 저장에는 영향을 주지
않는다.

## 전원 레일

현재 `power_enable_gpio=-1`이며 POGO 5V 공급 회로가 없다. 각 보드는 USB로 따로 전원을
받는다. 펌웨어는 5V를 차단할 수 있다고 주장하지 않는다.

최종 하드웨어에서는 별도 전원 입력용 USB-C의 5V를 Power Base와 POGO 전원 핀에 분배할
계획이다. 졸업작품 범위에서 GPIO 제어를 쓰지 않고 항상 켜진 레일로 구성할 수 있지만,
최소한 다음 보호는 회로에 포함해야 한다.

- 입력 퓨즈 또는 PTC
- 역극성·역급전 방지
- 체인 전체 전류에 맞는 배선과 POGO 접점 정격
- 충분한 bulk/decoupling capacitor
- USB 데이터 포트와 외부 5V를 동시에 연결할 때 전원 충돌 방지

GPIO로 전원을 제어하려면 단순 MCU 출력이 아니라 high-side load switch를 사용한다.
`power_enable_gpio`, active level, startup delay는 그 회로가 실제로 생긴 뒤 설정한다.

실물 회로를 만들면 아래 항목을 확인한다.

1. 무부하와 전체 체인 연결 시 돌입 전류
2. 정상 상태 전류와 load switch/PTC 온도
3. 마지막 모듈의 5V 전압 강하
4. 센서 측정과 무선 송신 순간의 brownout 여부
5. 각 보드 USB 연결 상태에서 POGO 5V 역급전 여부

## Light-sleep 검토

자동 Light-sleep은 구현하지 않았다. ESP-IDF v6.1 예제와 현재 POGO 구조를 비교했을 때
바로 켜기 어려운 이유가 있다.

- POGO는 UART 프로토콜이다.
- ESP32-H2에서 peripheral power-down과 UART wake는 함께 사용할 수 없다.
- UART wake가 발생해도 wake 전에 들어온 프레임 일부를 이미 잃을 수 있다.
- 현재 센서와 POGO 드라이버는 I2C/UART transaction 동안 `esp_pm_lock`을 사용하지 않는다.
- Matter에서는 센서가 계속 online으로 보이는 동작을 우선한다.

향후 적용하려면 wake 뒤 전체 POGO 프레임의 CRC가 보존되는지 실물로 확인하고, 손실 시
재동기화 규칙, PM lock 범위, CoAP/Thread timing, online 표시 정책을 함께 설계해야 한다.
Thread LEAF의 기존 SED 설정(`mRxOnWhenIdle=false`)은 Light-sleep과 별개로 유지한다.

## 여러 Power Base 추가

Power Base를 둘 이상 쓸 때는 모두 같은 Linux OTBR Thread 네트워크에 EXTERNAL_OTBR 모드로
가입해야 한다. STANDALONE Base는 각자 Thread 네트워크를 새로 만들어 Linux Bridge에 닿지 않는다.
Linux Bridge 한 인스턴스가 모든 Base의 CoAP 메시지를 같은 포트(기본 5684)에서 받는다.

- Base 식별자는 칩 eFuse의 EUI-64다. `cfg` 이미지에 Base별 값이 없으므로 첫 Base에 쓴
  `power_base_otbr_bridge_config.bin`을 두 번째 Base에도 그대로 쓸 수 있다.
- 모듈의 소속 Base는 POGO 유선 `SET_COLLECTOR`로 정해진다. 모듈은 그 Base의 Mesh-Local EID로만
  보고하므로, 같은 종류의 센서가 두 Base에 있어도 섞이지 않는다.
- PogoHome 앱과 Linux API는 Base를 등록하지 못한다(API는 읽기 전용). BLE·QR 커미셔닝,
  앱에서 Dataset 전달 같은 연결 수단은 아직 없다.

절차:

```bash
# 1) Linux OTBR 호스트
(umask 077; sudo ot-ctl dataset active -x | grep -v '^Done' > ~/otbr-active-dataset.hex)  # 비밀값: 저장소 밖, 0600
# 첫 Base 때 만든 파일이 있으면 그대로 쓴다. "Done" 줄이 남아 있으면 생성기가 거부한다.
sudo ot-ctl ipaddr mleid                                   # OTBR_MLEID

# 2) 개발 PC: cfg 이미지 (첫 Base 때 만든 파일이 있으면 재사용)
bash tools/gen_power_base_bridge_config.sh "$OTBR_MLEID" 5684 build/cfg_images "$HOME/otbr-active-dataset.hex"

# 3) 새 Base에 펌웨어와 cfg 기록
idf.py -C power_base -B ~/esp-builds/pogo_power_base -p "$BASE2_PORT" flash
python -m esptool --chip esp32h2 -p "$BASE2_PORT" write-flash \
  0x187000 build/cfg_images/power_base_otbr_bridge_config.bin
# 이 보드를 전에 STANDALONE으로 쓴 적이 있으면 한 번만:
python -m esptool --chip esp32h2 -p "$BASE2_PORT" erase-region 0x9000 0x6000
```

4) 새 Base에 전원을 넣으면 Thread에 가입한 뒤 10초마다 HEARTBEAT를 보낸다. Linux 로그에
`[power-base] pb=..xxxx first seen (2 Power Base(s) known)`이 찍히고, `/api/v1/snapshot`의
`power_bases`에 나타난다. PogoHome의 기기 → + → "POGO Power Base 연결" 화면은 화면을 연 뒤
새로 나타난 Base를 이 데이터로 표시한다.

5) 센서 모듈을 새 Base에 POGO로 연결하면 기존 자동 페어링으로 등록되고 그 Base 아래에 표시된다.

## 빌드와 플래시

```bash
source ~/.espressif/v6.1/esp-idf/export.sh

idf.py -C power_base -B ~/esp-builds/pogo_power_base set-target esp32h2
idf.py -C power_base -B ~/esp-builds/pogo_power_base build
idf.py -C power_base -B ~/esp-builds/pogo_power_base -p "$BASE_PORT" flash
```

cfg 파티션만 바꿀 때:

```bash
python -m esptool --chip esp32h2 -p "$BASE_PORT" write-flash \
  0x187000 build/cfg_images/power_base_config.bin
```

STANDALONE 보드를 처음 EXTERNAL_OTBR로 바꿀 때만 동적 Dataset 영역을 지운다.

```bash
python -m esptool --chip esp32h2 -p "$BASE_PORT" erase-region 0x9000 0x6000
```

최신 실물·host 검증 범위는 [`validation-status.md`](validation-status.md)를 기준으로 한다.
