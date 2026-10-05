# 검증 현황

이 문서는 프로젝트의 현재 검증 상태를 정리한다. 소스 검토, host test, 실물 시험은 서로
다른 근거이므로 한 항목으로 합쳐 쓰지 않는다.

상태 표기는 다음 기준을 사용한다.

- **실물 확인**: 실제 보드와 실제 전송 경로에서 확인
- **host 확인**: production 소스를 host test에 링크해 확인
- **빌드 확인**: 대상 툴체인으로 컴파일과 링크 확인
- **미확인**: 구현은 있으나 해당 조건의 실물 결과가 없음
- **미구현**: 코드나 회로가 없음

## POGO와 프로비저닝

| 항목 | 코드/host | 실물 | 메모 |
|---|---|---|---|
| `PEER_HELLO`와 capability 확인 | 확인 | 확인 | Power Base, 중간 모듈, 말단 모듈 조합 |
| 영구 module UUID | 확인 | 확인 | 재부팅과 재프로비저닝 뒤에도 같은 UUID 사용 |
| AUTO 역할 판정 | 확인 | 확인 | 센서 모듈은 LEAF 또는 INTERMEDIATE |
| `SET_COLLECTOR` | 확인 | 확인 | Leader RLOC 대신 Power Base Mesh-Local EID 사용 |
| `SET_DATASET`과 Thread join | 확인 | 확인 | STANDALONE과 EXTERNAL_OTBR 경로 모두 사용 |
| `JOIN_RESULT_ACK` 재전송 | 확인 | 확인 | 중복 메시지는 캐시 응답으로 처리 |
| hot-swap 복구 | 확인 | 확인 | timeout/liveness 손실 뒤 cooldown 재시작. 2026-10-03 이미지에서 추가된 오류 격리·cooldown HELLO는 이 행에 포함되지 않는다(아래 "POGO 체인 분리·재연결 실물 시험") |
| topology 등록 전 sample 차단 | 확인 | 확인 | `topology_gate` 적용 |
| 부모 offline일 때 depth>1 보고 거부 | 확인 | 확인 | `PARENT_OFFLINE` 뒤 재등록 |
| POGO depth 2 체인 | 확인 | 확인 | Power Base → INTERMEDIATE → LEAF |
| 중간 링크 분리 후 subtree 복구 | 확인 | 확인 | depth 1, 2 순서로 재등록 후 sample 재개 |
| 제3의 독립 sibling 보존 | 코드 검토 | 미확인 | 현재 물리 구조는 Power Base downstream 포트 1개 |

세 보드 로그 검사는 다음 명령으로 실행한다.

```bash
python3 tools/validate_three_tier_logs.py \
  --base base.log \
  --middle middle.log \
  --leaf leaf.log
```

이 도구는 POGO 체인의 depth와 프로비저닝 결과를 확인한다. Thread 무선 패킷이 중간
모듈을 실제로 릴레이했는지는 증명하지 않는다.

## POGO 체인 분리·재연결 실물 시험 (2026-10-03)

출처: 사용자가 2026-10-03(Asia/Seoul)에 직접 실행한 실물 시험의 터미널 기록(세 보드의 app
업데이트 출력과 `esp_idf_monitor` 로그)을 검토해 적었다. 이 문서를 고친 작업에서는 보드를
다시 시험하지 않았고 시리얼·flash에 접근하지 않았으며 host test와 빌드도 다시 실행하지 않았다.
아래 시각은 monitor가 붙인 호스트 시각이고, 간격은 Power Base 로그의 uptime 값으로 계산했다.

위 표의 "hot-swap 복구", "중간 링크 분리 후 subtree 복구"는 이전 이미지의 기록이다. 이 절은
2026-10-03 이미지로 확인한 범위와 확인하지 못한 범위를 따로 적는다.

### 시험 이미지와 기록 방법

| 보드 | EUI-64 | app 이미지 | 크기 | SHA-256 |
|---|---|---|---:|---|
| Power Base `..c00a` | `744dbdfffe65c00a` | `power_base_ble/power_base.bin` | 1,392,848 | `70040f6c93b9dfb62fdeb5dfc6282eccdd951c477b433e92a45d546b06fe78f0` |
| SGP41 `..d357` | `744dbdfffe65d357` | `module/pogo_module.bin` | 885,408 | `eb9a373b6086f53d16d1e1f972e87c789bcc92f3ad38c47d95e3ac722e6ed0fb` |
| PMS `..d07a` | `744dbdfffe65d07a` | `module/pogo_module.bin` (SGP41과 같은 파일) | 885,408 | `eb9a373b6086f53d16d1e1f972e87c789bcc92f3ad38c47d95e3ac722e6ed0fb` |

- manifest는 `$HOME/esp-builds/pogo_hwtest_20261003/release/SHA256SUMS`다. 업데이트 출력의
  `Release files verified ... SHA-256` 값이 manifest와 같고, `release/`의 파일 6개를 다시 해시한
  값도 manifest와 같다.
- 이 이미지는 HEAD만으로 식별되지 않는다. `feature/unified-module-role-manager`의 HEAD
  `431b280`에 미커밋 작업 트리를 더해 빌드했다(ESP-IDF v6.1, esp32h2). 빌드 직전에 남긴 미커밋
  파일 103개의 체크섬(`baseline/checksums_before.txt`)은 이 문서를 고치기 직전의 작업 트리와 모두
  같았다. 이미지는 위 SHA-256으로 식별한다.
- Power Base 이미지는 BLE 등록 시험 구성(`sdkconfig.ble_enrollment`)이다. monitor를
  `--no-reset`으로 붙여 부팅 로그가 없으므로, 이 기록은 등록·Joiner·BLE 경로의 근거가 아니다.
- 업데이트는 보드마다 eFuse EUI-64가 기대값과 같은지, 보드의 파티션 표가 일치하는지 확인한 뒤
  app 영역 `0x10000`에만 기록하고 검증했다(`Verification successful (digest matched)`).
  지우고 쓴 범위는 Power Base `0x10000`~`0x164fff`, 모듈 `0x10000`~`0xe8fff`다. cfg, nvs, 등록
  정보 영역은 초기화하지 않았다. 보드에 들어 있는 설정 내용을 읽어 확인한 것은 아니다.
- 구성은 Power Base(`..c00a`) → SGP41(`..d357`, depth 1) → PMS(`..d07a`, depth 2)다. PMS는 핀
  배열이 달라 Power Base에 직접 연결하는 시험은 하지 않았다.
- 세 로그 모두 uptime이 끝까지 단조 증가하고 부팅 로그가 없다. 기록 구간(17:59:40~18:09:45)에
  재부팅은 없었다.

### 결과

Power Base의 `sample recorded` 줄을 기준으로 센 값이다.

| 시나리오 | 시각 | SGP41 `..d357` (4채널) | PMS `..d07a` (3채널) |
|---|---|---|---|
| A. 정상 수신 | 17:59:40~18:03:39 | seq 64→282, 누락 없음 | seq 17→78, 누락 없음 |
| B. 하위 PMS 분리·재연결 | 18:05:43 분리, 18:06:11 재연결 | 누락 없음. 분리·재연결 때 각각 약 4초 간격 | seq 110 뒤 18:06:14에 seq 118로 재개 |
| C. 상위 Base–SGP41 분리·재연결 | 18:08:47 분리, 18:09:28~29 재연결 | seq 551 뒤 18:09:30에 seq 593으로 재개 | seq 164 뒤 18:09:31에 seq 181로 재개 |

A. 정상 수신

- 약 4분 동안 두 센서의 sample을 받았다. 수신 간격은 SGP41 최대 1.13초, PMS 최대 4.43초였다.
- 시나리오 B 직전까지도 이어졌다(SGP41 seq 395, PMS seq 110까지 누락 없음).
- 18:04:43에 Power Base의 OpenThread가 포트 5684로 가는 UDP 전송 1건의 `NoAck`를 기록했다.
  그 전후의 sample 순번은 이어졌다. 그 전송의 재시도 결과는 이 로그로 알 수 없다.

B. 하위 PMS 분리·재연결

- 18:05:43 PMS는 `LEAF -> UNASSIGNED`로 Thread를 중지했고, SGP41은 `INTERMEDIATE -> LEAF`가 됐다.
  Power Base는 SGP41의 보고로 `..d07a`를 제거했고, 제거 요청에 Linux가 2.04로 답했다.
- SGP41 sample 순번은 누락 없이 이어졌다(395→396, 412→413). 무중단은 아니다. LEAF ↔
  INTERMEDIATE 전환 때마다 SGP41이 link mode를 재적용하면서 Thread를 다시 붙였고, 수신 간격이
  분리 때 3.65초, 재연결 때 3.95초로 벌어졌다(평소 약 1.1초).
- 18:06:11 재연결(PMS `PEER_READY`, SGP41 `LEAF -> INTERMEDIATE`), 18:06:13 PMS Thread 가입과
  `JOIN_RESULT_ACK`, Power Base의 `..d07a` depth 2 등록, 18:06:14 PMS sample 재개.
  PMS의 seq 111~117은 분리 중이어서 Power Base에 오지 않았다.
- 이때 남은 경고: SGP41 `parent_service`의 재시도(`IDENTIFY_REQ` 1회, `SENSOR_HELLO` 2회),
  PMS의 중복 요청 캐시 응답 2회, Power Base의 `Failed to process Parent Request - Duplicated` 1회.
  모두 재시도 한도 안에서 끝났다. PMS 자신의 분리 보고는 Thread 중지 뒤라 전달되지 않았다
  (`delivery unconfirmed after 4 attempts`).

C. 상위 Base–SGP41 분리·재연결

- 18:08:47 SGP41은 `INTERMEDIATE -> UNASSIGNED`로 Thread를 중지했다. Power Base는 `..d357`과
  그 아래 `..d07a`를 함께 제거했고, 두 제거 요청 모두 Linux가 2.04로 답했다(미전달 0).
- PMS는 SGP41에 붙어 있는 채로 남았다. 18:08:48에 보낸 sample 1건은 Power Base가 버렸고
  (`unknown module`, PMS 쪽 `CoAP 4.04`), PMS는 전송 gate를 닫고 재등록을 시도했다.
- 분리 중 경고가 반복됐다. 18:08:49~18:09:30에 Power Base의
  `topology report rejected ... reason=UNKNOWN_PARENT` 55회와 PMS의 `CoAP 4.12` 55회,
  `delivery unconfirmed after 4 attempts`, `still not registered` 7회다. 부모가 등록되지 않은
  동안의 거부 동작이지만 경고 없이 복구된 것은 아니다. SGP41 자신의 분리 보고도 전달되지 않았다.
- 재연결은 SGP41이 18:09:28에, Power Base가 18:09:29에 감지했다. Power Base는 DETECT ISR이 아니라
  주기적 재확인으로 감지했다(`DETECT_IN reconciliation mismatch ... detect_isr_drops=7`).
- 18:09:30 SGP41이 ROUTER로 가입해 `JOIN_RESULT_ACK`를 받았고 Power Base가 `..d357` depth 1,
  `..d07a` depth 2를 다시 등록했다. sample은 SGP41 18:09:30, PMS 18:09:31에 재개됐고 로그 끝
  (18:09:45, SGP41 seq 606, PMS seq 185)까지 누락 없이 이어졌다. 분리 중의 순번은 오지 않았다.
- 재부팅 없이 복구됐다. 사용자 등록 조작도 없었다(사용자 보고. 로그에도 등록·Joiner 관련 줄이
  없다).
- 그 밖의 경고: Power Base `parent_service`의 `IDENTIFY_REQ` 재시도 1회, SGP41의
  `Failed to process Link Accept - Security` 1회. Thread 중지 직후의
  `process_thread_receive failed: ESP_FAIL`은 B에서 PMS 4회, C에서 SGP41 2회 나왔다.

### 검증 경계

| 항목 | 상태 | 메모 |
|---|---|---|
| 정상 수신, 하위 분리·재연결, 상위 분리·재연결(물리 분리) | 실물 확인 | 위 이미지, 로그 검토 |
| Linux 쪽 반영 | 제거 요청의 2.04 응답까지 | 앱 표시, Linux snapshot, Matter endpoint의 값은 이 로그로 확인하지 않았다 |
| DOWNSTREAM 일시 오류 격리와 cooldown 복구 (DETECT는 연결된 채 UART만 단절) | 실물 미확인 | 신호선만 분리하기 어려워 보류. 세 로그에 `DOCK_TIMEOUT`, `LIVENESS_LOST`, cooldown, 격리 로그가 0건이라 이 경로는 실행되지 않았다. host: `tools/test_role_manager_downstream_isolation.c`, `tools/test_module_downstream_fault_isolation.c` |
| cooldown 중 `PEER_HELLO_REQ` 응답 (시차 handshake, 한 방향 단절의 자동 복구) | host 확인만 | `tools/test_pogo_port_cooldown_hello.c`. 실물 미확인 |
| 오래된 Thread role event로 가입을 잘못 판정하지 않음 | host에서 재현·확인 | `tools/test_child_service_stale_role_events.c`. 실물에서는 정상 가입 순서만 관측했고 오래된 event를 강제로 넣지 않았다 |
| link mode 재적용 조건 | host 확인 | `tools/test_child_service_link_mode_reapply.c`. 실물에서는 LEAF ↔ INTERMEDIATE 전환 때의 재적용과 약 4초 간격만 관측 |
| 최초 `PEER_HELLO` 실패의 현장 원인 | 미확정 | 이번 기록에는 `PEER_HELLO` 실패가 없다. 첫 요청 재시도(`IDENTIFY_REQ`, `SENSOR_HELLO`)의 원인도 확인하지 않았다 |
| 보드 cfg·nvs의 내용 | 미확인 | app 영역만 기록했고 설정을 읽지 않았다 |

host 항목은 해당 변경 작업에서 통과했다고 보고된 결과다. 이 문서를 고친 작업에서는 다시
실행하지 않았다. 동작 규칙은 [`state-machine.md`](state-machine.md)의 pogo_port, role_manager,
SET_DATASET 절에 있다.

## 센서

| 센서 | 설정 | host test | 실물 결과 | Matter |
|---|---|---|---|---|
| SHT45 | `prototype_sht45.json` | 변환·드라이버 확인 | 온도/습도 수신 확인 | Temperature/Humidity 구현 |
| BH1750 | `prototype_bh1750.json` | 변환·드라이버 확인 | 조도 변화와 Linux 수신 확인 | endpoint 3에서 Illuminance 0~22369 확인 |
| SGP40 | `prototype_sgp40.json` | 드라이버·CRC·Gas Index 확인 | VOC RAW/INDEX Linux 수신 확인 | 의도적으로 매핑하지 않음 |
| SGP41 | `prototype_sgp41.json` | 드라이버·CRC·conditioning 확인 | VOC/NOx Linux 수신 확인 | 의도적으로 매핑하지 않음 |
| PMS5003 | `prototype_pms5003.json` | parser·드라이버 확인 | PM1/PM2.5/PM10 수신 확인 | PM 클러스터와 Air Quality 확인 |

### I2C 측정 timeout 수정

SHT45, BH1750, SGP40, SGP41의 `read_measurement()`에서 tick budget이 끝났을 때
`measuring=false`와 `SENSOR_STATUS_TIMEOUT`을 설정하지 않던 같은 버그를 수정했다.
각 드라이버를 실제로 링크한 host test로 timeout 상태 전이를 확인했다.

- `tools/test_sht45_driver.c`: 18/18
- `tools/test_bh1750_driver.c`: 18/18
- `tools/test_sgp40_driver.c`: 17/17
- `tools/test_sgp41_driver.c`: 18/18

실물 시험에서는 센서 초기화와 정상 측정 경로를 확인했다. timeout 분기가 실제 보드에서
발생한 상황은 따로 재현하지 않았다.

### SGP41 conditioning

실물 SGP41 모듈 `..d357`이 depth 1 INTERMEDIATE로 가입했고, PMS5003 `..d07a`가
그 아래 depth 2로 다시 등록됐다. Linux ingest에서는 부팅 직후 VOC 채널이
`status=7`을 보인 뒤 약 10초 후 VOC_RAW, VOC_INDEX, NOX_RAW, NOX_INDEX가
`status=0`으로 전환됐다. 이 결과는 conditioning 동작과 Linux 수신까지 확인한 것이다.
VOC/NOx의 Matter 표시를 확인한 결과는 아니다.

## Thread 무선 경로

POGO depth와 Thread radio hop은 별개다. 이를 구분하기 위해 역할 변경 시 자신의 Extended
Address와 `otDeviceRole`을 기록하고, LEAF가 실제 `/s` 요청을 만들 때 현재 parent의
Extended Address와 해당 `sample_seq`를 기록한다.

실물 시험에서 두 경로를 확인했다.

1. 첫 연결의 LEAF sample은 OTBR를 직접 parent로 사용했다.
2. LEAF 분리·재연결 뒤 sample은 Power Base를 직접 parent로 사용했다.

두 경우 모두 INTERMEDIATE가 parent로 선택되지 않았다. 데이터는 정상적으로 Linux에
도착했으며 OpenThread가 선택한 정상 라우팅 결과다. 따라서 현재 확인한 것은 다음과 같다.

- POGO depth 2: **실물 확인**
- parent 진단 로그와 sample 연결: **실물 확인**
- INTERMEDIATE를 통한 실제 무선 multi-hop: **미확인**

무선 multi-hop을 확정하려면 LEAF와 OTBR/Power Base의 직접 RF 도달을 제한하거나 패킷을
직접 캡처해야 한다.

## Power Base와 Dataset

| 항목 | host/빌드 | 실물 | 메모 |
|---|---|---|---|
| STANDALONE Dataset | 확인 | 확인 | Power Base가 네트워크 형성 |
| EXTERNAL_OTBR Dataset | 확인 | 확인 | OTBR Active Dataset으로 기존 네트워크 가입 |
| 잘못된 external 설정 fail-closed | 확인 | 미확인 | standalone fallback 없음 |
| Dataset 비밀 미출력 | 정적 확인 | 확인 | 로그에는 mode와 길이만 표시 |
| CHILD/ROUTER/LEADER에서 서비스 1회 시작 | 확인 | 확인 | attach supervisor 사용 |
| DETACHED 상태에서 provisioning 차단 | 확인 | 확인 | 재attach 뒤 자동 복구 |

실물 EXTERNAL_OTBR 시험에서 OTBR와 Power Base의 Partition ID가 일치했고,
Power Base → SHT45 → PMS5003 체인이 depth 1/2로 등록되어 두 센서의 데이터가 수신됐다.

## SED와 CoAP 응답 시간

센서 측정 주기와 Thread SED data poll 주기를 분리했다.

- 센서 측정 주기: POGO `POLL_CONFIG` 값 유지
- SED data poll: 기본 500ms
- CoAP ACK timeout: 2000ms
- 기본 응답 대기: 5000ms

관련 host test와 `module`, `power_base` 빌드는 통과했다. 초기 실물 로그에서 sample은
Power Base에 저장됐지만 LEAF가 ACK를 늦게 받아 전송 실패로 표시한 현상을 기준으로 수정했다.
수정 뒤 같은 타이밍 조건을 강제로 재현하는 장시간 실물 시험은 따로 기록하지 않았다.

## Linux northbound bridge

Power Base의 `linux_bridge_exporter`는 SAMPLE, TOPOLOGY, HEARTBEAT를 CoAP Confirmable
POST로 Linux ingest에 보낸다.

확인한 동작:

