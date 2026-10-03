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
6. Dataset 모드에 따라 Thread를 시작한다. mode 2는 등록 완료 전까지 Thread를 끄고
   전원 레일도 건드리지 않는다(아래 "앱 등록 대기 상태와 Joiner 가입").
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

### AWAIT_COMMISSIONING

Dataset 없이 부팅해 Thread를 끄고 기다린다. Joiner 가입과 완료 표식이 있어야 저장된 Dataset으로
Thread를 켠다. 자세한 규칙은 아래 "앱 등록 대기 상태와 Joiner 가입"을 본다.

센서 모듈에는 세 모드 모두 기존 `SET_DATASET` 단계로 Power Base가 가입한 Dataset을 전달한다.

## NVS 설정

`cfg` 파티션 안에 namespace를 나눠 사용한다.

| namespace | 내용 |
|---|---|
| `power_base_cfg` | POGO 포트, timeout, 전원 제어 |
| `pb_thread` | Dataset mode(0 STANDALONE, 1 EXTERNAL_OTBR, 2 AWAIT_COMMISSIONING)와 external Active TLV(mode 1만) |
| `pb_bridge` | Linux host IPv6 주소와 port |
| `pb_ble_auth` | BLE 등록용 보드별 Security 2 salt·verifier(mode 2의 BLE 시험 이미지에만 있음). 설정 코드 자체는 저장하지 않는다 |

기존 `power_base_cfg` blob은 44바이트 schema v1을 유지한다. `pb_thread` namespace가
없으면 STANDALONE으로 동작해 기존 이미지와 호환된다. `pb_bridge`가 없거나
`enabled=0`이면 exporter만 비활성화하고 나머지 기능은 정상 부팅한다.

동적 `nvs` 파티션에는 OpenThread 저장값 외에 mode 2의 등록 완료 표식(`pb_enroll/done`,
36바이트, 비밀값 없음)이 있다.

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

### 앱 등록 대기 상태와 Joiner 가입 (mode 2, 개발·실물시험 단계)

여분의 새 보드에 `tools/gen_power_base_await_config.sh`로 만든 `cfg` 이미지를 쓰면
`pb_thread:mode=2`(AWAIT_COMMISSIONING)가 된다. 기존 이미지의 기본값 STANDALONE(0)과
EXTERNAL_OTBR(1)의 부팅 동작은 바뀌지 않는다.

**제공 범위를 구분한다.**

| 단계 | 상태 |
|---|---|
| USB 콘솔 시험 경로 | 구현. 여분 보드의 USB-Serial/JTAG 콘솔에 앱 DEBUG 시험이 보여 준 PSKd를 입력한다. `sdkconfig.bench_joiner`로 만든 시험 빌드에만 있다. 여분 보드 1대에서 등록·재부팅 재가입을 실물로 확인했다는 보고가 있다([`validation-status.md`](validation-status.md)). |
| Bridge 주소 | 준비 이미지(`pb_bridge`)에 운영자가 OTBR의 Mesh-Local 주소와 ingest 포트를 직접 넣는다. Dataset과 분리돼 있고 코드·기본값에 특정 집 주소가 없다. |
| 앱 DEBUG 시험 | PogoHome의 DEBUG 전용 "Power Base 등록 시험"이 허브 등록 세션(EUI-64, 일회용 PSKd, 120초 창)을 연다. 성공 판정은 허브가 그 EUI-64의 **새** heartbeat를 받았을 때뿐이다. |
| BLE 전달 경로(펌웨어·통신 규약) | 구현, **실물 BLE 미검증**. `sdkconfig.ble_enrollment`로 만든 시험 빌드에만 있다. 아래 "BLE 등록 전달 경로". 앱의 BLE 화면은 아직 없다. |
| QR, 허브 주소 자동 전달 | 미구현. Bridge 주소는 여전히 준비 이미지에 직접 넣으므로 보드 준비에는 터미널이 필요하다. |
| 제품 빌드(`sdkconfig.defaults`) | Joiner, 콘솔 명령, BLE가 모두 없다. mode 2 보드는 안전 대기만 한다. |

**동작 규칙** (`components/power_base_enrollment`, `power_base/main/enrollment_runtime.c`)

1. 부팅: Thread 스택만 초기화하고 끈다. 전원 레일, exporter, POGO downstream,
   DETECT_OUT은 켜지 않는다. `pb_bridge`가 있어도 마찬가지다.
2. 자동 재가입은 동적 NVS의 완료 표식(`pb_enroll/done`)이 유효하고(크기·magic·버전·CRC-32),
   이 보드의 EUI-64로 기록됐고, OpenThread가 저장한 Active Dataset이 완전하며(Active
   Timestamp, Network Key, Network Name, Extended PAN ID, Mesh-Local Prefix, PAN ID,
   Channel) 표식의 Extended PAN ID·Mesh-Local Prefix와 같을 때만 한다. 그 밖에는 모두
   안전 대기다. 과거 STANDALONE 시험 Dataset은 표식이 없으므로 재개하지 않는다.
3. 시험 빌드의 `pb_join start`: PSKd를 화면에 표시하지 않고 읽어 형식(6~32자, 0-9·A-Z 중
   I·O·Q·Z 제외)을 확인한다. 기존 표식을 **먼저** 지운 뒤 OpenThread Joiner를 시작하고,
   Joiner 시작 직후 PSKd 버퍼를 지운다(형식 오류·실패 경로 포함).
