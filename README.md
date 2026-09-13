# POGO 기반 모듈형 Thread IoT 통합 펌웨어

요구사항/배경은 `CLAUDE-CODE-BOOTSTRAP.md`(초기 MVP) 및 이 리팩터링의 지시서를 참고.
이 문서는 빌드·설정 이미지 생성·플래시 절차만 다룬다. 아키텍처 설명은
[`docs/architecture.md`](docs/architecture.md), POGO 포트/개발보드 호환 모드는
[`docs/pogo-interface.md`](docs/pogo-interface.md), 고정 설정 스키마는
[`docs/module-config.md`](docs/module-config.md), 상태 머신/오류 처리는
[`docs/state-machine.md`](docs/state-machine.md) 참고.

## 구조

```
module/                   최종 공통 ESP-IDF 프로젝트 -- 모든 모듈이 이 하나의 바이너리를 사용
upper/, lower/             기존 검증용 참조 프로젝트 (회귀 기준, 변경하지 않음)
components/
  pogo_proto/               공용 프레임/CRC16/프로토콜 payload codec
  pogo_port/                양방향 POGO 포트 (UPSTREAM/DOWNSTREAM), 접촉 감지, PEER_HELLO
  role_manager/             ROOT/INTERMEDIATE/LEAF 런타임 판정
  thread_manager/           공통 OpenThread FTD 어댑터 (역할별 link mode 적용)
  child_service/            UPSTREAM 포트 전담 -- 부모에게 응답
  parent_service/           DOWNSTREAM 포트 전담 -- 자식을 프로비저닝
  module_config/            고정 센서/보드 설정 ("cfg" NVS 파티션)
  sensor_provider/          센서 드라이버 인터페이스 + registry + mock
configs/                    설정 이미지 원본 (사람이 수정하는 JSON)
tools/                      설정 이미지 생성 스크립트 + 순수 계산 로직 검증
docs/                       아키텍처/인터페이스/설정/상태머신 문서
```

`upper`/`lower`는 각각 독립된 ESP-IDF 프로젝트이며, `module`과 함께
`EXTRA_COMPONENT_DIRS`로 `components/`를 공유한다.

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

idf.py -C upper -B ~/esp-builds/pogo_upper set-target esp32h2
idf.py -C upper -B ~/esp-builds/pogo_upper build