- OT callback은 queue에 0-tick으로 넣고 즉시 반환
- SAMPLE은 UUID별 최신값으로 coalescing
- 기본 publish 간격은 UUID별 1000ms
- TOPOLOGY는 sample rate limit과 분리
- 2.xx는 성공, 4.xx는 재시도하지 않음, timeout/5.xx는 제한된 재시도
- 모든 attempt 직전에 aggregator의 최신 sample을 다시 읽음
- transient retry 소진 뒤 pending으로 돌려 다음 허용 시점에 재전송
- Linux registry는 topology가 online이고 bitmap이 맞는 sample만 적용
- 오래된 generation/sequence는 거부
- ACK source 주소를 요청 목적지 주소로 고정
- 수신 interface를 `IPV6_RECVPKTINFO`로 확인

host test와 Linux loopback test를 통과했다. 실제 Power Base에서 Linux Matter Bridge까지
SHT45, BH1750, SGP40, SGP41, PMS5003 데이터를 수신했다. retry 소진이나 queue overflow를
실물에서 의도적으로 만든 시험은 없다.

### 여러 Power Base (2026-09-23)

| 항목 | 코드/host | 실물 | 메모 |
|---|---|---|---|
| 한 ingest 인스턴스가 두 Base의 CoAP를 각자 수신 | host 확인 | 미확인 | UDP loopback, 서로 다른 source port |
| HEARTBEAT만으로 Base 표시, 반복 heartbeat는 generation 불변 | host 확인 | 미확인 | |
| 같은 종류 센서가 있는 두 Base의 모듈 분리 | host 확인 | 미확인 | 다른 Base의 모듈로 가는 sample은 4.04 |
| 한 Base만 stale → Base·모듈 offline, 다른 Base 유지 | host 확인 | 미확인 | background sweep, SSE 이벤트 |
| stale 뒤 변경 없는 resync로 online 복구 | host 확인 | 미확인 | 이전에는 새 seq 전까지 offline 유지 |
| tombstone을 Base 단위로 적용 | host 확인 | 미확인 | Base를 옮긴 모듈 보존 |
| 옮긴 모듈을 API에 한 번만 표시 | host 확인 | 미확인 | online 우선, 다음 `attach_order` |
| 단일 Base JSON 호환 | host 확인 | 미확인 | 추가 필드를 뺀 결과가 이전 출력과 바이트 단위로 같음 |
| 두 번째 Power Base 실물 가입 | - | 미확인 | 두 번째 보드 시험 전 |
| Matter endpoint(두 Base 모듈 동시) | host 확인(기존) | 미확인 | `BridgeManager` 변경 없음 |

### Bonjour 허브 광고와 hub_id (2026-09-27)

| 항목 | 코드/host | 실물 | 메모 |
|---|---|---|---|
| `hub_id` 최초 생성·재시작 유지·손상 파일 거부 | host 확인 | 미확인 | `--state-dir/hub_id`, 0600, link(2)로 원자적 생성 |
| `/api/v1/health`의 `hub_id` 추가 | host 확인 | 미확인 | 기존 필드 뒤에만 추가, hub_id가 없으면 이전 출력과 동일 |
| `_jogakbo._tcp` 등록(API 인터페이스·실제 포트·TXT) | host 확인(가짜 Avahi) | 미확인 | `txtvers`, `api`, `path`, `hub_id` |
| API 불가·주소 사라짐 → 광고 철회, 복구 → 재광고 | host 확인(가짜 Avahi) | 미확인 | 5초마다 재확인 |
| avahi-daemon 부재·재시작, 호스트 이름 재등록 | host 확인(가짜 Avahi) | 미확인 | `AVAHI_CLIENT_NO_FAIL` |
| 서비스 이름 충돌(LAN·같은 호스트) | host 확인(가짜 Avahi) | 미확인 | avahi 대체 이름, hub_id는 그대로 |
| 종료 시 광고 해제, 스레드 join | host 확인(가짜 Avahi) | 미확인 | 비정상 종료는 D-Bus 연결 종료로 daemon이 제거 |
| `libavahi-client.so.3` dlopen과 실제 mDNS 패킷 | - | 미확인 | Mac에서는 라이브러리가 없어 확인 불가 |
| CHIP 링크 빌드(`pogo_matter_bridge`) | stub 헤더로 문법 확인 | 미확인 | Linux 호스트에서 `tools/build_matter_bridge.sh` 필요 |
| iPhone·iPad·Mac의 실제 허브 검색 | 앱 단위 테스트(가짜 브라우저) | 미확인 | 로컬 네트워크 권한 포함 |

### 허브 기반 집 설정 공유 (2026-09-28)

집 이름, 방(ID·이름·아이콘·순서), 조각보 센서 모듈의 이름과 방 배치를 허브의
`<--state-dir>/pogohome/`에 저장하고 페어링한 기기끼리 공유한다. 설정 경로는 API 포트의 TLS에서만
응답한다. 자세한 규약은 [`pogohome-api-v1.md`](pogohome-api-v1.md) "Home settings".

| 항목 | 코드/host | 실물 | 메모 |
|---|---|---|---|
| 서버 revision, 기준 revision 불일치 시 409와 현재 설정 반환 | host 확인 | 미확인 | 첫 업로드는 설정이 없을 때만 |
| 원자적 저장(tmp+fsync+rename), 저장 실패 시 500·기존 설정 유지 | host 확인(쓰기 실패 주입) | 미확인 | |
| 손상 파일(잘린 JSON, 스키마, 잘못된 방 참조) → 503, 파일 그대로 | host 확인 | 미확인 | 센서 API·Matter 영향 없음 |
| 재시작 후 설정·revision·TLS 인증서 유지 | host 확인 | 미확인 | |
| 입력 검증(중복 이름·ID, 방 참조, 길이, 필드, 64방/256모듈) | host 확인 | 미확인 | 413(256 KiB 초과), 411(chunked), 422 |
| 같은 포트 평문/TLS 판별, 설정 경로 평문 거부 | host 확인 | 미확인 | 평문 센서 API 출력은 이전과 바이트 단위로 같음(설정 공유 끔) |
| 페어링 코드(10분, 1회, 5회 실패 폐기), 증명값, 인증서 바인딩 | host 확인 | 미확인 | 앱과 같은 test vector |
| 토큰 인증, 철회(`pogohome-hubctl revoke`) 즉시 반영, 자기 해제 | host 확인 | 미확인 | devices.json에는 토큰 해시만 |
| SSE가 설정 revision 변경을 알림 | host 확인 | 미확인 | registry 변경 없이도 1초 이내 |
| 앱 URLSession ↔ 실제 서버(OpenSSL) 페어링·업로드·충돌·핀 불일치·해제 | Mac loopback 확인 | 미확인 | iOS 시뮬레이터, `HubSettingsLiveNetworkTests` |
| macOS clang 빌드와 host test | 확인 | - | `-Wall -Wextra -Wshadow -Wconversion -Werror` 문법 검사 포함 |
| Linux gcc + libssl-dev 빌드, CHIP 링크(`pogo_matter_bridge`, `pogohome-hubctl`) | stub 헤더로 main.cpp 문법 확인 | 미확인 | Linux 호스트에서 `tools/build_matter_bridge.sh` 필요 |
| 실제 허브의 iPhone·iPad·Mac 두 대 이상 공유 | 앱 단위 테스트(가짜 허브) | 미확인 | |

## Power Base 앱 등록(mode 2) (2026-09-30)

범위: `pb_thread:mode=2` 여분 보드가 USB-Serial/JTAG 콘솔로 받은 PSKd로 Joiner 가입 →
attach → 등록 완료 표식 → 기존 exporter heartbeat. 시험 빌드(`power_base/sdkconfig.bench_joiner`)
전용이며 BLE·QR 제품 경로가 아니다. 규칙은 [`power-base.md`](power-base.md) "앱 등록 대기
상태와 Joiner 가입". BLE 전달 경로는 아래 "Power Base BLE 등록 전달 경로"에 따로 적는다.

### host test와 빌드 (Mac, ESP-IDF v6.1)

| 항목 | 결과 | 근거 |
|---|---|---|
| PSKd 형식, 버퍼 삭제, 로그에 PSKd 없음 | 확인 | `tools/test_power_base_enrollment.c`(실제 컴포넌트 + NVS simulator, 97개 검사) |
| 표식 없음/손상/다른 EUI-64/Dataset 없음·부분·다른 네트워크 → 대기, 유효 표식+같은 Dataset만 재가입 | 확인 | 같은 test. 과거 STANDALONE Dataset은 재개하지 않음 |
| Joiner 실패(security/not found/timeout/기타/guard), 취소, 이전 시도 결과 무시 | 확인 | 같은 test |
| Joiner 성공 ≠ 등록: attach+완전 Dataset+표식 commit 뒤에만 서비스 허용, attach 시간 초과·표식 기록 실패 시 Thread 정지 | 확인 | 같은 test |
| 중간 전원 차단(Joiner 뒤, attach 뒤 표식 전, 표식 뒤, 새 시도 중) 후 부팅 판정 | 확인 | 같은 test, fork한 부팅 + 파일 기반 simulated flash |
| 위 test가 규칙 위반을 잡는지 | 확인 | 규칙 6가지를 일부러 깨뜨린 변형 모두 실패 검출(수동 mutation) |
| mode 2 준비 이미지: Dataset 없음, Bridge 주소·포트 포함 가능, Dataset 파일과 조합 거부, mode 0/1 호환 | 확인 | `tools/test_pack_power_base_await_config.py`(실제 생성기 + ESP-IDF nvs_parser, 18개) |
| mode 2 경로 배선(auto_start 미사용, 대기 분기에서 서비스 미시작, Joiner 콜백 무잠금, 기본 빌드에 Joiner·콘솔·OT CLI 없음) | 정적 확인 | `tools/test_power_base_await_commissioning_paths.py` |
| 기존 host test(thread config 46, boot sequencing 128, bridge config 42, 구조 검사 3종) | 확인 | 변경 후 재실행 |
| ESP-IDF v6.1 `power_base` 기본 빌드 | 확인 | 경고 없음. 이미지에 `pb_join`, `otJoinerStart`, 콘솔 REPL, OT CLI 없음 |
| ESP-IDF v6.1 `power_base` 시험 빌드 | 확인 | `0xf1940` bytes, app 파티션 36% 여유. `pb_join`/Joiner 포함, OT CLI 없음 |
| ESP-IDF v6.1 `module` 빌드(공유 `thread_manager`) | 확인 | 경고 없음 |
| Linux API/ingest CMake test | 확인(macOS clang) | `pogohome_api_unit_tests` 0 failures, ingest 7/7. Linux gcc는 미확인(2026-09-30 기록) |
| `pogo_matter_bridge` stdout 줄 버퍼링(`app/chip/main.cpp`) | 미빌드 | CHIP SDK가 필요. 한 줄 변경. 운영 바이너리에는 미반영(2026-09-30 기록. 이후의 배포 상태는 아래 "Linux 등록 API 배포 후속 기록") |

기존 문제: `tools/test_conditioning_provisioning.c`는 이번 변경 전(HEAD)에도 stub
`openthread/thread.h`에 `otInstance`가 없어 컴파일되지 않는다. 이번 작업과 무관해 고치지 않았다.

### 실물 (여분 보드 1대, USB 콘솔 경로) (2026-10-01 기록)

출처: 사용자가 실물에서 확인했다고 보고한 내용이다. 이 문서를 고친 작업에서는 보드·허브의
로그 원문을 직접 보지 않았다. 대상은 여분 ESP32-H2 1대(EUI-64 `744dbdfffe65c00a`, 실제 flash
4MB), 시험 빌드(`sdkconfig.bench_joiner`), PSKd 전달 수단은 USB 콘솔의 `pb_join start`다.

| 항목 | 상태 |
|---|---|
| iPhone 앱 DEBUG 등록 시험이 만든 PSKd를 실제 `power_base` 시험 펌웨어의 `pb_join start`에 입력 | 실물 확인(사용자 보고) |
| Joiner 성공 → Thread CHILD 접속 → 완전한 Dataset 확보 → 등록 완료 표식 저장 | 실물 확인(사용자 보고) |
| Linux 허브에서 해당 Base `online=true` | 실물 확인(사용자 보고) |
| 앱의 등록 성공 표시 | 사용자가 확인했다고 보고 |
| USB 전원 재연결 후 `last=resumed`, `resumed=yes` | 실물 확인(사용자 보고) |
| CHILD → ROUTER 전환, 그 뒤 Linux 조회에서 `online=true` | 실물 확인(사용자 보고) |
| 여분 보드 Joiner EUI-64 == eFuse UUID | 별도 보고 없음. 펌웨어는 다르면 Joiner를 거부하므로 Joiner가 시작된 것과 모순되지 않는다 |
| 잘못된 PSKd / 세션 없음 / OTBR 부재에서 앱 세션 `expired`, 보드 대기 복귀 | 미확인 |
| 운영 Linux journal에서 등록 진행 로그 즉시 표시 | 미확인(2026-10-01 기록 당시 수정된 bridge 미배포. 이후 상태는 아래 "Linux 등록 API 배포 후속 기록") |

이 결과는 **USB 콘솔 등록 경로**의 검증이다. BLE 전달, QR, 센서 측정값 전달(이 Base에 모듈을
도킹해 sample이 허브까지 가는 경로)의 검증으로 넓혀 쓰지 않는다. BLE 전달의 실물 결과는 아래
"BLE 등록 실물 후속 검증 (2026-10-02)"에 따로 적는다.

### Linux 등록 API 배포 후속 기록 (사용자 보고, 2026-10-03 기록)

출처: 사용자가 Linux 허브에서 직접 실행해 확인했다고 보고한 결과다. 이 문서를 고친 작업에서는 그
출력 원문을 보지 않았고 허브에 접속하지 않았다. 실행 일시는 보고에 없다.

| 항목 | 상태 |
|---|---|
| 등록 API를 포함한 bridge를 배포한 뒤 서비스 active | 사용자 보고 |
| 서비스 실행 인자에 `--api-enroll-helper` 포함 | 사용자 보고 |
| health 응답의 `enrollment: available` | 사용자 보고 |
| helper가 추가 인자를 차단 | 사용자 보고 |
| helper로 Commissioner 시작, Joiner 추가, Joiner 제거 성공 | 사용자 보고 |
| 평문 HTTP 등록 요청 → 403 | 사용자 보고 |
| 미인증 HTTPS 등록 요청 → 401 | 사용자 보고 |

이로써 운영 허브에 등록 API가 배포돼 동작한다는 기록이 생겼다. 다음은 이 보고만으로 확정하지 않는다.

- Linux gcc 빌드와 CHIP 링크의 상세 결과. 실행 중인 바이너리가 있으므로 Linux 호스트에서 빌드가
  만들어진 것은 맞지만, 경고 유무, `pogohome_api_unit_tests`·ingest test의 Linux 실행 결과, 어느 작업
  트리로 빌드했는지는 보고에 없다. 위 표와 "Bonjour 허브 광고", "허브 기반 집 설정 공유" 표의
  Linux 빌드 "미확인"은 각 날짜의 기록으로 그대로 둔다.
- stdout 줄 버퍼링 수정(`app/chip/main.cpp`)이 배포된 바이너리에 들어갔는지. "운영 Linux journal에서
  등록 진행 로그 즉시 표시"는 미확인 그대로다.
- 인증된 HTTPS 요청으로 세션을 열어 `heartbeat_received`까지 가는 구간. 위 일곱 항목에는 없다.
  2026-10-01 USB 콘솔 시험과 2026-10-02 BLE 시험에서 보드의 Joiner가 성공했으므로 그때 허브가 이
  보드의 Joiner를 허용하고 있었던 것은 맞다. 그 허용이 이 배포본의 등록 API로 만들어졌는지는 배포
  일시가 없어 기록으로 이어지지 않는다.

### 실물 시험 절차 (여분 보드만)

운영 Base와 운영 Linux 서비스는 바꾸거나 재시작하지 않는다. Linux 쪽 명령은 모두 읽기
전용이다. PSKd, Dataset, Network Key는 어떤 명령에도 인자로 넣지 않는다. 앱이 등록 세션을
열려면 허브에 `--api-enroll-helper`가 이미 설정돼 있어야 한다([`pogohome-api-v1.md`](pogohome-api-v1.md)
"Installing the OTBR helper"). 없으면 이 절차를 멈춘다.

```bash
# 0) Linux OTBR 호스트 (읽기 전용)
systemctl show -p ExecStart pogo-matter-bridge.service | tr ' ' '\n' | grep -E -- '--api-enroll-helper|--api-port|--port'
sudo ot-ctl ipaddr mleid              # 출력된 주소를 Mac의 OTBR_MLEID로 쓴다
curl -s http://<LAN-address>:8787/api/v1/snapshot | jq -r '.power_bases[] | "\(.uuid) online=\(.online)"'
                                      # 운영 Base의 uuid를 적어 둔다

# 1) Mac: 여분 보드 식별 (운영 Base USB는 뽑아 두는 것을 권장)
ls /dev/cu.usbmodem*
SPARE_PORT=/dev/cu.usbmodemXXXX       # 여분 보드 포트로 바꾼다
python -m esptool --chip esp32h2 -p "$SPARE_PORT" read-mac
# 출력의 8바이트 MAC(EUI-64)이 위 운영 Base uuid와 같으면 즉시 중단한다.

# (권장) 여분 보드 전체 백업. Thread 비밀이 들어 있을 수 있으니 0700 디렉터리에 둔다.
# 크기는 flash-id가 알려 주는 실제 값을 쓴다. 여분 보드 744dbdfffe65c00a는 4MB(0x400000)다.
# 2MB(0x200000)만 읽으면 4MB 보드의 전체 백업이 아니다.
mkdir -p "$HOME/pogo-spare-backups" && chmod 700 "$HOME/pogo-spare-backups"
python -m esptool --chip esp32h2 -p "$SPARE_PORT" flash-id          # "Detected flash size" 확인
FLASH_BYTES=0x400000                                                # flash-id 결과에 맞춘다
(umask 077; python -m esptool --chip esp32h2 -p "$SPARE_PORT" read-flash 0 "$FLASH_BYTES" \
  "$HOME/pogo-spare-backups/spare-before-mode2.bin")

# 2) Mac: 시험 빌드와 준비 이미지 (보드에 쓰지 않음)
cd "/Users/junsoona/no-icloud/main project_1"
source ~/.espressif/v6.1/esp-idf/export.sh
export IDF_COMPONENT_MANAGER=0
BENCH_BUILD="$HOME/esp-builds/pogo_power_base_bench_joiner"
idf.py -C power_base -B "$BENCH_BUILD" -D SDKCONFIG="$BENCH_BUILD/sdkconfig" \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.bench_joiner" build
grep -cE '^CONFIG_(POWER_BASE_BENCH_JOINER_CONSOLE|OPENTHREAD_JOINER)=y' "$BENCH_BUILD/sdkconfig"   # 2
OTBR_MLEID='<0)에서 확인한 주소>'
bash tools/gen_power_base_await_config.sh build/cfg_images "$OTBR_MLEID" 5684   # --port가 다르면 그 값

# 3) Mac: 여분 보드의 동적 NVS만 초기화 (OpenThread 저장값·등록 표식·exporter journal).
#    재부팅하지 않고 다음 단계로 간다.
python -m esptool --chip esp32h2 -p "$SPARE_PORT" --after no-reset erase-region 0x9000 0x6000

# 4) Mac: 준비 이미지 → 시험 펌웨어 순서로 기록(마지막 기록 뒤에만 재부팅), 모니터
python -m esptool --chip esp32h2 -p "$SPARE_PORT" --after no-reset write-flash 0x187000 \
  build/cfg_images/power_base_await_bridge_config.bin
(cd "$BENCH_BUILD" && python -m esptool --chip esp32h2 -p "$SPARE_PORT" -b 460800 \
  --before default-reset --after hard-reset write-flash @flash_args)
idf.py -C power_base -B "$BENCH_BUILD" -D SDKCONFIG="$BENCH_BUILD/sdkconfig" -p "$SPARE_PORT" monitor
# 기대 로그: "Thread dataset mode: AWAIT_COMMISSIONING", "boot: not enrolled (none)",
#            "safe wait: BENCH build", 프롬프트 pb>
```

