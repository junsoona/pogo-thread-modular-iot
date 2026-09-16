# POGO 기반 모듈형 Thread IoT 통합 펌웨어

요구사항/배경은 `CLAUDE-CODE-BOOTSTRAP.md`(초기 MVP) 및 이 리팩터링의 지시서를 참고.
이 문서는 빌드·설정 이미지 생성·플래시 절차·실물 배선을 다룬다. 아키텍처 설명은
[`docs/architecture.md`](docs/architecture.md), POGO 포트/확정 GPIO 프로필은
[`docs/pogo-interface.md`](docs/pogo-interface.md), 고정 설정 스키마는
[`docs/module-config.md`](docs/module-config.md), 상태 머신/오류 처리는
[`docs/state-machine.md`](docs/state-machine.md), **전원 모듈(전용 ESP32-H2 보드)
아키텍처·배선·토폴로지 프로토콜은 [`docs/power-base.md`](docs/power-base.md)** 참고.

**중요 (이번 세션: 전원 모듈의 고정 애플리케이션 역할과 Thread 역할 분리):**
전원 모듈은 POGO 계층의 물리적 루트·downstream provisioning coordinator·
UUID/slot/topology registry 관리자·센서 데이터 aggregator라는 **고정된
애플리케이션 역할**을 갖지만, 더 이상 "시스템의 유일한 Thread ROOT/Leader"로
서술되지 않는다 -- 실제 `otDeviceRole`은 전적으로 Thread가 결정하며, 전원 모듈은
항상 켜진 FTD(rx-on-when-idle=true, router-eligible=true)로 CHILD/ROUTER/LEADER
어느 상태에서도 정상 동작한다 (DISABLED/DETACHED만 "준비되지 않은 상태"). 자세한
내용은 [`docs/power-base.md`](docs/power-base.md)의 "module/Thread role decoupling
and collector provisioning" 절 참고. 센서 모듈(`module/`)은 여전히 AUTO 모드에서
ROOT가 되지 않는다 (이 부분은 이번 세션에서 변경하지 않았다 -- role_manager의
LEAF/INTERMEDIATE 판정은 POGO 계층 개념으로 `otDeviceRole`과 무관).

**이번 세션에서 추가로 변경한 것:** 센서 데이터(`"/s"`)와 topology 보고(`"/t"`)의
전송 목적지를 "현재 Thread Leader"(`thread_manager_get_leader_address()`)가 아닌,
POGO `SET_COLLECTOR` 메시지로 전달되는 **collector endpoint**(전원 모듈의 영구
UUID + Mesh-Local EID + CoAP port, `components/collector_endpoint`)로 완전히
교체했다 -- 이제 전원 모듈 자신의 Thread 역할이 바뀌어도 데이터 목적지는 바뀌지
않는다. 또한 UPSTREAM DETECT_IN/DETECT_OUT GPIO를 교환했다 (GPIO12/GPIO2, 이전은
GPIO2/GPIO12) -- 미래의 terminal-leaf Deep-sleep wake용 GPIO12(ESP32-H2 RTC GPIO
채널) 확보가 목적이며, **Deep-sleep 자체는 이번 세션에서 구현하지 않았다**. 실물은
아직 이전 GPIO 배선 상태이므로, **이 세션에서 빌드한 firmware를 flash하기 전에
반드시 아래 "새 GPIO 프로필 flash 전 재배선" 절의 배선을 먼저 교체할 것.**

이 절 아래의 "실물 배선"/"ROOT용 passive power-base 시험 지그" 관련 옛 서술 중
"전원 모듈이 유일한 Leader"라는 표현은 위 변경으로 대체되었다 -- 자세한 내용과
근거는 [`docs/power-base.md`](docs/power-base.md) 참고. **전원 모듈<->센서 모듈 간
배선(이전 GPIO 프로필 기준)은 이전 세션에서 실제로 연결되었고, POGO 프로비저닝부터
Thread 가입, topology 보고, SHT45 sample 수집까지 실물에서 성공했다** -- 자세한
내용은 아래 "실물 검증 결과 및 데이터플레인 registration gate 수정" 절 참고.

**(이후 세션에서 갱신) 위 문단의 "아직 실물로 재검증되지 않았다"는 더 이상 최신
상태를 반영하지 않는다.** 현재 GPIO 프로필(UPSTREAM DETECT_IN=GPIO12/DETECT_OUT=GPIO2)
로 재배선한 firmware가 이후 세션에서 전원 모듈 + PMS5003 terminal 모듈 조합으로 실물
flash/monitor 검증까지 완료했다 -- 센서 측정, Thread 가입, topology 등록, 데이터
수집, 분리 후 재연결(POGO hotswap)까지 전부 실물로 성공했다. 항목별 최신 host-test/
실물검증 상태는 [`docs/validation-status.md`](docs/validation-status.md)를 단일
기준으로 참고할 것 -- 이 preamble을 포함한 문서 곳곳의 "이번 세션"/"아직 미검증"
표현은 그 문단이 쓰인 시점 기준이며, 서로 모순되면 `docs/validation-status.md`
쪽이 우선한다.

**최종 졸업작품 프로토타입은 ESP32-H2 SuperMini 기반 전원 모듈 1대(`power_base/`)와
센서 모듈 N대(`module/`, 현재 보유·검증 대상은 SHT45 1대 + BH1750 1대, 총 2대)로
구성된다.** 별도 PCB를 제작하지 않는다 -- 확정된 GPIO 프로필(POGO UPSTREAM/
DOWNSTREAM UART, DETECT_IN/OUT, BASE_ID, 센서 I2C)이 그대로 이 개발보드들에
적용되고, `dev_mode=AUTO`에서 센서 모듈은 BASE_ID/PEER_HELLO로 **LEAF/INTERMEDIATE**
역할만 실시간으로 판정한다 (ROOT는 전원 모듈 전용이며 AUTO 경로에서 센서 모듈에는
절대 산출되지 않는다 -- `docs/power-base.md` 참고). `configs/dev_root.json`/
`configs/dev_leaf.json`(DETECT/BASE_ID 없는 1-UART 개발모드, module-to-module 전용)은
과거 회귀 시험용으로만 보존한다.

## 실물 검증 결과 및 데이터플레인 registration gate 수정 (이번 세션)

**이전 세션에서 이미 실물로 성공한 항목** (전원 모듈 1대 + SHT45 센서 모듈 1대,
depth=1 단일 홉):

- `power_base`가 OpenThread LEADER가 됨.
- POGO PEER_READY 성공 (전원 모듈<->센서 모듈).
- 영구(efuse 기반) UUID 확인 및 프로비저닝 성공.
- `polling_period_ms=195`가 `POLL_CONFIG`로 정상 전달됨.
- Thread Active Dataset 110바이트가 정상 전달·저장됨.
- 센서 모듈이 `otDeviceRole=2(CHILD)`로 Thread에 가입.
- `JOIN_RESULT_ACK` 성공.
- topology report(`"/t"`) 적용 성공.
- SHT45 2채널 sample이 `sensor_root_aggregator`에 저장됨.

**같은 실물 시험에서 다음 시작 직후 순서 경쟁이 관측되었다:**

```text
power_base_transport: sensor sample from unknown module uuid=..d357 -- dropped, not stored
sensor_transport: sensor sample rejected by receiver: CoAP 4.04
power_base_transport: topology report applied: uuid=..d357 ...
sensor_root_aggregator: sample recorded: uuid=..d357 ...
```

원인: `sensor_service`의 주기적 측정 task는 polling 설정을 받은 즉시 시작하고,
Thread 가입 직후 `sensor_transport`의 `"/s"` 무선 전송을 허용한다. `topology_reporter`는
별도의 500ms polling task에서 `ATTACHED` report를 보내므로, 첫 sensor sample이 전원
모듈의 topology registry 등록보다 먼저 도착할 수 있다 -- 이 경우 전원 모듈은 아직 모르는
uuid이므로 CoAP 4.04 Not Found로 그 sample을 거부한다 (드물게 손실되는 하나의 sample,
치명적이지는 않지만 정책 위반).

**이번 세션에서 적용한 수정 (아직 실물 flash/monitor로 재검증되지 않음):** 새 컴포넌트
`components/topology_gate`가 FreeRTOS 이벤트 그룹 기반의 cross-task-safe
data-plane readiness gate를 제공한다. `topology_reporter`(유일한 writer)는 자신의
`TOPOLOGY_REPORT_ATTACHED`가 실제 CoAP 2.xx 응답을 받은 순간에만 이 gate를 열고,
upstream DETACHED/ERROR·role UNASSIGNED/ROLE_ERROR·Thread 가입 해제·새 provisioning
세션 시작 시 즉시(최대 한 poll tick, `TOPOLOGY_REPORTER_POLL_TICK_MS`) 닫는다.
`sensor_transport_send_sample()`(AUTO 모드에서만, `require_topology_registration=true`)은
이 gate가 열려 있을 때만 `"/s"` 무선 전송을 시도한다 -- 측정/시퀀스 갱신 자체는 gate와
무관하게 기존 정책대로 계속된다. 전원 모듈이 재부팅해 이미 등록된 모듈의 sample을 CoAP
4.04로 거부하면, `sensor_transport`가 즉시 gate를 닫고 `topology_gate_request_reregister()`로
재등록을 요청해 30초 heartbeat 주기를 기다리지 않고 다음 poll tick에 재등록을 시도한다.
DEV_ROOT/DEV_LEAF(module-to-module 회귀, `"/t"` 리소스가 아예 없는 피어)는
`require_topology_registration=false`로 이 gate의 영향을 받지 않는다. 자세한 설계는
`components/topology_gate`의 헤더 주석, `docs/power-base.md`의 관련 절 참고.