4. Joiner 성공은 등록 성공이 아니다. OpenThread는 이때 Network Key·Channel·PAN ID만
   저장한다. 펌웨어는 그 저장값으로 always-on FTD 설정의 Thread를 켠다
   (`thread_manager_start_as_always_on_ftd_stored()`, `esp_openthread_auto_start()`는
   쓰지 않는다). 부분 Dataset에서는 OpenThread가 자기 partition의 Leader가 되지 않는다
   (`Mle::BecomeLeader()`의 `!IsPartiallyComplete()` 조건).
5. CHILD/ROUTER/LEADER로 attach되고 네트워크로부터 완전한 Dataset을 받은 뒤 표식을
   기록·commit한다. 그 다음에만 기존 `attach_supervisor_task()` → `start_services_once()`를
   한 번 실행한다. exporter heartbeat 형식, eFuse EUI-64 UUID, POGO 동작은 그대로다.
6. 실패 처리: 잘못된 PSKd(`joiner_security`), OTBR·Commissioner 부재나 등록 창 종료
   (`joiner_not_found`), 응답 시간 초과, Joiner 결과 없음(180초 guard), Joiner 성공 후
   120초 안에 attach+완전 Dataset이 안 됨(`attach_timeout`), 표식 기록 실패는 모두
   Thread를 끄고 대기로 돌아간다. 표식이 없으므로 서비스·POGO가 켜지지 않고, heartbeat가
   없으니 앱 세션도 성공하지 않는다. 새 PSKd로 다시 시도한다.
7. 등록 후 Bridge 전송 실패는 기존 정책대로 Thread와 POGO를 막지 않는다. 다만 새 heartbeat가
   없으므로 앱의 등록 세션은 만료된다.

전원 중단 규칙: Joiner 성공 직후, attach 직후, 표식 commit 전에 전원이 끊기면 다음 부팅은
표식이 없으므로 대기다. 표식 commit 후에는 매 부팅 재가입한다. 새 시도를 시작하면 표식이
먼저 지워지므로 이후 어느 시점에 끊겨도 옛 표식과 새(또는 부분) Dataset이 짝지어지지 않는다.

재가입 경로(표식 유효)에서 OTBR이 꺼져 있으면 기존 EXTERNAL_OTBR와 같이 같은 네트워크의
단독 partition LEADER가 될 수 있고, 그 경우 서비스가 시작된다. 새 가입(Joiner) 경로에는
해당하지 않는다(규칙 4).

**보안상 남는 제한** (숨기지 않는다)

- PSKd는 USB로 평문 전송된다. 펌웨어는 echo하지 않지만, 로컬 echo를 켠 터미널에서는
  보일 수 있다(`idf.py monitor`는 로컬 echo를 하지 않는다).
- PSKd는 명령 인자로 받지 않는다(esp_console이 명령줄을 RAM 기록에 남기기 때문). `pb_join`은
  인자를 다시 출력하지 않는다.
- 펌웨어 버퍼는 지우지만 OpenThread 내부 Joiner/DTLS 객체의 사본은 제어하지 못한다.
- 시험 빌드는 USB에 접근할 수 있는 사람에게 Joiner 시작·표식 삭제를 허용한다. 운영 Base에
  쓰지 않는다.

**빌드 구분.** 시험 빌드는 반드시 별도 build 디렉터리와 별도 `SDKCONFIG`로 만든다.
같은 `power_base/sdkconfig`를 쓰면 이후 일반 빌드가 시험 설정을 물려받는다.

```bash
source ~/.espressif/v6.1/esp-idf/export.sh
BENCH_BUILD="$HOME/esp-builds/pogo_power_base_bench_joiner"
idf.py -C power_base -B "$BENCH_BUILD" -D SDKCONFIG="$BENCH_BUILD/sdkconfig" \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.bench_joiner" build
```

**준비 이미지.** Bridge 주소는 Linux OTBR 호스트에서 `sudo ot-ctl ipaddr mleid`로 확인해
직접 넣는다. Dataset은 들어가지 않는다(`--await-commissioning`과
`--external-dataset-file`은 함께 쓸 수 없다).

```bash
bash tools/gen_power_base_await_config.sh build/cfg_images "$OTBR_MLEID" 5684
# -> build/cfg_images/power_base_await_bridge_config.bin (주소 없이 실행하면 power_base_await_config.bin)
```

**시험 콘솔 명령** (`pb>` 프롬프트)

| 명령 | 동작 |
|---|---|
| `pb_join status` | 상태(`waiting`/`joining`/`attaching`/`enrolled`), 마지막 결과, 이 보드의 EUI-64 |
| `pb_join start` | PSKd를 표시 없이 입력(Enter 종료, Ctrl-C 취소, 90초) |
| `pb_join cancel` | 진행 중인 Joiner/attach를 멈추고 대기 |
| `pb_join forget <EUI-64 끝 4자리>` | 이 보드의 표식과 OpenThread 저장값을 지우고 재시작 |

`enrolled`/`resumed`는 attach와 표식 기록까지를 뜻한다. 앱의 `heartbeat_received`는 허브만
판정한다. 앱 DEBUG 화면의 "ot eui64" 안내 대신 `pb_join status`의 EUI-64를 쓴다(값은 같다).

실물 시험 절차와 결과 기록은 [`validation-status.md`](validation-status.md)의
"Power Base 앱 등록(mode 2)"을 따른다.

### BLE 등록 전달 경로 (mode 2, 펌웨어·통신 규약 단계)