5) 보드 콘솔에서 `pb_join status` → `state=waiting`, EUI-64 경고 없음. 표시된 EUI-64를 확인한다.

6) 앱(DEBUG): 기기 → Power Base 등록 시험 → 5)의 EUI-64 입력 → 시작 →
`waiting_for_heartbeat`와 PSKd 표시를 확인한다. PSKd를 파일·채팅·터미널 명령줄에 붙여넣지 않는다.

7) 보드 콘솔: `pb_join start` → 표시되지 않는 입력란에 PSKd 입력 → Enter. 이후 `pb_join status`로
`joining` → `attaching` → `enrolled`를 확인한다. 기대 로그: `Joiner succeeded`,
`enrolled: attached with a complete Active Dataset`, `power base ready`, exporter 시작.
실패하면 `last=`를 기록한다(`joiner_security`: PSKd 불일치, `joiner_not_found`: 세션/OTBR 없음,
`attach_timeout`). 재시도는 앱에서 새 세션을 열고 다시 `pb_join start`.

```bash
# 8) Linux OTBR 호스트 (읽기 전용)
curl -s http://<LAN-address>:8787/api/v1/health | jq .
curl -s http://<LAN-address>:8787/api/v1/snapshot | jq -r '.power_bases[] | "\(.uuid) online=\(.online)"'
sudo ot-ctl commissioner joiner table      # 세션이 끝난 뒤 이 EUI-64 항목이 없어야 함
sudo journalctl -u pogo-matter-bridge -o cat --since "-15 min" | grep -E 'enrollment|\[heartbeat\]|first seen'
# 2026-10-01 기록 당시 운영 bridge는 stdout 줄 버퍼링 수정 전이었다. 지금 배포본에 그 수정이
# 들어갔는지는 확인하지 않았으므로 위 줄이 늦게 보이거나 빠질 수 있다.
```

앱의 세션이 `heartbeat_received`가 되는지 확인한다. 같은 EUI-64로 새 세션을 열면
`409 power_base_already_connected`가 나와야 한다.

9) 전원 재시작: 여분 보드 USB를 뽑았다가 다시 꽂고 4)의 monitor를 다시 연다. 기대 로그:
`boot: enrollment marker and stored Dataset match -- rejoining that network`,
`power base ready`. 8)의 snapshot에서 그 uuid가 다시 `online=true`가 되는지 본다.

되돌리기: 여분 보드의 등록만 지우려면 콘솔에서 `pb_join forget <EUI-64 끝 4자리>`. 이전 상태로
완전히 되돌리려면 1)의 백업을 `write-flash 0 <백업 파일>`로 같은 `$SPARE_PORT`에만 쓴다.

## Power Base BLE 등록 전달 경로 (mode 2) (2026-10-01)

범위: mode 2 미등록 보드가 BLE로 발견되고, 보드별 설정 코드로 인증(protocomm Security 2)한
클라이언트만 일회용 PSKd를 넘겨 기존 등록 상태 머신을 시작하게 하는 **펌웨어와 통신 규약**.
규약과 수치는 [`power-base.md`](power-base.md) "BLE 등록 전달 경로".

**2026-10-01 기록 당시 실행한 것은 host test와 빌드뿐이다.** 그때는 보드에 기록하지 않았고, 실제
BLE 링크(광고, 연결, GATT), iPhone, 실제 OpenThread Joiner와 함께 실행한 적이 없었으며 앱의 BLE
화면도 없었다. 아래 "host test와 빌드" 표의 어느 항목도 "실제 BLE 통신 통과"가 아니다.

그 뒤의 실물 결과와 현재 상태는 아래 "BLE 등록 실물 후속 검증 (2026-10-02)"에 따로 적는다. 앱
저장소에는 DEBUG 빌드 전용 BLE 등록 시험 화면(`PowerBaseEnrollmentBLETestView.swift`, 미커밋)이
있다.

### host test와 빌드 (Mac, ESP-IDF v6.1)

| 항목 | 결과 | 근거 |
|---|---|---|
| 자격 증명 record: 온전하고 이 EUI-64용일 때만 통과. 없음, 크기·magic·CRC·예약 바이트 오류, 모르는 버전·SRP group, salt 0, verifier 0/1, 다른 EUI-64, `cfg` 읽기 오류는 거부 | host 확인 | `tools/test_power_base_ble_admission.c`(실제 `power_base_ble_auth.c` + NVS simulator) |
| `pb-status`/`pb-start`/`pb-cancel` parser: 잘림, 과대(PSKd 33자), 버전 2, 다른 type, 예약 바이트, request id 0 | host 확인 | 같은 test(실제 `pb_ble_admission.c`) |
| challenge: 발급한 세션에만 유효(이전 연결·재handshake 이전 것 거부), 1회용, 60초 만료, 32비트 clock wrap | host 확인 | 같은 test |
| 다른 보드용 요청, 잘못된 PSKd, Joiner 불가 보드 → Joiner 시작 전 거부 | host 확인 | 같은 test |
| 동일 요청 재전송은 Joiner 1회, 같은 request id에 다른 내용은 거부, 다른 요청·두 번째 클라이언트는 `BUSY`이고 진행 중인 시도를 바꾸거나 취소하지 못함 | host 확인 | 같은 test |
| disconnect 뒤에도 수락한 시도 진행, 명시적 취소(queued/joining/attaching), 등록 완료 뒤의 cancel은 표식·Thread를 건드리지 않음, 실패 후 새 request id로 재시도 | host 확인 | 같은 test. worker가 **실제** `power_base_enrollment.c` controller를 호출(가짜는 OpenThread 연산뿐) |
| Joiner 성공만으로 성공 처리하지 않음: attaching 동안 running이고 BLE 유지, attach 실패는 failed, enrolled 뒤에만 BLE 종료 지시 | host 확인 | 같은 test |
| worker queue 4개: 가득 차면 `BUSY`이고 PSKd를 보관하지 않음. 인증 실패 5회·handshake 메시지 40개에서 잠금, 잠금이 수락된 시도를 취소하지 않음. 광고 창 15분(진행 중인 시도가 있으면 그 뒤) | host 확인 | 같은 test |
| controller는 BLE 쪽 잠금을 푼 상태에서만 호출. 복호화 버퍼·PSKd slot 삭제. 로그에 PSKd·challenge·request id 없음 | host 확인 | 같은 test. 모두 194개 검사 |
| 위 test가 규칙 위반을 잡는지 | 확인 | 규칙을 하나씩 깨뜨린 37개 변형 중 35개 검출. 나머지 2개는 다른 층이 같은 것을 다시 막는 동치 변형(끝난 시도에 대한 늦은 cancel은 controller가 거부, record 크기는 parse가 다시 검사) |
| Security 2 상호운용: ESP-IDF 공식 Python 클라이언트 코드(`esp_local_ctrl/scripts`의 `security2.py`, `srp6a.py`, protocomm 생성 protobuf, 수정 없음) ↔ host로 빌드한 **실제** ESP-IDF `protocomm.c`·`security2.c`·`esp_srp.c` + 실제 `pb_ble_service.c`. record는 준비 도구가 만든 실제 cfg 이미지에서 ESP-IDF `nvs_parser`로 꺼냄 | host 확인(101개 검사) | `tools/test_power_base_ble_sec2_interop.py` + `tools/host_ble_sec2`. BLE 대신 `protocomm_open_session()`/`protocomm_req_handle()`/`protocomm_close_session()`을 직접 호출 |
| ├ 올바른 설정 코드 + 이 보드 EUI-64: SRP-6a 성립, 클라이언트가 device proof 확인, AES-GCM 카운터 방식으로 `pb-status`의 EUI-64·challenge 수신 | host 확인 | 같은 test |
| ├ 틀린 코드(1글자), 다른 보드 EUI-64를 username으로, 소문자 코드: handshake 거부 | host 확인 | 같은 test |
| ├ handshake 전 평문 `pb-start`·`pb-status`: protocomm이 거부(handler 미도달), Security 0/1 세션 요청 거부, `proto-ver`에 `no_sec` 없음 | host 확인 | 같은 test |
| ├ 변조·재전송한 암호문 거부, 재연결 뒤 이전 세션 키·challenge 무효, 재연결 뒤 request id로 시도 조회 | host 확인 | 같은 test |
| ├ start → worker → Joiner(가짜 OT) → attaching → enrolled → BLE 종료 지시, service 정지 뒤에도 등록·Thread 상태 불변, Joiner 시작 횟수 정확 | host 확인 | 같은 test |
| └ 장치 쪽 로그(ESP-IDF 포함, 펌웨어 빌드와 같은 INFO 레벨)에 설정 코드·PSKd·salt·verifier·세션 키 dump 없음 | host 확인 | 같은 test |
| 준비 도구: 네 namespace(Dataset 없음), 12288바이트, 0600, record 형식과 CRC, verifier를 식으로 독립 재계산해 일치, 코드가 출력·이미지에 없음, 기존 코드 파일·이미지를 덮어쓰지 않음, 명시적 재사용, 잘못된 EUI-64·Bridge 값·저장소 안 코드 파일 거부 | host 확인(44개) | `tools/test_gen_power_base_ble_config.py`(실제 도구 + ESP-IDF `nvs_parser`) |
| 기존 생성기 호환: 새 옵션 없이 만든 이미지는 이전과 같은 세 namespace, mode 0/1/2 규칙 그대로 | 확인 | 위 test + `tools/test_pack_power_base_await_config.py` 18개 재실행 |
| 배선: BLE callback에 OpenThread·NVS·controller 호출과 BLE 종료 없음, BLE 시작·poll·정지는 등록 task만, poll은 controller mutex 밖·서비스 시작 전, 재개 보드는 등록 task를 만들지 않음, Security 2만 등록, 기본·bench 설정에 BT 항목 없음, 파티션·표식 불변 | 정적 확인(72개) | `tools/test_power_base_ble_enrollment_paths.py` |
| 기존 host test 재실행: enrollment 97, await 배선 32, thread config 46, boot sequencing 128, bridge config 42, no-root-api 16 | 확인 | 변경 후 재실행, 모두 통과 |
| ESP-IDF v6.1 `power_base` 기본 빌드 | 빌드 확인 | `0xd0470` bytes(app 파티션 44% 여유), 정적 RAM 104964/258000. 경고 없음. 이미지에 BLE·protocomm·Joiner·콘솔 symbol 없음 |
| ESP-IDF v6.1 `power_base` USB bench 빌드 | 빌드 확인 | `0xf1940` bytes(36% 여유, 이전과 같은 크기), 정적 RAM 106500. BLE·protocomm symbol 없음 |
| ESP-IDF v6.1 `power_base` BLE 시험 빌드 | 빌드 확인 | `0x153fd0` bytes(1,392,592). app 파티션 `0x177000`에 들어가고 `0x23030` bytes(9%) 여유. 정적 RAM(DIRAM) 136326/258000(52.8%). `pb_join`·콘솔 REPL·OT CLI·Security 0/1 symbol 없음 |
| ESP-IDF v6.1 `module` 빌드 | 빌드 확인 | `0xd8000` bytes(42% 여유, 이전과 같은 크기). BLE·등록 symbol 없음 |
| 기본·bench 빌드의 설정과 코드가 그대로인지 | 확인 | 기존 `power_base/sdkconfig`·`module/sdkconfig`는 복사본으로 빌드해 원본 불변. bench는 새로 생성한 sdkconfig가 기존 bench build의 것과 동일. `enrollment_runtime.c`는 두 구성에서 전처리 결과가 변경 전과 같음 |

공식 iOS 클라이언트(ESPProvision `78e58f0`)는 소스를 읽어 endpoint 탐색 방식, `proto-ver` 해석,
SRP 식, IV 구성을 대조했을 뿐 실행하지 않았다.

### 실물에서 확인하지 않은 것 (2026-10-01 기록)

아래 표는 2026-10-01 시점의 상태이며 그대로 보존한다. 현재 상태는 바로 다음 절의 "현재 상태" 표를
따른다.

| 항목 | 상태 |
|---|---|
| BLE 광고·scan response·연결, GATT 표(5개 characteristic + 0x2901 descriptor) | 미확인 |
| Security 2 handshake의 GATT long write/read(특히 기본 MTU 23), iOS(ESPProvision)와의 handshake | 미확인 |
| NimBLE host task(stack 4096)에서 SRP 3072비트 연산의 stack·소요 시간, 동작 중 heap 여유 | 미확인 |
| BLE와 Thread 공존: BLE 연결을 유지한 채 Joiner가 성공하는지, 연결을 끊은 경우와의 차이 | 미확인 |
| 등록 완료 뒤 `protocomm_ble_stop()` 실제 동작, 그 뒤 Thread·exporter·POGO 정상 동작 | 미확인 |
| 광고 창 15분과 인증 실패 잠금의 실제 동작, 전원 재투입 뒤 다시 열림 | 미확인 |
| 실제 `nvs_flash`가 `cfg`의 `pb_ble_auth`를 읽는지(host는 `nvs_parser`와 simulator) | 미확인 |
| 등록 task stack 6144의 여유 | 미확인 |
| 앱 → 허브 세션 → BLE start → `heartbeat_received` 전 구간 | 미확인(당시 앱 BLE 화면 미구현) |
| 등록 뒤 재부팅 시 BLE를 다시 열지 않는지 | 실물 미확인(host·정적 확인만) |

### BLE 등록 실물 후속 검증 (2026-10-02)

출처: 사용자가 2026-10-02(Asia/Seoul)에 여분 보드에서 실행한 시험의 터미널 기록 4개(esptool 출력과
`esp_idf_monitor` 로그)를 검토해 적었다. 이 문서를 고친 작업에서는 보드를 다시 시험하지 않았고
시리얼·flash에 접근하지 않았으며 host test와 빌드도 다시 실행하지 않았다. 로그에 호스트 시각이
없어 아래 시각은 보드 uptime(초)이다. 설정 코드와 PSKd는 읽지 않았고 이 문서에 옮기지 않았다.

보드 로그가 보여 주는 것은 보드의 동작이다. 어떤 클라이언트가 연결했는지, 앱이 무엇을 표시했는지,
허브 세션이 어떤 상태였는지는 보드 로그에 없다. 그 부분은 "앱 쪽 결과"에 사용자 보고로 따로 적는다.

#### 대상과 기록 방법

- 보드: 여분 ESP32-H2 1대(EUI-64 `744dbdfffe65c00a`, esptool 출력의 MAC으로 확인).
- 이미지: BLE 시험 빌드(`sdkconfig.ble_enrollment`). 기록 A에서 `power_base.bin` 1,392,592 bytes를
  `0x10000`에 쓰고 `Hash of data verified`를 받았다. 위 빌드 표의 BLE 시험 빌드와 같은 크기다. 네 기록의
  부팅 로그는 App version `mvp-pairing-v1-39-g431b280-dirt…`, Compile time `Oct  1 2026 11:32:06`,
  ELF SHA256 `f0459bde9…`로 모두 같다. 2026-10-03 체인 시험의 Power Base 이미지(1,392,848 bytes)와는
  다른 빌드다.
- 준비: 기록마다 먼저 동적 NVS(`0x9000`부터 `0x6000`)를 지워 미등록 상태로 만들었다. 기록 A는 준비
  이미지(12,288 bytes, `0x187000`)와 bootloader·파티션 표·app을 함께 썼고, 기록 B~D는 지우기만 하고
  재부팅했다.
- 네 기록 모두 부팅 뒤 `Thread dataset mode: AWAIT_COMMISSIONING`, `boot: not enrolled (none)`,
  `BLE enrollment open as 'POGO-PB-744DBDFFFE65C00A' for up to 900 s`가 나왔다.
- 네 기록 모두 `rst:`는 기록 시작 때의 1회뿐이고 panic·재부팅 로그가 없다.
- 모듈은 도킹하지 않았다(`pogo_port`는 끝까지 `WAIT_ATTACH`).

#### 등록 성공 (기록 A~C, 보드 로그)

| 기록 | 연결·MTU 갱신 | start 수락 | Joiner 시작 → 성공 | CHILD | 등록 표식 | BLE 종료 | exporter ready | ROUTER |
|---|---:|---:|---|---:|---:|---:|---:|---:|
| A | 729.10 | 733.15 | 733.17 → 739.48 | 740.39 | 740.54 | 740.59 | 740.60 | 823.04 |
| B | 516.99 | 520.41 | 520.54 → 526.78 | 527.77 | 527.83 | 527.88 | 527.89 | 615.61 |
| C | 137.67 | 364.90 | 364.97 → 371.27 | 372.44 | 372.52 | 372.58 | 372.59 | 425.41 |

세 기록에서 같은 순서가 나왔다.

- 광고 시작 뒤 연결되고 ATT MTU가 256으로 갱신됐다(`mtu update event ... mtu=256`).
- Security 2 handshake가 시작되고(`security2: Using salt and verifier to generate public key...`)
  `start accepted (generation 1) and queued for the enrollment worker`가 나왔다. handshake 성립을
  직접 알리는 로그 줄은 없다. start는 인증된 세션에서만 handler에 닿으므로(host test로 확인한 규칙)
  start 수락이 그 간접 근거다.
- `Joiner started (attempt 1)` → 약 6.2~6.3초 뒤 `Joiner succeeded` → 저장된 Joiner Dataset으로
  Thread 시작 → `Role detached -> child` →
  `enrolled: attached with a complete Active Dataset and the marker is committed`.
- `BLE enrollment closes for this boot (enrolled)` → 광고 중지 →
  `terminate connection; conn_handle=0 hci_reason=19` → `BLE enrollment stopped (enrolled)`.
  start 수락부터 여기까지 재광고 로그가 없고 보드가 연결을 끊었으므로, 세 번 모두 BLE 연결이
  유지된 채 Joiner가 성공했다.
- 이어서 `starting services`, `CoAP power-base transport ready`,
  `linux bridge exporter ready ... port=5684`, `pogo_port ... WAIT_ATTACH`,
  `power base ready: uuid=..c00a`가 나왔고 그 뒤 `Role child -> router`가 됐다.
- 세 기록 모두 BLE 종료 과정에서 `E protocomm_nimble: Error setting advertisement data; rc = 30`이
  1건 나왔다. 원인은 확인하지 않았다. 그 뒤의 서비스 시작은 진행됐다.
- handshake 시작 로그에서 start 수락까지는 A 2.70초, B 2.71초였고 C는 226.57초였다. C의 그 사이에
  무엇이 있었는지는 로그에 없다.

보드 로그에 없는 것: heartbeat가 Linux에 도착했는지, 허브 세션이 `heartbeat_received`가 됐는지,
모듈 도킹 뒤의 POGO 프로비저닝과 sample. exporter는 "시작"까지만 기록돼 있다.

#### 취소와 광고 창 만료 (기록 D, 보드 로그)

| uptime(초) | 로그 |
|---:|---|
| 0.50 | BLE 등록 창 열림(900 s) |
| 345.04 → 346.84 | 연결·MTU 256·handshake 시작, start 없이 다시 광고 |
| 730.67 → 732.59 | 연결·MTU 256·handshake 시작, start 없이 다시 광고 |
| 737.75 → 741.35 | 연결, `start accepted (generation 1)` |
| 741.39 → 742.61 | `Joiner started (attempt 1)` → `enrollment not completed (cancelled) -- Thread stopped, waiting for a new attempt` |
| 742.82 | 다시 광고 |
| 765.47 → 768.98 | 연결, `start accepted (generation 2)` |
| 769.08 → 772.70 | `Joiner started (attempt 2)` → 같은 `cancelled` 로그 |
| 772.88 | 다시 광고 |
| 900.51 | `BLE enrollment closes for this boot (window_expired)` → 광고 중지 → `BLE enrollment stopped (window_expired)` |

