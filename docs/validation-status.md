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
| Power Base 등록 PSKd의 BLE 전달 | 펌웨어·통신 규약과 앱의 DEBUG 전용 등록 시험 화면까지 구현. 여분 보드 1대에서 등록 성공·취소·창 만료를 실물 확인(2026-10-02, "BLE 등록 실물 후속 검증"). 제품용(Release) 등록 화면은 미구현. 기본 MTU 23, 인증 실패 잠금, 등록 뒤 재부팅은 실물 미확인 |
| Power Base 등록의 QR 흐름, 허브 주소 자동 전달 | 미구현. Bridge 주소는 준비 이미지에 직접 넣는다 |

## 기록 원칙

새 결과를 추가할 때는 사용한 보드, 설정 이미지, 앱 커밋, 시험 경로를 함께 적는다. 로그를
직접 확인하지 못한 결과도 실물 시험 기록으로 남길 수 있지만, 확인 범위를 넓혀 쓰지 않는다.
host test와 빌드는 실물 동작의 대체 근거로 사용하지 않는다.