USB 콘솔 대신 BLE로 일회용 Thread PSKd를 받아 **같은** 등록 상태 머신을 시작하는 경로다.
Joiner, 완료 표식, 서비스 시작 로직은 위 "동작 규칙" 그대로이고 BLE용 사본은 없다
(`pb_enrollment_request_join()`/`pb_enrollment_cancel()`만 호출한다). Wi-Fi provisioning과
Thread Dataset 전달은 하지 않는다.

**범위와 한계.** 이번 단계는 펌웨어와 규약까지다. 실제 BLE 링크로는 한 번도 실행하지 않았다
(검증 범위는 [`validation-status.md`](validation-status.md) "Power Base BLE 등록 전달 경로").
Bridge 주소·포트는 여전히 준비 이미지의 `pb_bridge`에 직접 넣으므로, BLE 등록이 있어도 처음
준비(이미지 생성·기록)에는 터미널이 필요하다. 허브 주소 자동 전달과 QR 흐름은 후속 범위다.

**구성 요소**

| 위치 | 역할 |
|---|---|
| `components/power_base_ble_auth` | `cfg`의 보드별 자격 증명 record 검사 |
| `components/power_base_ble_enroll/pb_ble_admission.c` | 메시지 parser, challenge, 단일 시도 record, bounded queue, 종료 규칙, worker 1회 처리 |
| `components/power_base_ble_enroll/pb_ble_service.c` | ESP-IDF protocomm(Security 2)에 endpoint 연결 |
| `power_base/main/enrollment_ble.c` | protocomm NimBLE transport 시작·정지 |
| `power_base/main/enrollment_runtime.c` | 등록 task(이제 콘솔·BLE 공용)에서 BLE poll 호출 |

**빌드.** 별도 build 디렉터리와 별도 `SDKCONFIG`를 쓴다. 기본 빌드와 `sdkconfig.bench_joiner`
빌드의 설정은 바뀌지 않는다. BLE 시험 빌드에는 `pb_join` 콘솔과 OpenThread CLI가 없다
(`enrollment_ble.c`가 Security 0/1, 콘솔, CLI, DEBUG 로그가 켜진 구성을 컴파일 단계에서 거부한다).

```bash
source ~/.espressif/v6.1/esp-idf/export.sh
export IDF_COMPONENT_MANAGER=0
BLE_BUILD="$HOME/esp-builds/pogo_power_base_ble_enrollment"
idf.py -C power_base -B "$BLE_BUILD" -D SDKCONFIG="$BLE_BUILD/sdkconfig" \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.ble_enrollment" set-target esp32h2
idf.py -C power_base -B "$BLE_BUILD" -D SDKCONFIG="$BLE_BUILD/sdkconfig" \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.ble_enrollment" build
```

`sdkconfig.ble_enrollment`가 켜는 것: OpenThread Joiner(+DTLS/EC-JPAKE), NimBLE(peripheral +
GATT server, 연결 1개, central/observer 끔), protocomm Security 2만(0·1은 컴파일하지 않음),
`CONFIG_ESP_COEX_SW_COEXIST_ENABLE`(ESP32-H2는 BLE와 IEEE 802.15.4가 라디오 하나를 나눠 쓴다),
`CONFIG_POWER_BASE_BLE_ENROLLMENT`. 파티션 표와 완료 표식은 그대로다.

**부팅 규칙**

- mode 0/1, module 빌드: BLE 코드가 링크되지 않는다.
- mode 2, 유효한 완료 표식으로 재개: 기존 재가입 그대로. 등록 task를 만들지 않으므로 BLE를
  열지 않는다.
- mode 2, 미등록: 등록 task가 BLE를 연다. 조건은 ① Joiner EUI-64 == eFuse EUI-64,
  ② `cfg`의 `pb_ble_auth/cred`가 온전하고(크기·magic·CRC-32·버전·SRP group, salt≠0,
  verifier∉{0,1}) **이 보드의 EUI-64**로 만들어졌을 것. 하나라도 아니면(없음, 손상, 다른 보드용
  이미지, BLE 스택 초기화 실패) BLE를 열지 않고 안전 대기에 머문다. Thread와 POGO 서비스는
  시작하지 않는다. 다른 자격 증명이나 기본 비밀번호로 대체하지 않는다.
- 등록 완료(attach + 완전한 Dataset + 표식 commit): 등록 task가 BLE를 완전히 내린 뒤
  (`protocomm_ble_stop()`: 광고 중지, 연결 종료, NimBLE host·controller 정지, Security 2 세션
  해제) 기존 `on_enrollment_ready()`를 한 번 호출한다. Thread와 exporter는 건드리지 않는다.
  BLE 정지가 실패하면 객체를 해제하지 않고 남겨 두며, 이때도 요청은 모두 `CLOSED`로 거부된다.

**제한 수치** (모두 FreeRTOS tick 기반 단조 시계, 32비트 wrap 안전 비교)