- 두 번의 취소 모두 Joiner 진행 중(joining 단계)에 났고, 보드는 Thread를 멈춘 뒤 다시 광고했으며
  다음 start를 새 generation으로 받았다. Thread 정지 직후 `process_thread_receive failed: ESP_FAIL`이
  3회씩 나왔다.
- 창은 진행 중인 시도가 없는 상태에서 부팅 약 900초 뒤에 닫혔다. 이 기록에는 서비스 시작,
  exporter, `power base ready` 로그가 0건이다. 미등록 보드는 창이 닫힐 때까지 서비스를 시작하지
  않았다.
- 무엇이 취소를 일으켰는지(앱의 취소 조작인지)는 보드 로그에 없다. start 없이 끝난 두 연결의 이유도
  로그에 없다.

#### 앱 쪽 결과 (사용자 보고)

위 시도를 앱의 DEBUG BLE 등록 시험 화면으로 실행했고 그 결과를 앱에서 확인했다는 것은 사용자
보고다. 이 문서를 고친 작업에서는 앱 화면 기록, 앱 로그, 허브 세션 응답 원문을 보지 않았다. 그래서
앱이 표시한 상태를 항목별로 적지 않고, 보드 로그로 확인한 동작과 합쳐 쓰지 않는다.

#### 현재 상태

| 항목 | 현재 상태 | 근거와 경계 |
|---|---|---|
| BLE 광고·연결 | 실물 확인(보드 로그) | 기록 A~D. scan response 내용과 GATT 표(5개 characteristic + 0x2901 descriptor)를 스캐너로 대조한 기록은 없다 |
| Security 2 handshake(GATT) | MTU 256에서 실물 확인(보드 로그의 간접 근거) | 기본 MTU 23의 long write/read는 미확인. 공식 ESPProvision 앱과의 handshake도 미확인 |
| NimBLE host task(stack 4096)의 SRP 연산 stack·소요 시간, heap 여유 | 미확인 | 네 기록에 panic·재부팅은 없었지만 stack 최저 여유와 heap을 측정하지 않았다 |
| BLE와 Thread 공존 | BLE 연결을 유지한 경우만 실물 확인(3회) | `ACCEPTED` 직후 연결을 끊은 경우와의 차이는 미확인 |
| 등록 완료 뒤 BLE 종료 | 실물 확인(보드 로그, 3회) | 종료 중 `rc = 30` 오류 1건씩, 원인 미확인 |
| BLE 종료 뒤 Thread·exporter·POGO | Thread CHILD → ROUTER, exporter 시작, `pogo_port` `WAIT_ATTACH`까지 실물 확인 | 그 뒤의 heartbeat 도착, 모듈 프로비저닝, sample은 이 기록에 없다. 2026-10-03 체인 시험은 다른 빌드이고 부팅 로그가 없어 이 항목의 근거가 아니다 |
| 광고 창 15분 | 진행 중인 시도가 없을 때의 만료를 실물 확인(1회) | 진행 중인 시도가 있을 때 그 뒤로 미뤄지는지, 만료 뒤 전원 재투입으로 다시 열리는지는 미확인 |
| 인증 실패 잠금 | 미확인 | 틀린 설정 코드로 시험한 기록이 없다 |
| 명시적 취소 | joining 단계에서 실물 확인(2회) | queued·attaching 단계의 취소와 등록 완료 뒤의 cancel은 host 확인만 |
| 실제 `nvs_flash`가 `cfg`의 `pb_ble_auth`를 읽는지 | 실물 확인(간접) | 준비 이미지를 쓴 보드에서 BLE가 열리고 인증된 start가 수락됐다. 손상되거나 다른 EUI-64용인 record의 거부는 실물 미확인 |
| 등록 task stack 6144의 여유 | 미확인 | 측정하지 않았다 |
| 앱 → 허브 세션 → BLE start → `heartbeat_received` 전 구간 | 보드 구간(start 수락 → Joiner → attach → 표식 → exporter 시작)은 실물 확인, 앱·허브 구간은 사용자 보고 | 앱 화면과 허브 세션 상태의 원문 기록은 보지 않았다 |
| 앱의 백그라운드·foreground 전환 중 경쟁 조건 | 미확인 | 조건을 나눠 시험한 실물 기록이 없다 |
| 등록 뒤 재부팅 시 BLE를 다시 열지 않는지 | 실물 미확인(host·정적 확인만) | 네 기록에 등록 뒤의 재부팅이 없다 |

2026-10-05 추가: 위 표는 2026-10-02 당시의 판정이다. "앱 → 허브 세션 → BLE start → `heartbeat_received`
전 구간"과 "등록 뒤 재부팅 시 BLE를 다시 열지 않는지"에는 그 뒤의 실물 기록이 있다. 다른 빌드(주소 자동
전달, `0x04` start)의 기록이며 아래 "허브 Thread 수신 주소 자동 전달"의 "실물 후속 검증 (2026-10-05)"에
적었다.

### 다음 실물 시험 절차 (여분 보드 744dbdfffe65c00a만)

2026-10-01에 적은 절차다. 이 가운데 3-b와 5)의 등록 성공·취소·창 만료는 2026-10-02에 실행됐다(위
후속 검증). 3-a, 4)의 스캐너 확인, 5)의 인증 실패 잠금·`ACCEPTED` 직후 연결 끊기·재부팅 뒤 BLE
미광고는 아직 기록이 없다.

운영 Base와 운영 Linux 서비스는 바꾸거나 재시작하지 않는다. Linux 명령은 읽기 전용이다.
설정 코드, PSKd, Dataset은 어떤 명령에도 인자로 넣지 않는다. 기록 단계는 `&&`로 이어져 있어
앞 단계가 실패하면 뒤의 기록은 실행되지 않는다.

이 보드는 USB 콘솔 시험으로 **이미 등록돼** 있다(표식 + Dataset). 그대로 BLE 시험 펌웨어를
쓰면 재개 경로로 부팅해 BLE를 열지 않는다(그것도 확인 항목이다: 3-a). BLE 등록을 시험하려면
동적 NVS를 지워 미등록 상태로 만든다(3-b). 그러면 허브에서 이 Base는 offline이 된다.

```bash
# 1) Mac: 여분 보드 식별과 전체 백업 (운영 Base USB는 뽑아 둔다)
SPARE_PORT=/dev/cu.usbmodemXXXX        # 여분 보드 포트로 바꾼다
python -m esptool --chip esp32h2 -p "$SPARE_PORT" read-mac    # 74:4d:bd:ff:fe:65:c0:0a 가 아니면 중단
python -m esptool --chip esp32h2 -p "$SPARE_PORT" flash-id    # 4MB 인지 확인. 다르면 아래 크기를 그 값으로
mkdir -p "$HOME/pogo-spare-backups" && chmod 700 "$HOME/pogo-spare-backups" && \
(umask 077; python -m esptool --chip esp32h2 -p "$SPARE_PORT" read-flash 0 0x400000 \
  "$HOME/pogo-spare-backups/spare-744dbdfffe65c00a-before-ble.bin")
```

```bash
# 2) Mac: BLE 시험 빌드와 준비 이미지 (보드에 쓰지 않음)
cd "/Users/junsoona/no-icloud/main project_1"
source ~/.espressif/v6.1/esp-idf/export.sh
export IDF_COMPONENT_MANAGER=0
BLE_BUILD="$HOME/esp-builds/pogo_power_base_ble_enrollment"
EUI=744dbdfffe65c00a
OTBR_MLEID='<지금 이 보드 cfg의 pb_bridge에 넣었던 것과 같은 주소>'   # 이번 시험은 주소·포트를 그대로 유지
CODES="$HOME/pogo-setup-codes"
IMG="build/cfg_images/power_base_ble_${EUI}_config.bin"
idf.py -C power_base -B "$BLE_BUILD" -D SDKCONFIG="$BLE_BUILD/sdkconfig" \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.ble_enrollment" build && \
mkdir -p "$CODES" && chmod 700 "$CODES" && \
python tools/gen_power_base_ble_config.py --eui64 "$EUI" \
  --bridge-host "$OTBR_MLEID" --bridge-port 5684 --setup-code-file "$CODES/$EUI.setup-code" && \
test -s "$IMG" && echo "준비 이미지 확인: $IMG"
# 처음 빌드라면 build 앞에 같은 -D 옵션으로 `set-target esp32h2`를 한 번 실행한다.
```

```bash
# 3-a) (선택) 등록된 상태 그대로 BLE 시험 펌웨어만 기록: 재개 경로가 BLE를 열지 않는지 확인
(cd "$BLE_BUILD" && python -m esptool --chip esp32h2 -p "$SPARE_PORT" -b 460800 \
  --before default-reset --after hard-reset write-flash @flash_args) && \
idf.py -C power_base -B "$BLE_BUILD" -D SDKCONFIG="$BLE_BUILD/sdkconfig" -p "$SPARE_PORT" monitor
# 기대: "boot: enrollment marker and stored Dataset match", "power base ready",
#       "BLE enrollment" 로그 없음, 스캐너에 POGO-PB-... 가 보이지 않음.
#       (기존 cfg에는 pb_ble_auth가 없지만 재개 경로는 그것을 읽지 않는다.)
```

```bash
# 3-b) 미등록 상태로 되돌리고 준비 이미지 + 시험 펌웨어 기록 (마지막 기록 뒤에만 재부팅)
test -s "$IMG" && \
python -m esptool --chip esp32h2 -p "$SPARE_PORT" --after no-reset erase-region 0x9000 0x6000 && \
python -m esptool --chip esp32h2 -p "$SPARE_PORT" --after no-reset write-flash 0x187000 "$IMG" && \
(cd "$BLE_BUILD" && python -m esptool --chip esp32h2 -p "$SPARE_PORT" -b 460800 \
  --before default-reset --after hard-reset write-flash @flash_args) && \
idf.py -C power_base -B "$BLE_BUILD" -D SDKCONFIG="$BLE_BUILD/sdkconfig" -p "$SPARE_PORT" monitor
# 기대: "Thread dataset mode: AWAIT_COMMISSIONING", "boot: not enrolled (none)",
#       "safe wait: BLE enrollment open as 'POGO-PB-744DBDFFFE65C00A' for up to 900 s"
#       "power base ready"와 exporter 로그는 나오면 안 된다.
```

4) 앱 없이 확인할 수 있는 것(iPhone의 범용 BLE 스캐너 앱 등):

- 이름 `POGO-PB-744DBDFFFE65C00A`, service `2e8a0000-9ffb-4868-bc63-064a14069755`,
  characteristic 5개와 각 0x2901 descriptor 문자열(`prov-session`, `proto-ver`, `pb-status`,
  `pb-start`, `pb-cancel`).
- `proto-ver`에 `ESP`를 쓰고 읽으면 JSON(`sec_ver` 2, `sec_patch_ver` 1).
- `pb-status`나 `pb-start`에 임의 바이트를 쓰면 ATT 오류. 보드 로그에 `Secure session not
  established`가 나오고 `Joiner started`는 나오지 않는다.
- 15분 뒤 로그 `BLE enrollment closes for this boot (window_expired)`와
  `BLE enrollment stopped`, 스캐너에서 사라짐. USB를 다시 꽂으면 다시 광고한다.
- (선택) 다른 EUI-64용으로 만든 이미지를 0x187000에 쓰고 재부팅: 로그
  `setup credential in 'cfg' is foreign_eui64`, 광고 없음. 확인 뒤 3-b의 이미지를 다시 쓴다.

5) 인증이 필요한 확인(Security 2 클라이언트가 있어야 한다. 앱의 DEBUG BLE 등록 시험 화면을 쓴다):
handshake 성공과 `pb-status`의 EUI-64, 틀린 코드 5회 뒤 `auth_locked`, 그리고
[`power-base.md`](power-base.md) "앱이 지켜야 할 순서"대로 허브 세션 → `pb-start` →
`heartbeat_received`. 이때 BLE 연결을 유지한 경우와 `ACCEPTED` 직후 끊은 경우의 Joiner 결과를
둘 다 기록한다. 등록 뒤 로그 `BLE enrollment stopped (enrolled)` → `power base ready`,
재부팅 뒤 재개와 BLE 미광고를 확인한다.

되돌리기: 1)의 백업을 `write-flash 0 <백업 파일>`로 같은 `$SPARE_PORT`에만 쓴다. BLE 시험
빌드에는 `pb_join forget`이 없으므로 등록만 지우려면 3-b의 `erase-region 0x9000 0x6000`을 쓴다.

## 허브 Thread 수신 주소 자동 전달 (BLE 등록) (2026-10-03)

범위: BLE 등록 때 보드 exporter의 목적지(Linux bridge의 Thread 주소와 ingest port)를 준비 이미지에
넣지 않고 허브 → 앱 → 보드로 전달하고, 보드가 그것을 등록 완료 record와 함께 저장해 재부팅 뒤에도
복원하는 경로. 규약·저장 형식·호환 정책은 [`power-base.md`](power-base.md) "허브 Thread 수신 주소
자동 전달", API는 [`pogohome-api-v1.md`](pogohome-api-v1.md) "Bridge endpoint delivery".

**이 날 실행한 것은 Mac의 host test와 빌드뿐이다.** 보드에 기록하지 않았고(flash·시리얼 접근 없음),
실제 BLE 연결, 실제 OTBR·`ot-ctl`, 운영 Linux 허브, 실제 Thread Joiner로는 한 번도 실행하지 않았다.
운영 서비스는 배포·재시작하지 않았다. 아래 표의 어느 항목도 실물 통과가 아니다. 이전 날짜의 실물
기록(2026-10-01~03)은 정적 주소 경로와 version 1 BLE start에 대한 것이며 이 기능의 근거가 아니다.

2026-10-05 추가: 위 문단과 아래의 표, "실행하지 못한 것"은 2026-10-03 작성 당시의 판정이다. 그 뒤 여분
보드 1대에서 실물 시험이 실행됐다. 지금의 판정은 아래 "실물 후속 검증 (2026-10-05)"의 "현재 상태 (실물
시험 뒤)"를 본다. 이 절의 host test·빌드 결과는 그대로 유효한 별개의 근거다.

기준: 펌웨어 저장소 `feature/unified-module-role-manager` @ `ff11a07`(+ 미커밋 변경), 앱 저장소
`main` @ `056e02c`(+ 미커밋 변경). 커밋하지 않았다.

### host test와 빌드 (Mac, Apple clang, ESP-IDF v6.1, Xcode)