**이 수정은 host test(`tools/test_topology_gate.c` 포함)와 clean build 통과에 이어,
이후 세션에서 전원 모듈 + PMS5003 terminal 모듈 조합으로 실물 flash/monitor 재검증까지
완료했다 -- 첫 sample/topology 등록 경쟁이 실제로 사라졌고, 분리 후 재연결(hotswap)에도
gate가 올바르게 재동작한다.** 자세한 항목별 host-test/실물검증 상태는
[`docs/validation-status.md`](docs/validation-status.md) 참고.

## 구조

```
module/                   센서 모듈 공통 ESP-IDF 프로젝트 -- 모든 센서 모듈이 이 하나의
                          바이너리를 사용. AUTO 모드에서 절대 ROOT가 되지 않는다.
power_base/                전원 모듈 전용 ESP-IDF 프로젝트 (신규) -- POGO 계층의 고정
                          물리적 루트(현재는 otDeviceRole과 분리됨 -- CHILD/ROUTER/LEADER
                          어느 상태에서도 서비스 시작, docs/power-base.md 참고).
                          module/과 별도 보드/바이너리. 자세한 내용은 docs/power-base.md.
upper/, lower/             기존 검증용 참조 프로젝트 (회귀 기준, 변경하지 않음)
components/
  pogo_proto/               공용 프레임/CRC16/프로토콜 payload codec (ASSIGN_PARENT_INFO,
                            SET_COLLECTOR, PEER_HELLO capability negotiation 추가 -- 기존
                            wire format은 그대로)
  collector_endpoint/       (신규, 이번 세션) 센서 모듈이 자신의 collector(전원 모듈)
                            endpoint를 보관하는 FreeRTOS-task-safe 캐시 -- POGO
                            SET_COLLECTOR로 전달받고, sensor_transport/topology_reporter가
                            전송 목적지로 사용, parent_service context를 통해 downstream으로
                            그대로 전달(INTERMEDIATE)
  pogo_port/                양방향 POGO 포트 (UPSTREAM/DOWNSTREAM), 접촉 감지, PEER_HELLO,
                            idle PING/PONG liveness
  role_manager/             LEAF/INTERMEDIATE/UNASSIGNED/ROLE_ERROR 런타임 판정
                            (AUTO 모드에서는 ROOT를 절대 산출하지 않음 -- 전원 모듈이
                            시스템의 유일한 ROOT)
  thread_manager/           공통 OpenThread FTD 어댑터 (역할별 link mode 적용,
                            Leader/ROOT 주소 조회, Mesh-Local EID 조회 -- 이번 세션 추가,
                            collector endpoint 계산에 사용) -- module/과 power_base/
                            양쪽이 그대로 사용. thread_manager_get_leader_address()는
                            sensor_transport/topology_reporter의 전송 목적지 결정에는
                            더 이상 쓰이지 않는다 (다른 진단 코드를 위해 API 자체는 유지)
  child_service/            UPSTREAM 포트 전담 -- 부모에게 응답, JOIN_RESULT 재시도,
                            parent UUID 수신(ASSIGN_PARENT_INFO), collector endpoint
                            수신(SET_COLLECTOR, 이번 세션 -- POLL_SET과 SET_DATASET 사이)
  parent_service/           DOWNSTREAM 포트 전담 -- 자식을 프로비저닝, JOIN_RESULT ACK,
                            collector endpoint 전달(SET_COLLECTOR, 이번 세션).
                            module/과 power_base/ 양쪽에서 수정 없이 재사용 (작은 context
                            구조체로 역할별 동작을 주입 -- docs/power-base.md 참고)
  module_config/            고정 센서/보드 설정 ("cfg" NVS 파티션), ESP32-H2 GPIO 검증
                            (gpio_profile.h는 power_base_config도 재사용)
  power_base_config/        전원 모듈 전용 고정 보드 설정 (신규, 센서 필드 없음)
  sensor_provider/          센서 드라이버 인터페이스 + registry + mock
  sensor_channel/           공통 센서 샘플 wire format (pack/unpack)
  sensor_root_aggregator/   (module_uuid, sample_seq) 중복 제거 sample 저장소 (신규,
                            sensor_service에서 분리 -- power_base_transport도 그대로 사용)
  sensor_service/           영구 페어링 센서 소유, 주기 측정, ROOT aggregator (얇은 wrapper)
  sensor_transport/         Thread data plane (CoAP) 센서 데이터 전송 (module/ 측)
  topology_proto/           토폴로지 보고 고정 길이 versioned binary wire format (신규)
  topology_registry/        전원 모듈의 UUID 키 토폴로지 레지스트리, 순수 로직 (신규)
  topology_reporter/        센서 모듈 측 토폴로지 보고 송신자 (신규)
  topology_gate/            첫 sample/topology 등록 경쟁 수정용 data-plane readiness gate
                            (FreeRTOS 이벤트 그룹 기반, 신규 -- 이번 세션)
  power_base_transport/     전원 모듈의 Thread CoAP 수신측 -- "/s"(센서데이터)+"/t"(토폴로지)
                            (신규)
  node_calc/                polling period / UUID backoff 순수 계산 (공유)
  bh1750/, sht45/           실제 I2C 센서 드라이버
  sgp40/, sgp41/            VOC/NOx 센서 드라이버 (Sensirion Gas Index Algorithm 사용)
  sensirion_gas_index_algorithm/  Sensirion 공식 알고리즘 벤더링 (BSD-3-Clause, 출처는
                            THIRD_PARTY_NOTICE.md)
  sensirion_crc8/           SGP4x용 공용 Sensirion CRC-8
  pms5003/                  PMS5003 terminal UART driver + 프레임 파서
configs/                    설정 이미지 원본 (사람이 수정하는 JSON, power_base.json 포함)
tools/                      설정 이미지 생성 스크립트 + 순수 계산 로직 검증 + host test
docs/                       아키텍처/인터페이스/설정/상태머신/전원모듈 문서
```

`upper`/`lower`/`power_base`는 각각 독립된 ESP-IDF 프로젝트이며, `module`과 함께
`EXTRA_COMPONENT_DIRS`로 `components/`를 공유한다 (같은 이유로 idf.py 빌드 로그에
그 프로젝트가 실제로 쓰지 않는 컴포넌트들이 "발견됨"으로 나열/컴파일될 수 있으나,
`REQUIRES`로 링크되지 않는 한 최종 바이너리에는 포함되지 않는다 -- 예: `power_base.elf`에는
`bh1750`/`sensor_service`/`child_service`/`role_manager` 심볼이 전혀 없음을
`nm`으로 확인했다).

## 중요: 빌드 디렉터리는 반드시 이 저장소 밖(ASCII 경로)으로 지정할 것

이 저장소의 상위 경로에 한글 디렉터리명이 포함될 수 있는데, ESP-IDF v6.1의 picolibc
spec 파일 생성 스크립트가 비-ASCII(CJK) 절대 경로를 다루다 빌드가 깨질 수 있다.
소스는 지금 위치 그대로 두고, 빌드 디렉터리만 `-B`로 ASCII 경로에 지정한다.

아래 명령은 모두 **프로젝트 루트**(`main project_1/`)에서, `cd` 없이 `-C <프로젝트 디렉터리>`로
대상을 지정해 그대로 실행 가능하다.

```bash
cd "/Users/junsoona/no-icloud/main project_1"
source ~/.espressif/v6.1/esp-idf/export.sh

idf.py -C module -B ~/esp-builds/pogo_module set-target esp32h2
idf.py -C module -B ~/esp-builds/pogo_module build

idf.py -C power_base -B ~/esp-builds/pogo_power_base set-target esp32h2
idf.py -C power_base -B ~/esp-builds/pogo_power_base build

idf.py -C upper -B ~/esp-builds/pogo_upper set-target esp32h2
idf.py -C upper -B ~/esp-builds/pogo_upper build

idf.py -C lower -B ~/esp-builds/pogo_lower set-target esp32h2
idf.py -C lower -B ~/esp-builds/pogo_lower build
```

네 프로젝트 모두 위 명령(각 `fullclean` 후)으로 빌드 성공(경고 0개)을 확인했다 (아래
"빌드 결과" 참고). `power_base`는 신규 프로젝트로, 전원 모듈 전용 바이너리
(`power_base.bin`)를 만든다 -- `module`과는 별도 보드/별도 바이너리다.

## 설정 이미지 생성 (module 전용)

`module`의 애플리케이션 바이너리(`pogo_module.bin`)는 모든 보드에서 동일하다.
보드/센서별로 다른 것은 `cfg` NVS 파티션 이미지뿐이다. 사람이 수정하는 원본은
`configs/*.json`, 생성 도구는 `tools/pack_module_config.py`:

```bash
source ~/.espressif/v6.1/esp-idf/export.sh   # IDF_PATH + nvs_partition_gen.py 제공

bash tools/gen_config.sh build/cfg_images
# -> build/cfg_images/dev_root_config.bin        (회귀 시험용, dev_mode=DEV_ROOT)
# -> build/cfg_images/dev_leaf_config.bin        (회귀 시험용, dev_mode=DEV_LEAF)
# -> build/cfg_images/prototype_sht45_config.bin  (실물 AUTO, SHT45 페어링 보드용)
# -> build/cfg_images/prototype_bh1750_config.bin (실물 AUTO, BH1750 페어링 보드용)
```

각 실행마다 실제 CRC와 packing-time 검증(측정 timeout=0 금지, BASE_ID 규칙 등) 결과를
출력한다. 자세한 스키마/CRC/드라이버 레지스트리 설명은
[`docs/module-config.md`](docs/module-config.md).

`power_base`의 설정 이미지(`power_base_config.bin`, 센서 필드 없음)는 별도 스크립트로
생성한다:

```bash
bash tools/gen_power_base_config.sh build/cfg_images
# -> build/cfg_images/power_base_config.bin
#    (전체 NVS 파티션 이미지 파일 자체는 12288 bytes = 0x3000 -- power_base/partitions.csv의
#     "cfg" 파티션 크기와 정확히 일치. 그 안에 담긴 실제 설정 blob은 44 bytes뿐이며
#     나머지는 NVS 포맷 오버헤드다 -- 혼동하지 말 것.)
```

자세한 스키마는 [`docs/power-base.md`](docs/power-base.md) 14절.

## 새 GPIO 프로필 flash 전 재배선 (이번 세션, 필수)

**실물은 현재 이전 GPIO 배선(UPSTREAM DETECT_IN=GPIO2, DETECT_OUT=GPIO12)으로
연결되어 있다. 이번 세션에서 빌드한 firmware(UPSTREAM DETECT_IN=GPIO12,
DETECT_OUT=GPIO2로 교환됨)를 flash하기 전에 반드시 아래 DETECT 두 선을 먼저
바꿔 연결할 것.** 배선을 바꾸지 않고 새 firmware만 flash하면 DETECT_IN/DETECT_OUT이
서로 뒤바뀐 채로 동작해 도킹 감지 자체가 실패한다 (또는 완전히 우연히 동작하는 것처럼
보일 수도 있으나 절대 신뢰하지 말 것).

**전원 모듈 <-> 첫 센서 모듈** (`docs/power-base.md` 4절 참고, 전원 모듈 쪽 GPIO는
변경 없음):

| 이전 배선 | 새 배선 |
|---|---|
| 전원 모듈 GPIO22(DETECT_OUT) -- 모듈 GPIO2(DETECT_IN) | 전원 모듈 GPIO22(DETECT_OUT) -- 모듈 GPIO12(DETECT_IN) |
| 전원 모듈 GPIO23(DETECT_IN) -- 모듈 GPIO12(DETECT_OUT) | 전원 모듈 GPIO23(DETECT_IN) -- 모듈 GPIO2(DETECT_OUT) |

**모듈 A(DOWNSTREAM) <-> 모듈 B(UPSTREAM)** (module-to-module 체인, 아래 "실물 배선"
절도 참고):

| 이전 배선 | 새 배선 |
|---|---|
| A GPIO22(DETECT_OUT) -- B GPIO2(DETECT_IN) | A GPIO22(DETECT_OUT) -- B GPIO12(DETECT_IN) |
| A GPIO23(DETECT_IN) -- B GPIO12(DETECT_OUT) | A GPIO23(DETECT_IN) -- B GPIO2(DETECT_OUT) |

다른 모든 신호(UART TX/RX, BASE_ID, I2C, GND)는 변경되지 않는다. POGO 5V 공급은
이번 세션에서도 구현하지 않았다 -- 두 보드 모두 계속 각자 USB 전원을 쓰고, 신호선과
GND만 연결한다.

재배선 후 사용할 flash/monitor 명령은 이 문서 바로 아래 "플래시" 절과 동일하다 --
`bash tools/gen_config.sh build/cfg_images`/`bash tools/gen_power_base_config.sh
build/cfg_images`로 새 GPIO 값이 반영된 설정 이미지를 다시 생성한 뒤 (이 세션에서
이미 재생성해 커밋된 `build/cfg_images/*.bin`이 최신이면 다시 만들 필요는 없다),
아래 "전원 모듈 + 센서 모듈 배치" 또는 "실물 AUTO 배치" 절의 명령을 그대로 실행한다.
**배선을 바꾸기 전에는 아래 명령들을 실행해 실물 검증을 했다고 보고하지 않는다.**

## 플래시 (사용자가 실행)

포트는 재연결 시 바뀔 수 있으니 `ls /dev/tty.usbmodem*`로 먼저 확인.
현재 확인된 값: lower 역할이었던 보드 `/dev/tty.usbmodem113301`, upper 역할이었던 보드
`/dev/tty.usbmodem113401`. 아래 명령은 모두 프로젝트 루트에서 그대로 실행 가능하다
(설정 이미지 경로도 루트 기준 상대경로로 통일했다 -- 실행 위치가 달라져도 혼동되지 않는다).

### 실물 AUTO 배치 (권장 순서 -- 보드 A=SHT45, 보드 B=BH1750)

**중요**: 이전에 다른 이미지를 플래시해 본 보드라면 최초 시험에서 `erase-flash`부터
시작해 "정말 새 코드가 스스로 형성/가입한 것"임을 명확히 한다.

```bash
cd "/Users/junsoona/no-icloud/main project_1"
source ~/.espressif/v6.1/esp-idf/export.sh

# 0) 설정 이미지 생성 (아직 안 했다면)
bash tools/gen_config.sh build/cfg_images

# 1) 두 보드 모두 완전히 지운다 (포트는 실제 연결에 맞게 확인)
python -m esptool --chip esp32h2 -p /dev/tty.usbmodemAAAA erase-flash   # 보드 A
python -m esptool --chip esp32h2 -p /dev/tty.usbmodemBBBB erase-flash   # 보드 B

# 2) 두 보드에 동일한 module 앱/부트로더/파티션 테이블을 플래시
idf.py -C module -B ~/esp-builds/pogo_module -p /dev/tty.usbmodemAAAA flash
idf.py -C module -B ~/esp-builds/pogo_module -p /dev/tty.usbmodemBBBB flash

# 3) 보드 A(SHT45)에 prototype_sht45_config.bin 기록
python -m esptool --chip esp32h2 -p /dev/tty.usbmodemAAAA \
    write-flash 0x187000 build/cfg_images/prototype_sht45_config.bin

# 4) 보드 B(BH1750)에 prototype_bh1750_config.bin 기록
python -m esptool --chip esp32h2 -p /dev/tty.usbmodemBBBB \
    write-flash 0x187000 build/cfg_images/prototype_bh1750_config.bin

# 5) 배선 완료 후 (아래 "실물 배선" 참고) 두 monitor 연결 (각각 다른 터미널)
idf.py -C module -B ~/esp-builds/pogo_module -p /dev/tty.usbmodemAAAA monitor
idf.py -C module -B ~/esp-builds/pogo_module -p /dev/tty.usbmodemBBBB monitor
```

`/dev/tty.usbmodemAAAA`/`BBBB`는 실제 연결 시 `ls /dev/tty.usbmodem*`로 확인한 값으로
바꿀 것 (재연결 시 바뀔 수 있음). USB Serial/JTAG 콘솔로 전환했으므로 (아래
[`docs/pogo-interface.md`](docs/pogo-interface.md) 참고) `flash`/`monitor` 모두 같은
USB 포트로 동작한다. `0x187000`은 `module/partitions.csv`의 `cfg` 파티션 offset.

두 번째 시험부터는 앱/설정이 이미 맞게 들어가 있다면 2~5단계만 반복하면 된다 (앱 코드만
바뀌었다면 1번 없이 2번부터, `cfg` 이미지만 바뀌었다면 3~4번만).

### 전원 모듈 + 센서 모듈 배치 (depth=1 단일 홉 -- 실물 검증 완료)

전원 모듈<->센서 모듈 간 새 배선([`docs/power-base.md`](docs/power-base.md) 4절 참고)은
이후 세션에서 실제로 연결되어, 전원 모듈 + PMS5003 조합으로 실물 검증까지 완료했다 (위
"실물 검증 결과" 절, [`docs/validation-status.md`](docs/validation-status.md) 참고). 전원
모듈/센서 모듈의 실제 시리얼 포트는 연결할 때마다 `ls /dev/tty.usbmodem*`로 다시 확인해
아래 변수에 채울 것 -- 특정 경로를 고정값으로 가정하지 않는다:

```bash
cd "/Users/junsoona/no-icloud/main project_1"
source ~/.espressif/v6.1/esp-idf/export.sh

# 0) 설정 이미지 생성 (아직 안 했다면)
bash tools/gen_config.sh build/cfg_images
bash tools/gen_power_base_config.sh build/cfg_images

BASE_PORT=/dev/cu.usbmodemXXXX      # 전원 모듈 -- ls /dev/tty.usbmodem*로 확인 후 채울 것
MODULE_PORT=/dev/cu.usbmodemYYYY    # 첫 번째 센서 모듈

# 1) 이전에 다른 이미지를 플래시해 본 보드라면 최초 시험에서 erase-flash부터
python -m esptool --chip esp32h2 -p "$BASE_PORT" erase-flash
python -m esptool --chip esp32h2 -p "$MODULE_PORT" erase-flash

# 2) 각자의 앱/부트로더/파티션 테이블 플래시 (서로 다른 바이너리!)
idf.py -C power_base -B ~/esp-builds/pogo_power_base -p "$BASE_PORT" flash
idf.py -C module -B ~/esp-builds/pogo_module -p "$MODULE_PORT" flash

# 3) cfg 파티션 기록 -- offset은 각 프로젝트의 partitions.csv에서 확인한 값 사용
#    (power_base/partitions.csv, module/partitions.csv 모두 현재 0x187000, 0x3000)
python -m esptool --chip esp32h2 -p "$BASE_PORT" \
    write-flash 0x187000 build/cfg_images/power_base_config.bin
python -m esptool --chip esp32h2 -p "$MODULE_PORT" \
    write-flash 0x187000 build/cfg_images/prototype_sht45_config.bin

# 4) 새 배선 연결 후 (docs/power-base.md 4절) 두 monitor 실행
idf.py -C power_base -B ~/esp-builds/pogo_power_base -p "$BASE_PORT" monitor
idf.py -C module -B ~/esp-builds/pogo_module -p "$MODULE_PORT" monitor
```

**기대 로그와 합격 기준**은 아래 "기대 로그" 절과 [`docs/power-base.md`](docs/power-base.md)
15/16절 참고. 배선이 없는 상태에서 "실물 연동 성공"이라고 보고하지 않는다 -- 이 절차는
사용자가 새 배선을 마친 뒤 직접 실행해 검증해야 한다.

### 3단 센서 체인 실물 배선 (Power Base -> SHT45/BH1750 INTERMEDIATE -> PMS5003 LEAF, 다음 실물 시험)

**목표 구성**: 전원 모듈(`power_base/`) 1대 -> SHT45 또는 BH1750 중간 모듈(INTERMEDIATE,
depth=1) 1대 -> PMS5003 말단 모듈(LEAF, depth=2) 1대. 세 보드 모두 `module`/`power_base`
프로젝트의 동일한 앱 바이너리를 그대로 쓰고(전원 모듈만 별도 `power_base` 바이너리),
**서로 다른 것은 `cfg` 파티션의 설정 이미지뿐**이다 -- 두 센서 모듈에는 완전히 동일한
`module` 앱을 flash한 뒤 `prototype_sht45_config.bin`(또는 `prototype_bh1750_config.bin`)과
`prototype_pms5003_config.bin`만 서로 다르게 기록한다. 세 보드 모두 **POGO 5V는 아직
사용하지 않고 각자 USB로 독립 전원을 공급한다** ([`docs/validation-status.md`](docs/validation-status.md)의
"명시적으로 범위 밖" 참고 -- POGO 커넥터를 통한 5V 공급은 이번 범위에 포함되지 않는다).

**배선표** (전원 모듈<->중간 모듈은 [`docs/power-base.md`](docs/power-base.md) 4절과
동일, 중간 모듈<->말단 모듈은 위 "모듈 간 연결 (A의 DOWNSTREAM -> B의 UPSTREAM)"과
동일 -- 현재 GPIO 프로필 기준, UPSTREAM DETECT_IN=GPIO12/DETECT_OUT=GPIO2):

| 구간 | 신호 | 연결 |
|---|---|---|
| 전원 모듈 DOWNSTREAM -> 중간 모듈 UPSTREAM | UART TX/RX | 전원 모듈 GPIO10(TX) -- 중간 모듈 GPIO5(RX); 전원 모듈 GPIO11(RX) -- 중간 모듈 GPIO4(TX) |
| 전원 모듈 DOWNSTREAM -> 중간 모듈 UPSTREAM | DETECT | 전원 모듈 GPIO22(DETECT_OUT) -- 중간 모듈 GPIO12(DETECT_IN); 전원 모듈 GPIO23(DETECT_IN) -- 중간 모듈 GPIO2(DETECT_OUT) |
| 전원 모듈 DOWNSTREAM -> 중간 모듈 UPSTREAM | BASE_ID | 전원 모듈의 BASE_ID 접점(GND) -- 중간 모듈 GPIO14 |
| 전원 모듈 DOWNSTREAM -> 중간 모듈 UPSTREAM | GND | 공통 GND (전원선은 연결하지 않음 -- 각자 USB 전원) |
| 중간 모듈 DOWNSTREAM -> 말단 모듈 UPSTREAM | UART TX/RX | 중간 모듈 GPIO10(TX) -- 말단 모듈 GPIO5(RX); 중간 모듈 GPIO11(RX) -- 말단 모듈 GPIO4(TX) |
| 중간 모듈 DOWNSTREAM -> 말단 모듈 UPSTREAM | DETECT | 중간 모듈 GPIO22(DETECT_OUT) -- 말단 모듈 GPIO12(DETECT_IN); 중간 모듈 GPIO23(DETECT_IN) -- 말단 모듈 GPIO2(DETECT_OUT) |
| 중간 모듈 DOWNSTREAM -> 말단 모듈 UPSTREAM | GND | 공통 GND (전원선은 연결하지 않음) |
| 중간 모듈 자체 센서 | SHT45 또는 BH1750 I2C | 중간 모듈 GPIO0(SDA)/GPIO1(SCL) + GND + 3.3V |
| 말단 모듈 자체 센서 | PMS5003 UART | 말단 모듈 GPIO0(TX->PMS RX)/GPIO1(RX<-PMS TX) + GND + PMS5003 자체 5V |

**중간 모듈은 downstream POGO 포트가 살아 있는 일반 I2C 센서 모듈이므로 말단 모듈을 그
DOWNSTREAM에 그대로 꽂으면 된다 -- PMS5003의 downstream 비활성화 정책은 PMS5003 자신에게만
적용되고, PMS5003을 자식으로 받는 중간 모듈에는 영향이 없다.**

```bash
cd "/Users/junsoona/no-icloud/main project_1"
source ~/.espressif/v6.1/esp-idf/export.sh

# 0) 설정 이미지 생성 (SGP40/SGP41도 함께 생성됨 -- 이번 시험에는 사용하지 않음)
bash tools/gen_config.sh build/cfg_images
bash tools/gen_power_base_config.sh build/cfg_images

# 포트는 연결할 때마다 ls /dev/tty.usbmodem*로 확인해 채울 것 -- 임의로 추정하지 않는다
BASE_PORT=/dev/cu.usbmodemXXXX       # 전원 모듈
MIDDLE_PORT=/dev/cu.usbmodemYYYY     # SHT45(또는 BH1750) 중간 모듈
TERMINAL_PORT=/dev/cu.usbmodemZZZZ   # PMS5003 말단 모듈

# 1) 이전에 다른 이미지를 플래시해 본 보드라면 최초 시험에서 erase-flash부터
python -m esptool --chip esp32h2 -p "$BASE_PORT" erase-flash
python -m esptool --chip esp32h2 -p "$MIDDLE_PORT" erase-flash
python -m esptool --chip esp32h2 -p "$TERMINAL_PORT" erase-flash

# 2) 전원 모듈은 power_base 앱, 두 센서 모듈에는 동일한 module 앱을 flash
idf.py -C power_base -B ~/esp-builds/pogo_power_base -p "$BASE_PORT" flash
idf.py -C module -B ~/esp-builds/pogo_module -p "$MIDDLE_PORT" flash
idf.py -C module -B ~/esp-builds/pogo_module -p "$TERMINAL_PORT" flash

# 3) cfg 파티션만 서로 다르게 기록 (offset은 module/power_base 공통 0x187000)
python -m esptool --chip esp32h2 -p "$BASE_PORT" \
    write-flash 0x187000 build/cfg_images/power_base_config.bin
python -m esptool --chip esp32h2 -p "$MIDDLE_PORT" \
    write-flash 0x187000 build/cfg_images/prototype_sht45_config.bin   # 또는 prototype_bh1750_config.bin
python -m esptool --chip esp32h2 -p "$TERMINAL_PORT" \
    write-flash 0x187000 build/cfg_images/prototype_pms5003_config.bin

# 4) 배선 순서대로 도킹: 먼저 전원 모듈 <-> 중간 모듈, 그 다음 중간 모듈 <-> 말단 모듈

# 5) 세 보드 monitor 로그를 각각 파일로 저장 (타임스탬프 포함 파일명 권장)
mkdir -p logs
idf.py -C power_base -B ~/esp-builds/pogo_power_base -p "$BASE_PORT" monitor \
    | tee "logs/base_$(date +%Y%m%d_%H%M%S).log"
idf.py -C module -B ~/esp-builds/pogo_module -p "$MIDDLE_PORT" monitor \
    | tee "logs/middle_$(date +%Y%m%d_%H%M%S).log"
idf.py -C module -B ~/esp-builds/pogo_module -p "$TERMINAL_PORT" monitor \
    | tee "logs/terminal_$(date +%Y%m%d_%H%M%S).log"
```