| 항목 | 값 | 동작 |
|---|---|---|
| 등록 광고 창 | 15분(`PB_BLE_ADV_WINDOW_MS`), BLE를 연 시점부터 | 지나면 BLE를 내린다. 진행 중인 시도가 있으면 그 시도가 끝날 때까지는 열어 둔다. 다시 열려면 전원을 껐다 켠다 |
| 인증 실패 | 5회(`PB_BLE_MAX_AUTH_FAILURES`), 부팅당 | Security 2 handshake 메시지가 오류로 끝난 횟수(틀린 설정 코드, 형식 오류, Security 0/1 요청). 도달하면 이후 handshake를 처리하지 않고 BLE를 내린다 |
| handshake 메시지 총수 | 40개(`PB_BLE_MAX_SEC_REQUESTS`), 부팅당 | 성공 포함(완전한 handshake 1회 = 2개). 초과 시 위와 같다 |
| challenge 수명 | 60초(`PB_BLE_CHALLENGE_TTL_MS`) | 발급 60초 뒤부터 `CHALLENGE_EXPIRED` |
| worker queue | 4개(`PB_BLE_CMD_QUEUE_LEN`) | 가득 차면 `BUSY`. accepted를 주지 않는다 |
| worker 주기 | 200 ms | accepted된 start가 Joiner 시작으로 이어지기까지의 최대 지연 |
| Joiner guard / attach 기한 | 180초 / 120초 | 기존 값 그대로 |

인증 실패 잠금은 근처의 누구나 5번 틀리면 걸 수 있다(전원 재투입으로 풀림). 설정 코드가
128비트라 추측 공격에는 의미가 없고, 잠금은 SRP 연산 남용을 막는 용도다.

#### 인증과 암호화

ESP-IDF v6.1 `components/protocomm`의 Security 2를 그대로 쓴다: SRP-6a(RFC 5054 3072비트 group,
g=5, SHA-512) + AES-256-GCM. 자체 암호 프로토콜이나 SRP 구현은 없다.

- 등록하는 security 객체는 `protocomm_security2`의 사본이며 세 항목(new/close transport
  session, handshake handler)만 감싼다. 감싼 함수는 세션 변경과 handshake 실패를 세어
  admission에 알린 뒤 SDK 구현을 그대로 호출한다. `init/cleanup/encrypt/decrypt`는 SDK 것이다.
- Security 0/1은 빌드에 없다. `prov-session`에 Security 0/1 `SessionData`를 보내면 SDK가
  "Security version mismatch"로 거부한다. BLE 연결, 기기 이름, 광고의 EUI-64는 아무것도
  증명하지 않는다. `pb-*` endpoint는 Security 2 세션의 복호화가 성공해야만 handler에 도달한다.
- BLE link 자체의 pairing·bonding·암호화는 쓰지 않는다(`ble_bonding=0`, `ble_sm_sc=0`,
  `ble_link_encryption=0`). 보호는 전부 Security 2 세션이다.
- username = 보드 EUI-64의 소문자 16진수 16자(예: `744dbdfffe65c00a`). password = 설정 코드.
  username은 verifier 계산에 들어가므로 다른 EUI-64로는 같은 코드로도 인증되지 않는다.
- 설정 코드와 Thread PSKd는 서로 다른 자격 증명이다. 설정 코드는 보드마다 한 번 만들고
  (CSPRNG 128비트, RFC 4648 base32 대문자 26자, 구분자 없음), PSKd는 등록 세션마다 허브용으로
  새로 만든다.

`cfg`의 `pb_ble_auth/cred` (420바이트 blob, 정수는 표기대로):

| offset | 길이 | 내용 |
|---|---|---|
| 0 | 4 | magic `PBA1` |
| 4 | 1 | 형식 버전 = 1 |
| 5 | 1 | SRP group = 1 (Security 2: 3072비트, SHA-512) |
| 6 | 1 | salt 길이 = 16 |
| 7 | 1 | 0 |
| 8 | 8 | 대상 EUI-64 |
| 16 | 16 | salt |
| 32 | 384 | verifier `v = 5^x mod N`, big-endian, 앞을 0으로 채움. `x = SHA512(salt ‖ SHA512(username ":" code))` |
| 416 | 4 | CRC-32(IEEE) of [0..415], little-endian |

verifier는 설정 코드가 아니지만 flash를 읽은 사람이 코드 추측을 오프라인으로 검사하거나 이
보드인 척 클라이언트를 속이는 데 쓸 수 있다. 코드가 128비트 난수라 전자는 현실적이지 않다.
flash 암호화는 쓰지 않는다.

#### BLE 구성 (공식 iOS 클라이언트와 대조한 값)

대조 대상: `espressif/esp-idf-provisioning-ios`(ESPProvision) master `78e58f0`의
`ESPBleTransport.swift`, `ESPUtility.swift`, `ESPDevice.swift`, `ESPSecurity2.swift`,
`Security2/SRP/*`와 ESP-IDF v6.1 `protocomm_nimble.c`, `security2.c`, `esp_srp.c`.

- 광고: flags + 128비트 service UUID(전체 목록). scan response: 이름
  `POGO-PB-<EUI-64 대문자 16진수 16자>`(예: `POGO-PB-744DBDFFFE65C00A`). 광고 간격 160 ms
  (protocomm 고정값). 이름의 EUI-64는 목록에서 보드를 고르기 위한 힌트일 뿐이다.
  iOS 클라이언트는 이름 prefix(`POGO-PB-`, 대소문자 무시)로 고르고, 광고의 첫 service UUID로
  service를 찾는다.
- service UUID `2e8a0000-9ffb-4868-bc63-064a14069755`. 펌웨어(NimBLE)의 저장 순서는
  little-endian(문자열의 역순)이다:
  `55 97 06 14 4a 06 63 bc 68 48 fb 9f 00 00 8a 2e`.