| 항목 | 결과 | 근거 |
|---|---|---|
| 목적지 규칙(보드): fd00::/8의 EID만. unspecified, loopback, IPv4-mapped·compatible, multicast, link-local, global, fc00::/8, RLOC, ALOC, port 0 거부. 거부 시 PSKd 삭제·Joiner 미시작·기존 표식 유지 | host 확인 | `tools/test_power_base_enrollment.c`(실제 `power_base_enrollment.c` + NVS simulator), 전체 206개 검사 |
| record 버전 1은 이전과 바이트 단위로 같음. 버전 2는 56바이트 한 blob(주소 offset 32, port offset 48 big-endian, CRC가 목적지까지 덮음) | host 확인 | 같은 test. 예상 바이트는 test가 문서의 offset으로 따로 조립 |
| pending 목적지 폐기: joining 취소, Joiner 실패 4종, guard 만료, attaching 취소, attach 시간 초과, Thread 시작 실패, 시도 교체(새 시도는 이전 목적지를 물려받지 않음) | host 확인 | 같은 test |
| 가입한 네트워크의 Mesh-Local Prefix와 목적지 불일치 → 미등록(`dataset_mismatch`), record 없음 | host 확인 | 같은 test |
| 저장 실패(commit 오류) → 미등록, 서비스 불가, 반쯤 쓰인 record 없음 | host 확인 | 같은 test |
| 재부팅: 유효한 버전 2 record + 일치하는 Dataset이면 같은 목적지 복원. 1비트 변조 448가지, 1~70바이트의 다른 길이, 잘못된 버전·flags·예약 바이트, CRC가 맞는 port 0·RLOC·link-local·다른 prefix 주소, 다른 EUI-64, 다른 네트워크, Dataset 없음 → 모두 안전 대기, 목적지 미사용 | host 확인 | 같은 test |
| 전원 차단 모사(boot마다 별도 process + flash 파일): joining 중, Joiner 성공 뒤, attach 뒤 record 전, commit 실패, record 쓰기를 55개 위치에서 자른 경우(짧은 blob·0xff 채움) → 다음 부팅은 안전 대기. commit된 뒤에는 매 부팅 같은 목적지 | host 확인 | 같은 test |
| `0x04` start: golden vector가 실제 admission 코드에 그대로 들어가고 주소·port·PSKd가 문서의 offset에서 읽힘. 응답 `01840100`/`01841d00` | host 확인 | `tools/test_power_base_ble_admission.c`(실제 `pb_ble_admission.c`), 전체 295개 검사 |
| `0x04` 길이: 한 요청을 0~104바이트로 자르거나 늘린 105가지 중 72바이트만 수락, 나머지는 `MALFORMED`이고 challenge 유지. 70·96바이트 경계, `pskd_len` 불일치, `[3]`·`[45]` 예약 바이트, request id 0, 두 layout을 서로의 type으로 보낸 경우, 모르는 type, version 2 | host 확인 | 같은 test |
| `0x04` 내용 거부: 주소 10종, port 0, 목적지 형식 버전 0·2 → `INVALID_ENDPOINT`. 틀린·없는·만료된·이전 세션의 challenge, 다른 보드, Joiner 불가, 인증 실패 잠금 뒤(`CLOSED`). 모두 Joiner 미시작·slot 비움·record 없음 | host 확인 | 같은 test |
| 같은 request id: 같은 목적지는 `DUPLICATE`, 다른 주소·다른 port·다른 PSKd·`0x02`로 보낸 같은 내용은 `REQUEST_CONFLICT`. 수락된 시도는 그대로이고 Joiner는 한 번 | host 확인 | 같은 test |
| queued 취소 → slot의 목적지 즉시 삭제, worker 미호출. joining 실패·attaching 취소 → controller의 pending 삭제. 뒤이은 `0x02` 등록은 버전 1 record | host 확인 | 같은 test(worker가 실제 controller를 호출) |
| BLE callback은 목적지를 검사·복사만 함. controller는 leaf lock 밖에서 worker가 호출 | host 확인 + 정적 확인 | 같은 test, `tools/test_power_base_ble_enrollment_paths.py` |
| 로그에 PSKd·bridge 주소 없음 | host 확인 + 정적 확인 | 위 두 C test(stderr 캡처), paths test(7개 소스의 로그 문장) |
| exporter가 실제로 보내는 목적지: 주소 없는 이미지에서 등록 직후와 재부팅 3회 모두 전달받은 주소·port로 heartbeat. 정적 주소가 있는 이미지에서도 전달받은 쪽 우선 | host 확인 | `tools/test_power_base_enrollment_exporter_endpoint.c`(실제 enrollment + bridge config + `linux_bridge_exporter.c` + CoAP simulator가 기록한 송신 주소·port), 28개 검사 |
| 손상(주소 1비트, port 1비트, 잘린 쓰기)·다른 보드·prefix 밖 주소·port 0인 record, 다른 네트워크의 Dataset, Dataset 없음, commit 실패 → exporter 미시작. **이미지의 정적 주소로도 보내지 않음** | host 확인 | 같은 test |
| 호환: 목적지 없는 등록(버전 1)은 정적 주소 이미지에서 그 주소로, 주소 없는 이미지에서 exporter off | host 확인 | 같은 test |
| `power_base_bridge_config_apply_delivered()`: disabled·namespace 없음·정적·손상 이미지 위에 적용, `cfg`는 변하지 않음, 쓸 수 없는 목적지는 disabled(이미지 주소로 복귀 안 함) | host 확인 | `tools/test_power_base_bridge_config.c`, 전체 67개 검사 |
| 배선: `on_enrollment_ready()`가 attach supervisor 시작 전에 목적지 적용, 적용 위치 1곳, exporter 시작 1곳, bench 콘솔은 목적지 미전달, `cfg` 읽기 전용, digest가 type·목적지 block을 포함 | 정적 확인 | `tools/test_power_base_ble_enrollment_paths.py`, 전체 95개 검사(새 BLE 빌드의 sdkconfig 포함) |
| 위 test가 규칙 위반을 잡는지 | 확인 | 실제 소스의 사본에서 규칙을 하나씩 깨뜨린 11개 변형 중 10개 검출. 나머지 1개(digest에서 type 제외)는 목적지 block 유무가 이미 같은 구분을 하는 동치 변형이라 정적 검사로만 확인 |
| 허브 helper `mesh-local-eid`: 인자 없는 요청 한 줄, 고정 명령 `ipaddr mleid`만 stdin으로 실행, `ok mleid <주소>` 전용 parser. 기존 parser는 `[a-z_]`만 받는 그대로 | host 확인 | `linux/matter_bridge/app/api/tests/test_otbr_helper.cpp`(실제 helper test build + fake `ot-ctl`) |
| helper가 거부: 출력 없음, RLOC, ALOC, link-local, 주소 목록(그 안의 EID를 골라내지 않음), 같은 주소 2줄, 뒤에 다른 단어, IPv4, IPv4-mapped, 텍스트 → `err invalid_address`. OTBR 오류, 소켓 없음, 무응답 timeout. 구형 helper(`err invalid_request`) → Unsupported. 거짓 응답(link-local, 주소 2개, 다른 단어) → Failed | host 확인 | 같은 test |
| 주소 텍스트 규칙(허브): zone id, 대괄호, port, URL, hostname, 대문자, 공백, 점 표기 거부 | host 확인 | 같은 test |
| 세션: 목적지 조회 → Commissioner → Joiner 순서. 한 번 확정 후 GET·POST 재시도에서 같은 값(OTBR 주소가 바뀌어도). 실제 port 사용(5701로 시험). 다른 기기는 세션도 목적지도 못 봄 | host 확인 | `tests/test_enrollment.cpp`(실제 `EnrollmentManager` + Registry + fake Commissioner) |
| 조회 실패 12가지(unavailable, invalid state, timeout, failed, unsupported, Ok이지만 link-local·RLOC·IPv4-mapped·IPv4·hostname·빈 값·zone id) → 고정 오류 코드로 실패, Commissioner·Joiner·remove 호출 0회 | host 확인 | 같은 test |
| 조회 중 취소·만료·shutdown → 그 최종 상태 유지, 목적지 미부착, Joiner 추가 없음. 조회 중 Get·다른 Start·Registry heartbeat가 200 ms 안에 반환(잠금 없음) | host 확인 | 같은 test |
| API: 옵션 없는 요청의 응답에는 `bridge_endpoint` key 없음(끝이 이전과 같음). 옵션 있는 세션은 preparing에서 `null`, waiting에서 목적지. 다른 기기 404, 토큰 없음 401, 평문 403, health·snapshot에 주소·필드 없음 | host 확인 | `tests/test_enrollment_api.cpp`(실제 `Server`, TLS loopback) |
| API 검증: 버전 2·0 → 422, 문자열·bool·분수 → 400, 클라이언트가 보낸 `bridge_endpoint` 객체·주소·port 필드 → 400, ingest port를 모르는 bridge → 503 | host 확인 | 같은 test |
| 허브 API 전체 | 870개 검사 통과 | Mac에서 CMake + Apple clang + Homebrew OpenSSL로 빌드. ingest의 ctest 7개도 통과 |
| ESP-IDF 공식 Python Security 2 클라이언트 ↔ host로 빌드한 실제 protocomm + 실제 BLE 서비스: `proto-ver`의 capability, 인증 없는·평문 `0x04` 거부, `INVALID_ENDPOINT` 4종, 수락·중복·충돌, controller가 든 주소·port, 등록 뒤 record의 목적지 | host 확인(124개 검사) | `tools/test_power_base_ble_sec2_interop.py` + 새로 빌드한 `tools/host_ble_sec2`. `protobuf` 패키지를 임시 디렉터리에 설치해 실행 |
| 앱 Swift codec: `0x04` 요청이 C와 같은 golden vector, `0x02`는 변함없음, 70~96바이트 전 길이, 응답 type `0x84`, 주소·literal 규칙, capability 해석, capability 없는 보드에는 start를 쓰지 않음 | host 확인 | 앱 `PowerBaseBLEKit` `swift test` |
| Swift 클라이언트 ↔ 같은 host harness: Swift가 암호화해 보낸 `0x04`를 실제 C 서버가 받아 controller가 같은 주소·port를 듦, 중복·충돌, 등록 뒤 record, 취소·다른 네트워크 목적지 뒤 목적지 없음 | host 확인(53개 test, 건너뜀 0) | `HarnessInteropTests`(test용 자격 증명은 실제 준비 도구로 생성) |
| 앱 controller: preparing 동안 미전달, waiting + 목적지에서 한 번 전달, 구형 보드는 허브 세션 전에 중단, 구형 허브는 재요청·추정 없이 중단, 목적지 없는 waiting·바뀐 목적지·사라진 목적지는 새 start 없이 정리, 답 유실 재전송은 같은 request id·PSKd·목적지 | sim 확인 | 앱 `PowerBaseEnrollmentBridgeEndpointTests`(scripted hub·board, iOS Simulator) |
| 앱 등록 회귀(foreground permit, lifecycle, REQUEST_CONFLICT, 취소 거부, reconnect, 화면 이탈·허브 변경 정리, USB 콘솔 경로) | sim 확인 | 등록 관련 test class 14개의 190개 test 통과, 건너뜀 0(화면 렌더 test는 출력 디렉터리를 지정해 실행). 앱 전체 783개 중 774 통과, 실패 0, 건너뜀 9(렌더 7개와 실제 허브가 필요한 live 2개. 렌더 중 등록 화면 3개는 위에서 따로 실행) |
| 준비 도구: `--bridge-from-enrollment` 이미지는 `pb_bridge`가 enabled=0·주소·port 0이고 나머지 namespace는 주소 있는 이미지와 바이트 단위로 같음. 다른 보드의 이미지와는 BLE 자격 증명만 다름. 출처 미지정·혼용·한쪽만·형식 오류·빈 문자열·hostname·URL 주소는 아무것도 쓰지 않고 거부 | host 확인(67개 검사) | `tools/test_gen_power_base_ble_config.py`(실제 도구 + ESP-IDF `nvs_parser`). 설정 코드는 test가 임시 디렉터리에 새로 만든 것만 사용 |
| 기존 host test 재실행: thread config 46, boot sequencing 128, await 배선 32, await 생성기 18, no-root-api 16, exporter 8종(47·21·57·21·25·55·8·20) | 확인 | 모두 통과 |
| ESP-IDF v6.1 `power_base` 기본 빌드(esp32h2) | 빌드 확인 | `0xd0f20` bytes, app 파티션 44% 여유. 경고 없음 |
| ESP-IDF v6.1 `power_base` USB bench 빌드 | 빌드 확인 | `0xf2930` bytes, 35% 여유. 경고 없음 |
| ESP-IDF v6.1 `power_base` BLE 시험 빌드 | 빌드 확인 | `0x155220` bytes. app 파티션 `0x177000`에 `0x21de0` bytes(9%) 여유. 경고 없음 |
| 앱 빌드: iOS Simulator Debug·Release, iOS 기기용 Debug·Release, Mac Catalyst Debug·Release | 빌드 확인 | 기기용과 Catalyst는 서명 없이(`CODE_SIGNING_ALLOWED=NO`) 빌드만 |

세 펌웨어 빌드, host harness, 허브 API, 앱은 모두 저장소 밖의 별도 출력 디렉터리와 별도
`SDKCONFIG`·DerivedData로 빌드했다. 기존 `power_base/sdkconfig`와 기존 build 디렉터리는 쓰지 않았다.

### 실행하지 못한 것

| 항목 | 상태 |
|---|---|
| Linux GCC로 허브 API·helper 빌드와 test | **실행 불가**. 이 Mac에 Linux 환경이 없고 Docker도 동작하지 않았다. 위 허브 결과는 Apple clang 빌드다 |
| connectedhomeip(GN) `pogo_matter_bridge` 빌드 | **실행 불가**. CHIP SDK 환경이 없다. `app/chip/main.cpp`의 변경(ingest port를 등록 설정에 넘기는 한 줄)은 컴파일해 보지 못했다 |
| 실제 `ot-ctl ipaddr mleid`에 대한 helper 동작(stdin 방식, echo 여부, 출력 형식) | 미확인. fake `ot-ctl`로만 확인 |
| 운영 허브에 새 bridge·새 root helper 설치, sudoers 규칙 그대로 동작하는지 | 미실행(배포·재시작하지 않음) |
| 실제 보드에서 `0x04` 수락, 실제 `nvs_flash`의 56바이트 record 기록·복원, 실제 exporter의 송신 | 미확인(flash하지 않음) |
| 실제 BLE로 `proto-ver`(capability 포함, 더 길어진 JSON)와 `0x04`(최대 96 + 16바이트) 교환 | 미확인 |
| 앱 → 허브 세션 → BLE `0x04` → `heartbeat_received` 전 구간 | 미확인 |
| Mac Catalyst에서 앱 test 실행 | 미실행(서명·provisioning 갱신이 필요해 빌드만 확인) |
| BLE 시험 빌드의 app 파티션 여유(9%)와 등록 task stack(6144)의 실제 여유 | 미확인 |

2026-10-05 추가: 위 표는 2026-10-03 작성 당시의 상태다. Linux 쪽 네 줄은 "후속 기록: Linux 배포 검증과
실물 시험 산출물 준비 (2026-10-05)"의 ①에서, 실제 보드·BLE·전 구간에 관한 줄은 "실물 후속 검증
(2026-10-05)"에서 다시 판정했다. Mac Catalyst의 앱 test 실행과 stack 여유 측정은 그 뒤에도 기록이 없다.

### 후속 수정: 등록 시 목적지 필수 정책 (2026-10-04)

위 2026-10-03 구현에서는 `--bridge-from-enrollment` 이미지의 보드가 목적지 없는 구형 start(`0x02`)로도
Thread 가입과 버전 1 record 저장까지 끝낼 수 있었다(exporter는 꺼진 채 BLE 등록이 닫힘). 2026-10-03
표의 "주소 없는 이미지에서 exporter off" 줄과 power-base.md의 호환 표가 그 동작을 문서로만 적고
있었다. 이 후속 수정은 그 경로를 코드로 막는다. 규칙은 [`power-base.md`](power-base.md)
"등록 시 목적지 필수 정책".

**이 날 실행한 것도 Mac의 host test와 빌드뿐이다.** 보드·BLE·허브로는 실행하지 않았고 flash·erase·배포·
재시작을 하지 않았다. 앱 저장소와 Linux 등록 API·OTBR helper는 수정하지 않았다.

기준: 펌웨어 저장소 `ff11a07` + 2026-10-03의 미커밋 변경(40개 파일) 위에 추가. 커밋하지 않았다.

| 항목 | 결과 | 근거 |
|---|---|---|
| 정책 읽기: namespace 없음·정적 주소·exporter를 일부러 끈 이미지(`enabled=0`, 5번 바이트 0)는 "정책 없음". `enabled=0` + 5번 바이트 1만 "필수". 모르는 값(2·3·4·255), 필수인데 `enabled=1`, CRC 불일치(정책 바이트만 0→1·1→0으로 바뀐 경우 포함), 다른 schema·크기, blob·crc 누락, 파티션 읽기 실패는 "사용할 수 없음" | host 확인 | `tools/test_power_base_bridge_config.c`(실제 `power_base_bridge_config.c` + NVS simulator), 전체 87개 검사 |
| 기존 loader는 정책 바이트를 보지 않음: 같은 이미지를 이전과 똑같이 읽는다 | host 확인 + 정적 확인 | 같은 test, paths test |
| controller, 정책 필수 + 목적지 없는 요청: `ESP_ERR_NOT_ALLOWED`. Joiner·Thread 호출 0회, 상태·마지막 결과·attempt 불변, PSKd 버퍼 삭제. 기존 버전 1 record와 Dataset이 바이트 단위로 그대로 | host 확인 | `tools/test_power_base_enrollment.c`(실제 `power_base_enrollment.c`), 전체 256개 검사 |
| 거부 뒤 목적지 있는 요청: Joiner 시작(attempt 1), 등록 시 표식과 목적지가 한 56바이트 blob. 기존 record는 그 요청이 Joiner 전에 지움 | host 확인 | 같은 test |
| 부팅, 정책 필수 + 유효한 버전 1 record + 일치하는 Dataset: 안전 대기(`marker_corrupt`), Thread 미시작, 서비스 불가, record·Dataset 불변. tick이 지나도 재개하지 않음 | host 확인 | 같은 test |
| 부팅, 정책 필수 + 유효한 버전 2: 목적지 복원과 재개. 손상·다른 보드·다른 네트워크·Dataset 없음은 이전처럼 대기 | host 확인 | 같은 test |
| 정책 없음: 목적지 없는 요청으로 이전과 같은 버전 1 record 등록, 버전 1 재개 그대로 | host 확인 | 같은 test |
| 사용할 수 없는 정책: 목적지 유무와 관계없이 요청 거부(부작용 없음). 이미 등록된 버전 1·2는 이전처럼 재개 | host 확인 | 같은 test |
| 재부팅(boot마다 별도 process + flash 파일): 구형 방식으로 등록된 보드에 정책이 생기면 매 부팅 대기, 정책 없이 다시 부팅하면 재개(부팅과 거부된 요청이 아무것도 지우지 않음), 목적지 있는 등록 뒤에는 매 부팅 그 목적지로 재개 | host 확인 | 같은 test |
| BLE admission, 정책 필수 + `0x02`: 응답 `01 82 1b 00`(`UNAVAILABLE`). queue 0, 시도 record 없음, PSKd 미보관, challenge 유효 유지, worker·controller 호출 0회, Joiner 0회, 기존 record·Dataset 불변. 복호화 버퍼 삭제 | host 확인 | `tools/test_power_base_ble_admission.c`(실제 `pb_ble_admission.c` + 실제 controller), 전체 337개 검사 |
| 재전송 3회도 `UNAVAILABLE`(`DUPLICATE`·`REQUEST_CONFLICT` 아님), cancel은 `NO_SUCH_ATTEMPT`. `pb-status`에 0x08 flag 없음 | host 확인 | 같은 test |
| 거부 뒤 같은 challenge·같은 request id의 `0x04`: `ACCEPTED` → Joiner 1회 → 버전 2 record. challenge 없이 보낸 `0x02`, 다른 보드 EUI-64의 `0x02`도 `UNAVAILABLE`. 형식 오류·다른 version·닫힌 보드는 기존 코드(`MALFORMED`·`UNSUPPORTED_VERSION`·`CLOSED`) | host 확인 | 같은 test |
| 사용할 수 없는 정책: `0x02`·`0x04` 모두 `UNAVAILABLE`. 정책 없음: `0x02`가 이전처럼 수락·등록 | host 확인 | 같은 test |
| admission이 통과시켜도 controller가 단독으로 거부(Joiner 0회, record·Dataset 불변, 요청은 failed/0x80) | host 확인 | 같은 test(두 층의 정책을 다르게 설정) |
| exporter까지: 정책 이미지에서 목적지 없는 등록 요청은 거부(Joiner·서비스·송신 없음), 목적지 있는 등록과 재부팅은 그 주소·port로 heartbeat. 구형 방식으로 등록된 보드가 정책 이미지로 부팅하면 서비스 미시작(exporter 없이 켜지지 않음), 정적 주소로도 보내지 않음. 모르는 정책 값의 이미지는 새 등록 거부, 버전 2는 재개. 정책 없는 이미지(namespace 없음·disabled·정적)는 이전 동작 | host 확인 | `tools/test_power_base_enrollment_exporter_endpoint.c`(실제 정책 loader + controller + exporter), 전체 49개 검사 |
| 준비 도구: `--bridge-from-enrollment` 이미지의 `pb_bridge` blob이 `enabled=0`·정책 1·주소·port 0이고 CRC가 정책 바이트를 덮음. `--bridge-host/--bridge-port` 이미지는 정책 0. pack 도구의 `--require-enrollment-endpoint`는 mode 2 전용이고 주소와 함께 쓰면 거부. `--await-commissioning`만 준 이미지는 정책 0. 누락·충돌·잘못된 인자와 덮어쓰기 금지는 그대로 | host 확인(77개 검사) | `tools/test_gen_power_base_ble_config.py`(실제 도구가 만든 이미지를 ESP-IDF `nvs_parser`로 다시 읽음). `tools/test_pack_power_base_await_config.py` 18개도 통과 |
| ESP-IDF 공식 Python Security 2 클라이언트 ↔ 새로 빌드한 host harness: 정책 필수 보드에서 유효한 challenge의 `0x02` → `UNAVAILABLE`, 아무것도 시작 안 됨, 같은 challenge·request id의 `0x04` → 수락·등록. 사용할 수 없는 정책은 둘 다 `UNAVAILABLE`. 정책 없으면 `0x02` 수락 | host 확인(141개 검사) | `tools/test_power_base_ble_sec2_interop.py`. harness에 부팅 전 정책을 정하는 명령(`Y`)을 추가했고, 그 명령을 모르는 구동기는 이전과 같은 보드를 얻는다 |
| 앱 Swift 클라이언트 ↔ 같은 harness (앱 소스 변경 없음) | host 확인(53개 test, 건너뜀 0) | 앱 `PowerBaseBLEKit` `swift test` |
| 배선: 정책은 mode 2 분기에서 한 번만 읽고 runtime에 전달, controller에는 boot 규칙 전에, admission에는 protocomm 생성 전에 설정. 두 층 모두 삭제·queue·Joiner보다 먼저 검사. 사용할 수 없는 정책이면 BLE를 열지 않음. bench 콘솔은 controller 경로 하나뿐 | 정적 확인 | `tools/test_power_base_ble_enrollment_paths.py`, 전체 107개 검사(새 BLE 빌드의 sdkconfig 포함) |
| 수정 전에는 실패하는지 | 확인 | 수정 전 소스의 사본(정책 개념 없음)에 새 test를 돌리면 enrollment 23개, admission 25개, exporter 9개 검사가 실패. 수정 후 0개 |
| 방어를 하나씩 깨뜨리면 잡는지 | 확인 | 실제 소스 사본에서 9개 변형(admission 통과, controller 통과, 부팅 재개, 두 층의 INVALID 무시, 필수+정적 허용, CRC 생략, `enabled=0`에서 필수 추정, challenge 소모 뒤 거부) 모두 검출 |
| 기존 host test 재실행: thread config 46, boot sequencing 128, await 배선 32, no-root-api 16, exporter 47 | 확인 | 모두 통과 |
| ESP-IDF v6.1 `power_base` 기본 / USB bench / BLE 시험 빌드(esp32h2) | 빌드 확인 | `0xd1540`(44% 여유) / `0xf31e0`(35%) / `0x155b00`(`0x21500` bytes, 9% 여유). 경고 없음. 별도 출력 디렉터리와 `SDKCONFIG` |

실행하지 않은 것: 위 "실행하지 못한 것" 표가 그대로다(Linux GCC·CHIP 빌드 불가, 실물 전 구간 미확인).
추가로, 실제 `nvs_flash`가 정책 바이트가 든 `pb_bridge` blob을 읽는지, 실제 BLE에서 구형 앱이
`UNAVAILABLE`을 어떻게 보여 주는지는 확인하지 않았다. 앱은 이번에 수정하지 않았으므로 앱 test는 다시
돌리지 않았다(Swift 패키지 interop만 새 harness로 재실행).