idf.py -C lower -B ~/esp-builds/pogo_lower set-target esp32h2
idf.py -C lower -B ~/esp-builds/pogo_lower build
```

세 프로젝트 모두 위 명령으로 빌드 성공(경고 없음)을 확인했다 (아래 "빌드 결과" 참고).

## 설정 이미지 생성 (module 전용)

`module`의 애플리케이션 바이너리(`pogo_module.bin`)는 모든 보드에서 동일하다.
보드/센서별로 다른 것은 `cfg` NVS 파티션 이미지뿐이다. 사람이 수정하는 원본은
`configs/*.json`, 생성 도구는 `tools/pack_module_config.py`:

```bash
source ~/.espressif/v6.1/esp-idf/export.sh   # IDF_PATH + nvs_partition_gen.py 제공

bash tools/gen_config.sh build/cfg_images
# -> build/cfg_images/dev_root_config.bin
# -> build/cfg_images/dev_leaf_config.bin
```

자세한 스키마/CRC/개발 오버라이드 설명은 [`docs/module-config.md`](docs/module-config.md).

## 플래시 (사용자가 실행)

포트는 재연결 시 바뀔 수 있으니 `ls /dev/tty.usbmodem*`로 먼저 확인.
현재 확인된 값: lower 역할이었던 보드 `/dev/tty.usbmodem113301`, upper 역할이었던 보드
`/dev/tty.usbmodem113401`. 아래 명령은 모두 프로젝트 루트에서 그대로 실행 가능하다
(설정 이미지 경로도 루트 기준 상대경로로 통일했다 -- 실행 위치가 달라져도 혼동되지 않는다).

### 현재 개발보드 2대를 `module` 하나로 첫 통합 시험 (권장 순서)

**중요**: 기존 upper/lower 참조 프로젝트를 이미 플래시해 본 보드라면, 그 프로젝트들이 쓴
동적 NVS(예전 Thread dataset, 예전 UUID/slot)가 남아있을 수 있다. `module`의 동적 NVS
레이아웃은 그것들과 호환되지 않으므로(키/네임스페이스가 다름) 실제로 문제를 일으키진
않지만, 최초 통합 시험에서는 "정말 새 코드가 스스로 형성/가입한 것"임을 명확히 하기 위해
`erase-flash`부터 시작한다.

```bash
cd "/Users/junsoona/no-icloud/main project_1"
source ~/.espressif/v6.1/esp-idf/export.sh

# 1) 두 보드 모두 완전히 지운다 (포트는 실제 연결에 맞게 확인)
python -m esptool --chip esp32h2 -p /dev/tty.usbmodem113301 erase-flash
python -m esptool --chip esp32h2 -p /dev/tty.usbmodem113401 erase-flash

# 2) 두 보드에 동일한 module 앱/부트로더/파티션 테이블을 플래시
idf.py -C module -B ~/esp-builds/pogo_module -p /dev/tty.usbmodem113301 flash
idf.py -C module -B ~/esp-builds/pogo_module -p /dev/tty.usbmodem113401 flash

# 3) lower 역할 보드에 dev_leaf_config.bin 기록 (경로는 프로젝트 루트 기준)
python -m esptool --chip esp32h2 -p /dev/tty.usbmodem113301 \
    write-flash 0x187000 build/cfg_images/dev_leaf_config.bin

# 4) upper 역할 보드에 dev_root_config.bin 기록
python -m esptool --chip esp32h2 -p /dev/tty.usbmodem113401 \
    write-flash 0x187000 build/cfg_images/dev_root_config.bin

# 5) 두 monitor 연결 (각각 다른 터미널)
idf.py -C module -B ~/esp-builds/pogo_module -p /dev/tty.usbmodem113301 monitor
idf.py -C module -B ~/esp-builds/pogo_module -p /dev/tty.usbmodem113401 monitor

# 6) 두 보드를 reset하여(각 monitor에서 Ctrl+T Ctrl+R, 또는 보드의 RESET 버튼)
#    전체 부팅 로그를 수집한다
```

`build/cfg_images/*.bin`은 위 "설정 이미지 생성" 절의 `bash tools/gen_config.sh build/cfg_images`
로 미리 생성해 둔다. `0x187000`은 `module/partitions.csv`의 `cfg` 파티션 offset (파티션
테이블을 바꾸면 `idf.py -C module -B ~/esp-builds/pogo_module partition-table`로 실제
offset을 다시 확인할 것).

두 번째 시험부터는 앱/설정이 이미 맞게 들어가 있다면 2~6단계만 반복하면 된다 (앱 코드만
바뀌었다면 1번 단계 없이 2번부터, `cfg` 이미지만 바뀌었다면 3~4번만).

### 기존 upper/lower 참조 프로젝트 (회귀 확인용)

```bash
cd "/Users/junsoona/no-icloud/main project_1"
idf.py -C lower -B ~/esp-builds/pogo_lower -p /dev/tty.usbmodem113301 flash monitor
idf.py -C upper -B ~/esp-builds/pogo_upper -p /dev/tty.usbmodem113401 flash monitor
```

`monitor`는 `Ctrl+]`로 종료.

## 배선 (개발보드 호환 모드 -- 물리 배선은 예전 MVP와 동일)

| DEV_ROOT 보드 | DEV_LEAF 보드 |
|---|---|
| GPIO10 (TX) | GPIO11 (RX) |
| GPIO11 (RX) | GPIO10 (TX) |
| GND | GND |

두 보드 모두 각자 USB로 전원 공급 중일 때는 5V/3.3V 선은 연결하지 않는다 (GND·TX·RX만).
최종 하드웨어의 실제 UPSTREAM/DOWNSTREAM POGO 핀 배치는
[`docs/pogo-interface.md`](docs/pogo-interface.md) 참고 (아직 GPIO 확정 전).

## 기대 로그

### `module` (DEV_ROOT 이미지, 기존 upper 역할)

```
I (..) module_config: loaded: schema=1 rev=1 driver_id=1297040203 sensor_bitmap=0x0001 bus=i2c#0 timeout=1000ms
W (..) module_main: DEVELOPMENT OVERRIDE ACTIVE: dev_mode=DEV_ROOT (see docs/pogo-interface.md) -- this image is NOT representative of final hardware behavior
W (..) role_manager: development override: DEV_ROOT -- forcing role=ROOT (see docs/pogo-interface.md)
I (..) module_main: role -> ROOT: starting Thread as Leader/Router candidate
I (..) thread_manager: ROOT: no dataset in NVS -- forming a new Thread network from Kconfig defaults
I (..) module_main: ROOT ready: otDeviceRole=... -- downstream provisioning may now proceed
W (..) pogo_port: port 1: no DETECT_IN configured -- treating as permanently attached (dev compat mode)
I (..) pogo_port: port 1: PEER_READY (peer_type=2)
I (..) parent_service: starting downstream provisioning as ROOT
I (..) parent_service: child identified: mac=... fw=1 sensor_bitmap=0x01
I (..) parent_service: child sensor: bitmap=0x0001 response_time=120ms status=0
I (..) parent_service: polling_period_ms=960 sent (from response_time_ms=120)
I (..) parent_service: dataset accepted (.. bytes) -- waiting for join result (timeout 20000ms)
I (..) parent_service: JOIN OK -- child otDeviceRole=2 uuid_backoff=...ms depth=1 polling=960ms
```

### `module` (DEV_LEAF 이미지, 기존 lower 역할)

```
I (..) module_config: loaded: schema=1 rev=1 driver_id=1297040203 sensor_bitmap=0x0001 bus=i2c#0 timeout=1000ms
W (..) module_main: DEVELOPMENT OVERRIDE ACTIVE: dev_mode=DEV_LEAF (see docs/pogo-interface.md) -- this image is NOT representative of final hardware behavior
W (..) role_manager: development override: DEV_LEAF -- forcing role=LEAF (see docs/pogo-interface.md)
W (..) mock_sensor: mock sensor_provider ACTIVE (development only, not a real driver) -- bitmap=0x0001 delay=120ms timeout=1000ms
W (..) pogo_port: port 0: no DETECT_IN configured -- treating as permanently attached (dev compat mode)
I (..) pogo_port: port 0: PEER_READY (peer_type=2)
I (..) child_service: sensor measurement OK: bitmap=0x0001 response_time=120ms
I (..) child_service: polling_period_ms=960 accepted
I (..) child_service: dataset stored (.. bytes) -- starting Thread join as LEAF
I (..) thread_manager: LEAF link mode: router_eligible=false rx-on-when-idle=false poll_period=960ms
I (..) child_service: Thread join succeeded, otDeviceRole=2
```

`otDeviceRole=2` == `CHILD`; LEAF는 `router_eligible=false`/`rx-on-when-idle=false`로
로그에서 확인된다 (Router로 승격되지 않음). Network Key가 포함된 Dataset 원문과 전체
UUID는 어떤 로그에도 출력되지 않는다.

## 빌드 결과 (런타임 수정 반영 후 재확인)

- `idf.py -C upper -B ~/esp-builds/pogo_upper build` -> 성공 (경고 0개, fullclean 이후 재확인)
- `idf.py -C lower -B ~/esp-builds/pogo_lower build` -> 성공 (경고 0개, fullclean 이후 재확인)
- `idf.py -C module -B ~/esp-builds/pogo_module build` -> 성공 (경고 0개, fullclean 이후 재확인)
- 세 프로젝트 모두 ESP-IDF v6.1 / esp32h2 타겟.
- `bash tools/gen_config.sh build/cfg_images` -> `dev_root_config.bin`, `dev_leaf_config.bin`
  재생성 확인 (각 12288 bytes, `cfg` 파티션 크기와 일치; 새 pack-time 검증 통과).
- `tools/test_polling_calc.c` (호스트 컴파일) -> section 17의 모든 polling 경계값
  (0/49/50/99/100/499/500/1000/1001/10000/10001, mock RT=120→960) 통과.
- `tools/test_peer_hello_handshake.c` (호스트 컴파일, 실제 `pogo_proto` 소스와 링크) ->
  동시 부팅/ROOT 먼저/LEAF 먼저 staggered 부팅/PEER_READY 이후 한쪽 재부팅/분할 프레임 수신
  5개 시나리오, 25개 assertion 전부 통과.
- `git diff --check` -> 공백 오류 없음.

## 아직 실물 보드에서 확인되지 않은 항목

- `module` 프로젝트 자체의 실물 PING/PONG, PEER_HELLO, 프로비저닝, Thread 가입 동작
  (이번 세션은 빌드와 순수 로직 검증까지만 수행했고, `flash`/`monitor`는 실행하지 않았다).
- 기존 upper/lower 조합에서 이미 확인된 항목(하드웨어 도킹 감지, Dataset 전달, SED 동작 등)이
  `module`+`DEV_ROOT`/`DEV_LEAF` 조합에서도 동일하게 재현되는지.
- 최종 두 POGO 포트 + DETECT_IN/OUT + BASE_ID 하드웨어에서의 `pogo_port`의 attach/detach
  디바운스, PEER_HELLO/BASE_ID 교차검증, 동시 양쪽 포트 이벤트 처리.
- 실제 센서 드라이버(아직 미확정)로 `sensor_provider` 인터페이스를 구현했을 때의 동작.
- 여러 DOWNSTREAM 세대(3단 이상 체인)에서의 topology_depth 전달과 INTERMEDIATE 역할 전환.