(`tee`는 화면 출력과 파일 저장을 동시에 한다. 세 monitor는 서로 다른 터미널에서 실행할
것. 저장한 세 로그 파일은 `tools/validate_multihop_logs.py`로 사후 판정할 수 있다 --
아래 참고.)

**시험 순서와 합격 조건**:

1. **정상 부팅**: 전원 모듈이 attach(CHILD/ROUTER/LEADER 중 하나) 후 중간 모듈을 도킹,
   중간 모듈이 자신의 upstream 프로비저닝(POGO PEER_READY, Thread join, topology 등록)을
   완료한 뒤 말단 모듈을 중간 모듈의 DOWNSTREAM에 도킹. 합격: 두 센서 모듈 모두
   `JOIN_RESULT_ACK`를 받고, 전원 모듈의 topology registry에 서로 다른 UUID 2개가
   depth=1(중간)/depth=2(말단)로 등록되며, 두 UUID 모두에서 sensor sample이 수신된다.
2. **말단 분리/재연결**: 말단 모듈(PMS5003)을 중간 모듈에서 물리적으로 분리했다가 다시
   연결. 합격: 중간 모듈의 topology/parent_service 세션이 말단 재연결 후 정상적으로
   재프로비저닝되고, 전원 모듈의 registry에서 중간 모듈의 UUID/등록 상태는 영향받지
   않는다 (말단 UUID만 재등록되거나 갱신됨).
3. **중간 분리/재연결**: 중간 모듈(SHT45/BH1750)을 전원 모듈에서 물리적으로 분리했다가
   다시 연결 (말단 모듈은 중간 모듈에 계속 꽂힌 채). 합격: 중간 모듈이 재도킹 후 다시
   upstream 프로비저닝을 완료하고, 말단 모듈도 (중간 모듈이 다시 살아나면) 함께
   재프로비저닝된다. 전원 모듈의 registry는 다른 모듈의 상태를 잃지 않는다.
4. **전원 모듈 재부팅**: 두 센서 모듈은 그대로 둔 채 전원 모듈만 재부팅. 합격:
   `topology_gate` 4.04 복구 경로대로 두 모듈 모두 재등록되고, 재등록 후 다시 정상적으로
   sample이 수신된다 (depth=1 단일 홉에서 이미 확인된 것과 동일한 경로가 depth=2에서도
   두 모듈 각각에 대해 성립하는지가 이 시험의 핵심 확인 대상).

### 회귀 확인용 (기존 upper/lower, 그리고 module의 DEV_ROOT/DEV_LEAF 회귀 이미지)

```bash
cd "/Users/junsoona/no-icloud/main project_1"
idf.py -C lower -B ~/esp-builds/pogo_lower -p /dev/tty.usbmodemAAAA flash monitor
idf.py -C upper -B ~/esp-builds/pogo_upper -p /dev/tty.usbmodemBBBB flash monitor
```

`module`을 `dev_root_config.bin`/`dev_leaf_config.bin`으로 플래시하면 (POGO GPIO 프로필
쪽은 실물 배선 없이) upper/lower와 동일한 1-UART 개발모드 동작을 재현한다 -- "실물 AUTO
배치" 위 1~2단계까지는 동일하고, 3~4단계에서 `dev_leaf_config.bin`/`dev_root_config.bin`을
쓰면 된다.

`monitor`는 `Ctrl+]`로 종료.

## 실물 배선

확정 GPIO 프로필과 그 근거는 [`docs/pogo-interface.md`](docs/pogo-interface.md)의
"확정 GPIO 프로필" 절 참고. **현재 UPSTREAM DETECT_IN은 GPIO12, DETECT_OUT은
GPIO2다** (GPIO13은 보드 내장 LED와 충돌하고, 그 다음 시도했던 GPIO6/7은 ESP32-H2
SuperMini/SiP 패키지에서 아예 외부로 나오지 않아 사용할 수 없다는 것이 뒤늦게
확인되어 한때 DETECT_IN=GPIO2로 재수정했었다. **이번 세션에 미래 Deep-sleep wake
용도로 DETECT_IN/DETECT_OUT을 다시 GPIO12/GPIO2로 교환했다** -- 근거와 전체 이력은
docs/pogo-interface.md 참고. 실물은 아직 구배선(DETECT_IN=GPIO2/DETECT_OUT=GPIO12)
상태이므로 새 firmware flash 전 재배선 필수, 위 "새 GPIO 프로필 flash 전 재배선"
절 참고). 아래는 두 센서 모듈을 module-to-module로 직접 연결해 시험하는 (전원 모듈
없이) 배선이며, **전원 모듈<->첫 센서 모듈 배선표는
[`docs/power-base.md`](docs/power-base.md) 4절**을 참고할 것 (BASE_ID가 전원 모듈
커넥터의 GND 접점이지 GPIO가 아니라는 점, GPIO12/GPIO14가 서로 다른 신호라는 점 등
핵심 주의사항 포함).

### 모듈 간 연결 (A의 DOWNSTREAM -> B의 UPSTREAM)

**이번 세션에서 B의 UPSTREAM DETECT_IN/DETECT_OUT이 교환되었다 (GPIO12/GPIO2, 이전은
GPIO2/GPIO12) -- 아래 표는 새 프로필 기준. 실물이 이전 배선 상태라면 flash 전에
반드시 재배선할 것 (아래 "새 GPIO 프로필 flash 전 재배선" 절 참고).**

| A (DOWNSTREAM) | B (UPSTREAM) |
|---|---|
| GPIO10 (TX) | GPIO5 (RX) |
| GPIO11 (RX) | GPIO4 (TX) |
| GPIO22 (DETECT_OUT) | GPIO12 (DETECT_IN) |
| GPIO23 (DETECT_IN) | GPIO2 (DETECT_OUT) |
| GND | GND |

두 보드의 3.3V/5V 전원선은 서로 연결하지 않는다 (각자 USB로 독립 전원). 모듈-모듈 연결에서는
BASE_ID를 서로 연결하거나 Low로 만들지 않는다 (BASE_ID는 파워베이스 지그 전용, 아래 참고).

DETECT: 각 DETECT_IN에 수신 보드의 3.3V 기준 4.7kΩ~10kΩ pull-up. DETECT_OUT은 open-drain
Low/release만 사용 (push-pull High를 출력하지 않는다).

### (사용 중단) ROOT용 passive power-base 시험 지그

**이 절은 더 이상 유효하지 않다.** 전원 모듈이 시스템의 유일한 ROOT/Leader가 된 이후
(`docs/power-base.md` 참고), 아래 지그처럼 BASE_ID만 GND로 접지해도 센서 모듈은 더 이상
ROOT가 되지 않는다 -- `role_manager`의 AUTO 판정표가 UPSTREAM=POWER_BASE를 다른
UPSTREAM=MODULE과 동일하게 LEAF/INTERMEDIATE로 취급하도록 수정되었다
(`docs/pogo-interface.md`/`docs/power-base.md` 참고). 과거 기록으로만 남긴다:

| 신호 | 연결 |
|---|---|
| A UPSTREAM DETECT_IN (GPIO12, 이번 세션 교환 전 기록 당시에는 GPIO2였음) | GND |
| A UPSTREAM BASE_ID (GPIO14) | GND |
| A UPSTREAM UART (GPIO4/5) | 연결하지 않음 |

BASE_ID/PEER_HELLO의 실제 상호작용을 지금 시험하려면 실물 `power_base/` 보드가 필요하다
(또는 `tools/test_power_base_peer_hello.c`의 host-test 모델 참고) -- MCU 없는 GND 지그는
module-to-module 도킹과 사실상 동일한 코드 경로만 보여주므로 더 이상 별도 의미가 없다.

### 센서

| 보드 | 센서 | SDA | SCL | 비고 |
|---|---|---|---|---|
| A | SHT45 | GPIO0 | GPIO1 | I2C addr 0x44 |
| B | BH1750 | GPIO0 | GPIO1 | I2C addr 0x23 (ADDR low/floating -- breakout 확인) |

두 센서 모두 GND + breakout 사양에 맞는 전원(대개 3.3V)을 연결한다. breakout에 이미
pull-up이 있는 경우가 많으므로, ESP32-H2 I2C 드라이버의 `enable_internal_pullup`과
합성된 저항이 너무 낮아지지 않는지 확인할 것. **합성 저항이 너무 낮아지면(풀업이 너무
강해지면) 내부 pull-up을 끄거나(`enable_internal_pullup=false`), breakout의 외부
저항값을 더 높은 값으로 교체하거나, 중복되는 pull-up 중 하나를 제거한다 -- 더 작은
저항값을 추가하는 방향이 아니다** (저항값을 낮추면 합성 저항은 더 낮아져 문제가
악화된다). 이는 DETECT_IN/BASE_ID의 pull-up(각 신호를 유휴 시 정확히 한쪽 레벨로
고정하는 목적, 4.7k~10kΩ 권장)과는 별개의 문제다 -- I2C pull-up은 버스 전체의 rise time과
관련되므로 SDA/SCL 각각에 브레이크아웃 pull-up과 (활성화되어 있다면) ESP32-H2 내부
pull-up이 병렬로 겹쳐 합성 저항이 예상보다 낮아지는 것이 문제의 원인이다.