실물 시험 전에 새로 만들어야 하는 것: ① 이 수정이 들어간 BLE 시험 펌웨어, ② 새 도구로 다시 만든 준비
이미지(`--bridge-from-enrollment --existing-setup-code-file`; 이전 이미지는 정책이 없어 방어되지
않는다). 아래 절차의 1)이 둘 다 만든다. 시험 1에 한 가지를 더한다: 구형 start만 아는 클라이언트(예:
2026-10-03 이전 앱 빌드)로 먼저 시도해 보드 로그에 `start refused (code 0x1b) -- no Joiner started`가
나오고 보드가 계속 대기·광고하는지 본 뒤, 새 앱으로 등록한다.

2026-10-05 추가: 이 절의 "실행하지 않은 것"과 "실물 미확인"은 2026-10-04 작성 당시의 상태다. 그 뒤의
실물 결과는 아래 "실물 후속 검증 (2026-10-05)"에 있다. 실제 `nvs_flash`가 정책 바이트를 읽는 것은 그
시험의 보드 로그로 확인됐다. 위 문단의 "구형 start만 아는 클라이언트로 먼저 시도"는 실행되지 않았고,
구형 start(`0x02`) 거부는 지금도 host 확인뿐이다.

### 다음 실물 시험 절차 (준비만 했고 실행하지 않았다)

2026-10-05 추가: 제목의 "실행하지 않았다"는 2026-10-04 작성 당시의 상태다. 2026-10-05에 시험 1과, 시험 2
가운데 재시작 뒤 복원에 해당하는 부분이 실행됐다. 기록은 아래 1), 1-a)의 명령이 아니라 "후속 기록"
②의 업데이트 도구로 했고, 시험 2의 재시작은 USB를 뽑았다 꽂는 전원 재연결이 아니라 모니터의 RTS
재시작이었다. 결과와 경계는 "실물 후속 검증 (2026-10-05)". 아래 절차와 기대 로그는 당시 적은 그대로 둔다.

대상은 여분 보드 `744dbdfffe65c00a`뿐이다. 운영 Base는 건드리지 않는다. **순서 0은 운영 Linux 허브에
새 bridge와 새 root helper를 배포해야 하므로 운영자가 따로 결정한다.** 이 문서의 누구도 그 배포를
하지 않았다. 설정 코드·PSKd·Dataset은 어떤 명령에도 인자로 넣지 않는다.

```bash
# 0) Linux 허브 (운영자 작업, 서비스 재시작 포함): 새 소스로 bridge와 helper를 빌드·설치한다.
#    docs/pogohome-api-v1.md "Installing the OTBR helper" 2)~3)을 다시 수행 (sudoers 규칙은 그대로).
#    설치 뒤 읽기 전용 확인:
sudo ot-ctl ipaddr mleid                              # 주소 한 줄 + Done
#    아래의 <USER>는 운영 Linux 계정 이름을 치환한 표기다.
sudo -u <USER> sh -c "printf 'pogohome-otbr/1 mesh-local-eid\n' | sudo -n -- /usr/local/libexec/pogohome/pogohome-otbr-helper"
#    기대: "ok mleid <위와 같은 주소>". "err invalid_request"면 helper가 구형이다.
systemctl cat pogo-matter-bridge | grep -- --port     # 없으면 ingest port는 5684
```

```bash
# 1) Mac: BLE 시험 빌드와 주소 없는 준비 이미지 (보드에 쓰지 않음)
cd "/Users/junsoona/no-icloud/main project_1"
source ~/.espressif/v6.1/esp-idf/export.sh
export IDF_COMPONENT_MANAGER=0
BLE_BUILD="$HOME/esp-builds/pogo_power_base_ble_enrollment"
EUI=744dbdfffe65c00a
CODES="$HOME/pogo-setup-codes"
IMG="build/cfg_images/power_base_ble_${EUI}_config.bin"
idf.py -C power_base -B "$BLE_BUILD" -D SDKCONFIG="$BLE_BUILD/sdkconfig" \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.ble_enrollment" build
# 이 보드의 이미지와 설정 코드가 이미 있으면 그 코드로 다시 만든다(새 코드를 만들지 않는다):
python tools/gen_power_base_ble_config.py --eui64 "$EUI" --bridge-from-enrollment \
  --existing-setup-code-file "$CODES/$EUI.setup-code" && \
test -s "$IMG" && echo "준비 이미지 확인: $IMG"
# 기대 출력: "pb_bridge:enabled=0, NO address, enrollment endpoint REQUIRED (...)"
# 2026-10-04 이전에 만든 주소 없는 이미지는 "목적지 필수" 정책이 없으므로 반드시 이 명령으로 다시 만든다.
```

**시험 1: 집별 주소 없는 준비 이미지 → 앱 BLE 등록 → 정확한 Base의 새 `heartbeat_received`**

```bash
# 1-a) Mac: 여분 보드 식별, 미등록 상태로 되돌리고 준비 이미지 + 시험 펌웨어 기록
SPARE_PORT=/dev/cu.usbmodemXXXX
python -m esptool --chip esp32h2 -p "$SPARE_PORT" read-mac    # 74:4d:bd:ff:fe:65:c0:0a 가 아니면 중단
test -s "$IMG" && \
python -m esptool --chip esp32h2 -p "$SPARE_PORT" --after no-reset erase-region 0x9000 0x6000 && \
python -m esptool --chip esp32h2 -p "$SPARE_PORT" --after no-reset write-flash 0x187000 "$IMG" && \
(cd "$BLE_BUILD" && python -m esptool --chip esp32h2 -p "$SPARE_PORT" -b 460800 \
  --before default-reset --after hard-reset write-flash @flash_args) && \
idf.py -C power_base -B "$BLE_BUILD" -D SDKCONFIG="$BLE_BUILD/sdkconfig" -p "$SPARE_PORT" monitor
# 기대: "loaded: pb_bridge enabled=false", "enrollment endpoint policy: endpoint_required",
#       "AWAIT_COMMISSIONING: enrollment endpoint policy = required", "boot: not enrolled (none)",
#       "safe wait: BLE enrollment open as 'POGO-PB-744DBDFFFE65C00A'". exporter 로그는 나오면 안 된다.
```

1-b) 앱(DEBUG 빌드)의 BLE 등록 시험에서 이 보드를 골라 EUI-64와 설정 코드를 넣고 시작한다. 기록할 것:

- 앱: 허브 세션이 `preparing` → `waiting_for_heartbeat`로 가고 화면에 `bridge_endpoint: [주소]:port`가
  보인다. 그 주소가 0)의 `ipaddr mleid`와 같고 port가 허브의 ingest port와 같은지 대조한다.
- 보드 로그: `start accepted (generation 1, with a bridge endpoint)` →
  `Joiner started (attempt 1, bridge endpoint held in RAM)` → `Joiner succeeded` →
  `enrolled: ... (v2, with the delivered bridge endpoint)` → `BLE enrollment stopped (enrolled)` →
  `pb_bridge: using the endpoint delivered with the enrollment, port=<port>` →
  `linux bridge exporter ready` → `power base ready`. 로그에 주소가 나오면 안 된다.
- 허브: 그 세션이 `heartbeat_received`로 끝나고 journal에 `[heartbeat]`가 그 EUI-64로 찍힌다.
  **성공 판정은 이것뿐이다.** 앱의 ACCEPTED, 보드의 enrolled는 성공이 아니다.
- 허브 journal의 `pogohome_api: enrollment <id>: bridge endpoint fixed for this session (port N)` 한 줄과
  Joiner 추가가 그 뒤에 오는 순서.

**시험 2: 전원 재연결 → 등록 목적지 복원 → 같은 허브로 heartbeat 도착**

시험 1이 `heartbeat_received`로 끝난 보드로 한다. 앱은 쓰지 않는다.

```bash
# 2-a) 허브에서 그 Base가 offline이 될 때까지 USB를 뽑아 둔 뒤 다시 꽂고 로그를 본다.
idf.py -C power_base -B "$BLE_BUILD" -D SDKCONFIG="$BLE_BUILD/sdkconfig" -p "$SPARE_PORT" monitor
# 기대: "loaded: pb_bridge enabled=false"(이미지에는 여전히 주소가 없다),
#       "boot: enrollment marker and stored Dataset match -- rejoining that network (record v2: bridge endpoint restored)",
#       "pb_bridge: using the endpoint delivered with the enrollment, port=<port>",
#       "linux bridge exporter ready", "power base ready".
#       "BLE enrollment" 로그와 POGO-PB-... 광고는 없어야 한다.
```

```bash
# 2-b) 허브 (읽기 전용): 전원 재연결 뒤의 새 heartbeat
journalctl -u pogo-matter-bridge --since "5 min ago" | grep -i heartbeat
curl -s http://<허브>:8787/api/v1/snapshot    # 그 uuid의 Power Base가 online, last heartbeat가 재연결 뒤 시각
```

두 시험 모두 실패 시 보드 로그의 마지막 `pb_enroll`/`pb_bridge_config` 줄과 허브 세션의 `error`를 함께
기록한다. 되돌리기는 위 "다음 실물 시험 절차"의 백업 복원, 등록만 지우려면
`erase-region 0x9000 0x6000`.

### 후속 기록: Linux 배포 검증과 실물 시험 산출물 준비 (2026-10-05)

위 2026-10-03, 2026-10-04의 기록과 표는 고치지 않고 그대로 둔다. 이 절은 그 뒤에 생긴 두 가지를 따로
적는다.

- ① 운영 Linux 허브에 새 bridge와 새 OTBR helper를 빌드·설치·재시작하고 검증한 결과. **사용자가
  Linux에서 직접 실행한 출력으로 확인한 것**이다. 이 문서를 고친 작업(Mac)에서는 Linux에 접속하지
  않았고 그 명령들을 실행하지 않았다.
- ② 여분 Power Base `744dbdfffe65c00a`용 실물 시험 산출물 준비. Mac에서 빌드·생성·대역 검증만 했다.
  보드에는 접근하지 않았다(시리얼 접근, flash, erase, 실물 BLE 연결, 등록 시작·취소 없음).

**Linux 배포 검증은 끝났고, Power Base 주소 자동 전달의 실물 검증은 아직 하나도 하지 않았다.** 이
둘은 다른 것이다. ①에서 새 helper가 주소를 돌려준 것은 helper를 단독으로 호출한 결과다. 등록 세션이
`bridge_endpoint`를 내주는지, 그 값이 BLE로 보드에 전달되는지, 보드가 그것을 저장하고 재부팅 뒤
복원하는지의 근거가 아니다.

추가(2026-10-05, 실물 시험 뒤): 위 굵은 문장과 이 절의 "현재 상태", "다음 실물 시험 순서"는 산출물 준비
당시, 곧 실물 시험 전의 기록이다. 같은 날 이 산출물로 실물 시험이 실행됐고 결과는 아래 "실물 후속 검증
(2026-10-05)"에 따로 적었다. 준비 당시의 판정과 표는 고치지 않았다. 나중에 바꾼 것은 ①의 운영 환경
식별값을 치환 표기로 옮긴 것뿐이다(①의 설명).

#### ① Linux 배포 검증 (사용자 실행, 사용자가 보고한 출력)

검증 디렉터리: `/home/<USER>/pogo-hub-endpoint-verify-20261004/run2`

이 절의 운영 환경 식별값은 치환해 적었다. `<OTBR_MLEID>`(허브 OTBR의 Mesh-Local EID), `<HUB_ID>`,
`<SETTINGS_REVISION>`, `<USER>`(운영 Linux 계정), `<DEPLOY_TIMESTAMP>`(배포 백업 디렉터리 이름에 든
시각)는 사용자가 보고한 출력의 원문이 아니라 치환 표기이고, 같은 표기는 같은 값을 가리킨다. 실제 값은
이 문서에 싣지 않는다. 치환은 검증의 뜻을 바꾸지 않는다. helper가 돌려준 주소는 관리자가 `ot-ctl`로
조회한 주소와 같았고, `hub_id`와 `settings.json`은 배포 전후에 보존됐으며, 배포 전의 백업이 만들어졌다.
SHA-256, 날짜와 시각, 시험 보드의 EUI-64, port는 치환하지 않았다.

| 항목 | 결과 | 비고 |
|---|---|---|
| A: 환경, 서비스 정보 | 환경 PASS, 서비스 정보 COMPLETE | |
| B: B1~B10 | 모두 PASS | ingest·core·API 빌드와 test, GN/CHIP 빌드, 산출물 검사 |
| 첫 실행(run1) | 실패 | SDK `activate.sh`가 설정되지 않은 `PW_ACTIVATE_SKIP_CHECKS`를 읽어 실패했다. run2는 그 변수를 빈 문자열로 정의했고 환경 검사를 유지한 채 통과했다. 영구 소스 수정은 하지 않았다 |
| 새 bridge | SHA-256 `c277f4276a508012c44548e8c812cd9a2b66d1661c1879b018991f39fd71b913` | |
| 설치한 새 helper(CMake 빌드) | SHA-256 `00624133798770d2eb148bc3e2e331083cdda0a451cc1326f844231684745800` | `root:root` 0755, 상위 디렉터리도 root 소유 |
| sudo 규칙 | sudo 인증 캐시를 비운 상태에서 추가 인자를 붙인 실행이 차단됨 | sudoers는 이전과 동일 |
| helper `commissioner-state` | `ok disabled` | |
| helper `mesh-local-eid` | `ok mleid <OTBR_MLEID>` | 관리자가 실행한 `ot-ctl ipaddr mleid` 결과와 일치 |
| 새 bridge 설치·재시작 | `BRIDGE-OK` | |
| `hub_id` | `<HUB_ID>` | |
| `settings_state`, `settings_revision` | `ready`, `<SETTINGS_REVISION>` | |
| `power_base_enrollment` | `available` | |
| TLS 인증서 지문, 실행 인자, unit·drop-in, sudoers | 이전과 동일 | |
| 상태 보존 | `STATE-PRESERVED` | 서비스 정지 상태에서 state-dir 사본을 만든 뒤 교체했다. 사본과 비교해 `endpoint_map.bin`, `hub_id`, `devices.json`, `settings.json`, `tls_cert.pem`, `tls_key.pem`의 보존을 확인했다. `devices.json` 비교는 `last_used_at_unix_ms`의 정상적인 전진만 허용했다. `pairing.json`은 전후 모두 없었다 |
| 서비스 최종 상태 | `ActiveState=active`, `MainPID=7071`, `NRestarts=0` | |
| 백업 | `/var/backups/pogohome-deploy-<DEPLOY_TIMESTAMP>` | root 전용 |

이 결과가 2026-10-03 "실행하지 못한 것" 표에서 해소한 항목:

- Linux GCC로 허브 API·helper 빌드와 test: B가 통과했다.
- connectedhomeip(GN) `pogo_matter_bridge` 빌드: B의 GN/CHIP 빌드가 통과했다. `app/chip/main.cpp`의
  변경이 Linux에서 컴파일됐다는 근거는 이 통과다(해당 파일을 따로 짚은 출력은 보고에 없다).
- 실제 `ot-ctl ipaddr mleid`에 대한 helper 동작: 주소 한 건을 돌려줬고 `ot-ctl`의 값과 같았다. 거부해야
  하는 출력(RLOC, 주소 목록 등)을 실제 OTBR로 만든 기록은 없다. 그 부분은 host 확인 그대로다.
- 운영 허브에 새 bridge·새 root helper 설치, sudoers 규칙 그대로 동작: 위 표.

이 결과로 해소되지 않는 것: 같은 표의 나머지 줄 전부(실제 보드의 `0x04` 수락, 실제 `nvs_flash`의
56바이트 record 기록·복원, 실제 exporter의 송신, 실제 BLE의 `proto-ver`·`0x04` 교환, 앱 → 허브 세션 →
BLE `0x04` → `heartbeat_received` 전 구간, BLE 시험 빌드의 파티션·stack 여유). 보고된 출력에는 등록
세션을 연 결과가 없다. 세션 응답의 `bridge_endpoint` 값과 그 port가 실제 ingest port인지도 아직 본 적이
없다.

#### ② 실물 시험 산출물 준비 (Mac, 실행한 것은 빌드·생성·대역 검증뿐)

기준: 펌웨어 저장소 `feature/unified-module-role-manager` @ `ff11a07` + 미커밋 변경(수정 43개, 미추적
1개), 앱 저장소 `main` @ `056e02c` + 미커밋 변경(수정 17개, 미추적 1개). HEAD만 빌드하지 않았고 과거
시험 이미지를 다시 쓰지 않았다. 커밋·stage하지 않았다. 작업 전후에 두 저장소의 HEAD·브랜치·index와
파일별 체크섬을 비교했고, 이 문서 외에는 달라진 파일이 없었다.

산출물은 저장소 밖 `/Users/junsoona/esp-builds/pogo_bridge_endpoint_hwtest_20261005`에 있다(실행 안내는
그 안의 `README.md`).

| 항목 | 결과 | 근거 |
|---|---|---|
| ESP-IDF v6.1 `power_base` BLE 시험 빌드(esp32h2) | 빌드 확인, 경고 0 | 새 build 디렉터리와 새 `SDKCONFIG`(`sdkconfig.defaults;sdkconfig.ble_enrollment`에서 새로 생성). 기존 `power_base/sdkconfig`와 이전 build 디렉터리는 쓰지 않았다. `0x155b00`(1,399,552) bytes, factory `0x177000`에 `0x21500` bytes(9%) 여유 |
| 최종 설정 | 확인 | `CONFIG_POWER_BASE_BLE_ENROLLMENT=y`, `CONFIG_POWER_BASE_BENCH_JOINER_CONSOLE` 꺼짐, `CONFIG_OPENTHREAD_JOINER=y`, `OPENTHREAD_CLI`·`CONSOLE_ENABLE`·`COMMISSIONER` 꺼짐, protocomm Security 2만 켜짐(0·1 꺼짐). 1785개 key가 build의 `sdkconfig.json`과 일치하고 2026-10-01 BLE 빌드의 sdkconfig와 차이 0 |
| 이미지 | `power_base.bin` SHA-256 `2b535d759122e4ab8dc0e17affcda9a84cfa02ffaeebcb9627ddd1f518275a5b`, `power_base.elf` `f0cac123eceff253e66f05a2380ae7892ffe81408cfdebff2cbe58f23929b0a3`, `partition-table.bin` `89884ce7b6fe7d6a3146a7a53e919ebbc0171405ff85e8e8fb23b345ce264045` | `esptool image-info`(파일만): chip ID 16, 내장 ELF SHA-256 일치, 이미지 해시 유효. 이미지에 이 기능의 로그 문자열(`with a bridge endpoint`, `enrollment endpoint policy`, `record v2: bridge endpoint restored`)과 capability `bridge_endpoint_v1`이 있고 `pb_join`은 없다. 파티션 표는 2026-10-03 release의 것과 같은 파일이다 |
| 준비 이미지(cfg) | 생성, 다시 읽어 확인 | 현재 `tools/gen_power_base_ble_config.py --eui64 744dbdfffe65c00a --bridge-from-enrollment --existing-setup-code-file …`. 기존 설정 코드는 도구가 파일에서 읽어 다시 썼다(새 코드를 만들지 않았고, 코드를 화면·로그·인자에 내지 않았다). 이미지 0600, 보관 디렉터리 0700. 생성 도구와 별개의 검사(ESP-IDF `nvs_parser`)로 확인: namespace 4개, `pb_thread`는 `mode`=2 하나뿐(Dataset 없음), `pb_bridge`는 enabled=0·주소 0·port 0·정책 1·CRC 유효, `pb_ble_auth`는 `744dbdfffe65c00a`에 묶이고 record CRC 유효, 배선 blob은 `configs/power_base.json`을 pack한 값과 같고 이 보드의 이전 이미지의 것과도 같다. 이미지 안에 허브의 현재 mesh-local EID나 그 prefix 바이트가 없다. salt는 새로 뽑으므로 `pb_ble_auth` 바이트는 이전 이미지와 다르다 |
| 업데이트 도구(산출물 디렉터리의 `update/`) | 대역 검증 93개 검사 통과 | 가짜 esptool과 bytearray flash로만 실행했다(시리얼 포트를 열지 않음). 포트를 인자로만 받음, 다른 EUI-64(SGP41·PMS 포함)·다른 칩·Secure Download Mode·작은 flash·다른 파티션 표·해시 불일치·cfg 내용 위반(Dataset 있음, 정적 주소·port, 정책 없음, 다른 보드, 배선 변경)·미확인에서 erase·write 전에 중단, 중간 실패 시 뒤 단계와 재부팅을 하지 않음, 완전한 실행이 app·cfg·nvs 세 범위만 바꿈. 방어를 하나씩 깨뜨린 사본 11개 모두에서 자가검증이 실패하고, 바꾸지 않은 사본에서는 통과하는 것도 확인했다 |
| 앱 기기용 Debug 빌드 | 빌드 확인 | 별도 DerivedData, `generic/platform=iOS`, `CODE_SIGNING_ALLOWED=NO`. arm64, `-DDEBUG`, `PowerBaseEnrollmentBLETestView`가 들어 있다. 서명이 없어 설치할 수 없는 산출물이고 설치·실행하지 않았다 |