- characteristic UUID는 그 배열의 `[12]`(하위 바이트), `[13]`(상위 바이트)을 endpoint의 16비트
  값으로 바꾼 것이다. 문자열로는 `2e8aXXXX-9ffb-4868-bc63-064a14069755`.

| endpoint(0x2901 descriptor 값) | 16비트 | characteristic UUID | 세션 |
|---|---|---|---|
| `prov-session` | 0xFF51 | `2e8aff51-9ffb-4868-bc63-064a14069755` | Security 2 handshake |
| `proto-ver` | 0xFF53 | `2e8aff53-9ffb-4868-bc63-064a14069755` | 인증 전에도 읽힘 |
| `pb-status` | 0xFF60 | `2e8aff60-9ffb-4868-bc63-064a14069755` | 필요 |
| `pb-start` | 0xFF61 | `2e8aff61-9ffb-4868-bc63-064a14069755` | 필요 |
| `pb-cancel` | 0xFF62 | `2e8aff62-9ffb-4868-bc63-064a14069755` | 필요 |

- 클라이언트는 UUID가 아니라 각 characteristic의 User Description descriptor(0x2901) 문자열로
  endpoint를 찾는다(ESPProvision도 그렇게 한다. `prov-session`이 들어 있는 descriptor가 세션
  characteristic이다). 모든 characteristic은 read+write이며 notify는 없다.
- 요청/응답: 값을 write(with response)한 뒤 **같은 characteristic을 read**하면 응답이다.
  한 번에 한 요청만 보낸다.
- `proto-ver`: 아무 값(공식 클라이언트는 `ESP`)을 쓰고 읽으면
  `{"prov":{"ver":"v1.1","sec_ver":2,"sec_patch_ver":1,"cap":[]},"pb_enroll":{"proto":1}}`.
  `sec_ver=2`는 Security 2, `sec_patch_ver=1`은 AES-GCM IV가 "device nonce의 앞 8바이트 +
  big-endian 32비트 카운터(1부터, 암호화·복호화마다 양쪽이 1씩 증가)"임을 뜻한다. 공식 iOS
  클라이언트는 이 값이 1일 때만 카운터 방식을 쓴다. EUI-64는 여기 넣지 않는다.
- Security 2 메시지는 `session.proto`/`sec2.proto` protobuf다. 측정한 크기(host, A·B가 384바이트일
  때): Command0 414, Response0 416, Command1 75, Response1 89바이트. 암호문은
  `ciphertext ‖ 16바이트 tag`이고 AAD는 없다. 키는 `K = SHA512(S)`의 앞 32바이트.
- ATT 크기: `protocomm_nimble.c`는 한 값이 512바이트(`BLE_ATT_ATTR_MAX_LEN`)를 넘으면 거부한다.
  NimBLE의 선호 MTU는 256(`CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU`), 기본은 23이다. MTU−3을 넘는
  write는 Prepare/Execute Write(long write, prepare entry 최대 64개, 30초 timeout), MTU−1을 넘는
  read는 Read Blob으로 처리된다(NimBLE이 offset마다 callback에서 전체 값을 받아 잘라 준다).
  Command0/Response0은 어떤 iOS MTU에서도 한 PDU에 들어가지 않으므로 long write/read가 반드시
  필요하다. CoreBluetooth의 `writeValue(.withResponse)`/`readValue`가 자동으로 한다.
  **이 경로는 실물로 확인하지 않았다.** `pb-*` 메시지는 최대 92바이트(요청)·56바이트(응답)다.
- 알려진 클라이언트 제약: ESP-IDF는 client public key가 정확히 384바이트가 아니면 거부한다.
  공식 iOS·Python 클라이언트는 앞자리 0바이트를 떼고 보내므로 약 1/256 확률로 handshake가
  거부된다(인증 실패 1회로 계산된다). 새 연결로 다시 시도하면 된다.

#### 통신 규약 v1 (`pb_ble_proto.h`)

모든 메시지는 Security 2 세션 안의 평문이다(전송 시 AES-GCM). 다중 바이트 정수는 big-endian.
`pb-start`만 길이가 `pskd_len`에 따라 달라지고 나머지는 고정 길이다. 길이가 다르면 `MALFORMED`.

`pb-status` 요청(20바이트)

| offset | 길이 | 내용 |
|---|---|---|
| 0 | 1 | version = 1 |
| 1 | 1 | type = 0x01 |
| 2 | 1 | flags: bit0 = challenge 발급 요청. 다른 bit는 0 |
| 3 | 1 | 0 |
| 4 | 16 | 조회할 request id(없으면 모두 0) |

`pb-status` 응답(40바이트)

| offset | 길이 | 내용 |
|---|---|---|
| 0 | 1 | version = 1 |
| 1 | 1 | type = 0x81 |
| 2 | 1 | code |
| 3 | 1 | 0 |
| 4 | 8 | 이 보드의 실제 EUI-64 |
| 12 | 1 | 등록 상태: 0 waiting, 1 joining, 2 attaching, 3 enrolled |
| 13 | 1 | 마지막 결과(`pb_enroll_result_t`, 아래 표) |
| 14 | 1 | flags: 0x01 BLE 시도 진행 중, 0x02 조회한 request id가 그 시도임, 0x04 challenge 발급됨, 0x08 Joiner 사용 불가 |
| 15 | 1 | 조회한 요청의 phase: 0 없음, 1 queued, 2 running, 3 failed, 4 enrolled, 5 cancelled |
| 16 | 1 | 조회한 요청의 결과(`pb_enroll_result_t`, 또는 0x80 start 거부됨, 0x81 대체됨) |
| 17 | 1 | 0 |
| 18 | 2 | challenge 수명(초). 발급하지 않았으면 0 |
| 20 | 4 | controller 시도 번호 |
| 24 | 16 | challenge. 발급하지 않았으면 0 |

