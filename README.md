# POGO 기반 모듈형 Thread IoT 통합 펌웨어

요구사항/배경은 `CLAUDE-CODE-BOOTSTRAP.md`(초기 MVP) 및 이 리팩터링의 지시서를 참고.
이 문서는 빌드·설정 이미지 생성·플래시 절차·실물 배선을 다룬다. 아키텍처 설명은
[`docs/architecture.md`](docs/architecture.md), POGO 포트/확정 GPIO 프로필은
[`docs/pogo-interface.md`](docs/pogo-interface.md), 고정 설정 스키마는
[`docs/module-config.md`](docs/module-config.md), 상태 머신/오류 처리는
[`docs/state-machine.md`](docs/state-machine.md), **전원 모듈(전용 ESP32-H2 보드,
시스템의 유일한 Thread ROOT/Leader) 아키텍처·배선·토폴로지 프로토콜은
[`docs/power-base.md`](docs/power-base.md)** 참고.

**중요 (전원 모듈 도입 이후 역할 규칙 변경):** 센서 모듈(`module/`)은 AUTO 모드에서
더 이상 ROOT가 되지 않는다. 시스템의 유일한 ROOT/Leader는 별도 프로젝트
`power_base/`로 구현된 전용 전원 모듈이다. 이 절 아래의 "실물 배선"/"ROOT용 passive
power-base 시험 지그" 관련 옛 서술은 이 변경으로 대체되었다 -- 자세한 내용과 근거는
[`docs/power-base.md`](docs/power-base.md) 참고. **이번 세션에서는 전원 모듈<->센서
모듈 간 새 배선을 아직 실제로 연결하지 않았다** -- 소프트웨어 구현/빌드/host test는
모두 완료했지만 실물 연동 검증은 배선 완료 후 별도로 필요하다 (아래 "네 프로젝트 빌드
결과"와 `docs/power-base.md`의 "아직 실물 하드웨어에서 확인되지 않은 항목" 참고).

**최종 졸업작품 프로토타입은 ESP32-H2 SuperMini 기반 전원 모듈 1대(`power_base/`)와
센서 모듈 N대(`module/`, 현재 보유·검증 대상은 SHT45 1대 + BH1750 1대, 총 2대)로
구성된다.** 별도 PCB를 제작하지 않는다 -- 확정된 GPIO 프로필(POGO UPSTREAM/
DOWNSTREAM UART, DETECT_IN/OUT, BASE_ID, 센서 I2C)이 그대로 이 개발보드들에
적용되고, `dev_mode=AUTO`에서 센서 모듈은 BASE_ID/PEER_HELLO로 **LEAF/INTERMEDIATE**
역할만 실시간으로 판정한다 (ROOT는 전원 모듈 전용이며 AUTO 경로에서 센서 모듈에는
절대 산출되지 않는다 -- `docs/power-base.md` 참고). `configs/dev_root.json`/
`configs/dev_leaf.json`(DETECT/BASE_ID 없는 1-UART 개발모드, module-to-module 전용)은
과거 회귀 시험용으로만 보존한다.

## 구조

```
module/                   센서 모듈 공통 ESP-IDF 프로젝트 -- 모든 센서 모듈이 이 하나의
                          바이너리를 사용. AUTO 모드에서 절대 ROOT가 되지 않는다.
power_base/                전원 모듈 전용 ESP-IDF 프로젝트 (신규) -- 시스템의 유일한
                          Thread ROOT/Leader. module/과 별도 보드/바이너리.
                          자세한 내용은 docs/power-base.md.
upper/, lower/             기존 검증용 참조 프로젝트 (회귀 기준, 변경하지 않음)
components/
  pogo_proto/               공용 프레임/CRC16/프로토콜 payload codec (ASSIGN_PARENT_INFO
                            추가 -- 기존 wire format은 그대로)
  pogo_port/                양방향 POGO 포트 (UPSTREAM/DOWNSTREAM), 접촉 감지, PEER_HELLO,
                            idle PING/PONG liveness
  role_manager/             LEAF/INTERMEDIATE/UNASSIGNED/ROLE_ERROR 런타임 판정
                            (AUTO 모드에서는 ROOT를 절대 산출하지 않음 -- 전원 모듈이
                            시스템의 유일한 ROOT)
  thread_manager/           공통 OpenThread FTD 어댑터 (역할별 link mode 적용,
                            Leader/ROOT 주소 조회) -- module/과 power_base/ 양쪽이 그대로 사용
  child_service/            UPSTREAM 포트 전담 -- 부모에게 응답, JOIN_RESULT 재시도,
                            parent UUID 수신(ASSIGN_PARENT_INFO)
  parent_service/           DOWNSTREAM 포트 전담 -- 자식을 프로비저닝, JOIN_RESULT ACK.
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
  power_base_transport/     전원 모듈의 Thread CoAP 수신측 -- "/s"(센서데이터)+"/t"(토폴로지)
                            (신규)
  node_calc/                polling period / UUID backoff 순수 계산 (공유)
  bh1750/, sht45/           실제 I2C 센서 드라이버
  sgp40/, sgp41/            VOC/NOx 센서 드라이버 (Sensirion Gas Index Algorithm 사용)
  sensirion_gas_index_algorithm/  Sensirion 공식 알고리즘 벤더링 (BSD-3-Clause, 출처는
                            THIRD_PARTY_NOTICE.md)
  sensirion_crc8/           SGP4x용 공용 Sensirion CRC-8
  pms5003/                  PMS5003 프레임 파서 (완성) + 등록 가능한 driver 인터페이스
                            (실물 UART transport는 미확보 -- docs/pogo-interface.md 참고)
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

### 전원 모듈 + 센서 모듈 배치 (신규 -- 새 배선 완료 후에만 실행)

**이번 세션에서는 전원 모듈<->센서 모듈 간 새 배선([`docs/power-base.md`](docs/power-base.md)
4절 참고)을 아직 실제로 연결하지 않았다.** 아래는 배선을 완료한 뒤 실행할 순서이며, 전원
모듈의 시리얼 포트는 아직 확인되지 않았으므로 값을 추측해 고정하지 않는다:

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
"확정 GPIO 프로필" 절 참고. **UPSTREAM DETECT_IN은 GPIO2다** (GPIO13은 보드 내장 LED와
충돌하고, 그 다음 시도했던 GPIO6/7은 ESP32-H2 SuperMini/SiP 패키지에서 아예 외부로
나오지 않아 사용할 수 없다는 것이 뒤늦게 확인되어 GPIO2로 재수정했다 -- 근거와 이력은
docs/pogo-interface.md 참고). 아래는 두 센서 모듈을 module-to-module로 직접 연결해
시험하는 (전원 모듈 없이) 배선이며, **전원 모듈<->첫 센서 모듈 배선표는
[`docs/power-base.md`](docs/power-base.md) 4절**을 참고할 것 (BASE_ID가 전원 모듈
커넥터의 GND 접점이지 GPIO가 아니라는 점, GPIO2/GPIO14가 서로 다른 신호라는 점 등
핵심 주의사항 포함).

### 모듈 간 연결 (A의 DOWNSTREAM -> B의 UPSTREAM)

| A (DOWNSTREAM) | B (UPSTREAM) |
|---|---|
| GPIO10 (TX) | GPIO5 (RX) |
| GPIO11 (RX) | GPIO4 (TX) |
| GPIO22 (DETECT_OUT) | GPIO2 (DETECT_IN) |
| GPIO23 (DETECT_IN) | GPIO12 (DETECT_OUT) |
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
| A UPSTREAM DETECT_IN (GPIO2) | GND |
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

### 전원 모듈 + 센서 모듈 배치 -- 기대 로그 (신규, 새 배선 필요)

**아래는 새 배선을 완료한 뒤 위 "전원 모듈 + 센서 모듈 배치" 절차를 실행했을 때
기대되는 로그다 -- 이번 세션에서 실물로 확인한 것이 아니라, 구현된 코드가 그렇게
동작하도록 작성/host-test된 것이다.**

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

**합격 기준**: `ROLE_TRANSITION_ERROR`/`CONFIG_ERROR` 로그가 없을 것, `power_base_main`이
로그에 `otDeviceRole=4(LEADER)`를 남길 것 (전원 모듈은 **엄격히 LEADER만** 허용 --
`ROUTER`에서 멈추면 30초 뒤 `halt_forever`로 정지하고 downstream 포트를 절대 활성화하지
않는다), `stale_sweep=running`일 것 (`FAILED`면 회귀), `pogo_port: port 1: PEER_READY
(peer_type=2)`가 첫 센서 모듈 도킹 직후 나타날 것, `power_base_transport: topology report
applied: ... depth=1 role=3`(LEAF, 절대 role=1(ROOT) 아님)이 나타날 것.

#### 첫 센서 모듈 (`module`, 예: SHT45, depth=1)

```
I (..) module_config: loaded: schema=1 rev=1 driver_id=1397240885 sensor_bitmap=0x0006 ...
I (..) sht45: SHT45 initialized: addr=0x44 ...
I (..) pogo_port: port 0: PEER_READY (peer_type=1)
I (..) child_service: sensor measurement OK: bitmap=0x0006 response_time=..ms
I (..) child_service: dataset stored (.. bytes) -- starting Thread join as LEAF
I (..) thread_manager: LEAF link mode: router_eligible=false rx-on-when-idle=false poll_period=..ms
I (..) child_service: Thread join succeeded, otDeviceRole=2(CHILD)
I (..) child_service: JOIN_RESULT_ACK received (seq=1) -- provisioning session fully confirmed
I (..) topology_reporter: topology report sent OK (type=0 seq=1 attempt=1/4)
```

**합격 기준**: `module_main: role -> ROOT`가 **절대 나타나지 않을 것** (나타나면 회귀 --
`role_manager.c`의 AUTO 판정 수정이 깨진 것). `peer_type=1`(POWER_BASE)로 PEER_READY될
것. `otDeviceRole=2`(CHILD)로 join 성공할 것. `topology_reporter`가 최소 한 번은 성공
전송(`sent OK`, `type=0`=ATTACHED)할 것.

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
- 기존 17개 host test(15 C + 2 Python) 전부 재통과 + 신규 7개 host test 전부 통과
  (정확한 항목별 통과 수는 최종 보고 참고). `git diff --check` -> 공백 오류 없음.

## 보유 센서 현황

사용자는 PMS5003, BH1750, SGP40, SGP41, SHT45를 모두 실제로 보유하고 있다.

- **BH1750 / SHT45**: 최초 두 보드(보드 A=SHT45, 보드 B=BH1750) 실물 검증 대상으로 예정되어
  있다 -- 위 "실물 배선"/"기대 로그" 절이 이 두 센서 기준이다.
- **SGP40 / SGP41**: 코드(I2C 명령, CRC, 벤더링된 Sensirion Gas Index Algorithm)와 host test는
  모두 완료했지만, 사용자가 보유한 실물 SGP40/SGP41은 이번 세션에서 **배선·실행 검증을
  하지 않았다**. `sensor_provider_register()`로 등록만 되어 있고, `configs/prototype_*.json`
  어느 쪽 보드에도 아직 SGP40/SGP41을 `driver_id`로 지정하지 않았다 (필요하면 세 번째/네 번째
  보드용 config를 SHT45/BH1750 config와 동일한 POGO 포트 블록으로 복사해 만들면 된다).
- **PMS5003**: 사용자가 실물을 보유하고 있으나, ESP32-H2에 UART 컨트롤러가 2개뿐이고 둘 다
  POGO 포트에 이미 배정되어 세 번째 하드웨어 UART가 없다. 프레임 파서와 등록 가능한 driver
  인터페이스는 완성했지만 transport(실제 통신 경로)는 미구현이다 -- 아래 "아직 실물 보드에서
  확인되지 않은 항목" 및 `docs/pogo-interface.md`의 PMS5003 절 참고.

## 아직 실물 보드에서 확인되지 않은 항목

**전원 모듈 관련 항목 전체 (신규, 이번 세션 최우선 미검증 대상)**: 이번 세션에서는
전원 모듈<->센서 모듈 간 새 배선을 아직 실제로 연결하지 않았다. 아래는 소프트웨어
구현/host test/빌드는 완료했지만 실물 검증이 필요한 항목이다 (자세한 목록은
[`docs/power-base.md`](docs/power-base.md) 16절):

- `docs/power-base.md` 4절의 실제 배선표를 실물 전원 모듈 보드 + 실물 첫 센서 모듈
  보드에 적용했을 때의 동작 전체.
- 실물 전원 모듈 MCU와의 BASE_ID/PEER_HELLO 교차검증 (이번 세션은 host-test 모델로만
  검증, 실제 UART/GPIO 타이밍은 미검증).
- `thread_manager_start_as_root()`의 Dataset 재사용 경로를 실물 전원 모듈의 실제
  전원 재投입(power cycle)으로 확인.
- `"/s"`/`"/t"` CoAP 전송이 실물 다중 홉 Thread 메시 위에서 정상 동작하는지 (UUID-hash
  backoff의 실제 타이밍 포함).
- 전원 모듈의 30초 **LEADER(엄격, ROUTER는 더 이상 허용하지 않음)** 부팅 대기 예산이
  실제 무선 환경에서 충분한지.
- topology registry의 detach/reconnect/stale 처리가 실물 물리적 detach(단순 host-test
  아님)에서도 동일하게 동작하는지.
- DETECT_OUT이 실물에서도 Leader 준비 전에는 절대 활성화되지 않는지 (이번 세션은
  `power_base/main/app_main.c`의 순서 자체와 그 순서를 그대로 mirror한 host test로만
  확인 -- 실제 GPIO 레벨은 미측정).

**기존(module-to-module) 항목**:

- 확정 GPIO 프로필(POGO UART, DETECT_IN/OUT, BASE_ID -- **UPSTREAM DETECT_IN=GPIO2**)로
  배선된 실물 두 보드에서의 attach/detach 디바운스, PEER_HELLO/BASE_ID 교차검증.
  (passive power-base 지그를 이용한 ROOT 판정은 위 "실물 배선" 절에서 설명한 대로
  이제 유효하지 않은 시나리오다 -- 대신 전원 모듈 관련 항목 참고.)
- BH1750/SHT45 실물 I2C 통신, 변환값이 실제 조도/온습도와 맞는지.
- SGP40/SGP41: 코드/알고리즘/host test는 완료했으나 실물 배선·실행은 미검증 (위 "보유 센서
  현황" 참고).
- PMS5003: 파서는 완성했으나 실물 UART transport가 없어 센서 자체의 실물 동작은 애초에
  시험할 수 없다 (하드웨어 결정이 먼저 필요 -- docs/pogo-interface.md 참고).
- JOIN_RESULT/ACK 유실·중복 재시도 로직의 실물 동작 (host test 시나리오는 통과).
- 주기적 CoAP 센서 전송, ROOT aggregator 적재, Thread 재가입 후 세션 재개의 실물 동작.
- idle PING/PONG liveness 및 `LIVENESS_LOST` 판정의 실물 동작.
- 여러 DOWNSTREAM 세대(3단 이상 체인)에서의 topology_depth 전달과 INTERMEDIATE 역할 전환.