이번에 다시 실행하지 않은 것: 2026-10-03·04의 host test 전체와 앱 test. 그 결과는 위 표의 기록 그대로다.

업데이트 도구는 2026-10-03의 "app만 쓰고 등록 정보를 보존하는" 도구와 다르다. 새 등록을 시험하려면
미등록 상태가 필요하므로 app 기록, cfg 기록, **동적 nvs 초기화**를 한다. 전체 flash erase와
bootloader·파티션 표·phy 영역 기록은 하지 않는다. 위 2026-10-04 절차의 1-a)는 고정 주소로
`erase-region`·`write-flash`를 직접 실행하는 방식인데, 이번 시험에서는 그 대신 보드의 실제 파티션 표를
읽어 대조한 뒤 진행하는 이 도구를 쓴다. 1), 1-a)가 가리키는 `$BLE_BUILD`와 `build/cfg_images`의 이미지도
이번 시험의 대상이 아니다.

#### 현재 상태 (산출물 준비 당시)

아래 표의 "실물 미검증"은 산출물 준비 당시(실물 시험 전)의 판정이다. 실물 시험 뒤의 판정은 "실물 후속
검증 (2026-10-05)"의 "현재 상태 (실물 시험 뒤)".

| 구간 | 상태 |
|---|---|
| Linux: 새 bridge·helper의 빌드, 설치, 재시작, 상태 보존, sudo 규칙 | 검증(2026-10-05, 사용자 실행) |
| Linux: 실제 OTBR에 대한 helper `mesh-local-eid` | 검증(사용자 실행, 주소 한 건) |
| 허브 등록 세션이 `bridge_endpoint`로 현재 OTBR 주소와 실제 ingest port를 내주는지 | **실물 미검증** |
| 실제 BLE로 `proto-ver` capability와 `0x04` 교환, 보드의 `0x04` 수락 | **실물 미검증** |
| Joiner 성공 → Thread attach → record v2 저장(실제 `nvs_flash`) → exporter가 전달받은 목적지로 송신 | **실물 미검증** |
| 허브 세션의 `heartbeat_received`와 정확한 UUID의 새 heartbeat | **실물 미검증** |
| 전원 재연결 뒤 record v2 복원 → 목적지 적용 → exporter 재시작 → 새 heartbeat | **실물 미검증** |
| 실제 `nvs_flash`가 정책 바이트가 든 `pb_bridge` blob을 읽는지 | **실물 미검증** |
| 목적지 없는 구형 start(`0x02`) 거부 | host 확인(2026-10-04)뿐, **실물 미검증**. 구형 start만 보내는 실물 클라이언트를 준비하지 않았다(새 앱은 `0x02`로 바꿔 보내지 않는다). 2026-10-04에 적은 "구형 클라이언트로 먼저 시도"는 이번 준비에 포함되지 않았다 |
| 업데이트 도구의 실제 esptool·보드 동작 | 미실행(대역만) |

#### 다음 실물 시험 순서 (사용자 실행, 준비만 됨)

"준비만 됨"은 준비 당시의 상태다. 아래 순서 가운데 1~4와 6이 2026-10-05에 실행됐다. 5의 대조는 자료가
남지 않았다. 7은 앱 화면의 `heartbeat_received`까지이고 그 세션의 허브 journal은 자료가 없다. 8은 전원
재연결이 아니라 RTS 재시작으로만 실행됐다("실물 후속 검증 (2026-10-05)").

명령은 산출물 디렉터리의 `README.md`에 있다. 설정 코드·PSKd·Dataset·토큰은 어떤 명령에도 인자로 넣지
않는다.

1. 대상 보드와 포트를 직접 확인하고 센서 모듈을 분리한다. 마지막으로 확인된 Mac 포트는
   `/dev/cu.usbmodem113301`이지만 현재 값인지는 확인하지 않았다.
2. 업데이트 도구로 새 펌웨어와 새 cfg를 쓰고 동적 nvs를 초기화한다.
3. 부팅 로그에서 미등록 안전 대기(`boot: not enrolled (none)`, `enrollment endpoint policy = required`)와
   BLE 광고(`POGO-PB-744DBDFFFE65C00A`)를 확인한다.
4. 서명해 설치한 새 Debug 앱의 BLE 등록 시험 화면에 대상 EUI-64와 기존 보드별 설정 코드를 입력한다.
5. 등록을 시작하고, 허브 세션의 `bridge_endpoint`가 허브의 현재 OTBR 주소와 실제 ingest port인지
   대조한다.
6. 보드 로그에서 `start accepted (…, with a bridge endpoint)` → `Joiner succeeded` → attach →
   `enrolled: … (v2, with the delivered bridge endpoint)` → `linux bridge exporter ready`를 확인한다.
7. 앱의 `heartbeat_received`와, 허브에서 세션 시작 뒤 시각에 그 UUID로 찍힌 새 heartbeat를 확인한다.
   보드의 enrolled 표시나 남아 있던 online 표시만으로는 성공이 아니다.
8. 전원을 다시 연결해 `record v2: bridge endpoint restored` → 목적지 적용 → exporter 시작과, 재연결 뒤
   시각의 새 heartbeat를 확인한다.

### 실물 후속 검증 (2026-10-05)

출처: 사용자가 2026-10-05(Asia/Seoul)에 여분 Power Base 1대에서 직접 실행한 시험의 기록이다. 터미널 기록
4개(A~D: 업데이트 도구 출력과 `esp_idf_monitor --timestamps` 로그), 앱 최종 화면 1장(E), 사용자가
Linux 허브에서 실행해 전달한 journal·snapshot 출력을 검토해 적었다. 이 문서를 고친 작업에서는 보드·시리얼·
BLE·Linux에 접속하지 않았고 flash·erase·등록 시작·취소·서비스 조작을 하지 않았으며 host test와 빌드도
다시 실행하지 않았다. 설정 코드, PSKd, Dataset, 토큰, private-cfg의 내용은 읽지 않았고 이 문서에 옮기지
않았다.

- A~D는 원문 파일을 읽었다. 시각은 monitor가 붙인 Mac 호스트 시각이고 괄호 안은 보드 uptime(초)이다.
- E는 화면 파일이 이미 지워져 이 작업에서 직접 보지 못했다. 아래 E의 내용은 Codex가 검토한 사용자 제공
  화면의 판독을 옮긴 것이다.
- Linux 출력은 작업 지시에 옮겨 적힌 줄만 봤다. 줄 가운데 `…`로 생략된 부분은 받은 그대로다.

보드 로그는 보드의 동작을, 허브 journal은 허브가 받은 것을, 앱 화면은 앱이 표시한 것을 보여 준다. 아래에서
세 근거를 한 항목으로 합치지 않는다. 두 번의 등록(허브 세션 `60a15054`와 `74417a8e…`)도 합치지 않는다.

#### 대상과 기록

- 보드: 여분 Power Base 1대, EUI-64 `744dbdfffe65c00a`. 업데이트 도구가 eFuse의 EUI-64
  `74:4d:bd:ff:fe:65:c0:0a`를 읽어 기대값과 같다고 출력했다(C). 센서 모듈은 분리했다(사용자 보고. 보드
  로그의 `pogo_port ... WAIT_ATTACH`와 허브 heartbeat의 `node_count=0`이 이와 어긋나지 않는다).
- 이미지: "후속 기록" ②의 산출물. 도구의 `Inputs verified` 출력(C)이 `power_base.bin` SHA-256
  `2b535d759122e4ab8dc0e17affcda9a84cfa02ffaeebcb9627ddd1f518275a5b`, `power_base.elf` SHA-256
  `f0cac123eceff253e66f05a2380ae7892ffe81408cfdebff2cbe58f23929b0a3`을 보였다. A·B·D의 부팅 로그는 모두
  App version `mvp-pairing-v1-40-gff11a07-dirt…`, Compile time `Oct  5 2026 00:35:45`, ELF file SHA256
  `f0cac123e...`이다. 2nd stage bootloader의 compile time은 세 부팅 모두 `Oct  1 2026 11:32:18`로, 이날
  빌드한 것이 아니다.
- 준비 cfg: 도구가 기록 전에 다시 읽어 출력한 값(C)은 `pb_thread mode=2 keys=['mode']`,
  `pb_bridge enabled=0 policy=1 address-zero=True port=0`, `pb_ble_auth EUI-64=744dbdfffe65c00a`,
  `wiring=verified`다. 곧 mode 2, Dataset 없음, bridge 주소·port 없음, 목적지 필수 정책 1이다.
- 포트: A는 `/dev/cu.usbmodem1401`, B·C·D는 `/dev/cu.usbmodem113301`.

| 기록 | 시각 | 내용 |
|---|---|---|
| A | 01:06:13 부팅 | 첫 적용(도구 출력의 뒷부분)과 미등록 부팅. 01:06:14의 BLE 대기에서 끝난다 |
| B | 11:35:08 부팅 | 등록된 보드를 monitor가 RTS로 재시작한 부팅 |
| C | 도구가 만든 백업 이름의 시각 02:42:33Z, 02:42:52Z | 두 번째 적용. 첫 실행은 중단, 다음 실행은 완료 |
| D | 11:43:56 부팅 ~ 11:45:12 | 두 번째 적용 뒤의 미등록 부팅과 두 번째 등록 전체 |
| E | 파일 이름의 시각 11:45:15 | 두 번째 등록의 앱 최종 화면 |

A가 끝난 뒤부터 B가 시작하기 전까지의 보드 로그는 제공된 기록에 없다. 첫 번째 등록 시도의 보드 쪽 진행
(start 수락, Joiner, record 저장)은 그 사이에 있었고, 그 로그를 본 적이 없다.

#### 업데이트 도구의 실제 동작 (A, C)

"후속 기록" ②에서 대역으로만 검증했던 도구가 실제 esptool과 실제 보드에서 실행됐다.

- 기록 전 확인(C, 두 실행 모두): 입력 파일의 SHA-256, cfg 내용, 칩 `ESP32-H2 (revision v0.1)`, eFuse
  EUI-64 일치, flash 4MB, `Partition table on the board matches: nvs 0x9000+0x6000, phy_init 0xf000+0x1000,
  factory 0x10000+0x177000, cfg 0x187000+0x3000`. 이어서 보드의 현재 nvs와 cfg를 0700/0600의 private
  디렉터리에 백업했다고 출력했다. 백업의 내용은 보지 않았다.
- C의 첫 실행: `[2/3] CFG` 확인 줄에 `wirte-cfg`가 보이고 도구가
  `ABORTED: 'write-cfg' not confirmed -- nothing was erased or written`으로 끝났다. 쓰기 전에 중단됐다.
- C의 다음 실행과 A: 세 확인을 지나 app 1,399,552 bytes를 `0x10000`에 쓰고 `Hash of data verified`,
  `app: written and verified at 0x10000`. cfg 12,288 bytes를 `0x187000`에 쓰고 `Hash of data verified`,
  `cfg: written and verified at 0x187000`. `nvs: erased and verified blank at 0x9000 (24576 bytes)`.
  RTS로 재시작한 뒤
  `Done: app + cfg written, nvs erased, all verified; bootloader/partition table/phy_init unchanged; board reset.`
- 보존 영역: 도구가 `NOT touched: bootloader 0x0..0x7fff, partition table 0x8000..0x8fff, phy_init
  0xf000..0xffff`와 위 `Done` 줄을 출력했다. 이 영역의 보존은 도구 출력이 근거다. 위 bootloader compile
  time이 이와 어긋나지 않는다. 이 문서를 고친 작업에서 flash를 다시 읽어 대조하지는 않았다.
- 터미널에 보이는 입력: C의 두 실행 모두 `write-app` 확인 줄에는 `rite-app`이 보이고 도구는 다음 단계로
  넘어갔다. 같은 기록에서 그보다 앞의 출력 두 줄 머리에 `w` 한 글자가 붙어 있다(`wChip:`, `wSaved`).
  실제로 입력된 문자열이 무엇이었는지는 이 기록만으로 정하지 않는다. A에서는 세 확인 줄에 `write-app`,
  `write-cfg`, `erase-nvs`가 보인다.

다른 보드·다른 칩·다른 파티션 표·해시 불일치에서 도구가 중단하는지는 이번에 실물로 만들지 않았다. 그
부분은 대역 검증 그대로다.

#### 미등록 부팅 (A, D의 앞부분)

두 번의 적용 뒤 부팅에서 같은 로그가 나왔다(A 01:06:13~14, D 11:43:56).

- `pb_thread_config: loaded: mode=AWAIT_COMMISSIONING (Thread remains disabled)`
- `pb_bridge_config: loaded: pb_bridge enabled=false -- bridge exporter will not start`
- `pb_bridge_config: enrollment endpoint policy: endpoint_required`,
  `AWAIT_COMMISSIONING: enrollment endpoint policy = required`
- `pb_enroll: boot: not enrolled (none) -- Thread stays disabled`
- `safe wait: BLE enrollment open as 'POGO-PB-744DBDFFFE65C00A' for up to 900 s`

등록 전에는 서비스 시작, exporter, `power base ready` 로그가 없다(A는 기록 끝까지, D는 11:44:57 전까지).
정책 줄은 실제 `nvs_flash`가 정책 바이트가 든 `pb_bridge` blob을 읽어 `endpoint_required`로 해석했다는
근거다. 광고를 스캐너로 대조한 기록은 없다.

#### 첫 번째 등록 세션 (허브 세션 `60a15054`): 성공이 아니다

허브 journal(사용자 전달, 시각은 받은 자료에 없다):

- `started … for Power Base 744dbdfffe65c00a (120 s window, with bridge endpoint delivery)`
- `bridge endpoint fixed for this session (port 5684)`
- `started the OTBR Commissioner`
- `Joiner 744dbdfffe65c00a allowed for 120 s`
- `cancelled by device …`
- `cancelled`

이 세션의 최종 상태는 `cancelled`다. **이 세션을 등록 성공으로 기록하지 않는다.** `heartbeat_received`에
이르지 않았고, 이 세션의 앱 화면과 보드 로그는 제공된 자료에 없다.

- `cancelled by device …`는 허브 등록 API의 취소 처리 경로가 찍는 줄이다(`enrollment.cpp`의 `Cancel()`).
  페어링된 기기의 취소 요청으로 세션이 끝났다는 것까지가 journal의 내용이다. 그 경로는 이미 도착한
  heartbeat가 있으면 취소하지 않으므로, 취소가 받아들여진 시점까지 허브는 이 세션에서 그 Base의 heartbeat를
  받지 못했다.
- 사용자는 이때 앱을 닫았다고 보고했다. 그 취소 요청이 왜 나갔는지는 기록에 없으며, 단순한 백그라운드
  전환이 원인이라고 단정하지 않는다. 앱의 백그라운드·foreground 전환 중 동작은 이번에 조건을 나눠
  시험하지 않았다.
- 그런데도 보드에는 등록 결과가 남아 있었다(아래 B). 허브 세션이 취소로 끝난 것과 보드가 record를 저장한
  것은 서로 다른 사실로 둔다. 보드가 언제 어떤 순서로 등록을 끝냈는지는 로그가 없어 적지 않는다.

#### 등록된 보드의 RTS 재시작과 복원 (B)

B는 monitor를 열 때 일어난 RTS 재시작이다(`rst:0x15 (USB_UART_HPSYS)`, `app_main: entry
(reset_reason=11)`). **전원을 완전히 끊었다가 다시 넣은 시험이 아니다.**

| 시각 (uptime 초) | 보드 로그 |
|---|---|
| 11:35:08 (0.44) | `pb_thread_config: loaded: mode=AWAIT_COMMISSIONING`, `loaded: pb_bridge enabled=false`(이미지에는 여전히 주소가 없다) |
| 11:35:08 (0.49) | OpenThread가 저장된 `NetworkInfo`를 읽음(`role:router`) |
| 11:35:08 (0.52) | `enrollment endpoint policy: endpoint_required`, `enrollment endpoint policy = required` |
| 11:35:08 (0.53) | `stored-dataset start: Thread enabled on the stored complete Dataset (contents hidden)` |
| 11:35:08 (0.53) | `boot: enrollment marker and stored Dataset match -- rejoining that network (record v2: bridge endpoint restored)` |
| 11:35:08 (0.56) | `AWAIT_COMMISSIONING: enrolled -- enabling the power rail and the normal attach supervisor` |
| 11:35:08 (0.56) | `pb_bridge: using the endpoint delivered with the enrollment, port=5684 (host address never logged)` |
| 11:35:09 (0.96~0.98) | `Role detached -> router`, `thread radio: role=3(ROUTER)` |
| 11:35:09 (0.99) | `starting services`, `CoAP power-base transport ready`, `linux bridge exporter ready ... port=5684 ... heartbeat_interval_ms=10000` |
| 11:35:09 (1.00) | `pogo_port: port 1: WAIT_ATTACH`, `power base ready: uuid=..c00a otDeviceRole=3(ROUTER)` |

- 보드는 `cfg`의 `pb_bridge`가 꺼진 상태에서, 저장돼 있던 version 2 record의 목적지를 복원해 적용했고
  Thread ROUTER로 붙은 뒤 exporter를 다시 시작했다. 로그에 주소는 없다.
- 이 record는 첫 번째 등록 시도에서 보드에 저장된 것이다. 정책이 `required`인 보드는 version 1 record를
  재개하지 않으므로(host 확인), 이 재개 자체가 version 2 record가 실제 `nvs_flash`에 있었다는 근거다.
  record의 바이트를 읽어 대조한 것은 아니다.
- B에는 `BLE_INIT`, `protocomm_nimble`, `NimBLE`, `BLE enrollment` 로그가 한 줄도 없다. 등록된 보드는 이
  재시작에서 BLE 등록을 다시 열지 않았다. 스캐너로 광고가 없음을 확인한 기록은 없다.

재시작 뒤의 허브 journal(사용자 전달):

```text
2026-10-05T11:35:19+09:00 [heartbeat] pb=..c00a uptime_ms=10010 node_count=0
2026-10-05T11:35:29+09:00 [heartbeat] pb=..c00a uptime_ms=20030 node_count=0
2026-10-05T11:35:39+09:00 [heartbeat] pb=..c00a uptime_ms=30070 node_count=0
2026-10-05T11:35:49+09:00 [heartbeat] pb=..c00a uptime_ms=40100 node_count=0
2026-10-05T11:35:59+09:00 [heartbeat] pb=..c00a uptime_ms=50130 node_count=0
2026-10-05T11:36:09+09:00 [heartbeat] pb=..c00a uptime_ms=60150 node_count=0
2026-10-05T11:36:19+09:00 [heartbeat] pb=..c00a uptime_ms=70170 node_count=0
```