`code`가 `MALFORMED`/`UNSUPPORTED_VERSION`이면 offset 4 이후는 모두 0이다. 다른 클라이언트의
request id는 응답에 나오지 않는다(0x01 flag만 보인다).

`pb-start` 요청(44 + `pskd_len` = 50~76바이트)

| offset | 길이 | 내용 |
|---|---|---|
| 0 | 1 | version = 1 |
| 1 | 1 | type = 0x02 |
| 2 | 1 | `pskd_len` (6~32) |
| 3 | 1 | 0 |
| 4 | 8 | 대상 EUI-64 |
| 12 | 16 | request id: 클라이언트가 만든 난수. 모두 0이면 안 됨 |
| 28 | 16 | 이 세션의 마지막 `pb-status`가 발급한 challenge |
| 44 | `pskd_len` | PSKd(ASCII, 0-9·A-Z 중 I·O·Q·Z 제외) |

`pb-cancel` 요청(28바이트): `[0]`=1, `[1]`=0x03, `[2..3]`=0, `[4..11]` 대상 EUI-64,
`[12..27]` request id.

`pb-start`/`pb-cancel` 응답(4바이트): `[0]`=1, `[1]`=0x82/0x83, `[2]`=code, `[3]`=0.

| code | 이름 | 뜻 |
|---|---|---|
| 0x00 | OK | status 응답 / cancel 접수 |
| 0x01 | ACCEPTED | start를 받아 worker queue에 넣었다. **Joiner 성공도 등록 성공도 아니다** |
| 0x02 | DUPLICATE | 같은 요청을 이미 받았다. 새로 시작한 것은 없다 |
| 0x10 | UNSUPPORTED_VERSION | version ≠ 1. 내용은 해석하지 않는다. 응답의 version은 1 |
| 0x11 | MALFORMED | 길이·type·예약 바이트 오류, request id가 0 |
| 0x12 | WRONG_TARGET | 대상 EUI-64가 이 보드가 아니다 |
| 0x13 | CHALLENGE_INVALID | 이 세션에서 발급한 적 없음, 값 불일치, 이미 사용함 |
| 0x14 | CHALLENGE_EXPIRED | 60초 경과 |
| 0x15 | INVALID_PSKD | PSKd 형식 오류 |
| 0x16 | BUSY | 다른 시도가 진행 중이거나 worker queue가 가득 참 |
| 0x17 | REQUEST_CONFLICT | 같은 request id에 다른 내용, 또는 이미 끝난 request id의 재사용 |
| 0x18 | NOT_WAITING | 보드가 대기 상태가 아니다 |
| 0x19 | NO_SUCH_ATTEMPT | cancel: 그 request id는 취소할 수 있는 시도가 아니다 |
| 0x1a | CLOSED | 이번 부팅의 BLE 등록이 닫혔다(등록 완료, 잠금, 광고 창 종료) |
| 0x1b | UNAVAILABLE | 이 보드는 Joiner를 실행할 수 없다 |
| 0x1c | INTERNAL | 난수·해시 실패 |

`pb_enroll_result_t` 값: 0 none, 1 marker_corrupt, 2 marker_foreign, 3 marker_read_error,
4 dataset_missing, 5 dataset_mismatch, 6 marker_clear_failed, 7 joiner_start_failed,
8 joiner_security, 9 joiner_not_found, 10 joiner_timeout, 11 joiner_failed,
12 joiner_guard_expired, 13 cancelled, 14 thread_start_failed, 15 attach_timeout,
16 marker_write_failed, 17 enrolled, 18 resumed.

**start 처리 순서** (어느 단계에서 거부돼도 Joiner는 시작되지 않는다)

1. 형식 검사 → `UNSUPPORTED_VERSION`/`MALFORMED`. challenge는 소모하지 않는다.
2. 이미 아는 request id: 진행 중인 시도와 내용(대상·challenge·PSKd의 SHA-256)이 같으면
   `DUPLICATE`, 다르거나 이미 끝난 시도면 `REQUEST_CONFLICT`. 진행 중인 시도는 그대로다.
3. 새 요청이면 이 시점에 세션의 challenge를 소모한다(결과와 무관하게 1회용).
4. 대상 EUI-64 → challenge 일치 → 만료 → PSKd 형식 → Joiner 가능 여부 → 진행 중인 시도
   (`BUSY`) → 대기 상태(`NOT_WAITING`) → queue 여유(`BUSY`) 순으로 검사한다.
5. 통과하면 시도 record를 만들고 PSKd를 한 slot에 넣은 뒤 `START(generation)`를 queue에 넣고
   `ACCEPTED`를 준다. 복호화된 요청 버퍼는 모든 경로에서 지운다.

challenge는 발급한 Security 2 세션에 묶인다. 연결·해제와 모든 handshake 메시지가 epoch를
올리고 challenge를 지우므로 이전 연결이나 재handshake 이전의 challenge는 쓸 수 없다.

**시도의 수명**

- 시도 record는 하나뿐이다. 다른 request id의 start는 `BUSY`이고 진행 중인 시도를 바꾸지
  못한다. 설정 코드를 아는 두 번째 클라이언트도 마찬가지이며, request id를 모르므로 취소도
  못 한다(`NO_SUCH_ATTEMPT`).