### (사용 중단) 위치 교환 시험

**이 절도 더 이상 유효하지 않다** -- 위 "(사용 중단) ROOT용 passive power-base 시험
지그"와 같은 이유다. AUTO 모드 센서 모듈은 물리적 위치를 바꿔도 ROOT가 되지 않으므로
(`role_manager.c`의 판정 수정, 아래 "실물 배선" 절 참고), "이전 LEAF가 ROOT가 되고
이전 ROOT가 LEAF가 된다"는 옛 실험 절차는 재현되지 않는다. `dev_mode=DEV_ROOT` 회귀
이미지를 사용하는 module-to-module 배치에서만 여전히 의미가 있다 (아래 "위치 교환
배치 -- 기대 로그" 참고, 이 역시 DEV_ROOT 전용으로 재작성됨).

## 기대 로그

**아래 "AUTO 배치 -- 보드 A (ROOT, SHT45)" 절은 module-to-module(전원 모듈 없이) 배치의
과거 기록이다. 지금은 passive BASE_ID 지그로 센서 모듈이 ROOT가 되는 경로 자체가
없어졌으므로, 이 정확한 로그(`module_main: role -> ROOT`)는 더 이상 AUTO 모드에서 재현되지
않는다** (재현하려면 `dev_mode=DEV_ROOT` 회귀 이미지를 써야 한다 -- 아래 "회귀 확인용"
참고). **전원 모듈을 포함한 신규 배치의 기대 로그는 이 절 바로 다음
"전원 모듈 + 센서 모듈 배치 -- 기대 로그" 절을 참고할 것.**

### (역사적 기록, module-to-module 전용) AUTO 배치 -- 보드 A (ROOT, SHT45)

```
I (..) module_config: loaded: schema=1 rev=1 driver_id=1397240885 sensor_bitmap=0x0006 bus=i2c#0 timeout=200ms
I (..) sht45: SHT45 initialized: addr=0x44 bus=i2c#0 sda=0 scl=1 serial=0x........ timeout=200ms
I (..) sensor_service: self-calibrated: response_time=..ms -> effective_polling_period_ms=..
I (..) sensor_transport: CoAP sensor transport ready: resource='/s' port=5683
I (..) pogo_port: port 0: PEER_READY (peer_type=1)
I (..) module_main: role -> ROOT: starting Thread as Leader/Router candidate
I (..) thread_manager: ROOT: no dataset in NVS -- forming a new Thread network from Kconfig defaults
I (..) module_main: ROOT ready: otDeviceRole=4(LEADER) -- downstream provisioning may now proceed
I (..) pogo_port: port 1: PEER_READY (peer_type=2)
I (..) parent_service: starting downstream provisioning as ROOT
I (..) parent_service: child identified: mac=... fw=1 sensor_bitmap=0x01
I (..) parent_service: child sensor: bitmap=0x0001 response_time=..ms status=0
I (..) parent_service: polling_period_ms=.. sent (from response_time_ms=..)
I (..) parent_service: dataset accepted (.. bytes) -- waiting for join result (timeout 20000ms)
I (..) parent_service: JOIN OK -- child otDeviceRole=2(CHILD) uuid_backoff=..ms depth=1 polling=..ms
I (..) sensor_service: sample recorded: uuid=..xxyy seq=1 channels=1
```

(이 `module_main`의 DEV_ROOT 경로는 여전히 `LEADER`뿐 아니라 `ROUTER`에서도 준비 완료로
간주한다 -- `upper` 프로젝트의 옛 동작을 그대로 보존하는 회귀 픽스처이기 때문이다.
전원 모듈(`power_base`)만 엄격히 `LEADER`만 요구하도록 수정되었다 -- 위 "전원 모듈 +
센서 모듈 배치 -- 기대 로그" 참고.)

### AUTO 배치 -- 보드 B (LEAF, BH1750)

```
I (..) module_config: loaded: schema=1 rev=1 driver_id=1112027447 sensor_bitmap=0x0001 bus=i2c#0 timeout=300ms
I (..) bh1750: BH1750 initialized: addr=0x23 bus=i2c#0 sda=0 scl=1 timeout=300ms
I (..) sensor_service: self-calibrated: response_time=..ms -> effective_polling_period_ms=..
I (..) pogo_port: port 0: PEER_READY (peer_type=2)
I (..) child_service: sensor measurement OK: bitmap=0x0001 response_time=..ms
I (..) child_service: polling_period_ms=.. accepted
I (..) child_service: dataset stored (.. bytes) -- starting Thread join as LEAF
I (..) thread_manager: LEAF link mode: router_eligible=false rx-on-when-idle=false poll_period=..ms
I (..) child_service: Thread join succeeded, otDeviceRole=2(CHILD)
I (..) child_service: JOIN_RESULT_ACK received (seq=1) -- provisioning session fully confirmed
```

`otDeviceRole=2` == `CHILD`; LEAF는 `router_eligible=false`/`rx-on-when-idle=false`로
로그에서 확인된다. Network Key가 포함된 Dataset 원문, 전체 UUID(짧은 suffix만 표시)는
어떤 로그에도 출력되지 않는다.

### 전원 모듈 + 센서 모듈 배치 -- 기대 로그 (과거 정책, SUPERSEDED -- 당시 firmware 기준)

**이 절 전체(바로 아래 두 로그 블록과 "합격 기준" 단락)는 이번 세션(GPIO 재배선,
`thread_manager_start_as_always_on_ftd()`로의 전환, collector provisioning, 30초
halt 제거) 이전의 firmware로 실물 검증된 기록이다.** 당시 GPIO 배선은 UPSTREAM
DETECT_IN=GPIO2/DETECT_OUT=GPIO12(현재는 GPIO12/GPIO2로 교환됨)였고, 전원 모듈은
`thread_manager_start_as_root()`를 호출해 `ROOT:` 로그를 남겼으며 `otDeviceRole=4
(LEADER)`만 합격으로 인정하고 30초 안에 LEADER가 되지 못하면 `halt_forever()`로
영구 정지했다. **이 정책들은 이번 세션에서 모두 대체되었다 -- 현재 정책과 기대 로그는
바로 아래 "전원 모듈 + 센서 모듈 배치 -- 현재 firmware 기대 로그 (이번 세션, 아직
실물 미검증)" 절을 참고할 것.** 이 절은 GPIO2/12 구배선 + 구 정책 조합이 실제로
동작했다는 과거 기록으로만 보존한다.

**아래 로그는 (당시 firmware 기준으로) 실물로 확인되었다** (전원 모듈 1대 + SHT45
센서 모듈 1대, depth=1 단일 홉 -- 위 "실물 검증 결과 및 데이터플레인 registration
gate 수정" 절 참고). 다만 그
실물 시험에서는 시작 직후 첫 sensor sample이 topology 등록보다 먼저 도착해 CoAP
4.04로 한 번 거부되는 순서 경쟁도 함께 관측되었다 -- 아래 로그는 그 경쟁이 없는
정상 경로를 보여준다 (실제로 경쟁이 발생하면 `power_base_transport: sensor sample
from unknown module uuid=.. -- dropped, not stored`/`sensor_transport: sensor sample
rejected by receiver: CoAP 4.04`가 `topology report applied`보다 먼저 한 번 나타날
수 있다). **이번 세션에서 추가한 `topology_gate` 수정(같은 절 참고)은 정확히 이 경쟁을
없애기 위한 것이며, 아직 이 수정 자체를 다시 flash/monitor로 실물 재검증하지는
않았다** -- 수정이 적용된 상태에서 기대되는 추가 로그는 아래 두 블록 다음에 별도로
적어 둔다.

#### 전원 모듈 (`power_base`)

```
I (..) power_base_config: loaded: schema=1 rev=1 uart=1 tx=10 rx=11 detect_in=23 detect_out=22 ...
I (..) power_base_main: power_enable_gpio not configured -- 5V rail assumed always-on (no switching hardware yet)
I (..) thread_manager: ROOT: no dataset in NVS -- forming a new Thread network from Kconfig defaults
I (..) power_base_main: Thread ready: otDeviceRole=4(LEADER) -- starting CoAP services and downstream provisioning
I (..) power_base_transport: CoAP power-base transport ready: resources='/s','/t' port=5683
I (..) pogo_port: port 1: PEER_READY (peer_type=2)
I (..) parent_service: starting downstream provisioning as POWER_BASE
I (..) parent_service: child identified: mac=... fw=1 sensor_bitmap=0x06
I (..) parent_service: child sensor: bitmap=0x0006 response_time=..ms status=0
I (..) parent_service: dataset accepted (.. bytes) -- waiting for join result (timeout 20000ms)
I (..) parent_service: JOIN OK -- child otDeviceRole=2(CHILD) uuid_backoff=..ms depth=1 polling=..ms
I (..) power_base_main: power base ready: uuid=..xxyy otDeviceRole=4(LEADER) stale_sweep=running -- awaiting the first sensor module
I (..) power_base_transport: topology report applied: uuid=..xxyy depth=1 role=3 type=0
```

`otDeviceRole` 값은 OpenThread의 `openthread/thread.h`가 정의한 실제 값이다 (문자열을
함께 출력하므로 숫자를 외울 필요는 없다 -- `DISABLED`=0, `DETACHED`=1, `CHILD`=2,
`ROUTER`=3, `LEADER`=4). `topology_reporter`/`power_base_transport`의 `role=` 필드는
`topology_proto.h`의 `topology_role_t`(`module_role_t`와 동일한 수치 -- `UNASSIGNED`=0,
`ROOT`=1, `INTERMEDIATE`=2, `LEAF`=3, `ERROR`=4)이며 **`otDeviceRole`과는 서로 다른
숫자 체계**이므로 혼동하지 않는다.

**합격 기준 (당시 firmware, 과거 정책 -- 현재는 아래 "현재 firmware 기대 로그" 절의
기준을 사용할 것)**: `ROLE_TRANSITION_ERROR`/`CONFIG_ERROR` 로그가 없을 것,
`power_base_main`이 로그에 `otDeviceRole=4(LEADER)`를 남길 것 (당시 전원 모듈은
**엄격히 LEADER만** 허용 -- `ROUTER`에서 멈추면 30초 뒤 `halt_forever`로 정지하고
downstream 포트를 절대 활성화하지 않았다 -- **이 halt 정책은 이번 세션에서
제거되었다**, 아래 참고), `stale_sweep=running`일 것 (`FAILED`면 회귀), `pogo_port:
port 1: PEER_READY (peer_type=2)`가 첫 센서 모듈 도킹 직후 나타날 것,
`power_base_transport: topology report applied: ... depth=1 role=3`(LEAF, 절대
role=1(ROOT) 아님)이 나타날 것.

### 전원 모듈 + 센서 모듈 배치 -- 현재 firmware 기대 로그 (이번 세션, 아직 실물 미검증)

**이 절이 현재 코드 기준이다.** 아래는 이번 세션의 변경(GPIO 재배선, Thread/역할
분리, collector provisioning, 30초 halt 제거)이 반영된 firmware를 실제로 flash한
뒤 기대되는 로그이며, **아직 실물 flash/monitor로 검증되지 않았다** (재배선 자체가
아직 안 되어 있음 -- 위 "새 GPIO 프로필 flash 전 재배선" 절 참고).

핵심 차이:

- `thread_manager`가 `ROOT:`가 아닌 `always-on FTD:` 로 로그를 남긴다 (전원 모듈은
  `thread_manager_start_as_always_on_ftd()`를 호출 -- `thread_manager_start_as_root()`는
  이제 module의 DEV_ROOT 회귀 전용).
- `power_base_main`은 `attach_supervisor_task`가 별도 task에서 attach를 기다리며,
  attach가 CHILD/ROUTER/LEADER 중 무엇이든 되는 즉시(엄격히 LEADER를 기다리지 않음)
  `power-base FTD: attached, otDeviceRole=..(...) -- starting services`를 남기고
  서비스를 시작한다. **30초 안에 attach하지 못해도 halt하지 않는다** -- 대신
  `power-base FTD: still not attached after 30000ms (...) -- ... not halting` 경고가
  한 번 나타나고(이후 60초 간격으로만 반복), OpenThread는 계속 attach를 시도한다.
- `parent_service`가 `SET_COLLECTOR`를 `SET_DATASET`보다 먼저 보내고, 모듈 쪽에서
  `child_service: collector endpoint accepted: power_base_uuid=..` 로그가
  `dataset stored ...` 로그보다 먼저 나타난다.

```
I (..) power_base_config: loaded: schema=1 rev=1 uart=1 tx=10 rx=11 detect_in=23 detect_out=22 ...
I (..) power_base_main: power_enable_gpio not configured -- 5V rail assumed always-on (no switching hardware yet)
I (..) thread_manager: always-on FTD: no dataset in NVS -- forming a new Thread network from Kconfig defaults
I (..) power_base_main: power-base FTD: attached, otDeviceRole=2(CHILD) -- starting services (this runs exactly once; a later role change never re-triggers it)
I (..) power_base_transport: CoAP power-base transport ready: resources='/s','/t' port=5683
I (..) pogo_port: port 1: PEER_READY (peer_type=2)
I (..) parent_service: starting downstream provisioning as POWER_BASE
I (..) parent_service: child identified: mac=... fw=1 sensor_bitmap=0x06
I (..) parent_service: child sensor: bitmap=0x0006 response_time=..ms status=0
I (..) parent_service: polling_period_ms=.. sent (from response_time_ms=..)
I (..) parent_service: dataset accepted (.. bytes) -- waiting for join result (timeout 20000ms)
I (..) parent_service: JOIN OK -- child otDeviceRole=2(CHILD) uuid_backoff=..ms depth=1 polling=..ms
I (..) power_base_main: power base ready: uuid=..xxyy otDeviceRole=2(CHILD) stale_sweep=running -- awaiting the first sensor module
I (..) power_base_transport: topology report applied: uuid=..xxyy depth=1 role=3 type=0
```

`otDeviceRole=2(CHILD)`는 예시일 뿐이다 -- 실제 라디오 환경에 따라 이 전원 모듈이
CHILD/ROUTER/LEADER 어느 것으로 먼저 attach하든(단독 standalone 시험에서는 대개
LEADER로 바로 attach) 서비스가 즉시 시작되어야 하며, 어느 쪽이든 "합격"이다.

**합격 기준 (현재)**: `CONFIG_ERROR`/`ROLE_TRANSITION_ERROR`(진짜 API 오류일 때만)
로그가 없을 것, `power_base_main`이 `power-base FTD: attached, otDeviceRole=..(CHILD
또는 ROUTER 또는 LEADER)`를 남길 것 (셋 중 **무엇이든** 합격 -- LEADER만 요구하지
않음), attach가 30초를 넘겨도 `halt`/재부팅 로그 없이 계속 시도할 것(대신 rate-limited
경고), `stale_sweep=running`일 것, `pogo_port: port 1: PEER_READY (peer_type=2)`가
첫 센서 모듈 도킹 직후 나타날 것, 모듈 쪽에서 `SET_COLLECTOR` 성공 로그가
`SET_DATASET`/`dataset stored` 로그보다 먼저 나타날 것, `power_base_transport:
topology report applied: ... depth=1 role=3`(LEAF)이 나타날 것.

#### 첫 센서 모듈 (`module`, 예: SHT45, depth=1) -- 현재 firmware 기준, 아직 실물 미검증

```
I (..) module_config: loaded: schema=1 rev=1 driver_id=1397240885 sensor_bitmap=0x0006 ...
I (..) sht45: SHT45 initialized: addr=0x44 ...
I (..) pogo_port: port 0: PEER_READY (peer_type=1)
I (..) child_service: sensor measurement OK: bitmap=0x0006 response_time=..ms
I (..) child_service: collector endpoint accepted: power_base_uuid=..xxyy port=5683
I (..) child_service: dataset stored (.. bytes) -- starting Thread join as LEAF
I (..) thread_manager: LEAF link mode: router_eligible=false rx-on-when-idle=false poll_period=..ms
I (..) child_service: Thread join succeeded, otDeviceRole=2(CHILD)
I (..) child_service: JOIN_RESULT_ACK received (seq=1) -- provisioning session fully confirmed
I (..) topology_reporter: topology report sent OK (type=0 seq=1 attempt=1/4)
```

`collector endpoint accepted: ...` (SET_COLLECTOR ACK, 이번 세션 추가)는 반드시
`dataset stored ...` 보다 먼저 나타나야 한다 -- `child_service`가 `SET_DATASET`을
`CS_STATE_COLLECTOR_SET` 이전 상태에서는 `POGO_NACK_BAD_STATE`로 거부하기 때문이다.

**합격 기준**: `module_main: role -> ROOT`가 **절대 나타나지 않을 것** (나타나면 회귀 --
`role_manager.c`의 AUTO 판정 수정이 깨진 것). `peer_type=1`(POWER_BASE)로 PEER_READY될
것. `collector endpoint accepted`가 `dataset stored`보다 먼저 나타날 것(이번 세션
추가). `otDeviceRole=2`(CHILD)로 join 성공할 것. `topology_reporter`가 최소 한 번은
성공 전송(`sent OK`, `type=0`=ATTACHED)할 것.

#### `topology_gate` 수정 적용 후 기대되는 추가 로그 (실물 검증 완료)

이 수정을 반영해 다시 flash한 뒤 실물로 확인됨(전원 모듈 + PMS5003 조합, 위 "실물
검증 결과" 참고): `topology_reporter`의 `ATTACHED` 성공 로그(위
`topology report sent OK (type=0 ...)`)가 이 module의 첫 sensor sample 전송보다 먼저
(또는 늦어도 같은 poll tick 안에) 나타나야 하고, 그 사이 짧은 창에서 `DEBUG` 레벨로만
아래와 같은 로그가 보일 수 있다 (기본 `INFO` 레벨 모니터에는 나타나지 않는다):

```
D (..) sensor_transport: topology registration not yet confirmed -- sample uuid=..xxyy seq=1 held back
```

전원 모듈을 재부팅해 재현하는 4.04 복구 경로는:

```
W (..) sensor_transport: sensor sample rejected by receiver: CoAP 4.04
W (..) topology_reporter: power base no longer recognizes this module (a sensor sample was rejected as unknown-module) -- closing the data-plane gate and re-registering now
I (..) topology_reporter: topology report sent OK (type=0 seq=.. attempt=1/4)
```

`sensor_transport: sensor sample rejected ...: CoAP 4.04` 다음에 (최대 30초가 아니라)
`topology_reporter`의 다음 poll tick(<=500ms) 안에 새 `topology report sent OK
(type=0 ...)`가 나타나면 이 수정이 의도대로 동작하는 것이다.

### 위치 교환 배치 -- 기대 로그

동일한 로그가 A/B 역할만 뒤바뀌어 나타난다: 이전에 LEAF였던 보드(B, BH1750)가 이제
`role -> ROOT`를 로그에 남기고, 이전에 ROOT였던 보드(A, SHT45)가 `child_service: ... Thread
join succeeded`를 로그에 남긴다. 펌웨어와 `cfg` 파티션은 그대로이므로 재플래시는 필요 없다.
**(이 절은 module-to-module 전용 DEV_ROOT 회귀 배치에만 적용된다 -- 전원 모듈을 포함한
배치에서는 전원 모듈이 항상 유일한 ROOT이므로 "위치 교환" 개념 자체가 없다.)**

## 빌드 결과

- `idf.py -C module -B ~/esp-builds/pogo_module set-target esp32h2 && ... build` -> 성공 (경고 0개)
- `idf.py -C power_base -B ~/esp-builds/pogo_power_base set-target esp32h2 && ... build` ->
  성공 (경고 0개, 신규)
- `idf.py -C upper -B ~/esp-builds/pogo_upper set-target esp32h2 && ... build` -> 성공 (경고 0개)
- `idf.py -C lower -B ~/esp-builds/pogo_lower set-target esp32h2 && ... build` -> 성공 (경고 0개)
- 네 프로젝트 모두 ESP-IDF v6.1 / esp32h2 타겟, 실제 riscv32-esp-elf 툴체인으로 컴파일 확인
  (`set-target`이 매번 `fullclean`을 먼저 실행하므로 각 빌드는 clean build).
- `power_base.elf`에 `bh1750`/`sensor_service`/`child_service`/`role_manager` 심볼이
  전혀 없음을 `nm`으로 확인 -- EXTRA_COMPONENT_DIRS로 공유된 컴포넌트 중 실제로
  `REQUIRES`되지 않은 것은 컴파일은 될 수 있어도(공유 디렉터리라 "발견"되므로)
  최종 바이너리에 링크되지 않는다 (기존 upper/lower도 동일한 구조).
- `bash tools/gen_config.sh build/cfg_images` -> 4개 이미지(`dev_root_config.bin`,
  `dev_leaf_config.bin`, `prototype_sht45_config.bin`, `prototype_bh1750_config.bin`)
  각 12288 bytes로 생성 확인, pack-time 검증 통과, `cfg` 파티션(0x187000, 0x3000 bytes)
  offset/크기와 일치.
- `bash tools/gen_power_base_config.sh build/cfg_images` -> `power_base_config.bin`
  44 bytes로 생성 확인, pack-time 검증 통과 (신규).
- 기존 27개 host test(25 C + 2 Python) 전부 재통과 + 이번 세션 신규
  `tools/test_topology_gate.c` 1개(실제 `components/topology_gate/topology_gate.c`를
  링크, FreeRTOS 이벤트 그룹은 `tools/host_test_stubs/freertos/`로 대체) 통과 -- 총 30개
  (28 C + 2 Python) 전부 통과 (정확한 항목별 통과 수는 최종 보고 참고). `git diff --check`
  -> 공백 오류 없음.

## 보유 센서 현황

사용자는 PMS5003, BH1750, SGP40, SGP41, SHT45를 모두 실제로 보유하고 있다.
항목별 host-test/실물검증 상태를 한곳에 정리한 표는
[`docs/validation-status.md`](docs/validation-status.md) 참고 (아래는 요약).

- **SHT45**: 전원 모듈과의 depth=1 단일 홉 조합에서 **실물로 확인됨** (I2C 통신, 변환값,
  POGO 프로비저닝, Thread 가입, topology 보고, sample 수집 -- 위 "실물 검증 결과" 절).
- **PMS5003**: terminal 모듈(downstream POGO 완전 비활성화, UART1/GPIO0/1을 센서에 전용)로
  전원 모듈과의 조합이 **실물로 확인됨** -- 실제 측정값 수집, Thread 가입, topology 등록,
  `topology_gate` 데이터플레인 gate, 그리고 분리 후 재연결(POGO hotswap)까지 실물에서
  성공했다.
- **BH1750**: 코드/host test는 SHT45와 동일 경로로 완료했으나, 실물 배선·측정은 아직
  검증하지 않았다.
- **SGP40 / SGP41**: 코드(I2C 명령, CRC, 벤더링된 Sensirion Gas Index Algorithm)와 host
  test는 모두 완료했고, `configs/prototype_sgp40.json`/`configs/prototype_sgp41.json`로
  `driver_id`가 지정되어 있다 (이번 세션 신규 -- 아래 "3단 센서 체인 실물 배선" 절 참고).
  실물 배선·실행 검증은 아직 하지 않았다.
- **3단 이상 체인(depth>=2)**: 지금까지 확인된 것은 depth=1 단일 홉(전원 모듈 + PMS5003
  1대)뿐이다. Power Base -> SHT45/BH1750 INTERMEDIATE -> PMS5003 LEAF 3단 체인은 아직
  실물 미검증 -- 배선/시험 절차는 아래 "3단 센서 체인 실물 배선" 절 참고.

## 아직 실물 보드에서 확인되지 않은 항목

**이번 세션까지 실물로 확인된 조합은 전원 모듈 1대 + (SHT45 또는 PMS5003) 센서 모듈
1대, depth=1 단일 홉뿐이다.** 자세한 항목별 상태는
[`docs/validation-status.md`](docs/validation-status.md)에 정리했다. 아래는 소프트웨어
구현/host test/빌드는 완료했지만 여전히 실물 검증이 필요한 항목이다:

- **3단 센서 체인(Power Base -> SHT45/BH1750 INTERMEDIATE -> PMS5003 LEAF, depth=2)**:
  중간 모듈이 upstream 프로비저닝 완료 후 INTERMEDIATE로 전환되는지, downstream 자식에게
  UUID/slot/depth+1/polling/collector endpoint/Dataset을 올바르게 전달하는지, 중간
  모듈이 항상 켜진 router-eligible FTD로 유지되는지, 말단이 SED LEAF로 유지되는지 -- 코드
  리뷰와 host test까지만 확인, 실물 미검증. 아래 "3단 센서 체인 실물 배선" 절 참고.
- BH1750 실물 I2C 통신·변환값 -- SHT45와 동일 경로이나 아직 미확인.
- SGP40/SGP41 실물 배선·측정 -- 코드/알고리즘/host test는 완료했으나 미확인.
- 두 번째/세 번째 모듈이 동시에 붙는 경우의 BASE_ID/PEER_HELLO 타이밍 -- 아직 미확인
  (지금까지는 한 번에 한 모듈만 붙는 경로만 확인됨).
- `thread_manager_start_as_root()`의 Dataset 재사용 경로를 실물 전원 모듈의 실제 전원
  재投입(power cycle)으로 확인 -- 아직 미확인.
- 다중 홉 Thread 메시 위에서의 CoAP 전송 동작과 UUID-hash backoff의 실제 타이밍 -- 아직
  미확인 (지금까지 확인된 것은 depth=1 단일 홉뿐).
- 전원 모듈의 30초 첫 attach(CHILD/ROUTER/LEADER 중 아무 상태나 충분 -- 더 이상 LEADER
  전용이 아니다, `docs/power-base.md` 19.1/19.7절 참고) 부팅 대기 예산이 더 열악한 무선
  환경에서 CHILD/ROUTER로만 attach된 채로도 충분한지 -- 아직 미확인.
- DETECT_OUT이 실물에서도 전원 모듈이 attach(CHILD/ROUTER/LEADER 중 하나)되기 전에는
  절대 활성화되지 않는지 (더 이상 Leader 전제가 아니다 -- 위 `docs/power-base.md` 19.8절
  참고) -- `power_base/main/app_main.c`의 순서 자체와 그 순서를 mirror한 host test로만
  확인, 실제 GPIO 레벨은 미측정.
- `attach_supervisor_task`가 30초를 넘겨도 halt하지 않고 계속 재시도하는지, rate-limited
  경고가 실물 콘솔에 정상 출력되는지 -- host test만 통과, 실물 미검증.
- JOIN_RESULT/ACK 유실·중복 재시도 경로(ACK가 실제로 유실되는 경우) -- host test
  시나리오만 통과, 아직 실물 미확인.
- Thread 재가입 후 세션 재개 -- 아직 미확인.
- idle PING/PONG liveness 및 `LIVENESS_LOST` 판정의 실물 동작 -- 아직 미확인.
- Deep-sleep, Linux OTBR, Matter Bridge, POGO 5V 공급 -- 이번 세션 범위 밖, 구현 자체가
  없음 (`docs/validation-status.md` 참고).