같은 시기의 snapshot 조회 결과(사용자 전달): `여분 Base: 744dbdfffe65c00a online: True`.

- heartbeat 7건이 10초 간격으로 도착했고 `uptime_ms`가 10010에서 시작한다. 재시작 전부터 남아 있던
  표시가 아니라 이 부팅(11:35:08~09) 뒤에 보드가 새로 보낸 heartbeat다.
- journal은 UUID의 끝부분(`..c00a`)만 출력한다. 전체 UUID `744dbdfffe65c00a`를 확인한 것은 snapshot이다.

#### 두 번째 적용과 두 번째 등록 (C, D, E)

C의 완료된 실행으로 보드가 다시 미등록이 된 뒤(동적 nvs 초기화) D에서 한 번 더 등록했다.

| 시각 (uptime 초) | 보드 로그 (D) |
|---|---|
| 11:43:56 (0.51) | 미등록 부팅, BLE 등록 창 열림(위 "미등록 부팅") |
| 11:44:19 (22.89 → 23.54) | 연결, `mtu update event ... mtu=256`, Security 2 handshake 시작 로그 |
| 11:44:21 (25.02) | 다시 광고. 이 연결에서는 start 로그가 없다 |
| 11:44:45 → 11:44:46 (49.35 → 50.13) | 연결, MTU 256, handshake 시작 로그 |
| 11:44:49 (53.01) | `pb_ble_adm: start accepted (generation 1, with a bridge endpoint) and queued for the enrollment worker -- not a Joiner result` |
| 11:44:49 (53.05) | `pb_enroll: Joiner started (attempt 1, bridge endpoint held in RAM) -- this is not enrollment yet` |
| 11:44:55 (59.29) | `pb_enroll: Joiner succeeded -- starting Thread on the joined credentials (not enrolled until attached)` |
| 11:44:56 (60.40 → 60.44) | `Role detached -> child`, `thread radio: role=2(CHILD)` |
| 11:44:56 (60.54) | `pb_enroll: enrolled: attached with a complete Active Dataset and the record is committed (v2, with the delivered bridge endpoint)` |
| 11:44:56 (60.54) | `BLE enrollment closes for this boot (enrolled)`, 광고 중지, `terminate connection; conn_handle=0 hci_reason=19` |
| 11:44:57 (60.60) | `E protocomm_nimble: Error setting advertisement data; rc = 30` |
| 11:44:57 (60.60) | `BLE enrollment stopped (enrolled): advertising, connection and session are down` |
| 11:44:57 (60.60) | `pb_bridge: using the endpoint delivered with the enrollment, port=5684 (host address never logged)` |
| 11:44:57 (60.60 → 60.61) | `starting services`(CHILD), `CoAP power-base transport ready`, `linux bridge exporter ready ... port=5684` |
| 11:44:57 (60.61) | `pogo_port: port 1: WAIT_ATTACH`, `power base ready: uuid=..c00a otDeviceRole=2(CHILD)` |
| 11:45:12 (75.61) | `pogo_port: port 1: alive state=WAIT_ATTACH`. 기록은 여기서 끝난다 |

- start 수락에서 Joiner 성공까지 약 6.3초, CHILD와 record 저장까지 약 7.5초였다. `rst:`는 기록 시작의
  1회뿐이고 panic·재부팅 로그가 없다.
- 11:44:54에 `MeshForwarder-: Failed to send IPv6 UDP msg ... error:Abort` notice가 1건 있다. 원인은
  확인하지 않았다. 그 1초 뒤 Joiner가 성공했다.
- BLE 종료 과정에 `Error setting advertisement data; rc = 30`이 1건 있다. 같은 기록에서 등록은 끝났고
  (record 저장, BLE 종료, 서비스 시작) 그 오류 뒤의 단계도 진행됐다. 2026-10-02의 세 기록에도 같은 오류가
  있었다. 원인은 확인하지 않았고 이번에 고치지 않았다.
- 11:44:19의 첫 연결이 start 없이 끝난 이유는 보드 로그에 없다.
- 기록이 CHILD 상태에서 끝나므로 ROUTER 승격은 이 기록에 없다. 보드 로그는 exporter의 "시작"까지만
  보여 주고, heartbeat가 허브에 도착했는지는 보드 로그에 없다.
- 두 번째 세션의 허브 journal은 제공된 자료에 없다.

앱 최종 화면(E, Codex가 검토한 사용자 제공 화면의 판독):

- 초록색 "허브가 신호를 받아 등록 확인"
- `state: heartbeat_received`
- `eui64: 744dbdfffe65c00a`
- `joiner_entry: removed`
- `session: 74417a8e…`(화면에 보이는 부분만)
- `board: running / attaching`도 함께 남아 있다.

`heartbeat_received`와 `joiner_entry: removed`는 허브 세션의 상태를 앱이 표시한 것이고, 같은 EUI-64다.
세션 id는 첫 번째 세션(`60a15054`)과 다르다. `board: running / attaching`은 보드가 마지막으로 알려 준
진행 표시가 화면에 남은 것으로, 허브의 성공 결과와 구분한다. 이 표시는 이번 작업에서 고치지 않았다.

#### 근거 수준과 검증 경계

확인된 것: 허브 → 앱 → 보드의 목적지 전달, 실제 `nvs_flash`의 version 2 저장, RTS 재시작 뒤의 복원,
허브의 heartbeat 수신(B 뒤의 journal·snapshot), 앱의 최종 성공 표시(E).

근거가 간접인 것:

- `0x04` start와 `proto-ver` capability: 실제 BLE 패킷을 캡처한 자료는 없다. 근거는 보드 로그와 소스다.
  보드는 복호화한 요청의 type이 `0x04`이고 그 요청을 수락했을 때만 `with a bridge endpoint`를 찍는다
  (`pb_ble_admission.c`). 앱은 `proto-ver`가 `bridge_endpoint_v1`을 싣지 않은 보드에는 그 start를 보내지
  않고 구형 start로 바꿔 보내지도 않는다(앱 `PowerBaseBLEBoardClient`). 그래서 capability 광고와 `0x04`
  교환이 실제 BLE에서 이뤄졌다고 본다. MTU는 256이었다.
- 전달된 주소의 값: 등록 API 응답의 `bridge_endpoint` JSON 원문과 앱 화면의 주소·port 줄을 따로 캡처한
  자료가 없다. 시험 시점에 전달된 주소가 OTBR의 Mesh-Local EID(준비 당시 ①의 `<OTBR_MLEID>` 포함)와
  정확히 같은지 직접 대조하지 않았다. 간접 근거는 세 가지다. 첫 세션의 journal이 port 5684로 목적지를 확정했다고
  찍었다. 보드는 전달받은 주소가 가입한 네트워크의 Mesh-Local Prefix 안에 있고 다시 읽은 record가 같을
  때만 `record is committed (v2, ...)`를 찍는다(`power_base_enrollment.c`). 그 목적지로 보낸 heartbeat가
  허브에 도착했다. 보드 로그의 port는 5684다.
- 두 번째 등록의 허브 수신: 앱 화면이 표시한 허브 세션 상태(E)가 근거이고, 그 세션의 journal 원문은 없다.
  journal 원문이 있는 것은 B의 재시작 뒤 구간이다.

이번 실물 시험으로 검증되지 않은 것:

- 재시작 복원은 첫 번째 등록의 보드 상태에서 확인했다. 두 번째 등록 뒤에 다시 재시작한 기록은 없다.
- 물리 전원 차단 뒤의 복원. A와 B의 시리얼 포트 이름이 다르지만(`usbmodem1401` → `usbmodem113301`), 그
  사이에 전원이 끊겼는지와 그때의 부팅 로그는 기록에 없으므로 근거로 쓰지 않는다.
- 목적지 없는 구형 start(`0x02`)의 거부. 구형 start를 보내는 실물 클라이언트로 시험하지 않았다.
- 기본 MTU 23에서의 `proto-ver`·`0x04` 교환.
- 두 Base의 동시 등록.
- 앱의 백그라운드·foreground 전환 중 경쟁 조건. 첫 세션의 취소가 그 사례인지도 정해지지 않았다.
- 등록 뒤 허브 주소가 바뀌는 경우의 자동 갱신. 이 기능은 구현돼 있지 않다.
- 인증 실패 잠금, 창 만료 뒤의 동작, 사용할 수 없는 정책 값의 이미지, 손상된 record의 거부(host 확인뿐).
- BLE 시험 빌드의 stack·heap 여유(측정하지 않았다).

이 시험은 센서 측정, POGO 체인, Matter endpoint 값의 검증이 아니다. 모듈을 분리한 상태였고
heartbeat의 `node_count`는 0이었다.

시험 이미지와 현재 소스: 시험 이미지는 `ff11a07` + 미커밋 변경을 2026-10-05 00:35:45에 빌드한 것이다.
산출물 준비 때 남긴 저장소 체크섬(00:33, 00:49 KST)과 이 문서를 고치기 직전의 체크섬을 대조했다. 추적
파일 전체의 digest가 00:49의 것과 같고, 00:33과 00:49 사이에 달라진 파일은 이 문서 하나다. 곧 빌드 전후와
지금의 펌웨어 소스 파일은 내용이 같다. 이미지를 다시 빌드해 바이트를 비교한 것은 아니고, ESP-IDF·툴체인·
`sdkconfig`는 이 대조의 범위 밖이다. 앱 저장소도 같은 두 시점과 digest가 같다. 사용자가 기기에 설치한
서명 빌드가 정확히 어느 시점의 소스로 만들어졌는지는 기록에 없다.

#### 현재 상태 (실물 시험 뒤)

| 구간 | 상태 | 근거와 경계 |
|---|---|---|
| Linux: 새 bridge·helper의 빌드, 설치, 재시작, 상태 보존, sudo 규칙, 실제 OTBR의 `mesh-local-eid` | 검증(사용자 실행) | "후속 기록" ① 그대로 |
| 업데이트 도구의 실제 esptool·보드 동작 | 실물 확인(사용자 실행, 도구 출력) | 칩·EUI-64·파티션 확인, app·cfg 기록과 해시 검증, nvs 초기화와 빈 영역 검증, 확인 문구 불일치 시 쓰기 전 중단. 보존 영역은 도구 출력이 근거. 잘못된 대상에서의 중단은 대역 검증뿐 |
| 실제 `nvs_flash`가 정책 바이트가 든 `pb_bridge` blob을 읽는지 | 실물 확인(보드 로그 A·B·D) | `endpoint_required`. 다른 정책 값은 host 확인뿐 |
| 주소 없는 이미지의 미등록 안전 대기와 BLE 등록 창 | 실물 확인(보드 로그 A·D) | 스캐너 대조 없음 |
| 허브 등록 세션이 목적지를 확정해 내주는지 | 실물 확인(일부) | 첫 세션 journal의 `bridge endpoint fixed for this session (port 5684)`. 응답 JSON 원문과 주소의 직접 대조는 없다 |
| 실제 BLE로 `proto-ver` capability와 `0x04` 교환, 보드의 `0x04` 수락 | 실물 확인(보드 로그와 소스에 따른 간접 근거, 1회) | 패킷 캡처 없음. MTU 256만 |
| Joiner 성공 → Thread attach → record v2 저장(실제 `nvs_flash`) → 전달받은 목적지 적용 → exporter 시작 | 실물 확인(보드 로그 D, 1회) | record 바이트를 읽어 대조하지는 않았다 |
| 허브 세션의 `heartbeat_received` | 실물 확인(앱 화면 E의 판독) | 두 번째 세션. 이 작업에서 화면을 직접 보지 못했고 그 세션의 journal 원문은 없다 |
| 정확한 UUID의 새 heartbeat | 실물 확인(B 재시작 뒤의 journal 7건과 snapshot) | journal은 UUID 끝부분만, 전체 UUID는 snapshot |
| 등록된 보드의 재시작 뒤 record v2 복원 → 목적지 적용 → exporter 재시작 → 새 heartbeat | RTS 재시작 1회 실물 확인(B) | 첫 번째 등록의 보드 상태. **물리 전원 차단·재투입은 실물 미확인** |
| 등록 뒤 재시작에서 BLE 등록을 다시 열지 않는지 | 실물 확인(보드 로그 B, RTS 재시작 1회) | 스캐너 확인 없음 |
| 첫 번째 허브 세션 `60a15054` | `cancelled`. 성공 아님 | 취소 요청이 나간 이유는 미확정 |
| 목적지 없는 구형 start(`0x02`) 거부 | host 확인뿐, **실물 미확인** | |
| 기본 MTU 23, 두 Base 동시 등록, 백그라운드 경쟁 조건 | **실물 미확인** | |
| 등록 뒤 허브 주소 변경의 자동 갱신·재등록 | 미구현 | |
| BLE 종료 중 `Error setting advertisement data; rc = 30` | 관측(D 1건) | 원인 미확인, 미수정. 등록 완료와 서비스 시작은 진행됐다 |
| 앱 화면에 남는 `board: running / attaching` | 관측(E) | 미수정 |

## Linux Matter Bridge

Matter SDK는 connectedhomeip v1.6.0.0으로 고정했다. module UUID마다 동적 endpoint 하나를
만들고 endpoint ID를 `endpoint_map.bin`에 저장한다. CHIP KVS와 endpoint map은 서비스
재시작 뒤에도 유지한다.

| 기능 | host | Linux/실물 |
|---|---|---|
| ingest registry, ordering, interface 검사 | 확인 | 확인 |
| endpoint ID 저장과 CRC | 확인 | 재시작 뒤 유지 확인 |
| 온도/습도 변환 | 확인 | Apple Home/Google Home에서 값 수신 확인 |
| PM1/PM2.5/PM10 변환 | 확인 | Google Home PM10, chip-tool PM 경로 확인 |
| BH1750 Illuminance 변환 | 확인 | endpoint 3에서 변화 확인 |
| Identify | SDK 의존 | BH1750/PMS5003 `SUCCESS` |
| PMS5003 Air Quality | SDK 의존 | `AirQuality: 0` 확인 |
| Reachable 반영 | 확인 | 서비스 재시작 복구 확인 |
| 센서 bitmap 변경 감지 | 확인 | 실물 교체 절차는 미확인 |
| 확정 분리 시 endpoint 삭제, 재도킹 시 같은 endpoint ID 복원 | 확인 | 미확인, v1.6.0.0 빌드 미확인 |
| 미전달 삭제의 Power Base 재부팅 후 수렴 | 확인 | 미확인 |
| Matter 인증 | 해당 없음 | 미수행 |

분리·재도킹 수명주기는 [`pogo-lifecycle.md`](pogo-lifecycle.md)에 정리했다.
`ChipEndpointManager::RemoveEndpoint()`는 v1.6.0.0 헤더(`ServerClusterInterfaceRegistry::Unregister`,
`emberAfClearDynamicEndpoint`)와 대조해 수정했지만 Linux 호스트 빌드·링크와 chip-tool/Apple Home
실물 확인은 아직 하지 않았다.

BH1750 실물 시험에서 endpoint 3의 UniqueID가 module UUID `744dbdfffe65d357`과 일치했고,
센서를 가리고 밝히는 동안 `MeasuredValue`가 0~22369 범위에서 변했다. PMS5003 endpoint
4는 조도 클러스터를 지원하지 않아 `UNSUPPORTED_CLUSTER`를 반환했으며 이는 정상이다.

SGP40/41 Gas Index는 단위 없는 알고리즘 값이므로 Matter 농도값으로 공개하지 않는다.
Air Quality Sensor device type의 필수 클러스터를 맞추기 위해 PMS5003 endpoint에는
Air Quality 클러스터를 두되, 등급을 임의 계산하지 않고 `Unknown(0)`으로 유지한다.

## RCP와 OTBR

ESP32-H2 USB Serial/JTAG RCP에서 간헐적으로 HDLC parse 오류와 `RESET_FAULT`가 발생했다.
TX short write/EIO 처리와 RX `ESP_FAIL + EIO` 처리에 제한된 재시도를 추가했고, 예산을
소진하면 조용히 RCP를 재시작한다. ESP-IDF 설치본은 수정하지 않고 linker `--wrap`을
사용했다.

확인 결과:

| 시험 | 시간 | NRestarts | 오류 패턴 |
|---|---:|---:|---:|
| 진단 RCP, 전체 3단 체인 | 1시간 | 425 → 425 | 0 |
| 운영 RCP, 전체 3단 체인 | 1시간 | 0 → 0 | 0 |

두 시험 모두 TX와 RX 보강이 함께 적용된 상태였다. 어느 한쪽 수정의 단독 효과를 분리한
시험은 아니다. 다른 USB host controller, hub, cable과 2시간을 넘는 연속 운전은 미확인이다.

## 아직 구현하지 않은 항목

| 항목 | 상태 |
|---|---|
| POGO 5V 전원 회로 | 미구현. 각 보드는 USB로 별도 공급 |
| Deep-sleep | 미구현 |
| 자동 Light-sleep | 검토 후 보류. Matter online 유지와 UART 프레임 보존 우선 |
| SGP40/41 Gas Index의 Matter 표현 | 의도적으로 제외 |
| Matter 제품 인증 | 범위 밖 |
| Apple Home/Google Home용 전용 앱 | 미구현 |
| 실제 RF 강제 조건의 Thread multi-hop | 미확인 |
| Power Base 등록 PSKd의 BLE 전달 | 펌웨어·통신 규약과 앱의 DEBUG 전용 등록 시험 화면까지 구현. 여분 보드 1대에서 등록 성공·취소·창 만료를 실물 확인(2026-10-02, "BLE 등록 실물 후속 검증"). 제품용(Release) 등록 화면은 미구현. 기본 MTU 23과 인증 실패 잠금은 실물 미확인. 등록 뒤 재시작에서 BLE 등록을 다시 열지 않는 것은 주소 자동 전달 빌드의 RTS 재시작 1회에서 보드 로그로 확인(2026-10-05, "실물 후속 검증"). 물리 전원 재투입 뒤는 실물 미확인 |
| Power Base 등록의 허브 주소 자동 전달 | 구현(2026-10-03, 위 "허브 Thread 수신 주소 자동 전달"). host test·빌드 확인과 Linux 배포 검증(사용자 실행)에 더해, 2026-10-05에 여분 보드 1대에서 실물 확인("실물 후속 검증 (2026-10-05)"): 허브 → 앱 → 보드의 목적지 전달(`0x04` 수락은 보드 로그와 소스에 따른 간접 근거), record v2 저장, RTS 재시작 뒤의 복원과 새 heartbeat, 앱의 `heartbeat_received`. **실물 미확인**: 물리 전원 차단 뒤의 복원, 구형 start(`0x02`) 거부, 기본 MTU 23, 두 Base 동시 등록, 백그라운드 경쟁 조건, 세션의 `bridge_endpoint` 원문과 OTBR 주소의 직접 대조. 최초 전달과 재부팅 복원까지이고, 등록 뒤 허브 주소가 바뀌는 경우의 자동 갱신·재등록은 미구현. 제품용(Release) 등록 화면도 미구현(위 줄) |
| Power Base 등록의 QR 흐름 | 미구현 |

## 기록 원칙

새 결과를 추가할 때는 사용한 보드, 설정 이미지, 앱 커밋, 시험 경로를 함께 적는다. 로그를
직접 확인하지 못한 결과도 실물 시험 기록으로 남길 수 있지만, 확인 범위를 넓혀 쓰지 않는다.
host test와 빌드는 실물 동작의 대체 근거로 사용하지 않는다.