- BLE disconnect는 시도를 취소하지 않는다. 세션 키(SDK)와 challenge만 지우고, Joiner는 기존
  제한 시간 안에서 계속된다. 다시 연결해 handshake한 뒤 request id로 `pb-status`를 조회하면
  된다.
- `pb-cancel`은 request id가 일치할 때만 받는다. 아직 queued면 그 자리에서 PSKd를 지우고
  취소한다. running이면 `CANCEL(generation)`을 queue에 넣고, worker가 기존
  `pb_enroll_ctl_cancel()`로 처리한다(joining/attaching일 때만 동작). 등록이 끝난 뒤의 cancel은
  표식과 Thread 연결을 건드리지 않는다.
- 실패한 뒤 재시도는 새 request id와 새 challenge로 한다. 보통 허브 세션도 새로 열어야 한다.

**실행 위치와 잠금**

| 실행 위치 | 하는 일 | 하지 않는 일 |
|---|---|---|
| NimBLE host task(BLE callback) | SDK의 Security 2 handshake(SRP 큰 수 연산 포함), AES-GCM 복호화·암호화, `pb-*` handler: leaf mutex를 잡고 admission 상태만 읽고 쓴다 | OpenThread 호출, NVS 기록, controller 호출, BLE 종료, queue 대기 |
| 등록 task | 200 ms마다 queue를 비우고 controller(Joiner 시작·취소, 표식, Thread 시작)를 호출, 상태 게시, BLE 종료 | BLE callback 안에서 실행되는 일 |
| OpenThread task | Joiner callback이 결과를 queue에 넣는다(기존) | |

잠금 순서: controller mutex(`s_lock`) → OpenThread API lock. BLE service의 leaf mutex는
단독으로만 잡는다. leaf mutex를 잡은 채 controller·OpenThread·NVS·BLE 스택을 호출하지 않고,
BLE callback은 `s_lock`을 잡지 않는다. BLE 종료(`protocomm_ble_stop()` →
`pb_ble_service_stop()`)는 등록 task가 아무 잠금도 잡지 않은 상태에서 호출하며, NimBLE host
task가 event loop를 빠져나온 뒤에 반환하므로 그 뒤에 protocomm 객체를 해제한다. callback이
참조하는 admission 상태와 mutex는 파일 범위 static이고 해제하지 않는다.
queue의 각 명령에는 시도 generation이 붙어 있어서, 취소·종료·교체된 시도의 늦은 명령은
버린다. Joiner의 늦은 결과는 기존대로 attempt 번호로 버린다.

**비밀값 처리**

- PSKd는 광고, 평문 characteristic, 로그, 오류 응답, 파일, NVS 어디에도 없다. 전송 구간은
  AES-GCM 암호문이다.
- 펌웨어가 지우는 사본: protocomm이 넘겨주는 복호화 버퍼(handler가 모든 경로에서 지움),
  admission의 PSKd slot(worker가 꺼낼 때·취소 시), worker의 stack 사본, controller 버퍼(기존).
- 지울 수 없는 것(한계): OpenThread Joiner/DTLS(EC-JPAKE) 객체 안의 PSKd 사본(USB 경로와
  동일), mbedTLS/PSA AES-GCM의 내부 작업 버퍼, SDK가 `free()`만 하고 지우지 않는 SRP 세션
  키·client public key heap 잔여. 시도가 진행되는 동안에는 재전송 식별용
  `SHA-256(domain ‖ 대상 ‖ challenge ‖ PSKd)`가 RAM에 있고, 시도가 끝나면 지운다.
- 로그: ESP-IDF `security2.c`/`esp_srp.c`는 DEBUG 레벨에서 세션 키·proof·SRP 중간값을
  16진수로 출력한다. BLE 시험 빌드는 `CONFIG_LOG_MAXIMUM_LEVEL`이 INFO를 넘으면 컴파일되지
  않는다. INFO 이하에서는 SDK가 payload를 출력하지 않는다(실제 SDK 코드를 host에서 돌려
  로그에 설정 코드·PSKd·salt·verifier가 없음을 확인).
- BLE로는 factory reset, Dataset·Network Key 읽기, 콘솔 실행을 제공하지 않는다. endpoint는
  위 다섯 개뿐이다.

#### 준비 이미지 (BLE)

보드 1대마다 한 번 실행한다. 설정 코드는 인자·환경변수로 받지 않고 화면에도 출력하지 않는다.
코드 파일은 저장소 밖에 0600으로 만들며, 이미 있으면 덮어쓰지 않고 거부한다.

```bash
source ~/.espressif/v6.1/esp-idf/export.sh
EUI=<esptool read-mac으로 확인한 이 보드의 EUI-64 16자리>
CODES="$HOME/pogo-setup-codes"
IMG="build/cfg_images/power_base_ble_${EUI}_config.bin"
mkdir -p "$CODES" && chmod 700 "$CODES" && \
python tools/gen_power_base_ble_config.py \
  --eui64 "$EUI" --bridge-host "$OTBR_MLEID" --bridge-port 5684 \
  --setup-code-file "$CODES/$EUI.setup-code" && \
test -s "$IMG" && echo "준비 이미지 확인: $IMG"      # 0600, 12288바이트
```

- 이미지에는 `power_base_cfg`, `pb_thread:mode=2`(Dataset 없음), `pb_bridge`, `pb_ble_auth`가
  들어간다. salt/verifier는 ESP-IDF 공식 SRP-6a 코드
  (`examples/protocols/esp_local_ctrl/scripts/security/srp6a.py`)로 계산하고, 도구가 식으로 한 번
  더 대조한 뒤 ESP-IDF `nvs_parser`로 완성 이미지를 다시 읽어 네 namespace와 크기를 검사한다.
  검사를 통과한 이미지만 최종 이름을 얻는다.
- 같은 코드로 이미지를 다시 만들 때는 `--existing-setup-code-file`을 쓴다(0600이어야 함).
  그 보드의 이미지가 이미 있는데 새 코드를 만들려 하면 거부한다.
- 이 이미지는 그 EUI-64의 보드에서만 BLE를 연다. 다른 보드에 쓰면 `foreign_eui64`로 거부한다.
- 도구는 실패하면 0이 아닌 값으로 끝나고 이미지를 남기지 않는다. 기록 명령은 생성 명령과
  `&&`로 이어 쓰거나 `test -s "$IMG" &&`로 시작해, 생성이 실패하면 실행되지 않게 한다. 절차는
  [`validation-status.md`](validation-status.md)의 "다음 실물 시험 절차"를 따른다.

#### 앱이 지켜야 할 순서 (후속 앱 구현용)

허브 인증 토큰, 허브 주소, Dataset은 보드에 보내지 않는다. 보드가 받는 것은 PSKd뿐이다.

1. **BLE 인증과 보드 확인.** 이름 prefix `POGO-PB-`로 찾아 연결 → `proto-ver` 확인
   (`sec_ver=2`, `sec_patch_ver=1`) → username = 예상 EUI-64(소문자 16진수), password = 설정
   코드로 Security 2 handshake → `pb-status`의 EUI-64가 예상 값과 같은지 확인. 다르면 중단한다.
   상태가 `waiting`이 아니거나 0x08 flag가 있으면 진행하지 않는다.
   ESPProvision을 쓰면 `ESPProvisionManager.shared.createESPDevice(deviceName:,
   transport: .ble, security: .secure2, proofOfPossession: 코드, username: EUI-64, …)`로 얻은
   `ESPDevice`에 `connect()` → `sendData(path: "pb-status", data:)`. (API 이름은 위 commit 기준이며
   앱에서 직접 실행해 확인한 것은 아니다.)
2. **허브 세션.** 현재 선택·페어링된 허브에 `POST /api/v1/power-base-enrollments`
   (`eui64`, 새 난수 PSKd). `409 power_base_already_connected` 등 오류면 보드에 아무것도 보내지
   않는다.
3. **허브가 받아들인 뒤 전달.** 세션이 `waiting_for_heartbeat`(`joiner_entry: allowed`)가 된 것을
   확인하고, 그때 `pb-status`(challenge 발급) → 60초 안에 `pb-start`(대상 EUI-64, 새 request id,
   challenge, 그 PSKd). challenge를 허브 세션보다 먼저 받아 두지 않는다.
4. **진행 표시.** `ACCEPTED`는 "보드가 요청을 받았다"는 뜻이다. 이후 `pb-status`(request id
   포함)의 queued → running(joining → attaching)을 진행 상태로만 보여 준다. phase가 failed면
   결과 코드를 보여 주고 새 허브 세션·새 request id로 다시 시작한다. 연결이 끊겨도 보드는
   계속 진행하므로 실패로 처리하지 않는다. 등록이 끝나면 보드가 BLE를 내리므로 enrolled
   응답을 못 받고 연결이 끊기는 것이 정상이다.
5. **최종 성공.** 허브 세션의 `heartbeat_received`만 성공이다. BLE의 어떤 응답도, Joiner 성공도,
   phase=enrolled도 성공으로 표시하지 않는다. 허브 세션이 `expired`/`failed`면 실패다.
   사용자가 취소하면 `pb-cancel`(같은 request id)과 허브 `DELETE`를 둘 다 보낸다.

### 기존 수동 추가 절차

Power Base를 둘 이상 쓸 때는 모두 같은 Linux OTBR Thread 네트워크에 EXTERNAL_OTBR 모드로
가입해야 한다. STANDALONE Base는 각자 Thread 네트워크를 새로 만들어 Linux Bridge에 닿지 않는다.
Linux Bridge 한 인스턴스가 모든 Base의 CoAP 메시지를 같은 포트(기본 5684)에서 받는다.

- Base 식별자는 칩 eFuse의 EUI-64다. `cfg` 이미지에 Base별 값이 없으므로 첫 Base에 쓴
  `power_base_otbr_bridge_config.bin`을 두 번째 Base에도 그대로 쓸 수 있다.
- 모듈의 소속 Base는 POGO 유선 `SET_COLLECTOR`로 정해진다. 모듈은 그 Base의 Mesh-Local EID로만
  보고하므로, 같은 종류의 센서가 두 Base에 있어도 섞이지 않는다.
- 이 절차는 운영용 경로다. 앱 등록은 위의 mode 2 시험 경로(USB 콘솔 PSKd 입력, 또는 실물
  미검증인 BLE 시험 빌드; 모두 여분 보드용)만 있고, 앱의 BLE 화면과 QR 전달은 아직 없다. Linux API의 센서 데이터 경로는 읽기 전용이며, 등록 세션
  경로는 TLS·페어링된 기기 전용이다(`pogohome-api-v1.md`).

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
