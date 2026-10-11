# 검증 현황

최신 실물 결과(2026-10-11)는 [두 iPhone 시험 기록](release-records/2026-10-11-two-iphones-power-base.md)에 있습니다. 아래 작업별 미검증 표기는 각 작업 당시의 범위입니다.

> 공개용 기록: 보드 식별값·계정·포트·주소는 예시로 바꿨습니다. 명령 실행 시 자신의 실제 값으로 바꾸세요. 원본 근거는 비공개로 보존했습니다.


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
| Power Base `..c00a` | `020000fffe65c00a` | `power_base_ble/power_base.bin` | 1,392,848 | `70040f6c93b9dfb62fdeb5dfc6282eccdd951c477b433e92a45d546b06fe78f0` |
| SGP41 `..d357` | `020000fffe65d357` | `module/pogo_module.bin` | 885,408 | `eb9a373b6086f53d16d1e1f972e87c789bcc92f3ad38c47d95e3ac722e6ed0fb` |
| PMS `..d07a` | `020000fffe65d07a` | `module/pogo_module.bin` (SGP41과 같은 파일) | 885,408 | `eb9a373b6086f53d16d1e1f972e87c789bcc92f3ad38c47d95e3ac722e6ed0fb` |

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
로그 원문을 직접 보지 않았다. 대상은 여분 ESP32-H2 1대(EUI-64 `020000fffe65c00a`, 실제 flash
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
# 크기는 flash-id가 알려 주는 실제 값을 쓴다. 여분 보드 020000fffe65c00a는 4MB(0x400000)다.
# 2MB(0x200000)만 읽으면 4MB 보드의 전체 백업이 아니다.
mkdir -p "$HOME/pogo-spare-backups" && chmod 700 "$HOME/pogo-spare-backups"
python -m esptool --chip esp32h2 -p "$SPARE_PORT" flash-id          # "Detected flash size" 확인
FLASH_BYTES=0x400000                                                # flash-id 결과에 맞춘다
(umask 077; python -m esptool --chip esp32h2 -p "$SPARE_PORT" read-flash 0 "$FLASH_BYTES" \
  "$HOME/pogo-spare-backups/spare-before-mode2.bin")

# 2) Mac: 시험 빌드와 준비 이미지 (보드에 쓰지 않음)
cd "$HOME/pogo-thread-modular-iot"
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

- 보드: 여분 ESP32-H2 1대(EUI-64 `020000fffe65c00a`, esptool 출력의 MAC으로 확인).
- 이미지: BLE 시험 빌드(`sdkconfig.ble_enrollment`). 기록 A에서 `power_base.bin` 1,392,592 bytes를
  `0x10000`에 쓰고 `Hash of data verified`를 받았다. 위 빌드 표의 BLE 시험 빌드와 같은 크기다. 네 기록의
  부팅 로그는 App version `mvp-pairing-v1-39-g431b280-dirt…`, Compile time `Oct  1 2026 11:32:06`,
  ELF SHA256 `f0459bde9…`로 모두 같다. 2026-10-03 체인 시험의 Power Base 이미지(1,392,848 bytes)와는
  다른 빌드다.
- 준비: 기록마다 먼저 동적 NVS(`0x9000`부터 `0x6000`)를 지워 미등록 상태로 만들었다. 기록 A는 준비
  이미지(12,288 bytes, `0x187000`)와 bootloader·파티션 표·app을 함께 썼고, 기록 B~D는 지우기만 하고
  재부팅했다.
- 네 기록 모두 부팅 뒤 `Thread dataset mode: AWAIT_COMMISSIONING`, `boot: not enrolled (none)`,
  `BLE enrollment open as 'POGO-PB-020000fffe65c00a' for up to 900 s`가 나왔다.
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

### 다음 실물 시험 절차 (여분 보드 020000fffe65c00a만)

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
  "$HOME/pogo-spare-backups/spare-020000fffe65c00a-before-ble.bin")
```

```bash
# 2) Mac: BLE 시험 빌드와 준비 이미지 (보드에 쓰지 않음)
cd "$HOME/pogo-thread-modular-iot"
source ~/.espressif/v6.1/esp-idf/export.sh
export IDF_COMPONENT_MANAGER=0
BLE_BUILD="$HOME/esp-builds/pogo_power_base_ble_enrollment"
EUI=020000fffe65c00a
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
#       "safe wait: BLE enrollment open as 'POGO-PB-020000fffe65c00a' for up to 900 s"
#       "power base ready"와 exporter 로그는 나오면 안 된다.
```

4) 앱 없이 확인할 수 있는 것(iPhone의 범용 BLE 스캐너 앱 등):

- 이름 `POGO-PB-020000fffe65c00a`, service `2e8a0000-9ffb-4868-bc63-064a14069755`,
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

대상은 여분 보드 `020000fffe65c00a`뿐이다. 운영 Base는 건드리지 않는다. **순서 0은 운영 Linux 허브에
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
cd "$HOME/pogo-thread-modular-iot"
source ~/.espressif/v6.1/esp-idf/export.sh
export IDF_COMPONENT_MANAGER=0
BLE_BUILD="$HOME/esp-builds/pogo_power_base_ble_enrollment"
EUI=020000fffe65c00a
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
#       "safe wait: BLE enrollment open as 'POGO-PB-020000fffe65c00a'". exporter 로그는 나오면 안 된다.
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
- ② 여분 Power Base `020000fffe65c00a`용 실물 시험 산출물 준비. Mac에서 빌드·생성·대역 검증만 했다.
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

산출물은 저장소 밖 `$HOME/esp-builds/pogo_bridge_endpoint_hwtest_20261005`에 있다(실행 안내는
그 안의 `README.md`).

| 항목 | 결과 | 근거 |
|---|---|---|
| ESP-IDF v6.1 `power_base` BLE 시험 빌드(esp32h2) | 빌드 확인, 경고 0 | 새 build 디렉터리와 새 `SDKCONFIG`(`sdkconfig.defaults;sdkconfig.ble_enrollment`에서 새로 생성). 기존 `power_base/sdkconfig`와 이전 build 디렉터리는 쓰지 않았다. `0x155b00`(1,399,552) bytes, factory `0x177000`에 `0x21500` bytes(9%) 여유 |
| 최종 설정 | 확인 | `CONFIG_POWER_BASE_BLE_ENROLLMENT=y`, `CONFIG_POWER_BASE_BENCH_JOINER_CONSOLE` 꺼짐, `CONFIG_OPENTHREAD_JOINER=y`, `OPENTHREAD_CLI`·`CONSOLE_ENABLE`·`COMMISSIONER` 꺼짐, protocomm Security 2만 켜짐(0·1 꺼짐). 1785개 key가 build의 `sdkconfig.json`과 일치하고 2026-10-01 BLE 빌드의 sdkconfig와 차이 0 |
| 이미지 | `power_base.bin` SHA-256 `2b535d759122e4ab8dc0e17affcda9a84cfa02ffaeebcb9627ddd1f518275a5b`, `power_base.elf` `f0cac123eceff253e66f05a2380ae7892ffe81408cfdebff2cbe58f23929b0a3`, `partition-table.bin` `89884ce7b6fe7d6a3146a7a53e919ebbc0171405ff85e8e8fb23b345ce264045` | `esptool image-info`(파일만): chip ID 16, 내장 ELF SHA-256 일치, 이미지 해시 유효. 이미지에 이 기능의 로그 문자열(`with a bridge endpoint`, `enrollment endpoint policy`, `record v2: bridge endpoint restored`)과 capability `bridge_endpoint_v1`이 있고 `pb_join`은 없다. 파티션 표는 2026-10-03 release의 것과 같은 파일이다 |
| 준비 이미지(cfg) | 생성, 다시 읽어 확인 | 현재 `tools/gen_power_base_ble_config.py --eui64 020000fffe65c00a --bridge-from-enrollment --existing-setup-code-file …`. 기존 설정 코드는 도구가 파일에서 읽어 다시 썼다(새 코드를 만들지 않았고, 코드를 화면·로그·인자에 내지 않았다). 이미지 0600, 보관 디렉터리 0700. 생성 도구와 별개의 검사(ESP-IDF `nvs_parser`)로 확인: namespace 4개, `pb_thread`는 `mode`=2 하나뿐(Dataset 없음), `pb_bridge`는 enabled=0·주소 0·port 0·정책 1·CRC 유효, `pb_ble_auth`는 `020000fffe65c00a`에 묶이고 record CRC 유효, 배선 blob은 `configs/power_base.json`을 pack한 값과 같고 이 보드의 이전 이미지의 것과도 같다. 이미지 안에 허브의 현재 mesh-local EID나 그 prefix 바이트가 없다. salt는 새로 뽑으므로 `pb_ble_auth` 바이트는 이전 이미지와 다르다 |
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
   `/dev/cu.usbmodemXXXX`이지만 현재 값인지는 확인하지 않았다.
2. 업데이트 도구로 새 펌웨어와 새 cfg를 쓰고 동적 nvs를 초기화한다.
3. 부팅 로그에서 미등록 안전 대기(`boot: not enrolled (none)`, `enrollment endpoint policy = required`)와
   BLE 광고(`POGO-PB-020000fffe65c00a`)를 확인한다.
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

- 보드: 여분 Power Base 1대, EUI-64 `020000fffe65c00a`. 업데이트 도구가 eFuse의 EUI-64
  `74:4d:bd:ff:fe:65:c0:0a`를 읽어 기대값과 같다고 출력했다(C). 센서 모듈은 분리했다(사용자 보고. 보드
  로그의 `pogo_port ... WAIT_ATTACH`와 허브 heartbeat의 `node_count=0`이 이와 어긋나지 않는다).
- 이미지: "후속 기록" ②의 산출물. 도구의 `Inputs verified` 출력(C)이 `power_base.bin` SHA-256
  `2b535d759122e4ab8dc0e17affcda9a84cfa02ffaeebcb9627ddd1f518275a5b`, `power_base.elf` SHA-256
  `f0cac123eceff253e66f05a2380ae7892ffe81408cfdebff2cbe58f23929b0a3`을 보였다. A·B·D의 부팅 로그는 모두
  App version `mvp-pairing-v1-40-gff11a07-dirt…`, Compile time `Oct  5 2026 00:35:45`, ELF file SHA256
  `f0cac123e...`이다. 2nd stage bootloader의 compile time은 세 부팅 모두 `Oct  1 2026 11:32:18`로, 이날
  빌드한 것이 아니다.
- 준비 cfg: 도구가 기록 전에 다시 읽어 출력한 값(C)은 `pb_thread mode=2 keys=['mode']`,
  `pb_bridge enabled=0 policy=1 address-zero=True port=0`, `pb_ble_auth EUI-64=020000fffe65c00a`,
  `wiring=verified`다. 곧 mode 2, Dataset 없음, bridge 주소·port 없음, 목적지 필수 정책 1이다.
- 포트: A는 `/dev/cu.usbmodemXXXX`, B·C·D는 `/dev/cu.usbmodemXXXX`.

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
- `safe wait: BLE enrollment open as 'POGO-PB-020000fffe65c00a' for up to 900 s`

등록 전에는 서비스 시작, exporter, `power base ready` 로그가 없다(A는 기록 끝까지, D는 11:44:57 전까지).
정책 줄은 실제 `nvs_flash`가 정책 바이트가 든 `pb_bridge` blob을 읽어 `endpoint_required`로 해석했다는
근거다. 광고를 스캐너로 대조한 기록은 없다.

#### 첫 번째 등록 세션 (허브 세션 `60a15054`): 성공이 아니다

허브 journal(사용자 전달, 시각은 받은 자료에 없다):

- `started … for Power Base 020000fffe65c00a (120 s window, with bridge endpoint delivery)`
- `bridge endpoint fixed for this session (port 5684)`
- `started the OTBR Commissioner`
- `Joiner 020000fffe65c00a allowed for 120 s`
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

같은 시기의 snapshot 조회 결과(사용자 전달): `여분 Base: 020000fffe65c00a online: True`.

- heartbeat 7건이 10초 간격으로 도착했고 `uptime_ms`가 10010에서 시작한다. 재시작 전부터 남아 있던
  표시가 아니라 이 부팅(11:35:08~09) 뒤에 보드가 새로 보낸 heartbeat다.
- journal은 UUID의 끝부분(`..c00a`)만 출력한다. 전체 UUID `020000fffe65c00a`를 확인한 것은 snapshot이다.

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
- `eui64: 020000fffe65c00a`
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

## Power Base 관리(일반 등록·삭제) 검증 (2026-10-05)

범위: 앱의 제품용 Power Base 등록 화면, 허브가 발급하는 관리 자격증명과 그 BLE 전달(`pb-start` `0x06`),
보드의 인증된 등록 해제(CoAP `pogo/v1/mgmt`), 허브의 삭제 작업·차단·Registry/Matter/공유 설정 정리, 앱의
관리·삭제 화면. 규약은 [`power-base-management.md`](power-base-management.md), HTTP API는
[`pogohome-api-v1.md`](pogohome-api-v1.md) "Power Base management and removal".

**2026-10-05 후속.** 아래 "host test와 빌드" 표는 처음 구현의 판정이다. 그 구현에는 세 가지 결함이
있었고(완료를 Dataset 삭제 전에 판정, 차단 목록이 차면 오래된 차단을 버림, 방 배정 정리를 복구하지
않음), 표의 일부 행은 그 잘못된 동작을 "확인"한 것이다. 지금의 동작과 판정은 이 절 끝의 "후속 수정:
완료의 정의, 차단 용량, 설정 정리"를 본다.

**이 날 실행한 것은 Mac의 host test, iOS 시뮬레이터·Mac Catalyst test, 빌드뿐이다.** 보드에 기록하지
않았고(flash·erase·reset·시리얼 접근 없음), 실제 Bluetooth 검색·연결·등록·삭제, 실제 OTBR·Thread Joiner,
운영 Linux 허브(접속·배포·서비스 조작), 실제 iPhone 설치·실행은 한 번도 하지 않았다. 아래 표의 어느
항목도 실물 통과가 아니다. 같은 날 앞서 기록된 실물 결과("실물 후속 검증 (2026-10-05)")는 이 변경
**이전** 이미지(record v2, `0x04`)의 것이며 이 기능의 근거가 아니다.

기준: 펌웨어 저장소 `feature/unified-module-role-manager` @ `590e2a1` + 미커밋 변경, 앱 저장소 `main` @
`0537534` + 미커밋 변경. 커밋·stage·push하지 않았다. 기존 `~/esp-builds/*`, 프로젝트의 `sdkconfig`, 비밀
`cfg`·백업 파일은 읽거나 바꾸지 않았고, 펌웨어 빌드는 새 디렉터리(`~/esp-builds/pbmgmt_20261005_*`)에만
만들었다. test의 설정 코드·PSKd·관리 키는 모두 test 안에서 만든 값이다.

### host test와 빌드 (Mac, Apple clang, ESP-IDF v6.1, Xcode 27)

| 항목 | 결과 | 근거 |
|---|---|---|
| 관리 규약 codec: 6개 메시지의 길이·offset·MAC 범위, golden vector, RFC 4231 HMAC-SHA-256, 잘린·늘어난·예약 bit·다른 type의 거부 | host 확인 | `tools/test_power_base_mgmt.c`(실제 `pb_mgmt_proto.c`·`pb_mgmt_client.c`·`power_base_enrollment.c` + NVS simulator), 161개 검사 |
| record v3(200바이트): 형식, CRC, `ENROLLED`만 재개, `UNENROLLING`·`RELEASED`는 Thread를 건드리지 않고 대기. record v1·v2의 바이트와 부팅 동작 불변, 읽기만으로 재기록 없음 | host 확인 | 같은 test + `tools/test_power_base_enrollment.c`(256개 검사) |
| 온라인 해제 순서: poll → 명령 검증 → 신규 작업 차단 → journal → 수락 응답 → 영속 해제(키 삭제, 다시 읽어 확인) → 완료 증명 → Dataset 삭제 → 최종 record. 완료 증명은 영속 해제 뒤에만, 네트워크 삭제는 증명 전송 뒤에만 | host 확인 | 같은 test의 호출 순서 기록 |
| 거부: 다른 보드·다른 hub·이전 세대·다른 키·변조·다른 poll의 nonce·재생된 명령 → 상태 변화 없음 | host 확인 | 같은 test |
| 응답 유실과 재시도: 명령 응답 유실, 완료 증명 유실(6회, 2·4·8·16·30초 대기), 확인을 못 받아도 미등록 복귀, 저장 실패 시 이전 단계 유지 | host 확인 | 같은 test(대기 시간은 fake로 기록) |
| 전원 차단: 해제의 각 저장 지점 직후 프로세스를 끊고 flash 파일로 다시 부팅 → 이전 등록으로 서비스 재개 없음, 증명 재전송, 최종 record | host 확인 | 같은 test(fork + NVS simulator 파일) |
| 재등록: 해제 뒤 새 세대·새 키로 등록되고 이전 세대의 명령은 적용되지 않음. 로그에 키·세대·MAC 없음 | host 확인 | 같은 test |
| BLE `0x06` admission: 형식·길이·예약 바이트, `INVALID_MANAGEMENT`, 재전송 digest에 관리 block 포함, 모든 경로의 버퍼·slot 삭제, `0x02`·`0x04` 불변 | host 확인 | `tools/test_power_base_ble_admission.c`, 380개 검사 |
| Security 2 상호운용: 실제 protocomm·NimBLE 없는 host harness(실제 admission + 등록 상태 머신)에 Python 클라이언트로 `proto-ver`(`mgmt_v1`), `0x06` 전달, record v3 저장 확인 | host 확인 | `tools/test_power_base_ble_sec2_interop.py`, 157개 검사. harness는 `~/esp-builds/pbmgmt_20261005_host_ble_sec2` |
| exporter 정지: 해제가 시작되면 SAMPLE·SNAPSHOT·REMOVED·resync·heartbeat를 보내지 않고 tombstone도 남기지 않음 | host 확인 | `tools/test_linux_bridge_exporter_suspend.c`, 28개 검사(정지하지 않은 대조 scenario 포함) |
| 펌웨어 host C test 전체 | host 확인 | `tools/test_*.c` 66개 프로그램을 각 파일 머리말의 명령으로 빌드·실행, 실패 0 |
| ESP-IDF v6.1 빌드: Power Base 기본·bench·BLE, host harness | 빌드 확인 | 기본 871,888 B, bench 1,008,352 B, BLE 1,413,200 B(app 파티션 여유 8%). `power_base/sdkconfig`·`sdkconfig.defaults`의 SHA-256이 빌드 전후 같음 |
| ESP-IDF v6.1 빌드: module | 빌드 확인 | 새 디렉터리 `~/esp-builds/pbmgmt_20261005_module`, 885,408 B(app 파티션 여유 42%). module은 바뀐 component를 쓰지 않는다. 새 component(`power_base_mgmt`)가 `components/`를 같이 쓰는 다른 프로젝트의 configure·빌드를 깨지 않는지 본 것이다. `module/sdkconfig`의 SHA-256이 빌드 전후 같음 |
| Linux ingest: 차단된 Base의 `4.03`, Base와 그 아래 노드 제거(다른 Base 아래의 같은 모듈 유지), 관리 resource의 payload ACK | host 확인 | `linux/matter_bridge/ingest` ctest 8/8 (macOS) |
| Linux core: 삭제된 모듈의 Matter endpoint가 기존 Reconcile로 지워지고 다른 endpoint와 `endpoint_map`의 id가 유지됨 | host 확인 | `app/core` unit test 183개 검사 |
| Linux api: 허브 관리자 ↔ **실제 펌웨어 관리 client**(NVS simulator)의 등록→poll→삭제→증명→차단, 오프라인 대기와 재시작 복구, 응답 유실, 재등록으로 차단 해제, 삭제 API의 인증·한도·중복·취소·경쟁, snapshot·health의 추가 field와 관리 없는 허브의 byte 동일성, 방 배정 해제 | host 확인 | `app/api` unit test 1,086개 검사, 실패 0 (macOS, OpenSSL) |
| 앱 BLE 패키지: `0x06` codec(펌웨어와 같은 golden vector), 자격증명 규칙, capability, host harness와의 실제 Security 2 상호운용 19개 | sim 확인 | `swift test` Debug와 Release(`-c release -Xswiftc -enable-testing`) 각각 61개 통과 |
| 앱 제품 등록 run: 인증 전 허브 미요청, `management_version` 요청, `0x06`만 사용, 보드·허브 구버전은 fallback 없이 중단, 자격증명 없는 답·세대 불일치는 전달하지 않고 세션 취소, 다른 보드·틀린 코드는 허브 세션 없음, `heartbeat_received`만 성공, 화면에 비밀 없음 | sim 확인 | `PowerBaseManagedEnrollmentTests` 12개 (scripted hub·board) |
| 앱 삭제: 요청 본문, 오류 구분, snapshot의 `management`·`power_base_removals` 해석, controller(페어링된 허브만, 중복 요청 없음, 답 유실, 취소는 허브 판단, 허브 변경 시 늦은 답 무시), 표시 규칙(`completed`만 삭제 완료, 지원하지 않음, 목록에 남는 작업) | sim 확인 | `PowerBaseRemoval*Tests`·`PowerBaseManagementPresentationTests` 30개 |
| 앱 문구: 두 화면의 모든 문구가 en·es에 있고(한국어 실행), en·es 실행에서 한국어가 남지 않음. 기존 번역 검사·브랜드 이름 검사 유지 | sim 확인 | `PowerBaseSetupPresentationTests`, `LocalizationCatalogTests`, `BrandNamingTests`; `-testLanguage ko`·`en`·`es` |
| 앱 회귀 test 전체 (iPhone 13 mini simulator, Debug) | sim 확인 | 838개 중 825개 통과, 13개 skip(환경 변수가 필요한 렌더링·실 네트워크 test와 한국어 실행 전용 test), 실패 0. iPad Air 11 simulator에서도 같은 결과. 기존 `PowerBaseEnrollment*` test(수명·취소·재연결·목적지 전달)는 그대로 통과 |
| 앱 새 test + 렌더링 (iPad Air 11 simulator; Mac Catalyst) | sim·Mac 확인 | 새 test와 번역·브랜드 검사 70개(등록 run, 삭제, 표시 규칙, 문구, 렌더링)를 iPhone 13 mini·iPad Air 11 simulator와 Mac Catalyst(서명한 Debug test host)에서 `-testLanguage ko`·`en`·`es`로 각각 실행, 실패 0. Mac Catalyst에서 **전체** 회귀 test는 돌리지 않았다 |
| 앱 빌드 | 빌드 확인 | iOS Simulator Debug·Release, Mac Catalyst Release(`CODE_SIGNING_ALLOWED=NO`), Mac Catalyst Debug(test 포함, 자동 서명) |
| Release 점검 | 확인 | `Scripts/check_release_enrollment_surface.sh`: iOS Simulator Release와 Mac Catalyst Release에 시험 화면·USB 콘솔 시험·PSKd 표시/복사 클립보드·DEBUG 문구 표·`UIPasteboard` 없음, 제품 등록·삭제 코드와 Bluetooth 권한 문구 있음. Debug 빌드에서는 같은 항목을 찾음(`debug` 모드) |
| 화면 렌더링 | 눈으로 확인(일부) | 제품 등록 화면 19개 상태, 관리 화면 12개 상태, 기기 목록의 진입점·삭제 상태, "기기 추가" 시트를 iPhone 13 mini·iPad(form sheet 폭)·Mac Catalyst(좁은 창·넓은 창)에서 ko·en·es, 밝은 화면·어두운 화면·가장 큰 글자(AX XXXL)로 PNG 렌더링했다(iOS 1,133장, Mac 302장). 그 가운데 일부만 열어 확인했다(각 플랫폼·언어의 표본 십여 장). 나머지는 만들기만 했고 사용자의 눈으로 본 것은 없다. 렌더링으로 찾아 고친 것: 카드 안의 큰 버튼이 카드를 자르던 것, 가장 큰 글자에서 설정 코드가 잘리고 글자 수 표시가 세로로 깨지던 것 |

### 실행하지 않은 것

- **Linux용 `pogo_matter_bridge`의 GN/CHIP 빌드.** `app/chip/main.cpp`와 `app/BUILD.gn`의 변경(관리자 생성,
  ingest handler 연결, 삭제 완료 시 설정 정리 hook)은 Mac에서 컴파일하지 않았다. 재현: Linux 빌드
  호스트에서 `tools/build_matter_bridge.sh`. 나머지 Linux 코드(`ingest`, `app/core`, `app/api`)는 macOS의
  CMake host 빌드로만 확인했다.
- 실물 보드의 어떤 동작도(flash 포함). 실제 BLE 광고·연결·Security 2·MTU, 실제 Joiner·attach, 실제
  `pogo/v1/mgmt` 왕복, 실제 전원 차단, 실제 NVS·OpenThread 저장값 삭제.
- 운영 Linux 허브: TLS listener 위의 삭제 경로, `power_base_mgmt.json`의 실제 저장·재시작 복구, 실제
  controller에서 본 Matter endpoint 제거, 두 기기에서 본 설정 revision.
- 실제 iPhone·iPad·Mac에서의 Bluetooth 권한 요청, `PasteButton`, VoiceOver 읽기, 백그라운드 전환.
  렌더링은 SwiftUI 본문만 그린 것이고 큰 글자·어두운 화면은 그 그림으로만 봤다. VoiceOver는 실행하지
  않았다(접근성 label·trait는 코드에 넣었다).
- (처음 판에는 "차단 목록 64개 초과 시의 밀어내기는 따로 test하지 않았다"고 적었다. 그 동작은 한계가
  아니라 결함이었고 후속 수정에서 없앴다.)
- Mac Catalyst에서의 전체 앱 회귀 test(위 70개만 실행), watchOS 대상의 test.
- `PasteButton`은 시뮬레이터 렌더링에서 비활성(클립보드가 빔)으로만 보였고 눌러 보지 않았다.

### 실물 시험 준비와 시나리오 (준비만 했고 실행하지 않았다)

대상은 여분 보드 한 대뿐이다. 운영 Base는 건드리지 않는다. **0번은 운영 Linux 허브에 새 bridge를
배포해야 하므로 운영자가 따로 결정한다.** 설정 코드·PSKd·Dataset·관리 키는 어떤 명령에도 인자로 넣지 않고
로그·보고서에 적지 않는다.

준비:

0. Linux 허브(운영자): 새 소스로 `tools/build_matter_bridge.sh`, 설치, 서비스 재시작. helper와 sudoers
   규칙은 바꾸지 않는다(이 변경은 helper를 건드리지 않았다). 확인: `GET /api/v1/health`에
   `"power_base_management":1,"power_base_removal":"available"`, `<state-dir>/pogohome/power_base_mgmt.json`이
   0600으로 생김.
1. Mac: BLE 빌드(`sdkconfig.defaults;sdkconfig.ble_enrollment`)를 새 build 디렉터리에 만들고 여분 보드에
   기록한다(기존 준비 이미지와 설정 코드는 그대로 쓴다. `cfg`는 다시 쓰지 않는다). 보드가 이미 등록돼
   있으면 먼저 등록 표식을 지워 미등록으로 만든다.
2. 앱: Debug 빌드를 iPhone에 설치한다. Release 구성의 화면 확인은 TestFlight나 Release 실행으로 따로 한다.

시나리오(각각 보드 로그, 허브 로그, 앱 화면을 같이 기록한다):

| # | 시나리오 | 기대 |
|---|---|---|
| 1 | 제품 화면으로 등록 | 보드 로그에 `0x06` 수락과 record v3 저장, 허브에 `provisional` → 첫 poll에서 `active`, 앱은 `heartbeat_received` 뒤에만 "등록했습니다". snapshot에 `"management":"supported"` |
| 2 | 틀린 설정 코드, 다른 보드 선택 | 허브 세션이 만들어지지 않음(허브 로그에 세션 없음). 틀린 코드 5회 뒤 잠김 문구 |
| 3 | 온라인 삭제 | 앱 확인 → `pending` → 다음 poll(10초 안)에 `sent` → `accepted` → `released`. 보드 로그: 서비스 정지, 등록 사용 중지 저장, 통지 확인, Thread 정지, OpenThread 저장소 삭제, **NVS `openthread` namespace가 비었는지 확인**, 최종 record, 재시작 뒤 미등록 대기와 BLE 광고. 허브: Base와 센서가 snapshot에서 사라짐, 작업은 `released`로 남음(`completed` 아님), Matter controller에서 endpoint 사라짐, 방 배정 해제 revision과 `settings_cleanup_pending`이 false가 되는 것 |
| 3-a | 삭제 확인 | 보드 가까이에서 앱의 "삭제 확인" → 설정 코드 → 영수증 읽기 → 허브가 `completed`. 보드에는 변화 없음. 통지 없이(허브를 끈 채 삭제) 영수증만으로도 `completed`가 되는지 |
| 3-b | 삭제 실패 주입이 가능하면 | Dataset 삭제가 실패한 보드는 영수증이 없고 허브는 `released`에 머무는지. 다음 부팅에서 다시 시도하는지 |
| 4 | 삭제 뒤 같은 보드 재등록 | 등록 화면이 `pb-start` 전에 영수증을 읽어 전달(이전 작업 `completed`), 새 세대로 등록, 차단 해제, 센서를 끼우면 같은 endpoint id로 다시 나타남. 영수증 전달 없이 재등록한 경우 이전 작업이 `superseded`인지 |
| 5 | 오프라인 삭제 | 보드 전원을 끈 채 삭제 → "삭제 대기". 앱을 종료했다 켜도 유지. 취소 가능. 전원을 켜면 3과 같이 진행 |
| 6 | 삭제 대기 중 허브 재시작 | 작업이 남아 있고 보드가 돌아오면 진행 |
| 7 | 삭제 도중 보드 전원 차단(수락 직후, 통지 전, 삭제 직후) | 다음 부팅에서 서비스 재개 없이 해제를 이어 감. 허브는 통지를 받으면 `released`, 못 받으면 5분 뒤 "결과 확인 불가". 어느 경우에도 영수증 없이는 `completed`가 아님 |
| 8 | 완료 뒤 늦은 데이터 | 삭제된 Base가 목록에 다시 나타나지 않음(허브 로그에 `4.03`) |
| 9 | 구형 조합 | record v2로 등록된 보드: "지원하지 않음". `mgmt_v1` 없는 펌웨어: 등록 화면이 허브 세션 없이 "펌웨어 업데이트 필요" |
| 10 | 센서가 다른 Base로 옮겨진 뒤 원래 Base 삭제 | 옮겨진 센서는 남고 방 배정도 유지 |
| 11 | 화면 | iPhone·iPad·Mac에서 Bluetooth 권한 요청 시점(버튼을 누른 뒤), 붙여넣기, 큰 글자, VoiceOver, 백그라운드 전환 뒤 이어가기 |
| 12 | 설정 정리 복구 | 방이 지정된 센서가 있는 Base를 삭제하고 `released` 직후 허브 서비스를 재시작 → 약 2분 뒤 그 센서의 방 배정이 해제되고 `settings_cleanup_pending`이 false. 다른 센서·방·이름은 그대로 |

### 후속 수정: 완료의 정의, 차단 용량, 설정 정리 (2026-10-05)

처음 구현을 검토한 결과 세 가지 결함이 있었고 같은 날 고쳤다. 이 소절이 지금 소스의 판정이다. 위
"host test와 빌드" 표에서 아래와 어긋나는 행(완료 증명 → Dataset 삭제 순서, `completed`가 증명만으로
되는 것, 검사 수)은 처음 판의 기록으로만 남긴다.

**실행한 것은 여전히 Mac의 host test, iOS 시뮬레이터·Mac Catalyst test, 빌드뿐이다.** 보드·BLE·OTBR·운영
허브·실제 iPhone은 건드리지 않았다. 특히 아래 "OpenThread 제약"은 ESP-IDF v6.1에 들어 있는 OpenThread
소스를 읽어 확인한 것이고 보드에서 실행해 본 것이 아니다.

기준과 보존: 두 저장소의 HEAD·브랜치는 그대로이고(`590e2a1`, `0537534`), stage된 것과 stash가 없다.
시작할 때 미커밋 변경 전체의 목록과 SHA-256을 기록했다. 처음 구현의 파일 가운데 펌웨어 저장소 31개,
앱 저장소 18개를 이 수정에서 고쳤고, 각각 4개 파일이 새로 변경 목록에 들어왔다(펌웨어:
`pb_ble_service.c`, `thread_manager.h`·`.c`, host harness의 `CMakeLists.txt`. 앱: 삭제 확인 controller·
화면과 그 test 2개). 나머지(31개, 53개)는 바이트가 같다. 검토자의 재현 파일
(`/private/tmp/pogo-management-review-20261005/`)은 읽기만 했고 SHA-256이 그대로다. 펌웨어 빌드는 새
디렉터리 `~/esp-builds/pbmgmt2_20261005_*`에만 만들었고 프로젝트의 `sdkconfig`는 빌드 전후 같다.

#### 수정 전 재현

검토자의 재현 프로그램을 scratch로 복사해(원본은 그대로) **수정 전** 소스에 붙여 빌드·실행했다. 설정
정리 결함은 같은 방식의 시나리오를 하나 더 붙였다. 네 줄 모두 실패로 나왔다.

| 결함 | 수정 전 출력 |
|---|---|
| 1 | `REVIEW_FAIL: hub already completed before Dataset erase, which now fails` |
| 2 | `REVIEW_FAIL: first deleted Base unblocked=1; old heartbeat accepted=1 after 65 deletions` |
| 2 | `REVIEW_FAIL: removed block remains absent after reload` |
| 3 | `REVIEW_FAIL: a failed or interrupted room clean-up is never retried and nothing persistent records it` |

수정 뒤에는 같은 상황을 저장소의 회귀 test가 본다(아래 표). 검토자의 프로그램은 바뀐 API(`released`,
영수증, 용량 거부)에 맞지 않으므로 그대로 다시 돌리지 않았다.

#### OpenThread 제약 (소스로 확인, 실물 미실행)

- `otInstanceErasePersistentInfo()`는 MLE가 꺼져 있어야만 동작한다. 지운 뒤 Thread로 무언가를 보내려면
  다시 attach해야 하고, attach는 받은 Active Dataset을 저장소에 **다시 저장한다**
  (`ReadAndSaveActiveDataset` → `ActiveDatasetManager::Save`). 그래서 "지웠다"는 사실을 Thread로 알리는
  길은 Dataset을 다시 영속 저장하지 않고는 없다. 그 우회는 쓰지 않았다.
- `otPlatSettingsWipe()`는 `nvs_erase_all()`의 결과를 버리고, `otDatasetIsCommissioned()`는 RAM의 표식을
  읽는다. 둘 다 삭제의 증거가 아니므로, 보드는 OpenThread 설정이 들어 있는 NVS namespace(`nvs` 파티션의
  `openthread`)에 entry가 하나도 없는지를 직접 읽어 확인한다.

#### 판정

| 항목 | 결과 | 근거 |
|---|---|---|
| 결함 1, 보드: 통지(`RELEASED`)와 영수증(`ERASED`)의 분리. 순서는 신규 작업 차단 → journal → 수락 → 등록 사용 중지 저장(키 유지) → 통지(최대 6회) → Thread 정지·저장소 삭제 → **다시 읽어 확인** → 영수증 계산 → 최종 record(다시 읽어 확인) → 재시작. 영수증은 삭제가 확인된 뒤에만 만들어지고 Thread로 보내지 않는다 | host 확인 | `tools/test_power_base_mgmt.c`, 219개 검사(처음 판 161) |
| 결함 1, 보드: 삭제 실패, 삭제가 성공이라고 답했지만 저장소에 남은 경우, 최종 record 저장 실패 → 영수증 없음, record는 "삭제 대기"로 남음, 반환값은 `RELEASED_NOT_ERASED`(완료 아님), 다음 부팅에서 삭제부터 다시 | host 확인 | 같은 test의 `g_erase_lies`·저장 실패 주입과 전원 차단 시나리오 7개(저장 지점 5곳 각각, 부팅 경로 안에서 거듭 끊기는 경우, 전원 차단 대신 저장이 실패하는 경우. fork + NVS simulator 파일) |
| 결함 1, 허브: 통지는 `released`까지만. `completed`는 그 세대의 키로 MAC이 맞는 `ERASED` 영수증으로만. 통지를 영수증 자리에 넣으면 `422`, 다른 허브·다른 세대·취소된 작업·변조는 `409`, 같은 영수증의 재전달은 같은 답. 통지 없이 영수증만 와도 완료와 정리를 한 번에 | host 확인 | `app/api` unit test의 "the board's erase fails, lies, …: never 'completed'", "the receipt arrives although the released notice never did", `test_removal_api.cpp`의 영수증 route. **실제 펌웨어 client**(NVS simulator)와 허브 관리자를 맞물려 실행 |
| 결함 1, 호환: 규약 version byte는 1 그대로. 통지의 바이트·MAC은 처음 판의 "완료 증명"과 같고(golden vector 동일), 지금의 허브는 그것을 `released`로만 받는다. 상태 파일 schema 1의 `completed`는 읽을 때 `released`로 내려온다(영수증으로 확인된 적이 없으므로). `RESULT_ACK`는 무엇을 확인했는지(`acked_result`)를 MAC 안에 담는다 | host 확인 | 같은 test의 schema 1 migration, golden MAC 5개(펌웨어 test와 허브 test가 각각 계산. Python `hmac`으로 따로 계산한 값과 같음) |
| 결함 1, BLE: `pb-status` endpoint의 영수증 요청 `0x07`/응답 `0x87`(112바이트). Security 2 세션 뒤에만, 대상 EUI-64가 맞을 때만, 최종 record가 있을 때만 영수증을 준다. GATT 배치와 기존 메시지는 그대로. capability `release_receipt_v1` | host 확인 | `tools/test_power_base_ble_admission.c` 397개 검사(처음 판 380), `tools/test_power_base_ble_sec2_interop.py` 169개 검사(157), `tools/test_power_base_ble_enrollment_paths.py` 109개 검사 |
| 결함 2: 차단은 밀려나지 않는다. 자리는 삭제 **요청** 때 잡는다(차단 수 + `pending`·`sent`·`accepted` 작업 수 ≤ 64). 가득 차면 `409 removal_capacity_full`, 작업이 만들어지지 않고 보드에 명령이 가지 않음. 취소·대체는 자리를 돌려주고, 중복 요청과 재시작은 수를 바꾸지 않음. 64개를 삭제한 뒤 첫 번째로 삭제한 Base의 데이터가 여전히 `4.03`. 새 인증된 세대만 차단을 풂. 자격증명 96개 한도와 함께: `active`와 영수증을 기다리는 자격증명은 밀려나지 않음 | host 확인 | `app/api` unit test의 "blocks are never dropped to make room: capacity is reserved when a removal is requested"(관리자), `test_removal_api.cpp`의 "the block list is full"(TLS route로 64개를 삭제한 뒤 `409`와 본문, 작업·명령 없음, 재등록 뒤 수락) |
| 결함 3: 정리 대상(모듈 목록)과 시도 횟수·다음 시도 시각을 `released`와 같은 저장에 기록. 재시작 뒤 120초를 기다려 이어 감. 실패하면 5초부터 두 배씩 최대 10분 간격으로 계속 재시도. 시도할 때마다 모듈의 현재 소속을 다시 봐서 다른 Base나 새 세대 아래에 있는 모듈은 건드리지 않음. 설정 저장은 전용 worker가 관리자·Registry mutex 없이 수행. 대상이 아닌 이름·방·집 이름·그사이의 다른 저장은 유지, 이미 정리된 것을 다시 해도 revision이 늘지 않음. 작업의 `settings_cleanup_pending`으로 노출 | host 확인 | `app/api` unit test의 "the settings clean-up is stored with the release and retried until it is done", "shared settings: removed modules lose their room, keep their name". mutex 검사는 정리 hook이 도는 동안 다른 thread가 관리자와 Registry를 쓰는 것으로 보며, hook 호출을 관리자 mutex 안으로 옮긴 변형 빌드에서 이 검사 하나만 실패하는 것을 확인했다(소스는 되돌렸고 SHA-256이 같다) |
| Linux api·core·ingest 전체 | host 확인 | `app/api` 1,246개 검사(처음 판 1,086), 실패 0(네 번 연속 실행). `app/core` 183개 검사. `ingest` ctest 8/8 (macOS, OpenSSL) |
| 펌웨어 host C test 전체 | host 확인 | `tools/test_*.c` 66개 프로그램, 실패 0. `test_power_base_enrollment.c` 256개 검사와 `test_linux_bridge_exporter_suspend.c` 28개 검사는 수가 그대로 |
| ESP-IDF v6.1 빌드 | 빌드 확인 | 기본 873,056 B, bench 1,009,520 B, BLE 1,415,264 B(app 파티션 여유 8%), module 885,408 B, host harness. 새 디렉터리 `~/esp-builds/pbmgmt2_20261005_*` |
| 앱 BLE 패키지: 영수증 codec(`0x07`/`0x87`), 대상·길이·예약 바이트·오류 code, 실제 host harness와의 Security 2 상호운용(최종 record를 가진 harness에서 영수증을 읽어 바이트 비교) | sim 확인 | `swift test` Debug와 Release 각각 68개 통과(처음 판 61) |
| 앱: 삭제 확인 controller(대상 Base만 검색, 영수증을 읽기 전 허브 요청 없음, 다른 보드·틀린 코드·영수증 없음·구버전 보드·허브 거부의 구분, 다시 보내기), 등록 화면의 영수증 전달(`pb-start` 전에 읽어 허브에 전달, 실패해도 등록은 계속), 상태 해석(`released`는 완료가 아님, `settings_cleanup_pending`, 용량 오류) | sim 확인 | `PowerBaseReleaseConfirmationTests`·`PowerBaseEnrollmentReceiptRelayTests` 19개, `PowerBaseRemovalTests` 31개 |
| 앱 새 test + 렌더링 | sim·Mac 확인 | 94개를 iPhone 13 mini·iPad Air 11 simulator와 Mac Catalyst에서 `-testLanguage ko`·`en`·`es`로 실행, 실패 0(ko 94 통과, en·es는 한국어 실행 전용 1개 skip) |
| 앱 회귀 test 전체 (Debug) | sim 확인 | iPhone 13 mini와 iPad Air 11 simulator 각각 862개 중 848개 통과, 14개 skip, 실패 0(처음 판 838/825/13) |
| 앱 빌드 | 빌드 확인 | iOS Simulator Debug(test 포함)·Release, iOS 기기 Debug·Release(`CODE_SIGNING_ALLOWED=NO`. 설치·실행하지 않음), Mac Catalyst Release(`CODE_SIGNING_ALLOWED=NO`), Mac Catalyst Debug(test 포함) |
| Release 점검 | 확인 | `Scripts/check_release_enrollment_surface.sh`가 iOS Simulator·iOS 기기·Mac Catalyst의 Release에서 통과, 세 Debug 빌드에서는 `debug` 모드로 통과 |
| 화면 렌더링 | 눈으로 확인(일부) | 삭제 확인 화면 9개 상태, 관리 화면의 새 상태 3개(등록 해제됨, 설정 정리 대기, 용량 초과)를 포함해 다시 렌더링(iOS 1,799장, Mac 415장). 열어 본 것은 여섯 장이다(iPhone ko 3장, iPhone en 1장, Mac es 어두운 화면 1장, Mac ko 1장). 나머지는 만들기만 했다 |

#### 바뀐 test 기대값

단언을 느슨하게 한 것은 없다. 아래는 처음 판의 잘못된 동작을 "확인"하던 기대값을 바꾼 것이다.

- `tools/test_power_base_mgmt.c`: 호출 순서 `PPQADE`(증명 뒤 삭제)와 "증명 시점에 Dataset이 남아 있음",
  저장 상태 30/31/32(`RELEASED`, 증명 대기, 증명 확인)를 새 순서와 새 record 표식(삭제 대기 31, 최종
  34, 최종 + 통지 확인 36)으로 바꿨다. 전원 차단 시나리오는 끊는 지점 5곳이 그대로이고 기대 상태만 새
  표식으로 바꿨으며, 각 지점에서 "영수증이 아직 없다"는 검사와 저장 실패 시나리오를 더했다.
  "다른 result의 `UNENROLL_DONE`은 만들 수 없다"는 result 2가 이제 `ERASED`이므로 3으로 검사한다.
  `RESULT_ACK`의 본문 검사는 새 field(`acked_result`)를 포함한다.
- `app/api/tests/test_power_base_management.cpp`·`test_removal_api.cpp`: 통지 뒤의 기대 상태를
  `completed`에서 `released`로 바꾸고, `completed`는 영수증 전달 뒤에 검사한다. `RESULT_ACK`의 golden MAC은
  본문이 바뀌어 새 값이다(`UNENROLL_DONE`/`RELEASED`의 golden MAC은 그대로). 통지가 끝내 오지 않는
  시나리오의 보드 상태는 30에서 "삭제 뒤 최종(34)"으로.
- `proto-ver`의 capability 목록을 정확히 비교하던 검사 다섯 곳(Python 2, Swift 3)에 `release_receipt_v1`을
  더했다.
- 앱: 상태 목록을 도는 test에 `.released`를 더했고, 문구 수 하한을 110에서 135로 올렸다.

#### 실행하지 않은 것 (후속 수정 기준)

- **Linux용 `pogo_matter_bridge`의 GN/CHIP 빌드.** `app/chip/main.cpp`의 이번 변경(`MakeRoomUnassigner`
  연결, `StartCleanupWorker()` 호출)은 컴파일하지 않았다. 재현: Linux 빌드 호스트에서
  `tools/build_matter_bridge.sh`. 설정 정리 worker thread가 실제 bridge 프로세스에서 도는 것도 따라서
  확인하지 않았다(worker 자체는 `app/api` unit test가 실제 thread로 돌린다).
- 실물의 모든 것: 실제 NVS에서 `openthread` namespace가 비는지, `otInstanceErasePersistentInfo()`의 실제
  동작, 실제 BLE로 영수증을 읽는 것, 실제 허브의 영수증 route와 상태 파일 migration, 실제 재시작 뒤의
  설정 정리.
- Mac Catalyst에서의 전체 앱 회귀 test(위 94개만), watchOS 대상의 test, VoiceOver.

## BOOT 버튼 로컬 재등록 검증 (2026-10-06)

범위: Power Base의 BOOT 버튼 기반 로컬 재등록 — 버튼 판정, 일회성 재등록 창(재시작 방식), 인증된 로컬
초기화와 그 journal, BLE 규약 추가(`0x08`·`0x09`, `local_reset_v1`), 앱의 "다른 허브에 연결하기" 화면.
설계는 [`power-base.md`](power-base.md) "BOOT 버튼 로컬 재등록", 원격 삭제와의 관계는
[`power-base-management.md`](power-base-management.md).

**이 날 실행한 것은 Mac의 host test, host harness 상호운용 시험, iOS 시뮬레이터·Mac Catalyst test, 빌드뿐이다.**
보드·직렬 포트·실물 BLE에 접근하지 않았고(flash·erase·reset 없음), Linux 호스트에 접속·배포하지 않았다.
따라서 **물리 버튼의 동작, 실제 전원 차단, 실제 BLE 광고·연결, 새 Thread 네트워크로의 이동은 검증하지
않았다.** 아래 표의 어느 항목도 실물 통과가 아니다. GPIO9 시험 구성은 SuperMini 보드의 실제 BOOT 버튼
배선을 확인하지 않은 하드웨어 가정이다.

기준: 펌웨어 저장소 `feature/unified-module-role-manager` @ `590e2a1` + 미커밋 변경(일반 등록·삭제·영수증
기능 포함), 앱 저장소 `main` @ `0537534` + 미커밋 변경. 커밋·stage·push하지 않았다. 기존 `~/esp-builds/*`와
프로젝트의 `sdkconfig`는 읽거나 바꾸지 않았다(`power_base/sdkconfig`의 SHA-256이 빌드 전후 같음). 산출물과
로그는 새 디렉터리 `~/esp-builds/reenroll_20261006_*`에만 있다. test의 설정 코드·키·식별자는 모두 test
안에서 만든 값이다.

### host test와 빌드 (Mac, Apple clang, ESP-IDF v6.1, Xcode 27)

근거의 종류: **host** = 실제 C 소스를 host에서 컴파일해 fake/simulator와 실행, **상호운용** = 실제 C 구현
(host harness)과 실제 Swift client, **sim** = iOS 시뮬레이터의 XCTest(보드·BLE는 scripted), **빌드** = 컴파일·링크만,
**정적** = 소스 텍스트 검사.

| 항목 | 결과 | 근거 |
|---|---|---|
| 버튼: 짧은 누름, 4999/5000ms 경계, 접점 튐(누름 중·놓는 중·8초 채터), 부팅부터 눌림(7초·20초), 부팅 직후 안정된 놓음 전에 누름, 누른 채 유지, 15초 초과, 반복 요청, 같은 시각 반복 sample, ms counter wraparound, sampling 끊김 | host 확인 | `tools/test_power_base_reenroll.c`(실제 `pb_reenroll_button.c`) |
| 버튼 GPIO: 문서의 실제 배선(TX 10, RX 11, DETECT_OUT 3, DETECT_IN 23)과의 충돌, `power_enable_gpio`, GPIO9 자체가 cfg에 쓰인 경우, 범위 밖, flash·USB·미bonding·다른 strapping 핀 | host 확인 | 같은 test(실제 `gpio_profile.c` 포함) |
| gate: 등록 진행·원격 삭제·로컬 초기화·중복 요청 중에는 창 요청을 만들지 않음 | host 확인 | 같은 test(`pb_reenroll_gate`) |
| record: `win` 24바이트·`op` 40바이트의 형식, 모든 byte의 변조 검출, 다른 보드·크기 불일치·읽기 실패의 구분, write 실패·commit 실패·기록되지 않은 write(다시 읽기로 검출)·read-back 실패·지워지지 않은 erase | host 확인 | 같은 test + NVS simulator의 오류 주입(`nvs_sim_inject`) |
| 창 열기·120초 만료·연결/인증만 반복·잘못된 설정 코드 5회·연결 해제·창 도중 재시작 → 등록 record·Dataset·`cfg`가 byte 단위로 그대로이고, 다음 부팅이 평소 규칙으로 재개. 요청은 한 번만 소비(재부팅 반복 없음). 요청을 지우지 못하면 창 없음. 기능 없는 빌드·다른 보드의 요청·손상된 요청은 버림 | host 확인 | 같은 test(부팅은 `app_main.c`의 호출 순서를 test 안에서 따라 한 것이고 각 판정은 실제 소스) |
| 인증만 하고 확인된 명령이 없으면 변경 없음 | host·상호운용·sim 확인 | 같은 test, Swift `LocalResetInteropTests`, 앱 `PowerBaseLocalResetControllerTests` |
| 명령 검증: 길이 ±1, 형식·예약 byte, 확인 표시 없음, 작업 ID 0, 다른 EUI-64, 틀린·만료·이전 세션의 challenge, challenge 없음, 창 만료 뒤(`WINDOW_CLOSED`), 창 없는 부팅, BLE 닫힘, capability 없는 보드(두 message 모두 기존 `MALFORMED`), 창 부팅의 `pb-start`(`NOT_WAITING`) | host·상호운용 확인 | 같은 test, Swift `LocalResetInteropTests` 7개 |
| 중복·충돌: 같은 작업 재전송 `DUPLICATE`, 다른 작업 `BUSY`, 시작 못 한 작업 ID·재등록 뒤의 옛 작업 ID `REQUEST_CONFLICT`. 재시작 뒤 같은 작업 ID로 상태 조회 | host·상호운용 확인 | 같은 test |
| 승인 journal 저장 실패(write·commit·기록 안 됨·read-back) → 초기화 미시작, record·Dataset 불변, 남은 APPROVED 없음, 다음 부팅은 재개 | host 확인 | 같은 test |
| 각 단계 실패(record 삭제 오류, 지워지지 않은 삭제, 저장소 삭제 오류, 삭제했다고만 하는 삭제, 일부만 삭제, journal 종료 실패) → 그 부팅과 다음 부팅에서 기존 네트워크 재개 없음, 결함이 사라진 부팅에서 이어서 검증 완료(버튼 없는 빌드 포함) | host 확인 | 같은 test |
| 전원 차단: 초기화의 모든 commit 직전에서 프로세스를 끊고 flash 파일로 다시 부팅 → 승인이 기록되기 전이면 등록 보존·재개, 그 뒤면 기존 네트워크 재개 없음·다음 부팅이 이어서 완료 | host 확인 | 같은 test(fork + NVS simulator 파일, `nvs_sim_power_cut_at_commit`). 실제 전원 차단이 아니다 |
| journal 손상·다른 보드·읽기 실패 → 안전 정지(재개·삭제 없음). 버튼 창 + 인증·확인된 새 초기화만 복구. 대체 journal을 못 쓰면 정지 유지 | host 확인 | 같은 test |
| 보존: `cfg`의 네 namespace와 `nvs`의 다른 namespace가 초기화 전후 byte 동일. 파티션·namespace 전체 삭제 호출과 `cfg` 접근이 코드에 없음 | host·정적 확인 | 같은 test, `tools/test_power_base_ble_enrollment_paths.py` |
| 원격 삭제와의 우선순위: `UNENROLLING`·`RELEASED`(삭제 대기)에서는 창 없음·명령 `BUSY`·record 불변, 최종 영수증은 버튼·명령·복구 초기화 모두에서 byte 그대로, 로컬 초기화는 영수증을 만들지 않음, journal이 있으면 초기화가 먼저 | host 확인 | 같은 test(실제 `pb_mgmt_client.c`의 `pb_mgmt_client_load`·`pb_mgmt_receipt_load` 사용) |
| 초기화 뒤: 다음 부팅은 Thread를 켜지 않고 서비스 허용 없음(exporter·하위 provisioning이 시작될 조건이 없음), 관리 poll 시작 조건 없음, 캐시된 endpoint 없음. 새 등록이 목적지·관리 자격증명을 교체하고 이전 키가 record에 없음 | host 확인 | 같은 test(실제 `power_base_enrollment.c`). 새 허브의 `heartbeat_received` 판정 자체는 허브·앱의 기존 test가 다루고 이 test는 주장하지 않는다 |
| 위 host test 합계 | host 확인 | `tools/test_power_base_reenroll.c` 366개 검사, 실패 0 (`-Wall -Wextra -Werror`) |
| host test의 민감도 | 확인 | 실제 소스 복사본에 결함 21가지를 하나씩 넣어 실행: 처음 19개 검출, 놓친 2개(최종 영수증 삭제, 부팅 뒤 안정된 놓음 생략)에 검사를 더해 21개 모두 검출. `~/esp-builds/reenroll_20261006_results/host/mutation/mutation_summary.txt` |
| 정적 불변식: BLE start/stop은 두 worker task에서만, 한 부팅에 worker 하나, 창 부팅은 부팅 규칙·`s_ready`를 부르지 않음, 버튼 task는 gate → 저장 → 재시작만 함, 버튼에 인터럽트 핸들러 없음, 재등록 코드는 mode 2 분기 안에만, 부팅 순서, 구성별 Kconfig | 정적 확인 | `tools/test_power_base_ble_enrollment_paths.py` 127개 검사(기존 검사 4개를 새 구조에 맞게 고치고 17개 추가) |
| Security 2 상호운용(Python, 기존): capability 목록이 버튼 없는 보드에서 이전과 같음 등 | host 확인 | `tools/test_power_base_ble_sec2_interop.py` 169개 검사, harness는 `~/esp-builds/reenroll_20261006_host_ble_sec2` |
| **C ↔ Swift 상호운용**: 실제 protocomm·Security 2 server + 실제 admission·service·`pb_reenroll.c`에 실제 Swift client로 전체 경로(수락 → 대기 → 수행 → 검증 → 중복 → 유예 뒤 종료), 인증만으로 변경 없음과 120초 만료, 다른 보드·옛 challenge·만료 challenge·창 없음, 틀린 설정 코드, 답 유실 뒤 같은 작업 조회(명령 재전송 없음), 미등록 보드, capability 없는 펌웨어의 실제 응답 | 상호운용 확인 | `PowerBaseBLEKit`의 `LocalResetInteropTests` 7개. BLE 링크는 pipe이고 OpenThread 저장소는 fake다 |
| 앱 BLE 패키지: 두 message의 codec(바이트 고정), 응답의 모든 거부 조건, 새 코드가 옛 message의 답으로 받아들여지지 않음, capability 이름, capability 없는 보드에는 아무것도 쓰지 않음 | sim 없이 Mac에서 확인 | `swift test` Debug와 Release(`-c release -Xswiftc -enable-testing`) 각각 84개 통과, skip 0 (이전 61개 + receipt 6개는 그대로, 새 16개) |
| 펌웨어 host C test 전체 | host 확인 | `tools/test_*.c` 67개 프로그램을 각 파일 머리말의 명령으로 빌드·실행, 최종 소스에서 67/67 통과(다른 작업이 없을 때). 그 전에 Xcode·ESP-IDF 빌드와 동시에 돌린 실행에서는 시간 측정 test 1개(`test_sensor_transport_gate_retry`, 이번 변경과 무관한 component)가 1회 실패했고 단독 5회 재실행은 모두 통과했다 — 아래 "생략과 실패" |
| ESP-IDF v6.1 빌드: Power Base 기본·bench·BLE·BLE+GPIO9, host harness | 빌드 확인 | 기본 880,848 B, bench 1,017,024 B, BLE 1,426,608 B, BLE+GPIO9 1,430,864 B(app 파티션 1,536,000 B의 93%). 재등록 창 로그 문자열과 `CONFIG_POWER_BASE_REENROLL_BUTTON=y`는 GPIO9 구성에만 있다. `local_reset_v1` 문자열은 일반 BLE 바이너리에도 들어 있지만(선택은 실행 시) 버튼 구성이 아니면 알리지 않는다 |
| module 펌웨어 빌드(새 component가 `components/`를 공유하는 다른 프로젝트를 깨지 않는지) | 빌드 확인 | 새 디렉터리 `~/esp-builds/reenroll_20261006_module`, 885,408 B. RCP는 `components/`를 쓰지 않아 빌드하지 않았다 |
| 앱 로컬 초기화 controller: 인증 → 확인 → 명령 1회 → 같은 작업 조회 → 보드의 "검증 완료 + 미등록"만 준비 완료. 인증만으로는 challenge도 요청하지 않음. 확인 거절·화면 이탈·허브 변경 뒤 명령 없음. capability 없는 보드·다른 보드·미등록 보드·창 없음·원격 삭제 중·다른 초기화 중에는 명령 없음. 거부 코드는 결과로 처리 | sim 확인 | `PowerBaseLocalResetControllerTests` 27개 (scripted 보드·provider, 허브 없음) |
| 앱 응답 유실·늦은 응답: 답 유실 → "결과 확인 불가" → 재연결해 같은 작업 ID로 조회(명령 재전송 0회), 도착하지 않은 명령은 "시작하지 않음"으로 끝나고 새 시도는 새 확인·새 작업 ID, 수락 직후 보드 재시작, 조회 실패·틀린 코드에서도 "실패"로 떨어지지 않음, 끝난 실행의 늦은 답이 다음 실행의 상태를 덮지 않음, 다른 보드에는 묻지 않음 | sim 확인 | 같은 test |
| 앱 foreground·이탈: 백그라운드에서 연결·opening의 각 message·상태·초기화 상태 조회·challenge·초기화 명령·후속 조회를 시작하지 않음(진행 중인 답은 받음). 확인 직후 이탈·challenge 대기 중 이탈은 명령을 보내지 않음. 명령이 나간 뒤의 이탈·허브 변경은 "취소"가 아니라 "결과 확인 불가" | sim 확인 | 같은 test |
| 같은 보드 재연결 | sim 확인(간접) | controller는 매 실행마다 provider의 `openSession`으로 새 연결을 연다(같은 test). 이전 연결의 종료를 확인한 뒤 연결하는 것은 기존 `PowerBaseEnrollmentBLELifecycleTests`·`…ConnectPermitTests`가 다루는 공용 코드다(이번에 다시 실행, 통과). 실제 CoreBluetooth는 미실행 |
| 앱 표시 규칙과 문구: 여덟 단계가 서로 다른 단계·문구로 표시됨, "검증 완료"와 "등록됨"·"결과 확인 불가"와 "지우지 않음"이 같은 문구가 아님, 모든 문구가 en·es에 있고(한국어 실행) en·es 실행에서 한국어가 남지 않음 | sim 확인 | `PowerBaseReenrollPresentationTests` 6개, `-testLanguage ko`·`en`·`es` |
| 앱 Power Base test·번역·브랜드 검사 (새 3개 class 포함 30개 class) | sim·Mac 확인 | 340개. iPhone 13 mini·iPad Air 11 simulator와 Mac Catalyst(서명한 Debug test host)에서 `ko` 340개 통과, `en`·`es` 338개 통과 + 2개 skip(한국어 실행 전용), 실패 0 |
| 기존 일반 등록·삭제·영수증과 앱 후속 수정 세 가지(삭제 확인 진입, 끝난 실행의 요청 차단, 백그라운드 차단)의 회귀 | sim·Mac 확인 | 위 340개 안의 `PowerBaseEnrollmentBLE*`, `PowerBaseManagedEnrollmentTests`, `PowerBaseRemoval*`, `PowerBaseReleaseConfirmation*`(`…FollowUpTests`, `…ConnectPermitTests`, `PowerBaseEraseConfirmationEntryTests`, `PowerBaseEnrollmentReceiptRelay*`), 실패 0 |
| 앱 회귀 test 전체 (iPhone 13 mini·iPad Air 11 simulator, Debug) | sim 확인 | 각각 922개 중 906개 통과, 16개 skip(환경 변수가 필요한 렌더링·실 네트워크 test와 한국어 실행 전용 test), 실패 0 |
| 앱 빌드 | 빌드 확인 | iOS Simulator Debug·Release, iOS 기기 Debug·Release(`CODE_SIGNING_ALLOWED=NO`), Mac Catalyst Release(`CODE_SIGNING_ALLOWED=NO`), Mac Catalyst Debug(test 포함, 자동 서명) |
| Release 점검 | 확인 | `Scripts/check_release_enrollment_surface.sh`(재등록 화면·로컬 초기화 controller 항목 추가): iOS Simulator·iOS 기기·Mac Catalyst Release에 재등록 화면과 controller가 있고, 시험 화면·USB 콘솔 시험·PSKd 표시/복사·DEBUG 문구 표·`UIPasteboard`가 없음. Debug 3종에서는 `debug` 모드로 시험 전용 항목을 찾음 |
| 화면 렌더링 | 눈으로 확인(일부) | "다른 허브에 연결하기" 20개 상태를 iPhone 13 mini·iPad(form sheet 폭)·Mac Catalyst(좁은 창·넓은 창)에서 ko·en·es, 밝은 화면·어두운 화면·가장 큰 글자(AX XXXL)로 PNG 렌더링(iOS 1,725장, Mac 342장; 기기 추가 시트 포함 전체는 1,818장·365장). 그 가운데 7장만 열어 봤다(iPhone ko 확인·시작·결과 확인 불가(어두운 화면), iPad en 검증 완료, Mac es 확인(가장 큰 글자)). 나머지는 만들기만 했다 |

### 생략과 실패

- **실물 전부.** 버튼 누름 판정, GPIO9의 실제 배선과 pull-up, 재시작 뒤의 BLE 광고와 120초, 창 부팅에서
  Thread가 실제로 꺼져 있는지, `otInstanceErasePersistentInfo()`와 `openthread` namespace의 실제 삭제, 실제
  전원 차단, 초기화 뒤 새 허브·새 Thread 네트워크로의 Joiner 가입과 `heartbeat_received`, BLE와 Thread의
  실제 공존, 센서 모듈이 Base의 2분 부재 뒤 다시 붙는 것.
- RCP 펌웨어 빌드(이번 변경과 무관, `components/`를 쓰지 않는다).
- Linux 쪽 전체(이번 변경 없음): host CMake test도 다시 돌리지 않았다.
- 실제 iPhone·iPad·Mac에서의 실행, VoiceOver, 실제 Bluetooth 권한·연결, 확인 대화상자의 실제 표시.
  렌더링은 SwiftUI 본문만 그린 것이고 `confirmationDialog`는 그려지지 않는다.
- Mac Catalyst에서의 전체 앱 회귀 test, watchOS 대상의 test.
- 실패 기록: (1) `tools/test_power_base_ble_enrollment_paths.py`가 처음에 4개 실패 — 이전 구조("BLE는
  `enrollment_task` 한 곳에서만", "`enrollment_ble.c`에 erase라는 말이 없음")를 고정한 검사였고, 새 worker와
  로그 문구가 걸렸다. 불변식을 새 구조로 다시 적고(두 worker, 한 부팅에 하나) 로그 문구를 고쳤다.
  (2) Swift `CodecTests.testEveryCodeOfTheHeaderIsKnown`이 코드 수 18을 고정하고 있어 20으로 고쳤다.
  (3) 새 host test의 첫 실행에서 1개 실패 — test의 시간 계산 착오였다. (4) 위의 시간 측정 test 1회 실패.
- 앱 쪽 실패 기록: 새 controller test 27개 중 1개가 첫 실행에서 실패 — test helper가 "명령이 아직 없음"을
  단언하는데 그 시나리오는 이미 명령 1회를 보낸 뒤였다(test 착오, 고침). Swift interop test 1개는 첫
  컴파일에서 `async` autoclosure 오류(고침).

### 2026-10-05 기록에서 달라진 것 (현재 근거로 정리)

위쪽 "Power Base 관리(일반 등록·삭제) 검증 (2026-10-05)"의 표와 수치는 **그 날의 기록으로 그대로 둔다.**
그 뒤의 앱 후속 수정과 이번 변경으로 지금은 다음과 같다(2026-10-06 실행 기준).

| 2026-10-05 기록 | 지금 |
|---|---|
| "삭제 확인"은 `released` 작업에서 진입 | `released` **또는** `sent`·`accepted`인 채 `result_unknown`이 된 작업에서 진입(`offersEraseConfirmation`). 그 밖에서는 제공하지 않는다. [`power-base-management.md`](power-base-management.md) 11절 |
| `tools/test_power_base_mgmt.c` 161개 검사 | 220개 |
| `tools/test_power_base_enrollment.c` 256개 검사 | 257개(실행 로그의 `ok:` 줄 기준) |
| `tools/test_power_base_ble_admission.c` 380개 검사 | 398개 |
| `tools/test_power_base_ble_sec2_interop.py` 157개 검사 | 169개 |
| `tools/test_*.c` 66개 프로그램 | 67개(`test_power_base_reenroll.c` 추가) |
| Swift 패키지 61개 | 84개 |
| 앱 Power Base test·번역·브랜드 검사 70개 / 94개 | 340개(30개 class; `ko` 340 통과, `en`·`es` 338 통과 + 2 skip) |
| 앱 회귀 test 838개 중 825개 통과, 13개 skip | 922개 중 906개 통과, 16개 skip |
| 펌웨어 빌드 크기: 기본 871,888 B, bench 1,008,352 B, BLE 1,413,200 B | 기본 880,848 B, bench 1,017,024 B, BLE 1,426,608 B |

### 다음 실물 시험 절차 (준비만 했고 실행하지 않았다)

여분 보드 1대(mode 2 준비 이미지와 그 보드의 설정 코드가 있는 것), 현재 허브 A, 옮겨 갈 허브 B(없으면 A를
"새 허브"로 다시 써서 초기화·재등록만 확인), iPhone의 Debug 또는 Release 앱.

0. **배선 확인(전원을 넣기 전).** 보드의 BOOT 버튼이 GPIO9와 GND 사이에 있는지 회로도나 멀티미터로
   확인한다. 아니면 이 구성으로 시험하지 않는다.
1. `~/esp-builds/reenroll_20261006_ble_gpio9`의 이미지를 기록하고(기존 `cfg`는 그대로) 허브 A에 평소대로
   등록한다. 기대 로그: `re-enrollment button: GPIO9, pressed = low`.
   같은 날 뒤에 버전 체계가 들어간 같은 구성의 이미지가 따로 있다:
   `~/esp-builds/pogover_20261006_power_base_ble_reenroll_gpio9/artifact/`(manifest 포함, 아래 "버전 관리
   체계 검증"). 이쪽을 쓰면 기록 전에 `tools/pogo_version.py manifest-verify --dir <artifact>`를 통과시키고,
   부팅 로그의 `POGO power_base 0.1.0 build … (development, ble+reenroll_button_gpio9)` 줄을 함께 남긴다.
   **어느 이미지를 썼는지(SHA-256)를 기록한다.** 두 이미지는 다른 파일이며 어느 쪽도 실물에서 실행해 보지
   않았다.
2. 짧게(1초), 4초 누름 → 아무 로그도 재시작도 없어야 한다. 전원을 넣으면서 BOOT를 누르는 조작은 하지 않는다.
3. 5초 넘게 누른 뒤 놓음 → `re-enrollment button: window requested … -- restarting`, 재시작 뒤
   `boot: re-enrollment window requested by the button`, `re-enrollment window open as 'POGO-PB-…' for 120 s`.
   `power base ready`·exporter 로그가 **없어야** 한다.
4. 아무것도 하지 않고 120초 → `re-enrollment window over -- restarting`, 그 뒤 평소처럼
   `boot: enrollment marker and stored Dataset match` 와 허브 A의 새 heartbeat. (복원 확인)
5. 다시 3번 → 앱 "기기 추가" → "다른 허브에 연결하기" → 찾기 → 틀린 설정 코드 → "지우지 않았습니다",
   창 만료 뒤 4번과 같은 복원.
6. 다시 3번 → 올바른 설정 코드 → 확인 화면에서 "지우지 않고 그만두기" → 창 만료 뒤 복원.
7. 다시 3번 → 올바른 설정 코드 → "기존 등록 정보 지우기" → 대화상자 "지우기". 기대 로그:
   `local reset accepted`, `reset approved and journaled`, `reset verified`, 약 3초 뒤 재시작,
   `boot: not enrolled (none)`, `safe wait: BLE enrollment open`. 앱은 "기존 등록 정보를 지웠습니다".
   허브 A에는 그 Base가 offline으로 남아야 한다(삭제 작업은 생기지 않는다).
8. 앱에서 허브 B를 선택하고 "새 허브에 등록" → 기존 등록 흐름 → 허브 B의 `heartbeat_received`.
9. 전원 차단: 7번의 `reset approved and journaled` 직후 전원을 뽑는 것을 몇 번 시도하고, 다시 켰을 때
   `boot: a local reset was journaled and not finished -- carrying on`과 `reset verified`가 나오고 허브 A로
   돌아가지 않는지 본다. (시점을 맞추기 어렵다. 못 맞춘 시도는 그렇게 기록한다.)
10. 응답 유실: 7번에서 "지우기" 직후 앱을 Bluetooth 범위 밖으로 가져가거나 Bluetooth를 끈다 → "지우기
    결과를 확인하지 못했습니다" → 돌아와 "결과 확인" → "기존 등록 정보를 지웠습니다". 보드 로그에 초기화가
    한 번만 있어야 한다.

각 단계의 보드 로그와 앱 화면을 남기고, 하지 못한 단계는 하지 못했다고 적는다.

## 버전 관리 체계 검증 (2026-10-06)

> 이 절은 첫 구현의 기록이다. 같은 날의 검토에서 결함 두 건이 재현되어 고쳤고, 그 뒤 모든 구성요소의
> 빌드 식별자가 달라졌다. 현재 상태는 아래 "버전 관리 체계 보완: 빌드 입력과 manifest 대조"를 본다. 이 절의
> 식별자와 산출물은 당시 규칙으로 만든 것이며 고치지 않았다.

범위: 구성요소 버전의 단일 원본(`versions.json`, 앱 `Config/Version.xcconfig`), 소스 지문과 빌드 식별자,
빌드에 들어가는 식별 정보(ESP-IDF app description, 부팅 로그, Linux `--version`과 health `build`, 앱 설정
화면), 빌드 manifest와 그 검증, 설치·실물 시험 기록 양식. 규칙과 도구는 [`versioning.md`](versioning.md),
앱 쪽은 앱 저장소의 `docs/versioning.md`.

**이 날 실행한 것은 Mac의 host test, 펌웨어 빌드, Linux API의 CMake host 빌드, iOS 시뮬레이터·Mac Catalyst
test와 빌드뿐이다.** 보드·직렬 포트·실물 BLE에 접근하지 않았고(flash·erase 없음), Linux 호스트에
접속·배포하지 않았으며, 어떤 산출물도 설치하지 않았다. 따라서 **부팅 로그의 버전 줄, 운영 허브의 health
`build`와 `pogo_matter_bridge --version`, 실제 기기의 앱 정보 화면은 실물로 확인하지 않았다.** Linux
bridge 본체(GN/CHIP)는 Mac에서 빌드할 수 없어 컴파일도 하지 않았다.

기준: 펌웨어 저장소 `feature/unified-module-role-manager` @ `590e2a11d2cb93f1cbc2b2b0eb094d50ea289a50` +
미커밋 변경(일반 등록·삭제·영수증, BOOT 재등록, 이 체계), 앱 저장소 `main` @
`05375341f70d93c788b5f08889e312a023f83202` + 미커밋 변경. stage·commit·tag·push하지 않았다. 기존
`~/esp-builds/*` 55개 파일의 크기·수정 시각과 프로젝트 `sdkconfig` 3개의 SHA-256은 작업 전후 같다. 새
산출물은 `~/esp-builds/pogover_20261006_*`에만 있다. **만든 산출물은 모두 dirty 트리의 개발 빌드다.
배포 후보가 아니다.**

### 도구와 host test

근거의 종류: **host** = Mac에서 실행한 test, **빌드** = 대상 툴체인으로 컴파일·링크, **이미지 대조** =
만들어진 `.bin`·번들·실행 파일이 스스로 말하는 값을 읽어 비교, **sim** = iOS 시뮬레이터·Mac Catalyst의
XCTest.

| 항목 | 결과 | 근거 |
|---|---|---|
| 펌웨어 도구 `tools/test_pogo_version.py` | host: 131 checks, 0 failed | 임시 디렉터리(경로에 공백)와 임시 Git 저장소. 버전 형식 거부, clean·dirty·untracked, 같은 HEAD의 다른 미커밋 소스 → 다른 지문, 다른 디렉터리·다른 수정 시각 → 같은 지문, Git 없는 묶음(메타데이터 유무·변조), stamp가 내용이 같으면 파일을 다시 쓰지 않음, manifest 생성과 거부 사례, 도구가 트리·index를 바꾸지 않음 |
| 앱 도구 `PogoHome/Scripts/test_pogo_app_version.py` | host: 117 checks, 0 failed | 같은 항목의 앱판. 타깃이 버전을 따로 정한 프로젝트·숫자를 직접 적은 Info.plist 검출, 번들(위젯·Watch 앱 포함)과 버전 파일의 대조, manifest 거부 22가지 |
| 지문이 두 저장소에서 같은 형식 | host | 앱 test가 문서의 형식대로 직접 계산한 값과 도구의 값을 비교 |
| 펌웨어 7개 구성 빌드 + manifest | 빌드·이미지 대조: 7/7 | `tools/pogo_version.py build-firmware`. 각 이미지의 app description `version`이 `0.1.0`, 이미지 안의 `POGO_BUILD_INFO`가 현재 트리의 식별자·variant와 일치. 아래 표 |
| manifest 변조·누락·불일치 (실제 산출물의 사본) | host: 17/17 거부 | 이미지 1바이트 변경, bootloader 누락, manifest 누락, 다른 variant·다른 구성요소·체계 이전 이미지로 바꿔치기, 기대 구성요소·target·variant·버전·release 불일치, manifest를 고쳐 release·`0.2.0`·다른 target·실물 검증을 주장, 알 수 없는 스키마, 줄인 commit. `logs/manifest_checks.log` |
| Git 없는 소스 묶음 (실제 RCP 빌드) | 빌드·이미지 대조 | 사본(`.git` 없음, 경로에 공백): 메타데이터 없으면 `unknown.<지문>`, 일치하는 `SOURCE_INFO.json`이 있으면 원래 commit 유지, manifest `origin: source-info-verified` |
| 증분 빌드의 메타데이터 갱신 (실제 RCP 빌드) | 빌드·이미지 대조 | 같은 빌드 디렉터리에서 4회: 변경 없음 → 식별자 같음·헤더 다시 쓰지 않음·재컴파일 0, 소스 1줄 수정 → 2개 object만 다시 컴파일(전체 862)되고 이미지가 **새** 식별자, 되돌림 → 원래 식별자 |
| 빌드 중 소스 변경 감지 (실제 RCP 빌드) | 빌드 | 빌드가 도는 동안 소스 수정 → 도구 종료 코드 2, `artifact/`와 manifest 없음 |
| Linux API host 빌드 (CMake) | host: ctest 3/3 | `pogohome_api_unit_tests`(기존 전체 + `test_build_info.cpp`), `versions.json`과의 일치, stamp 없는 빌드는 `unknown`. `report_build`를 켜지 않은 health는 이전과 바이트 단위로 같고, 켜면 이전 본문 끝에 `,"build":{…}`만 붙는다 |
| `pogohome-hubctl`의 manifest (Mac host 빌드) | 이미지 대조 | ESP-IDF 이미지가 아닌 실행 파일도 같은 방식으로 기술·검증된다: `manifest-create` → `manifest-verify` 통과, manifest의 버전만 `0.2.0`으로 고치면 "실행 파일은 0.1.0이라고 한다"로 거부. `logs/linux_hubctl_manifest.log` |
| `pogohome-hubctl --version` | 이미지 대조 | `pogohome-hubctl 0.1.0 (linux_bridge, build 590e2a11d2cb+dirty.b11a7937825a, development)`. 증분 재빌드 전의 실행 파일은 이전 소스의 식별자(`…cfbfdd18c5bd`)였고, 재빌드 뒤 현재 트리의 값이 됐다 |
| Linux ingest·core host 빌드 | host: ctest 8/8, 1/1 | 이 변경과 무관한 회귀 확인 |
| 펌웨어 host C test 전체 | host: 67/67 | `host/all_host_c_summary.txt` |
| 펌웨어 Python test 전체 | host: 14/14 | `host/py_summary.txt`. `test_rcp_config.py` 52 checks |
| 앱 영향 class (iPhone 13 mini, iPad Air 11, Mac Catalyst × ko·en·es) | sim: 각 85개. ko 85 통과, en·es 84 통과 + 1 건너뜀 | `AppBuildInfoTests`(12), 허브 발견·검증, 번역 catalog, 브랜드, 설정 화면 렌더. 건너뛴 1개는 한국어 실행에서만 도는 번역 조회 |
| 앱 전체 test (iPhone 13 mini, iPad Air 11) | sim: 934개 중 916 통과, 18 건너뜀, 0 실패 (양쪽 같음) | 건너뛴 18개는 모두 조건부: 렌더 디렉터리 미지정 13, 실 네트워크 2, 한국어 실행 전용 번역 조회 3(이 가운데 이번에 추가한 것은 위 한국어 실행에서 통과). BOOT 재등록 검증 때의 922개에서 `AppBuildInfoTests` 12개가 늘었다 |
| 앱 빌드 5종 + manifest | 빌드·이미지 대조: 5/5 | `Scripts/pogo_app_version.py build`: 시뮬레이터 Debug·Release, 기기 Debug·Release(서명 없음), Mac Catalyst Release(서명 없음). 번들의 앱·위젯·Watch 앱·Watch 위젯이 모두 `1.0.0 (2)`, `PogoBuild*`가 현재 트리의 값 |
| 앱 manifest 변조 (실제 산출물의 사본) | host: 10/10 거부 | 실행 파일 변경, 위젯 Info.plist 누락, manifest 누락, 위젯만 다른 버전, Debug 번들로 바꿔치기, release·다른 버전 기대, manifest를 고쳐 release·빌드 번호 3·배포됨을 주장. `logs/app_manifest_checks.log` |
| Xcode로 그냥 빌드한 앱 | 이미지 대조 | 버전·빌드 번호는 `1.0.0 (2)`, `PogoBuild*`는 모두 `unknown`. 도구 빌드의 식별자를 기대하면 5개 항목이 불일치로 나온다(추정값이 들어가지 않는다) |
| Release 표면 검사 | 빌드: 5/5 as expected | `Scripts/check_release_enrollment_surface.sh` (BOOT 재등록 검증과 같은 검사) |

그 과정에서 고친 것:

- **RCP 격리 규칙 위반(회귀).** 처음에는 RCP가 공유 컴포넌트 `components/pogo_build_info`를
  `EXTRA_COMPONENT_DIRS`로 끌어다 썼고, 기존 정적 검사 `tools/test_rcp_config.py`의 "rcp/는 다른 프로젝트와
  아무것도 공유하지 않는다"가 실패했다(45 중 1). RCP가 같은 몇 줄을 `rcp/main/rcp_build_info.c`에 따로
  갖도록 바꾸고 `esp_ot_rcp.c`는 원래대로 되돌렸다. 검사에 7개 항목을 더했다(52 checks 통과). 그 전의
  펌웨어 산출물은 현재 소스와 맞지 않으므로 `pogover_20261006_results/superseded_before_rcp_isolation/`으로
  옮기고 7개 구성을 다시 빌드했다.
- **RCP 진단 구성 판별.** 도구가 진단 구성을 로그 수준으로 판별했는데 두 구성 모두 로그가 없다. 빌드는
  됐지만 도구가 "요청한 variant가 아니다"라며 manifest를 거부했다. 판별 기준을 core dump 설정으로 바꿨다.
- **링커가 빌드 정보를 버림.** 첫 빌드에서 `POGO_BUILD_INFO` 문자열이 section GC로 빠져 manifest가
  거부됐다. `-u pogo_build_info_string`으로 유지한다.
- **health 필드의 기존 test 영향.** `build`를 무조건 붙이자 기존 health 비교 6건이 실패했다. bridge 본체만
  켜는 옵션(`report_build`)으로 바꿔 라이브러리 기본 동작을 이전과 같게 했다.

### 산출물 (모두 개발 빌드, dirty)

펌웨어 (`~/esp-builds/pogover_20261006_<구성요소>_<variant>/artifact/`, ESP-IDF v6.1, esp32h2):

| 구성 | 버전 | 빌드 식별자 | 앱 이미지 크기 (B) | 앱 이미지 SHA-256 |
|---|---|---|---:|---|
| `power_base` default | 0.1.0 | `590e2a11d2cb+dirty.16b17f694c7a` | 881,440 | `0797814a3c2c253a33d1249d199e06b4100063840634535dbc12ffe41d599075` |
| `power_base` bench | 0.1.0 | 같음 | 1,017,616 | `297a9ea3cae7c3c26a03f4390609d464a4f9df696991dcb8281d5c1be0b26d8a` |
| `power_base` ble | 0.1.0 | 같음 | 1,427,168 | `4980ad07510eaccdd7165405e570c77b595d48cef84e4b1501d5a4cf1ab871d6` |
| `power_base` ble + GPIO9 버튼 | 0.1.0 | 같음 | 1,431,488 | `95fb2070621f9d65c69418b49da0581902e7298f521adfa62c85ab1a2a559abb` |
| `module` default | 0.1.0 | `590e2a11d2cb+dirty.f8f8d6837397` | 885,984 | `ffb23a72bdab6b2140bb22bada897911997220620886e8fc2df64e7199b777b3` |
| `rcp` default | 0.1.0 | `590e2a11d2cb+dirty.1c9a0fe4f323` | 254,288 | `a7621f4a92568da2cd1eb4e0a029e8b0f657df18c7d1c27ca04e0087fa1a3439` |
| `rcp` diag | 0.1.0 | 같음 | 259,600 | `5fb694261b23d45f4116610e7e64c29341d9db55a0bfcbcaeda7bc8cd664e18a` |

- Power Base의 가장 큰 구성(BLE + 버튼)은 앱 파티션 1,536,000 B의 93.2%다. 버전 정보로 늘어난 크기는
  같은 구성의 BOOT 재등록 검증 빌드(1,430,864 B) 대비 624 B다.
- 같은 소스로 만든 네 Power Base 구성은 빌드 식별자가 같고 variant와 `config_id`(sdkconfig 해시)로
  구분된다. manifest 검증은 variant가 다른 이미지로 바꿔치기한 것을 거부한다.

앱 (`~/esp-builds/pogover_20261006_app_<이름>/artifact/`, 빌드 식별자 `05375341f70d+dirty.899cba196633`):
`sim_debug`, `sim_release`, `device_debug`, `device_release`, `catalyst_release`. 모두 `1.0.0 (2)`,
`development`. 기기용 두 개는 서명하지 않았으므로 그대로 설치할 수 없다.

Linux: `~/esp-builds/pogover_20261006_linux_api_host/`(API 라이브러리와 `pogohome-hubctl`의 Mac host
빌드)와 그 manifest 예 `~/esp-builds/pogover_20261006_linux_hubctl_host_artifact/`. Mac용 실행 파일이므로
허브에 올릴 산출물이 아니다.

### 하지 않은 것

| 항목 | 상태 |
|---|---|
| Linux 장비의 GN/CHIP 빌드 (`tools/build_matter_bridge.sh`) | **미실행.** `BUILD.gn`의 변경(빌드 정보 소스와 include 경로), 스크립트의 stamp 단계, `chip/main.cpp`의 `--version`과 `report_build`는 컴파일해 보지 않았고, 스크립트도 실행해 보지 않았다. 같은 `build_info.cpp`와 stamp 도구는 Mac의 CMake 빌드로만 확인 |
| `pogo_matter_bridge --version`, 운영 허브의 health `build` | 미확인 |
| 보드 기록 뒤 부팅 로그의 `POGO … build …` 줄 | 미확인. 이미지 안의 문자열과 app description만 `.bin`에서 읽어 확인 |
| 버전 체계가 들어간 RCP의 OTBR 연결 | 미확인. RCP 소스(`esp_ot_rcp.c`, Spinel 경로)는 바꾸지 않았고 빌드 정보는 호출되지 않는 문자열 하나다 |
| 실제 iPhone·iPad·Apple Watch의 앱 정보 화면, `1.0 (1)` 위 업데이트 설치 | 미확인 |
| release 모드 빌드 | 만들지 않음(트리가 dirty). 거부되는 것은 host test와 `settings --release`로 확인. clean 트리에서 통과하는 것은 임시 Git 저장소의 host test로만 확인 |
| 서명된 배포용 archive, TestFlight | 미실행 |
| 설치·실물 시험 기록 | 한 건도 없음. 양식만 있다(`docs/release-records/README.md`) |
| PowerBaseBLEKit Swift package test 재실행 | 하지 않음. package와 host harness의 입력이 이 작업으로 바뀌지 않았다(BOOT 재등록 검증의 84개 결과가 최신) |

### Linux에서 이어서 확인할 것

```bash
# Linux 빌드 장비 (읽기·빌드만. 설치와 서비스 재시작은 별도의 운영 작업이다)
python3 tools/pogo_version.py identity --component linux_bridge
tools/build_matter_bridge.sh                     # stamp 단계와 GN 빌드가 통과하는가
<out>/pogo_matter_bridge --version               # "pogo_matter_bridge 0.1.0 (linux_bridge, build …, development)"
<out>/pogohome-hubctl --version
strings -a <out>/pogo_matter_bridge | grep 'POGO_BUILD_INFO{'   # identity의 지문과 같은가
# 산출물 디렉터리(새 디렉터리에 실행 파일만 복사)의 manifest. 설정·키 파일은 넣지 않는다.
python3 tools/pogo_version.py manifest-create --dir <artifact> --component linux_bridge \
    --configuration <gn args 이름> --variant gn-linux --target <예: linux-arm64> \
    --primary pogo_matter_bridge pogo_matter_bridge pogohome-hubctl pogohome-otbr-helper
python3 tools/pogo_version.py manifest-verify --dir <artifact> --expect-component linux_bridge

# 설치한 뒤 (운영 작업, 기록은 docs/release-records/ 양식으로)
curl -s http://<LAN-address>:8787/api/v1/health   # 마지막 필드가 "build":{…}이고 --version과 같은가
```

## 버전 관리 체계 보완: 빌드 입력과 manifest 대조 (2026-10-06)

위 "버전 관리 체계 검증"의 구현을 검토하면서 재현된 결함 두 건을 고쳤다. **위 절의 식별자·산출물 표는
그 당시 규칙의 기록으로 그대로 둔다.** 이 절이 그 뒤의 상태다. 제품 버전은 바꾸지 않았다(각 `0.1.0`, 앱
`1.0.0 (2)`).

**이 보완에서도 실행한 것은 Mac의 host test, 펌웨어·앱 빌드, Linux API의 CMake host 빌드뿐이다.** 보드·직렬
포트·실물 BLE·Linux 호스트에 접근하지 않았고 아무것도 설치하지 않았다. Linux GN/CHIP 빌드는 여전히
실행하지 않았다. stage·commit·tag·push하지 않았다.

### 결함과 수정

| | 결함 (수정 전 도구로 재현) | 수정 |
|---|---|---|
| 1 | 소스 지문이 빌드가 실제로 쓰는 `tools/cmake/pogo_version.cmake`, `tools/pogo_version.py`(앱은 `Scripts/pogo_app_version.py`)를 빼고 있었다. 공용 CMake 파일에 컴파일 정의를 더해도 지문·빌드 식별자가 같았고, clean으로 판정되어 release가 허용됐다 | 빌드 입력을 구성요소별로 파일 단위로 명시(`COMPONENT_SOURCES`, 앱 `INCLUDED_FILES`). 지문·dirty 판정·빌드 전후 비교가 한 정의(`is_input`)를 쓴다. Git이 무시하지만 빌드가 읽는 파일도 dirty, 삭제된 입력도 dirty |
| 2 | `manifest-verify`가 바이너리의 variant·config를 manifest의 `build.variant`·`config_id`와 대조하지 않았다. 일반 BLE 이미지(`variant=ble`)의 manifest를 `ble+reenroll_button_gpio9`·`config_id: unknown`으로 고치고 `--expect-variant`로 그 구성을 요구하면 **통과**했다 | `primary` 바이너리를 매번 다시 읽어 variant·config id·dirty·app description·이미지 헤더의 대상 칩까지 대조. `--expect-*`는 그 위에 추가. 필수 필드·형식 검사. 읽을 수 없는 값은 "not verified"로 출력. `--match-current-tree`로 현재 트리와의 일치를 따로 묻는다 |

초기화 정책·BLE 규약·API는 바꾸지 않았다. 바뀐 파일은 두 도구와 그 test, 문서뿐이다(펌웨어 소스 변경
없음). 다만 도구가 빌드 입력이 됐으므로 **모든 구성요소의 지문과 빌드 식별자가 달라졌다.**

| 구성요소 | 수정 전 도구가 계산한 식별자 | 수정 후 (같은 소스) |
|---|---|---|
| `power_base` | `590e2a11d2cb+dirty.16b17f694c7a` | `590e2a11d2cb+dirty.79bad6c10044` |
| `module` | `590e2a11d2cb+dirty.f8f8d6837397` | `590e2a11d2cb+dirty.a7327b0a85fe` |
| `rcp` | `590e2a11d2cb+dirty.1c9a0fe4f323` | `590e2a11d2cb+dirty.6334e84b8702` |
| `linux_bridge` | `590e2a11d2cb+dirty.b11a7937825a` | `590e2a11d2cb+dirty.fdd81af24e05` |
| 앱 | `05375341f70d+dirty.899cba196633` | `05375341f70d+dirty.c5e8b536e6e7` |

### 실행한 검사

근거의 종류는 위 절과 같다(host / 빌드 / 이미지 대조).

| 항목 | 결과 | 근거 |
|---|---|---|
| 새 회귀 검사가 **수정 전** 도구에서 실패하는가 | 펌웨어 도구: 250 checks 중 75 실패, 앱 도구: 180 중 37 실패. 실패는 모두 새로 더한 검사이고 기존 검사는 전부 통과 | `logs/fw_tool_test_BEFORE_fix.log`, `logs/app_tool_test_BEFORE_fix.log`. 수정 전 도구 사본은 `prefix_tools/` |
| 검토 사례의 독립 재현 (수정 전) | `pogover_20261006_power_base_ble/artifact`의 **사본**에서 manifest만 고쳐 `--expect-variant ble+reenroll_button_gpio9` → 종료 코드 0 | `logs/repro_wrong_variant_BEFORE_fix.log` |
| 같은 사례 (수정 후) | 종료 코드 1: "the binary says variant=ble …", "the binary says config=… the manifest's build.config_id says unknown". 검토자의 사본(읽기만)에서도 같다 | `logs/repro_wrong_variant_after_fix.log` |
| 펌웨어 도구 `tools/test_pogo_version.py` | host: 252 checks, 0 failed | 공용 CMake만 수정 → 세 펌웨어 구성요소의 지문·식별자 변경·dirty·release 거부, bridge는 불변(구성요소별 의존성). 도구 수정 → 넷 다 변경. 이미 dirty인 트리에서 도구만 수정 → 식별자 변경. 입력 삭제 → dirty. Git이 무시하는 입력 → dirty. 출력·캐시·생성 헤더·manifest → 불변. Git 없는 묶음에서 입력 수정 → `SOURCE_INFO` 불인정. 빌드 중 입력 변경 → manifest 거부. 실제 빌드 파일이 가리키는 `../tools/…`·`components/<이름>`이 입력 목록에 있음. 작업 트리의 모든 파일에 대해 지문 목록과 dirty 판정이 같은 파일을 고름 |
| 같은 test의 manifest 부분 | 위 252에 포함 | 정상 산출물 통과. BOOT 구성으로 표시한 manifest 거부(`--expect-variant`로 요구해도, `binary_says`까지 고쳐도). config id·target·`binary_says` 불일치 거부. 필수 필드 19개 삭제, 형식 오류 17가지 거부. 변조·누락·기대 불일치는 계속 거부. 다른 칩용 이미지는 manifest 생성 거부. ELF·Mach-O 실행 파일의 target |
| 앱 도구 `Scripts/test_pogo_app_version.py` | host: 181 checks, 0 failed | 빌드 도구 수정 → 지문·식별자 변경·dirty·release 거부, 다른 스크립트는 불변. 가짜 `xcodebuild`로 빌드 흐름 전체를 돌려 **빌드 중 도구 변경 → manifest 없음**. 필수 필드·형식, `bundle_says`와 실제 번들, 플랫폼(`DTPlatformName`) 대조 |
| 실제 RCP 증분 빌드 (사본, `.git` 없음, 경로에 공백) | 빌드·이미지 대조: 19 checks | 같은 빌드 디렉터리에서 7회. 변경 없음 → 재컴파일 0. **공용 CMake 파일에 주석 한 줄** → 이미지의 식별자가 트리의 새 값(63개 재컴파일: CMake 재구성). **도구에 주석 한 줄** → 새 값(1개 재컴파일). 소스 수정 → 새 값(2개). 되돌림 → 원래 값. **검토 사례(컴파일 정의 추가)** → 트리와 이미지 모두 새 값(740개 재컴파일). `logs/fw_scratch_verify.log`, `fw_scratch/` |
| 실제 빌드 중 입력 변경 | 빌드 | 빌드가 도는 동안 공용 CMake 파일 수정 → 도구 종료 코드 2, `artifact/`·manifest 없음 |
| 펌웨어 7개 구성 재빌드 (새 디렉터리) | 빌드·이미지 대조: 7/7 | 무결성 통과, 현재 트리와 일치. 아래 표 |
| 새 산출물의 사본 변조 | host: 11/11 거부 | 검토 사례 3가지(기대 variant 유무, `binary_says` 조작), config id, target(+`--expect-target`), variant 삭제, `binary_says` 삭제, dirty 뒤집기, BOOT 구성 이미지로 바꿔치기, 1바이트 변경, 파일 누락 |
| **이전 산출물**(`pogover_20261006_*`, 수정 전 도구로 만든 것) | 무결성: 펌웨어 7/7 + hubctl 1 + 앱 5/5 **통과**. 현재 트리와의 일치: 전부 **불일치** | 정상이다. 이전 산출물과 manifest는 고치지 않았다(`artifact/manifest.json` 12개의 SHA-256, 그리고 기존 `~/esp-builds`의 이미지·manifest·sdkconfig 155개의 크기와 수정 시각이 작업 전후 같음) |
| Linux API host 빌드 (새 디렉터리) | host: ctest 3/3 | `pogohome-hubctl --version`의 식별자가 현재 트리와 같다. 그 실행 파일의 manifest: target `macos-arm64`(Mach-O 헤더), `linux-arm64`로 고치면 거부 |
| 펌웨어 host C / Python test 전체 | host: 67/67, 14/14 | 펌웨어 소스는 바뀌지 않았다 |
| 앱 빌드 5종 재빌드 (새 디렉터리) | 빌드·이미지 대조: 5/5 | 무결성 통과, 현재 트리와 일치, Release 표면 검사 as expected. 사본 변조 5/5 거부(플랫폼, `bundle_says`, 구성 삭제·불일치, Info.plist의 빌드 식별자) |

새 산출물 (모두 개발 빌드, dirty. 배포 후보가 아니다):

| 구성 | 빌드 식별자 | 앱 이미지 크기 (B) | 앱 이미지 SHA-256 |
|---|---|---:|---|
| `power_base` default | `590e2a11d2cb+dirty.79bad6c10044` | 881,440 | `5d64b05155e46d749e48745b232a2aeba903eb1967356124f396311a61a2069c` |
| `power_base` bench | 같음 | 1,017,616 | `f3ae2ea269527efdfe657a8d824b7ad0e7e42158c96dd4ff07acfc2eb0d66273` |
| `power_base` ble | 같음 | 1,427,168 | `3ce167dd46192559c3d7c7e843e42f3ff2e61dd556073fcbf10444f575512bce` |
| `power_base` ble + GPIO9 버튼 | 같음 | 1,431,488 | `f058198fe13678da602abd5b04fc4e68b667b53715e00722e179537f2e06c5a6` |
| `module` default | `590e2a11d2cb+dirty.a7327b0a85fe` | 885,984 | `a855aa06621dda8a6e17aaca48e1f00c6a75700eaa053c4c6e8638b343075d5f` |
| `rcp` default | `590e2a11d2cb+dirty.6334e84b8702` | 254,288 | `5978be9000716c51dff831a8b690ddd6ed1fb2c6b8d9a203c33863c2db7b6208` |
| `rcp` diag | 같음 | 259,600 | `c719c85461d09b81674be6ad7a1a26dec6b2d6169b700f17213cb170e2ac9425` |

- 위치: `~/esp-builds/pogoverfix_20261006_<구성요소>_<variant>/artifact/`(각각 `manifest.json`). 앱:
  `~/esp-builds/pogoverfix_20261006_app_{sim_debug,sim_release,device_debug,device_release,catalyst_release}/artifact/`
  (빌드 식별자 `05375341f70d+dirty.c5e8b536e6e7`). Linux host: `pogoverfix_20261006_linux_api_host/`,
  `pogoverfix_20261006_linux_hubctl_host_artifact/`.
- 이미지 크기는 이전 빌드와 같고 내용(SHA-256)은 다르다: 들어 있는 식별 문자열이 다르기 때문이다.
- 실물 시험 절차("BOOT 버튼 로컬 재등록 검증"의 1단계)에서 버전 체계가 들어간 이미지를 쓰려면 이제
  `pogoverfix_20261006_power_base_ble_reenroll_gpio9`가 현재 소스의 것이다. `pogover_20261006_…`의 같은
  구성은 이전 규칙의 식별자를 가진, 무결한 다른 파일이다.

### 검증 경계

- manifest 검증이 **확인하지 않는** 값: `build.configuration`, ESP-IDF 버전 외의 `build.toolchain`,
  `compatibility`, `created_utc`, `source.origin`, `source.inputs_unchanged_during_build`. 통과 메시지가
  이 이름들을 "not verified"로 적는다. 앱은 `build.configuration`(Debug/Release)과 `destination`도
  번들에서 읽을 수 없다.
- bridge 실행 파일의 target은 헤더의 형식과 아키텍처까지만 확인한다(ELF가 Linux용이라는 것은 증명되지
  않는다). **실제 Linux ELF로는 확인하지 않았다**: host test의 가짜 ELF 헤더와 Mac의 Mach-O 실행 파일뿐이다.
- GN 빌드(`tools/build_matter_bridge.sh`)의 stamp 단계와 증분 동작은 실행하지 않았다. 그 스크립트는
  이제 `linux_bridge`의 빌드 입력으로 지문에 들어 있다(전에도 들어 있었다).
- 앱의 XCTest는 다시 실행하지 않았다(Swift 코드 변경 없음. 마지막 실행은 위 절의 934개).
- "빌드 입력"은 저장소 안의 파일까지다. ESP-IDF·Xcode·CHIP SDK 같은 저장소 밖의 도구는 지문에 들어가지
  않고 manifest의 `toolchain`(검증하지 않는 값)과 app description의 IDF 버전으로만 남는다.
- 새 빌드 도구를 빌드에 연결하고 입력 목록에 넣지 않으면 같은 결함이 다시 생긴다. 이를 잡는 검사는
  `../tools/…`와 `components/<이름>` 형태의 참조만 본다.

## QR 설정 코드 입력 검증 (2026-10-06)

대상은 설정 코드의 QR 형식(`POGO-PB:1:<eui64>:<setup_code>`), Mac의 생성 도구
(`tools/gen_power_base_setup_qr.swift`), 앱의 QR 입력(앱 저장소, 1.1.0 (3))이다. 설계는
[`power-base.md`](power-base.md) "QR 설정 코드".

**이 절의 어떤 것도 실물이 아니다.** 실제 카메라, 실제 iPhone·iPad, 실제 보드, 실제 BLE, 운영 허브를
쓰지 않았고, 앱을 기기에 설치하지 않았으며, 실제 보드의 설정 코드 파일을 열지 않았다. 모든 EUI-64와
설정 코드는 공개된 가짜 값(`tools/power_base_setup_qr_vectors.json`)이다. 아래는 서로 다른 세 가지를
따로 적는다: 가짜 의존성 test, 실제 이미지 디코딩, SDK 빌드.

### 기준

| | |
|---|---|
| 앱 저장소 | `main`, `05375341f70d93c788b5f08889e312a023f83202`, 커밋하지 않은 작업 트리. 앱 `1.1.0 (3)`, 빌드 식별자 `05375341f70d+dirty.6b5658a19fa8` |
| 이 저장소 | `feature/unified-module-role-manager`, `590e2a11d2cb93f1cbc2b2b0eb094d50ea289a50`, 커밋하지 않은 작업 트리. 네 구성요소 모두 `0.1.0` |
| 도구 | macOS 26.6.2 (25G83), Xcode 27.0 (27A266a), Apple Swift 6.4, iOS 27.0 시뮬레이터 런타임 (24A434), Python 3.9.6 |

이 추가 전후로 네 구성요소의 소스 지문과 빌드 식별자는 같다(`power_base` `…79bad6c10044`, `module`
`…a7327b0a85fe`, `rcp` `…6334e84b8702`, `linux_bridge` `…fdd81af24e05`): 생성 도구·test·벡터는 빌드 입력이
아니다. `tools/pogo_version.py check` 통과, `tools/test_pogo_version.py` 252개 검사 통과. 펌웨어와 Linux
bridge는 다시 빌드하지 않았다(바뀐 입력이 없다).

### 생성 도구 (Mac host test)

`POGO_APP_REPO=<앱 저장소> python3 tools/test_gen_power_base_setup_qr.py` — **240개 검사, 실패 0.** 도구와
독립 판독기를 그 자리에서 컴파일해 실행한다.

- 유효 벡터 6개 각각: 만든 PNG를 Vision과 Core Image 검출기(도구와 코드를 공유하지 않는
  `tools/power_base_setup_qr_check.swift`)로 읽어 QR이 정확히 하나이고 내용이 벡터의 53바이트와 같음을
  확인. 기대 문자열은 도구가 아니라 벡터 파일에서 온다. 다른 보드의 문자열과는 같지 않음도 확인.
- 그림: 8비트 gray, alpha 없음, 값은 0과 255뿐, 모듈당 12픽셀, 사방 여백 4모듈, 33모듈 심볼, PNG 청크는
  `IHDR`·`IDAT`·`IEND`뿐. (첫 실행에서 ImageIO가 붙인 `eXIf` 청크를 이 검사가 잡았고, 도구가 그림 청크만
  남기도록 고쳤다.) 같은 입력은 같은 바이트를 만든다.
- 비밀 취급: PNG mode 0600, 새로 만든 디렉터리 mode 0700, 표준 출력·오류에 설정 코드와 QR 문자열이
  없음, 파일 안에 코드가 글자로 들어 있지 않음.
- 거부(아무것도 쓰지 않고, 거부 메시지에 파일 내용이 없음): 벡터의 잘못된 EUI-64 10종과 잘못된 파일
  내용 15종, mode 0644·0640·0604·0660, 심볼릭 링크, 디렉터리, FIFO(멈추지 않음), 없는 파일, Git 작업 트리
  안의 설정 코드 파일, 작업 트리 안의 출력(이 저장소 포함, 심볼릭 링크를 거친 경우 포함), 이미 있는 출력
  파일(덮어쓰지 않음), 심볼릭 링크인 출력·디렉터리, mode 0755 디렉터리, `.png`가 아닌 이름, 인자·환경
  변수로 준 설정 코드. 디렉터리를 만든 뒤 실패하면 만든 디렉터리도 지운다.
- 실행하지 못한 경로: 다른 사용자 소유의 파일·디렉터리 거부(root가 필요하다), 실행 중 신호로 끊겼을 때의
  정리.

### 파서 (Mac host test)

앱 저장소 `PogoHome/Packages/PowerBaseBLEKit`에서 `swift test`와
`swift test -c release -Xswiftc -enable-testing` — 각각 **94개 실행, 실패 0, 27개 skip**(skip은 ESP-IDF host
harness가 필요한 기존 상호운용 시험이고 이번 변경과 무관하다). 이 가운데 QR 파서 test가 10개다.

- 벡터 파일(이 저장소 파일과 바이트 단위로 같음을 양쪽 test가 대조): 유효 6개, 거부 75개를 이유별로
  (`not_power_base_code`, `unsupported_version`, `malformed`, `invalid_eui64`, `invalid_setup_code`).
- 벡터와 별개로: 유효한 문자열의 모든 위치를 모든 ASCII 바이트로 바꾼 6,731가지 가운데 그 자체가 다른
  유효한 QR인 1,022가지만 받아들이고 나머지를 거부, 한 바이트를 빼거나 넣은 모든 경우 거부, 마지막
  문자로 가능한 것은 `AEIMQUY4`뿐, Swift가 같다고 하는 닮은 문자(U+212A)가 든 문자열 거부, 값과 오류를
  출력·reflect해도 코드가 보이지 않음.

### 앱 test (가짜 의존성)

카메라·BLE·보드·허브는 모두 스크립트된 가짜다. 최종 트리에서 실행했다.

| 대상 | 결과 |
|---|---|
| iPhone 13 mini 시뮬레이터, Debug 전체 | 1001개 중 980 통과, 실패 0, skip 21 |
| iPad (A16) 시뮬레이터, Debug 전체 | 1001개 중 980 통과, 실패 0, skip 21 |
| Mac Catalyst, Power Base·QR·빌드 정보 test 40개 클래스 | 408개 중 392 통과, 실패 0, skip 16 |
| iPhone 13 mini, 한국어(번역 검사가 한국어에서만 실행된다) | 44개 모두 통과 |
| iPhone 13 mini, 영어·스페인어 | 각 44개 중 40 통과, 실패 0, skip 4 |

skip은 환경 변수가 있어야 도는 렌더링·실망 시험과, 한국어에서만 도는 번역 검사다. 기존 test의 실패는
없었다.

QR test 9개 클래스가 확인하는 것:

- **보드 결합.** QR의 코드는 그 순간 선택된 보드의 것일 때만 들어가고, 다른 보드를 선택하면 지워지며,
  손으로 고치면 직접 입력한 코드가 된다. "다른 허브에 연결하기"에서 넘기는 코드는 인증한 보드에만
  전달된다.
- **세 화면.** 실제 controller 세 개(등록, 로컬 초기화, 삭제 확인)에 대해: 다른 보드의 유효한 QR·여러
  QR·다른 종류의 QR·손상된 QR은 입력 칸과 선택을 바꾸지 않고, 맞는 QR을 넣는 동안 연결·보드 명령·허브
  요청이 **0회**다(요청 기록 전체를 전후 비교). 그 뒤 사용자의 버튼에 해당하는 호출은 읽은 코드를 그
  보드에 보내고, 인증 뒤 보드가 다른 EUI-64를 말하면 이전처럼 거부된다.
- **늦은 결과.** 화면 이탈, 취소, 허브 변경, 보드 선택 변경, 더 새로운 스캔, 백그라운드 뒤에 도착한
  결과와 반복 결과는 무시된다.
- **카메라 순서.** 권한은 스캐너를 열 때만 묻고, 카메라가 없으면 묻지도 않는다. 권한 응답이나 세션
  시작이 스캐너를 닫은 뒤·화면을 떠난 뒤·백그라운드에서 끝나도 카메라가 켜지지 않는다. 백그라운드에서
  꺼지고, 스캐너가 아직 열려 있을 때만 복귀 후 다시 켜진다. 닫기는 foreground를 기다리지 않는다.
- **실제 화면.** 세 화면의 실제 SwiftUI 뷰를 띄워, 화면의 등장·이탈·백그라운드와 "어느 보드의 코드를
  읽을 수 있는가"가 QR 입력기에 전달되는지 확인. 스캐너 sheet를 실제로 띄웠을 때 아래 화면이 사라진
  것으로 처리되지 않고, 코드를 읽으면 스스로 닫히는지 확인.

실행 중 드러나 고친 것 하나: "어느 보드인가"를 목록의 행에 붙인 `onChange`로 전달했더니 12회 반복에서
2회, 구역이 바뀌는 순간의 변경이 전달되지 않았다. 화면을 소유한 뷰로 옮긴 뒤 같은 12회 반복에서 실패가
없었다.

### 실제 이미지 디코딩

앱의 실제 판독 경로(`PowerBaseSetupQRImageReader`: ImageIO로 줄여 디코딩 → Vision)에 실제 그림 파일을
넣었다. iPhone 13 mini·iPad 시뮬레이터와 Mac Catalyst에서 같은 test 11개가 통과했다.

- **생성 도구 → 앱 왕복.** 도구가 만든 PNG(test 번들의 고정 2개와, 실행 직전에 유효 벡터 6개로 새로 만든
  6개)를 읽어 벡터의 문자열과 바이트 단위로 같고, 그 보드의 화면에서만 받아들여짐을 확인.
- 방향: EXIF 방향 8가지로 저장한 파일 각각에서 바로 선 그림이 픽셀 단위로 나오고 QR이 읽힘.
- 서로 다른 QR 두 개가 든 그림은 둘 다 검출되고 어느 쪽 보드에서도 거부, 같은 QR 두 개는 하나로 처리,
  QR 없는 그림, 이미지가 아닌 데이터, 파일 크기·화소 수 제한(헤더로 판정), 큰 그림의 축소와 두 번째 시도.
- 개행이 붙은 QR, URL, 다른 버전, 대문자 EUI-64 등을 담은 QR을 그림으로 만들어 넣으면 내용이 그대로
  전달되고 그대로 거부됨(중간에서 다듬지 않는다).

**구분해 둘 점.** iOS 시뮬레이터에서는 Vision의 현재 바코드 검출기(revision 2–4)가 "Could not create
inference context"로 실행되지 않는다. 앱은 그런 경우 첫 번째 검출기(revision 1)로 다시 읽으며,
시뮬레이터의 결과는 모두 그 경로다. 현재 검출기는 Mac Catalyst test와 Mac의 생성 도구 test에서 실행됐다.
실제 iPhone·iPad에서 어느 검출기가 어떻게 읽는지는 확인하지 않았다.

### 빌드와 산출물

`Scripts/pogo_app_version.py build … --no-sign`으로 새 디렉터리에 만들었다. **모두 개발 빌드다**(커밋하지
않은 트리, 빌드 종류 `development`, 서명 없음). Release 구성도 정식 release가 아니다. 어디에도 설치하지
않았다.

| 산출물 (`~/esp-builds/…/artifact`) | 구성 | 대상 | manifest SHA-256 |
|---|---|---|---|
| `pogoqr_20261006_app_sim_debug` | Debug | iOS 시뮬레이터 | `97086e837411c70c842ef10bc20f95a1435a0bc7b91af3d2b8bf9a4dbcf8fb76` |
| `pogoqr_20261006_app_sim_release` | Release | iOS 시뮬레이터 | `8c1aafe269f09bea768c5a671be55ae6b44b60ce7aa211d01be4e72959aa326a` |
| `pogoqr_20261006_app_device_debug` | Debug | iOS 기기 | `3e01c5717273b671bc3d0af28ca0f5acfd4ec338a05d4b1f9b54b6e909d26815` |
| `pogoqr_20261006_app_device_release` | Release | iOS 기기 | `8e60eeb331a7e7f3e7c442d9a5ca50d22957716d528891b87cfe25650e3c40e7` |
| `pogoqr_20261006_app_catalyst_debug` | Debug | Mac Catalyst | `f74fd378fe40004b532237723bb0da73d52d6d1d57a1bd13ed92b5427656bae8` |
| `pogoqr_20261006_app_catalyst_release` | Release | Mac Catalyst | `36b3e77fdcff035d620965943274d2eddfa690427e62c1094218541f531c0210` |

- 여섯 개 모두: 버전 `1.1.0`, 빌드 번호 `3`, commit `05375341f70d93c788b5f08889e312a023f83202`, dirty,
  소스 지문 `6b5658a19fa8882f40137fd4885a73e219a77a4f3ac8d536d42ce3ead259ad27`, 빌드 식별자
  `05375341f70d+dirty.6b5658a19fa8`, 빌드 중 입력 불변. `manifest-verify --match-current-tree
  --expect-version 1.1.0`과 `verify-bundle --expect-identity` 통과.
- 번들 안의 앱, 위젯, Watch 앱, Watch 위젯의 Info.plist가 모두 `1.1.0 (3)`이다(타깃별 숫자 없음,
  `pogo_app_version.py check` 통과).
- `Scripts/check_release_enrollment_surface.sh`: QR 입력 코드(입력기, 이미지 판독, 파서)가 Debug와 Release에
  모두 들어 있고, 카메라 검출 코드는 iOS 빌드에만 있다. Release에는 DEBUG 전용 시험 화면·문자열 표·
  pasteboard 접근이 여전히 없다. `NSCameraUsageDescription`은 앱에만 있고 위젯·Watch 앱의 Info.plist에는
  없으며, 사진 보관함 사용 설명은 어디에도 없다. Mac Catalyst의 서명된 Debug 빌드(test에 쓴 것)의
  entitlements에 카메라 권한이 없다(서명하지 않은 산출물에서는 이 항목을 확인할 수 없다).
- 위젯·Watch 번들의 `InfoPlist.strings`에는 카메라 사용 설명의 번역 문자열이 들어 있다. 문자열 표를
  타깃들이 함께 쓰기 때문이고(Bluetooth 설명도 같다), 그 번들의 Info.plist에 키가 없으므로 권한으로
  선언되지 않는다.

### 화면 렌더링

스캐너의 카메라는 가짜이고 뷰파인더는 자리 표시다. iPhone 13 mini(375pt)에서 ko·en·es × 밝음·어두움·
가장 큰 글씨(AX5), iPad에서 ko, Mac Catalyst에서 ko·es를 PNG로 만들었다(코드 입력 7개 상태, 스캐너 6개
상태). 직접 본 것: ko 입력 완료, ko 스캐너의 "다른 보드" 안내, es 가장 큰 글씨의 "다른 보드" 안내와
권한 거부 화면, en 어두운 화면의 입력 줄, iPad ko 어두운 화면의 "여러 코드" 안내, Mac es의 파일 불러오기
줄과 안내, Mac es 어두운 넓은 창의 삭제 확인. 잘린 글자나 닿지 않는 버튼은 없었고, 가장 큰 글씨에서는
스크롤로 모두 보인다. 실제 카메라 영상이 들어간 화면은 본 적이 없다.

### 하지 않은 것

- 실제 카메라로 QR 읽기: 미리보기의 방향, 초점 거리, 인쇄물·다른 화면의 QR, 어두운 곳, 반사.
- 실제 iPhone·iPad에서의 권한 질문, 설정에서 권한을 바꾸고 돌아오기, 시스템 사진 선택기에서 고르기.
  실제 Mac에서 파일 선택 창을 손으로 조작하기.
- 실제 보드의 설정 코드로 QR을 만들고, 그 QR로 등록·"다른 허브에 연결하기"·삭제 확인을 끝까지 하기.
- 전화·알림·화면 잠금으로 카메라가 중단되는 경우, 화면 회전 중의 미리보기.
- Linux bridge와 펌웨어는 이번에 실행·빌드하지 않았다(바뀌지 않았다).

### 다음 실물 시험 (준비만 했고 실행하지 않았다)

1. 시험용 여분 보드 1대의 설정 코드 파일로 Mac에서 QR을 만든다(위 문서의 명령. 출력은 저장소 밖의
   mode 0700 디렉터리). QR은 설정 코드처럼 다룬다.
2. 기기용 빌드를 서명해 시험용 iPhone에 설치한다(이번 산출물은 서명하지 않았으므로 다시 빌드한다).
3. "Power Base 추가"에서 보드를 고르고 "카메라로 QR 코드 읽기": 처음에만 권한을 묻는지, 미리보기가 바로
   서 있는지, 읽으면 26자리가 들어가고 스캐너가 닫히는지, **등록이 시작되지 않는지**를 본다. 그 뒤
   "등록"을 눌러 허브의 `heartbeat_received`까지 간다.
4. 다른 보드의 QR(또는 다른 종류의 QR)을 비춰 입력 칸과 선택이 그대로이고 안내가 나오는지 본다.
5. QR을 찍은 사진으로 "사진에서 QR 코드 가져오기"를 해 본다. 사진 보관함 권한을 묻지 않아야 한다.
6. 스캐너를 연 채 홈으로 나갔다 돌아와 카메라가 다시 켜지는지, 닫은 뒤에는 켜지지 않는지 본다.
7. 가능하면 "다른 허브에 연결하기"와 "삭제 확인"에서도 3을 한 번씩 한다. Mac에서는 QR PNG를 "QR 코드
   이미지 파일 불러오기"로 연다.

결과는 `docs/release-records/`의 양식으로 남긴다. 설정 코드와 QR은 기록에 넣지 않는다.

## Power Base 자동 삭제 확인 검증 (2026-10-08)

범위: 삭제를 끝낸 Power Base가 영수증을 스스로 허브에 보내는 경로(Power Base 0.2.0, Linux bridge 0.2.0, 앱
1.2.0 (7)). 규약은 [`power-base-management.md`](power-base-management.md)의 2026-10-08 머리말과 2·4·6·7절, HTTP
API는 [`pogohome-api-v1.md`](pogohome-api-v1.md). 앱 쪽 기록은 앱 저장소의 `docs/validation-status.md`.

**이 절의 어느 것도 실물이 아니다.** 보드에 기록하지 않았고(flash·erase·시리얼 접근 없음), 실제 Thread
네트워크·실제 BLE·운영 Linux 허브·실제 iPhone을 쓰지 않았다. **빌드와 host test, host 재현이 통과했다는 것은
실물에서 자동 삭제가 된다는 뜻이 아니다.** 기존 경로(BLE 영수증)로 실물 삭제를 끝냈다는 것은 사용자가 알려
온 것이고, 이 저장소에는 그 기록이 없다.

### 기준

| | |
|---|---|
| 저장소 | `feature/unified-module-role-manager`, `590e2a11d2cb93f1cbc2b2b0eb094d50ea289a50`, 커밋하지 않은 작업 트리 |
| 작업 전 | `power_base` 0.1.0 지문 `79bad6c10044…9b3d`, `linux_bridge` 0.1.0 `fdd81af24e05…ba30`, `module` 0.1.0 `a7327b0a85fe…bee3`, `rcp` 0.1.0 `6334e84b8702…e832`. 변경 파일 111개(추적 63, 미추적 48), stage 0 |
| 작업 후 | `power_base` **0.2.0** 지문 `bb07a74e0f597ad08161fc990a2ef8daabc4b4244912ee548508446792682173`, `linux_bridge` **0.2.0** `2ac06fb62e2d62631442309e04718a049bfa8f77c7a18b3035456eac742eb72c`, `module` 0.1.0 `20047f33564a1692d9450acd13e56df9035e98298ee379bd170203c3962ce4d9`, `rcp` 0.1.0 `b1657c1a3148f657f11584f9870fcb00516ad1a1b8145325432fa10444322449` |
| 도구 | macOS 26.6.2 (25G83), Apple clang 21.0.0, CMake 4.4.3, ESP-IDF v6.1 (`fff9895c82d7`), `riscv32-esp-elf-gcc` 15.2.0, 그 ESP-IDF의 OpenThread `b678a4f63b6f`와 Mbed TLS 4.1.0 / TF-PSA-Crypto 1.1.0 (`a2b32072ea89`) |
| 출력 | 모두 새 디렉터리 `~/esp-builds/pogoauto_20261008_*`. 기존 산출물, 프로젝트의 `sdkconfig`, 비밀 파일, ESP-IDF 트리는 바꾸지 않았다 |

`module`과 `rcp`의 지문이 달라진 것은 `versions.json`과 빌드 도구(`tools/pogo_version.py`)가 그 둘의 빌드
입력이기도 해서다(`module`은 `components/` 전체도). 두 구성요소의 소스는 바꾸지 않았고 `module` 이미지의 크기는
이전과 같다(885,984 B).

### 설계 검증: 실제 OpenThread와 PSA 키 저장소로 한 재현

`tools/test_power_base_release_session_openthread.sh <새 디렉터리>`. ESP-IDF v6.1이 펌웨어에 넣는 소스를 Mac에서
빌드해 실행한다. 스위치는 펌웨어가 link하는 바로 그 파일(`components/power_base_ot_store/pb_volatile_store.c`)이다.
기록: `~/esp-builds/pogoauto_20261008_results/release_session_repro.log`.

**OpenThread core (Nexus: 한 프로세스 안의 두 instance, 모의 radio, 가상 시간).** 허브 쪽 노드가 네트워크를
만들고 보드 쪽 노드가 router로 붙은 뒤, key sequence를 3으로 올리고, 펌웨어의 삭제 순서(Dataset을 RAM으로 읽음 →
Thread 정지 → `otInstanceErasePersistentInfo()` → 저장소 read-back)를 그대로 한다. 보드의 "flash"는 그 노드의
settings 객체이고 `Node::Reset()`에도 남는다.

| 경우 | 결과 |
|---|---|
| 대조: 스위치 없이 다시 붙음 | 붙고 CoAP도 오가지만 **저장소에 Active Dataset이 다시 생기고, 재시작한 뒤 지운 네트워크에 다시 붙는다.** 영수증을 그냥 Thread로 보낼 수 없는 이유 그대로다 |
| 위험: 스위치는 켰지만 extended address가 그대로 | **30초 안에 붙지 못한다**(frame counter가 0으로 돌아가 재생으로 버려진다). 저장소는 비어 있다 |
| 세션: 스위치 + 새 extended address + router 자격 없음 | 가상 시간 1.5초에 child로 붙고, confirmable CoAP 요청(96바이트)과 응답이 온전히 오가며, **붙어 있는 동안과 끝난 뒤 저장소가 비어 있다.** OpenThread는 settings를 3번 썼고 모두 RAM에 들어갔다. 재시작 뒤 Dataset 없음, Thread를 켜도 붙지 않음 |

**PSA 키 저장소 (TF-PSA-Crypto core + 그 자체의 파일 저장소).** OpenThread가 Network Key에 대해 하는 PSA 호출
(`KeyManager::StoreNetworkKey()`의 영속 HMAC 키 import, export, `psa_get_key_attributes`, destroy)을 같은 key id로 한다.

| 경우 | 결과 |
|---|---|
| 대조: 스위치 없이 영속 키 import | **저장소에 항목이 하나 생기고, 키 저장소를 다시 시작해도 키가 있다.** destroy하면 없어진다 |
| 스위치를 켠 뒤 같은 import | 성공하고, 키가 존재·export·서명되며, RAM cache에서 내린 뒤(`psa_purge_key`) 다시 읽히고, 같은 id의 재import는 거부되고, destroy된다. **그동안 저장소에는 항목이 하나도 없다.** 다시 시작하면 키가 없다 |

**이 재현이 보여 주지 않는 것.** OpenThread 재현은 key reference가 없는 구성이다(Network Key가 Dataset 안에
있고 PSA를 쓰지 않는다). PSA 재현에는 OpenThread가 없다. 두 저장소를 함께 쓰는 ESP-IDF 구성, ESP-IDF의 settings
port(`esp_openthread_settings.c`)와 PSA 저장소(`esp_psa_its.c`), NVS, radio, 실제 부모 router는 어느 쪽에도 없다.
`--wrap`은 Mac의 linker에 없어서 같은 이름 대응을 컴파일 때 만들었다(아래 이미지 검사가 실제 link를 본다).

### 기존 결함 두 가지 (수정 전의 실패와 수정 후)

새 기능의 검사와 구분해 적는다.

**1. 삭제 검증이 Network Key가 있는 저장소를 읽지 않았다.**

- 근거(정적): 0.1.0 이미지의 map에 `esp_psa_its.c`의 `psa_its_set`·`psa_its_remove`와
  `KeyRefManager::DestroyPersistentKeys()`가 link돼 있고, disassembly에서 `psa_save_persistent_key`가 `psa_its_set`을
  부른다. ESP-IDF v6.1의 FTD 구성은 `OPENTHREAD_CONFIG_PLATFORM_KEY_REFERENCES_ENABLE 1`이다. 그런데 이전의
  `pb_network_store_is_empty()`는 `openthread` namespace만 읽었다.
- 재현(host): 작업 전 소스의 그 함수 본문을 그대로 가져와 NVS simulator에 붙이고 "settings는 비었고 Network Key는
  PSA 저장소에 남은" 상태를 만들었다. **수정 전: "비어 있음"(영수증을 만든다). 수정 후
  (`pb_stored_network_is_erased`): "지워지지 않음".** 둘 다 빈 경우는 양쪽 모두 "비어 있음". 기록:
  `erase_check_before_after.txt`. 회귀 test: `tools/test_power_base_ot_store.c`("the Network Key outlives a settings wipe").
- 한계: 실제 보드에서 `psa_destroy_key()`가 실패한 적이 있는지는 모른다. 정상이라면 키는 지워졌고, 확인하지 않았을
  뿐이다. 새 read-back이 실제 보드에서 통과하는지도 실행해 보지 않았다(아래 "하지 않은 것").

**2. 부팅 경로의 통지가 나가지 못했다.**

- 근거(정적): OpenThread의 `Coap::Send()`는 socket이 bind돼 있지 않으면 `kErrorInvalidState`를 돌려주고, bind는
  `otCoapStart()`가 한다. 그 호출은 `power_base_transport_init()`에만 있었고 그것은 서비스가 시작될 때 불린다.
  끝나지 않은 해제를 마치는 부팅은 서비스를 시작하지 않는다.
- 수정: 통지와 영수증을 보내기 전에 `otCoapStart()`를 부른다(`coap_socket_ready()`; 이미 bind돼 있으면 OpenThread는
  아무것도 하지 않고 성공을 돌려준다. `Coap::Start()`를 읽어 확인).
- 한계: host에서 실행해 보인 것이 아니다(target 전용 코드다). Nexus 재현의 보드 노드는 요청 전에 `otCoapStart()`를
  부르도록 돼 있고, 부르지 않는 경우를 따로 실행하지는 않았다.

### host test (실제 production 소스)

| 대상 | 결과 |
|---|---|
| `tools/test_power_base_mgmt.c` (보드의 `pb_mgmt_proto.c`·`pb_mgmt_client.c`·`power_base_enrollment.c` + NVS simulator + 스크립트된 허브) | **300개 검사, 실패 0** (작업 전 219). 새 81개: `HELLO`/`HELLO_ACK`의 golden MAC과 형식, 세션이 있는 플랫폼의 정상 전달(순서, 영수증이 나갈 때 flash에 네트워크·키 없음, BLE 영수증과 같은 바이트), 답 유실·위조·다른 result의 확인·거부, 이전 허브(HELLO 거부, 완료 뒤 키 없음), attach 실패, 세션이 저장소를 다시 쓴 경우(다시 지움 / 지우지 못하면 삭제 대기로 되돌림), 전원 차단 세 지점과 부팅 경로의 전달, 로그에 비밀 없음 |
| `tools/test_power_base_ot_store.c` (새 component의 두 파일 + NVS simulator) | **69개 검사, 실패 0** (새 test). 스위치 전: 그대로 통과. 스위치 뒤: platform 함수 호출 0회, settings의 Get/Set/Add/Delete/Wipe와 ITS의 get_info/get/set/remove 의미, 용량 초과는 거부, 경로에 없는 이미지는 `engage()`가 false. 저장소 확인: 두 저장소 모두, 일곱 키 각각, 읽지 못하면 통과 아님, 스위치의 RAM이 아니라 flash를 읽음 |
| 펌웨어 host test 전체 (`tools/test_*`) | **83개 프로그램, 실패 0.** 각 파일 머리말의 명령으로 빌드·실행(80개), ESP-IDF 환경이 필요한 3개는 따로 실행(`test_power_base_ble_sec2_interop.py` 169개 검사 — `tools/host_ble_sec2`를 새 디렉터리에 다시 빌드, `test_gen_power_base_ble_config.py` 77, `test_pack_power_base_await_config.py` 18). 작업 전 트리의 사본에서는 82개였고 달라진 것은 위 두 줄과 `test_pogo_version.py`(252 → 253)뿐이다 |
| 허브 `linux/matter_bridge/app/api` (실제 `PowerBaseManagement` + 실제 ingest + 보드 쪽은 실제 펌웨어 client) | **1,342개 검사, 실패 0** (작업 전 1,271). ctest 3/3. 새 71개: 세션이 있는 보드의 끝에서 끝까지(`released`일 때 세션 시작 → 영수증 → `completed`, 키 삭제), 같은 영수증에 같은 확인(재시작 뒤에도, 보관 기간 뒤에는 `4.01`), 변조·다른 명령 id 거부, 답 유실 뒤 재전송, `erase_confirmation`의 `automatic`/`manual`과 4분 경계, 늦게 온 영수증, `HELLO`의 신뢰(다른 보드의 키, 변조, 저장 실패, 재시작, `released` 뒤 재생), 이전 형식의 상태 파일, 재등록 뒤 이전 영수증의 재도착(상태 파일 불변, 새 등록 영향 없음), HTTP로 본 같은 흐름 |
| 허브 `ingest`, `app/core` | ctest 8/8, 1/1 |
| `tools/test_pogo_version.py` | 253개 검사, 실패 0 |

필수 시나리오와 그것을 실행한 곳:

| 시나리오 | 어디서 |
|---|---|
| 온라인 삭제의 정상 완료 | 보드 test "receipt delivered", 허브 test `ThreadReceipt`, 허브 HTTP test, 앱 test |
| 오프라인 요청 후 재연결 | 허브 test `OfflineAndRestart`(기존) + `ThreadReceipt`의 capability, 앱 test "offline Base waits" |
| 저장·삭제·증명 생성·전송 경계의 중단과 재시작 | 보드 test: 기존 다섯 지점 + 세션 시작·영수증 전송·세션 종료 직전, 부팅 경로의 세션, 부팅 경로 안에서의 재차단 |
| 삭제 실패, 저장 실패, 통지·응답 유실 | 보드 test(기존 + 영수증의 답 유실·거부), 허브 test(`HELLO` 저장 실패, 답 유실) |
| 중복 요청, 중복 증명, 위조·다른 보드·다른 세대의 증명 | 허브 test(같은 영수증 반복, 변조, 다른 명령 id, 다른 보드의 키, 새 키로 서명한 이전 명령), 보드 test(위조된 확인) |
| 만료 | 허브 test: `released` 뒤 4분의 `manual` 전환, 보관 기간(10분)이 지난 완료 작업의 영수증 `4.01`, `result_unknown` |
| 삭제 후 재등록과 이전 삭제 작업의 재도착 | 허브 test `ThreadReceipt` 마지막 묶음 + 기존 `ReenrollAfterRemoval` |
| 마지막 Base와 센서의 화면 갱신, background·화면 이탈·허브 변경, 신구 앱 | 앱 저장소의 test |
| 신구 허브·펌웨어 조합 | 보드 test(HELLO를 모르는 허브, 완료 뒤 키를 지우는 허브), 허브 test(세션 없는 보드, 이전 상태 파일). 0.1.0 허브의 실제 코드가 관리 resource의 영수증으로 완료하는 것은 작업 전 소스의 `HandleDoneLocked()`를 읽어 확인했다 |

### link된 이미지 검사

`tools/check_power_base_ot_store.py --elf <build>/power_base.elf --idf $IDF_PATH`. disassembly에서 아홉 함수 각각에
대해 ① platform의 함수와 `__wrap_` 함수가 모두 link됐는가 ② platform의 함수를 부르는 것이 `__wrap_` 함수(와
flash를 일부러 직접 읽는 `pb_stored_network_keys_gone`)뿐인가 ③ `__wrap_` 함수를 OpenThread·PSA가 실제로 부르는가를
보고, ESP-IDF 소스에서 `pb_stored_network.c`가 옮겨 적은 값(namespace, key id의 offset과 범위, key reference 사용,
PSA 저장소의 namespace)을 대조한다.

- 새 이미지 네 구성(default, bench, ble, ble+reenroll_button_gpio9): **각 37개 검사, 실패 0.** 부르는 쪽은
  `ot::SettingsDriver::Get/Set/Add/Delete`, `ot::Settings::Wipe`, `psa_save_persistent_key`,
  `psa_load_persistent_key`, `psa_destroy_persistent_key`, `psa_is_key_present_in_storage`다.
- 대조: 0.1.0 이미지(`pogoverfix_20261006_power_base_ble`)는 이 검사에서 실패한다(11개: `__wrap_` 없음).
- 한계: 이미지가 **그렇게 link됐다**는 것이다. 실행한 것이 아니다. 펌웨어는 켤 때 한 번 더 확인한다
  (`pb_volatile_store_engage()`가 false면 Thread를 켜지 않는다).

### 빌드와 산출물

**이 표의 산출물은 같은 날의 수정 전 빌드다. 쓰지 않는다.** 두 실패 경로가 들어 있고(아래 "Power Base 자동
삭제 확인: 두 실패 경로 수정"), 지금의 트리와 일치하지도 않는다. 파일은 그대로 두었다. 쓸 것은 그 절의
`pogoautofix_20261008_*`다.

`tools/pogo_version.py build-firmware --component … --variant … --out <새 디렉터리>`. 모두 **개발 빌드**다
(커밋하지 않은 트리). 빌드 전후 입력이 같고, 만든 시점에 `manifest-verify --match-current-tree`를 통과했다.
어디에도 기록하지 않았다.

| 디렉터리 (`~/esp-builds/`) | 구성요소·버전·variant | 주 산출물, 크기, SHA-256 | manifest SHA-256 |
|---|---|---|---|
| `pogoauto_20261008_power_base_default` | `power_base` 0.2.0 `default` | `power_base.bin` 887,632 B `6a94063370ce4bc0cc5a3945b44044ee7e94e83d9256f447883ea3bcbac45756` | `da3675aaab0b3864172dec767de96911ad60b7720b36c8614370f07cc029bfce` |
| `pogoauto_20261008_power_base_bench` | `power_base` 0.2.0 `bench` | `power_base.bin` 1,023,808 B `8f35a4a4e880e320e8d7cd999719c3f881d411029c2c8d12a350bc13d6776f1b` | `dd26dd4011f022fd9d5cd2f21279d0c09f379dc13c6cebf098a0520e1107f57c` |
| `pogoauto_20261008_power_base_ble` | `power_base` 0.2.0 `ble` | `power_base.bin` 1,433,440 B `065a8504ed8a89b18338d31b3e1d62281c32b4d24f793d0297201803418e6d35` | `571c939b3068d08ea72865e3953062ed6fc89b0220492b5836cdfec88b1d298e` |
| `pogoauto_20261008_power_base_ble_reenroll_gpio9` | `power_base` 0.2.0 `ble+reenroll_button_gpio9` | `power_base.bin` 1,437,712 B `964eb527af377304a64e9d33a3712e0f4a0b57651d2fcf0407e5c5be07a4cba5` | `20e8e3df9779b0500458f5de9df13382715cbd32c90619c334e977868a6d3232` |
| `pogoauto_20261008_module_default` | `module` 0.1.0 `default` (회귀 빌드: 새 component가 `components/`를 같이 쓰는 프로젝트를 깨지 않는가) | `pogo_module.bin` 885,984 B `03b5f4dc723f0e98ec4b02069ffda66e0762eb0694d297ac123b2a5b681f20cd` | `aa440af47411b6945483b1f724baba061737432ae89e794d294cec9dd1f6b2cd` |
| `pogoauto_20261008_linux_hubctl_host_artifact` | `linux_bridge` 0.2.0, Mac host의 `pogohome-hubctl` (`macos-arm64`. **허브에 올릴 산출물이 아니다**) | 187,808 B `0cdc255f9b3cca89dc244a98ee10ed3f71ddc8e71ffd84621e7861f4e9cc9a72` | `c7e7329fc5353ab8aaa76aec485335b3ab815af5f87765c46104743306393267` |

- Power Base 네 이미지의 빌드 식별자는 `590e2a11d2cb+dirty.bb07a74e0f59`, toolchain은 ESP-IDF v6.1 /
  `riscv32-esp-elf-gcc` 15.2.0, 대상은 `esp32h2`(이미지 헤더). manifest의 `compatibility`에 관리 메시지
  `0x01`–`0x03`·`0x05`·`0x81`–`0x83`·`0x85`와 capability 두 개가 들어 있다.
- 크기: 0.1.0 대비 default +6,192 B, bench +6,192 B, ble +6,272 B, ble+reenroll +6,224 B. app 파티션(1,536,000 B)의
  여유는 ble 6.7%, ble+reenroll 6.4%다.
- `rcp`는 빌드하지 않았다(소스가 바뀌지 않았다. 지문만 달라졌다).
- Linux API·ingest·core의 Mac host 빌드는 `pogoauto_20261008_linux_{api_host_final,ingest_host,core_host}`.
  `pogohome-hubctl --version`은 `0.2.0 (linux_bridge, build 590e2a11d2cb+dirty.2ac06fb62e2d, development)`.
- 그 밖의 새 디렉터리: `…_release_session_repro`(재현의 빌드), `…_host_ble_sec2`(host harness),
  `…_results`(로그), 조사 중에 만든 `…_ot_nexus`, `…_tf_psa_crypto`, `…_scratch1_power_base_ble`(버전을 올리기
  전의 시험 빌드. 0.1.0이라고 말하는 이미지이며 쓸 것이 아니다), `…_linux_api_host`(작업 중의 빌드).

### 하지 않은 것

| 항목 | 상태 |
|---|---|
| 보드에 0.2.0 기록, 부팅 | **미실행** |
| 실제 Thread 네트워크에서의 영수증 전달 세션 | **미실행.** 새 extended address의 child를 부모가 받아 주는지, 붙는 데 걸리는 시간, 허브가 직접 이웃이 아닐 때, Power Base가 leader였을 때를 모른다 |
| 세션 전후 실제 flash의 NVS (`openthread`, `psa_its`) | **미실행.** 재현의 "저장소"는 Nexus의 객체와 파일이다 |
| **새 PSA 키 read-back이 실제 보드에서 통과하는가** | **미실행.** 통과하지 못하면 삭제가 "삭제 대기"에 머물고 영수증이 만들어지지 않는다(자동 전달도 BLE 영수증도). 실물 시험의 첫 확인 항목이다 |
| 실제 전원 차단 | 미실행. host에서는 프로세스를 끊는 것으로 대신했다 |
| RAM·stack 여유, BLE와 Thread의 공존 중 세션 | 미실행. 세션은 BLE가 열리지 않은 상태에서만 돈다 |
| Linux 장비의 GN/CHIP 빌드, 운영 허브 | **미실행.** 허브 코드는 Mac의 CMake 빌드로만 컴파일했다. `chip/main.cpp`와 `BUILD.gn`은 이 변경에서 바꾸지 않았다 |
| 실제 iPhone | 미실행 |

### 실물 시험 절차 (준비만 했고 실행하지 않았다)

**보드 이미지는 아래 절의 `pogoautofix_20261008_power_base_ble`로 바꿔 읽고, 그 절의 "실물 시험에 더할 것"을
함께 한다.**

여분 보드 한 대와 시험용 허브로 한다. 값(설정 코드, QR, 키, Dataset)은 기록에 적지 않는다.

1. **준비.** 허브에 `linux_bridge` 0.2.0을 Linux 장비에서 빌드해 올리고(`tools/build_matter_bridge.sh`,
   `pogo_matter_bridge --version` 확인), 보드에 `pogoauto_20261008_power_base_ble`의 이미지를 기록한다(cfg와
   NVS는 지우지 않는다: 0.1.0으로 등록된 상태 그대로 올리는 경우와, 새로 등록하는 경우를 각각 본다). 앱 1.2.0 (7)을
   설치한다. 각 산출물의 manifest SHA-256을 `docs/release-records/` 양식에 적는다.
2. **등록과 capability.** 등록 뒤 허브 로그에 `announced that it sends its release receipt itself`가 한 번 나오고,
   `GET /api/v1/snapshot`의 그 Base에 `"erase_confirmation":"automatic"`이 있는가.
3. **온라인 삭제.** 앱에서 삭제 → 확인. 보드 시리얼 로그의 순서: `authenticated unenroll command accepted` →
   `release: stored network erased and verified, final record stored` → `receipt session: Thread enabled from RAM only` →
   `receipt session ended: N settings and M key-store writes were kept in RAM (0 refused)` →
   `release complete: the hub acknowledged the receipt` → 재시작. 허브 로그: `released` 다음에
   `completed -- … receipt verified`. 앱: '삭제 중' → '삭제 완료', 보드 검색·QR·"삭제 확인" 없이.
   **`erase … not empty` 또는 `released, but its stored network is NOT confirmed erased`가 나오면 중단한다**(위 "하지
   않은 것"의 PSA read-back 항목).
4. **재시작 뒤.** 보드가 미등록 안전 대기이고 Thread에 붙지 않는가(허브의 child/router table에 새 extended
   address가 남아 있다가 timeout으로 사라지는가). 같은 보드를 다시 등록할 수 있는가.
5. **오프라인 요청.** 보드 전원을 끈 채 삭제 → 앱 '삭제 대기' → 전원을 켜면 3번과 같이 끝나는가.
6. **세션 중 전원 차단.** `receipt session: Thread enabled` 직후 전원을 끈다 → 다시 켜면 미등록 안전 대기,
   Thread 미접속. 허브는 `released`, 4분 뒤 앱에 "삭제 확인 필요". "삭제 확인"(BLE)으로 `completed`가 되는가.
7. **허브가 꺼진 동안의 삭제 마무리**, **이전 조합**(보드 0.2.0 + 허브 0.1.0, 보드 0.1.0 + 허브 0.2.0).

**실패했을 때의 복구.** 어느 단계에서 멈추든 보드는 다음 셋 중 하나다. ① 아직 등록됨(`ENROLLED`): 그대로 쓰거나
다시 삭제한다. ② 삭제 대기(`RELEASED`, 영수증 없음): 부팅마다 다시 지우고 확인한다. 계속 실패하면 0.1.0 이미지
(`pogoverfix_20261006_power_base_ble`, 그 manifest로 확인)를 다시 기록해 이전의 확인으로 끝내거나, BOOT 버튼 로컬
초기화(버튼 구성 이미지)를 쓴다. ③ 최종(영수증 있음): 앱의 "삭제 확인" 또는 같은 보드의 재등록으로 허브의
작업이 끝난다. 허브의 작업이 `released`로 남아도 그 Base는 이미 목록에서 빠져 있고 차단돼 있으며, 같은 보드를
다시 등록하면 `superseded`로 정리된다. 허브는 0.1.0으로 되돌릴 수 있다(상태 파일의 새 필드는 무시된다).

## Power Base 자동 삭제 확인: 두 실패 경로 수정 (2026-10-08)

범위: 위 절의 구현을 검토하다 재현된 두 실패 경로와, 그것을 고치면서 확인한 "스위치를 켠 뒤의 재삭제". 바뀐
것은 Power Base 펌웨어(`components/power_base_mgmt`, `components/power_base_ot_store`, `power_base/main`)와 test·
검사·재현·문서다. 허브와 앱의 코드, 관리 프로토콜, BLE 메시지, record 형식, HTTP API는 바뀌지 않았다.

**이 절의 어느 것도 실물이 아니다.** 보드에 기록하지 않았고, 시리얼·BLE·Linux 장비에 접근하지 않았다.

### 기준

| | |
|---|---|
| 저장소 | `feature/unified-module-role-manager`, `590e2a11d2cb93f1cbc2b2b0eb094d50ea289a50`, 커밋하지 않은 작업 트리. 시작 시 변경 파일 124개(추적 64, 미추적 60), stage 0 — 검토에서 준 기준선과 같다 |
| 작업 전 | `power_base` 0.2.0 `bb07a74e0f59…2173`, `linux_bridge` 0.2.0 `2ac06fb62e2d…b72c`, `module` 0.1.0 `20047f33564a…e4d9`, `rcp` 0.1.0 `b1657c1a3148…2449` |
| 작업 후 | `power_base` 0.2.0 지문 `03479fbce5edb84cc0a57fbce0f1bb0a445f24adb033d120b66c62e59ea558dc`, `linux_bridge` 0.2.0 `1b1bc214f04ac1bff4ab22e20bbe7b2861ef55eb7f46b90e35d45bb013532a10`, `module` 0.1.0 `f55d4d04aedb9490a17e3d7afa3ea74576bd3a12dcbc3628effecb1e94f8b39e`, `rcp` 0.1.0 `b1657c1a3148f657f11584f9870fcb00516ad1a1b8145325432fa10444322449`(그대로) |
| 버전 | **유지.** 0.2.0은 배포도 기록도 된 적 없는 개발 버전이고(`CHANGELOG.md`의 Unreleased), 이 수정은 그 기능을 끝내는 일이다. 다른 구성요소가 보는 동작·형식은 바뀌지 않았다. 어느 빌드인지는 빌드 식별자가 말한다 |
| 출력 | 모두 새 디렉터리 `~/esp-builds/pogoautofix_20261008_*`. `pogoauto_20261008_*`를 비롯한 기존 산출물은 그대로다 |

`module`과 `linux_bridge`의 지문이 달라진 것은 `components/` 전체가 그 둘의 빌드 입력이기 때문이다(허브는
test도). `module`은 바뀐 두 component를 link하지 않고, 허브의 실행 코드는 그대로다.

### 재현된 두 결함: 수정 전의 실패와 수정 후

검토가 준 재현 파일은 이 저장소의 `tools/test_power_base_mgmt.c`에 검사 두 개를 더한 것이다. 기록은
`~/esp-builds/pogoautofix_20261008_results/before/`.

| | 수정 전 | 수정 후 |
|---|---|---|
| **1. 세션이 Dataset을 다시 저장하고 그 뒤의 재삭제가 실패** | 보드 `PB_MGMT_POLL_RELEASED_NOT_ERASED`, 그런데 허브는 `completed`(영수증 1개 수신). 재현 파일 그대로: `FAIL: REVIEW: erase failure must not already have completed at the hub` | 보드 `…RELEASED_NOT_ERASED`, **허브 `completed` 아님, 받은 영수증 0개**, 보드 record는 삭제 대기, 영수증 없음. 다음 부팅이 지우고 확인한 뒤 그 부팅의 세션이 보내고, 그때 허브가 `completed` |
| **2. 같은 세션 중 전원 차단 뒤의 부팅** | 최종 record + 저장된 Dataset인데 `pb_mgmt_receipt_load()`가 영수증을 돌려줌. 재현 파일 그대로: `FAIL: REVIEW: power cut with a stored Dataset must not expose an erase receipt`, `code=11` | **영수증 없음(`code=10`)**, 재개 안 함. 삭제가 실패하는 부팅 뒤에도 없음. 삭제가 되는 부팅이 지우고 read-back한 뒤에야 영수증이 나온다(`code=1`) |

- **수정 전**: 재현 파일을 고치지 않고 작업 시작 시점의 소스에 대해 빌드·실행했다(`reviewer_fault_review.*`):
  검사 300개 통과, 위 두 개 실패. 검토의 로그와 같다.
- 새 `tools/test_power_base_mgmt.c`를 **작업 시작 시점의 소스**에 대해서도 돌렸다(`new_test_on_prefix_sources.*`,
  소스는 `before/prefix_tree/`에 다시 만들었다). 이전 interface에 없는 것 둘(영수증 함수의 저장소 인자, 부팅
  검사 함수)만 `make_prefix_tree.py`가 기계적으로 맞췄고 검사문은 그대로다: **42개 실패**, 그 안에 위 둘을 옮긴
  `[fault review 1]`, `[fault review 2]`가 있다. 지금의 소스에서는 355개 모두 통과한다.
- 재현 파일을 지금의 소스에 그대로 돌릴 수는 없다. `pb_mgmt_receipt_load()`의 인자가 바뀌었고(고친 내용 그
  자체다), 두 번째 재현이 전원을 끊던 지점("영수증 전송 중")이 그 세션에는 더 이상 없다: 저장소가 쓰인 세션은
  영수증을 보내지 않는다. 그래서 전원을 "붙은 직후"에 끊는 경우로 같은 상태(최종 record + 저장된 Dataset)를
  만들고, 검토의 순서 그대로도 따로 넣었다(그 경우 보드는 끝까지 가고 허브는 `completed`가 아니다).
- 허브의 `completed` 여부는 실제 허브 코드로도 본다(`test_power_base_management.cpp`의 같은 두 경우): 1번에서
  허브 작업은 `released`, `completed_at` 0, 키 보관. 2번에서 보드는 영수증을 내주지 않고 허브는 `released`.

**스위치를 켠 뒤의 재삭제 (검토의 세 번째 항목).** 닿지 않고 있었다.

- 수정 전의 세션 뒤 재삭제는 `otInstanceErasePersistentInfo()`였다. 스위치가 켜진 뒤에는 그 호출의
  `otPlatSettingsWipe()`와 `psa_its_remove()`를 스위치가 RAM에서 답한다. host test의 가짜 삭제는 flash를 지웠기
  때문에 "다시 지우고 통과"가 실제 보드에서도 된다고 읽혔다.
- 확인: `tools/test_power_base_ot_store.c`(실제 component, 스위치를 켠 뒤 wipe 1번·remove 7번이 platform에 한
  번도 닿지 않고 flash는 그대로) / **실제 PSA 키 저장소**(`tools/ot_host_repro/pb_volatile_store_psa.c`: 스위치를
  켠 뒤 `psa_destroy_key()` 두 번이 성공을 돌려주고 키 저장소에는 키가 없다고 나오지만 저장소의 파일 2개는
  그대로다).
- 수정: 스위치가 켜져 있으면 `pb_network_store_erase()`가 `pb_stored_network_erase()`로 두 저장소 자체를
  지운다. 같은 두 test에서 그 호출 뒤 flash·저장소 파일이 비고 read-back이 "비어 있음"을 답한다. 지우기 실패,
  성공이라고 답하고 지우지 않은 경우, 키 하나만 지워지지 않는 경우, 다른 namespace·다른 key id를 건드리지
  않는 것, commit 뒤 재시작해도 지워져 있는 것도 본다.
- **실행하지 않은 것**: ESP-IDF의 `nvs_erase_all`·`esp_psa_its.c`의 `psa_its_remove`로 하는 그 삭제 자체. host의
  NVS는 시뮬레이터이고 PSA 저장소는 TF-PSA-Crypto의 파일 backend다.

### 삭제하지 않은 부팅이 묻는 것 (이 수정에서 새로 정한 것)

**이 소절의 둘째 질문은 결함이 있었다**(`0x20001`은 "있음/없음 어느 쪽이든"): 아래 "Network Key 자리의 키의
출처" 절이 고쳤다. 표와 그 아래 설명은 그때의 기록으로 둔다.

검토의 요구는 "최종 record가 있어도 settings나 PSA 네트워크 키가 남아 있으면 영수증을 거부"였다. 글자 그대로
(`0x20001`이 있으면 거부) 만들면 **실제 보드에서는 모든 해제된 보드가 영수증을 내주지 못한다**: OpenThread가
부팅마다 그 자리에 자기 키를 만들기 때문이다(`Instance::AfterInit()` → `KeyManager::Init()`, ESP-IDF v6.1 소스.
위 절의 "삭제 확인이 읽는 것"에 이미 적어 둔 이유다). host의 저장소에는 그 키가 없으므로 test는 통과했을
것이다. 그래서 둘로 나눴다.

| 질문 | 함수 | 언제 | 읽는 것 |
|---|---|---|---|
| 비어 있는가 | `pb_stored_network_is_erased()` | 삭제한 그 부팅에서만: 삭제 직후, 세션 직전, **세션 안에서 영수증을 보내기 직전마다**, 세션 뒤 | settings 항목 0개, 영속 키 일곱 개 모두 없음 |
| 네트워크가 저장돼 있는가 | `pb_stored_network_holds_no_network()` | 그 밖의 부팅: 부팅 시 최종 record 대조, **BLE 영수증 요청마다** | settings 항목 0개, `0x20002`~`0x20007` 없음, `0x20001`은 있음/없음 어느 쪽이든(읽지 못하면 실패) |

- **둘째 질문이 알 수 없는 것**: `0x20001`의 키가 부팅 때 만든 것인지 네트워크의 것인지. 저장된 네트워크는
  Active Dataset(settings)과 그 Network Key 사본(`0x20003`)을 함께 두므로 그것으로 본다.
- host test는 이 차이를 모사한다: 부팅(자식 process)마다 "부팅 키"를 만들고, 삭제는 그것까지 지운다. 나중
  부팅의 저장소는 "비어 있음"이 아닌데 영수증은 나와야 하고(검사 있음), 엄격한 질문을 나중 부팅에 쓰면 그
  검사들이 실패한다.
- 근거로 삼은 것: `tools/check_power_base_ot_store.py`가 ESP-IDF 소스에서 확인한다(부팅 때 저장하는 키는
  `StoreNetworkKey(…, false)` 하나, 자리는 `kNetworkKey` = 1, `AfterInit()`은 settings에 쓰지 않는다). OpenThread
  재현은 재시작 뒤 Thread를 켜지 않은 채 10분(가상 시간) 동안 settings가 비어 있음을 본다. PSA 재현은 부팅 키가
  생긴 저장소가 "비어 있음"은 아니고 "네트워크 없음"이며, Dataset의 키 사본이 생기면 "네트워크 있음"임을 본다.
- **실물로 확인하지 못했다.** 재시작한 보드에서 둘째 질문이 통과하지 않으면, 해제된 보드는 BLE로 영수증을
  내주지 않는다(수동 "삭제 확인"이 "영수증 없음"으로 끝난다. 지웠다고 잘못 말하지는 않는다). 부팅 로그의
  `released and no network is stored`가 그 확인이다.

### 순서로 막을 수 없는 것

영수증을 보내기 직전의 read-back **뒤에** 저장소가 쓰이고, 그것이 다시 지워지지도 않는 경우. 허브는 보낼 때
참이었던 영수증으로 `completed`이고, 보드는 record를 삭제 대기로 되돌려 영수증을 내주지 않고 부팅마다 지운다.
host test에 그대로 있다("what no order can rule out"). 스위치를 우회하는 쓰기와 flash 삭제 실패가 겹쳐야 한다.
세션이 끝나기 전에 전원이 끊기면 최종 record + 저장된 네트워크가 되고, 위 2번의 경로가 처리한다.

### host test (실제 production 소스)

| 대상 | 결과 |
|---|---|
| 보드 관리 코드 `tools/test_power_base_mgmt.c` | **355개, 실패 0** (300 → 355). 새로: 붙으면서 저장(삭제 성공/실패/성공이라 답하고 안 지움/record도 못 씀), 보낸 뒤 저장(답 유실 뒤 재전송 전, 확인받은 뒤, 지워지지 않음), 세션 안의 read-back 실패, 영수증 함수의 저장소 확인(매번 묻는다, 확인 없음·네트워크 있음·읽기 실패는 거부, 부팅 키만 있으면 내줌, 최종이 아니면 묻지도 않음), 부팅 검사(통과·다시 지움·실패·읽기 실패·다른 record), 전원 차단 네 지점과 그 뒤 부팅, 두 재현 |
| 응답 유실 후 재전송, 정상 자동 삭제, 수동 확인, 전원 차단 복구 | 같은 test의 기존 검사가 그대로 통과한다. 달라진 것은 사건 순서 문자열에 전송 전 read-back(`v`)이 들어간 것뿐이다(`PHPQADEVSvZs`, 재전송 `…SvZvZvZs`) |
| 저장소 component `tools/test_power_base_ot_store.c` | **109개, 실패 0** (69 → 109). 새로: 나중 부팅의 질문, 스위치 뒤의 삭제 |
| 로컬 초기화 `tools/test_power_base_reenroll.c` | 367개, 실패 0. 영수증 함수의 인자에 맞췄고, "최종 record + 저장된 네트워크"를 만들어 쓰던 한 경우는 그 상태에서 영수증이 나오지 않는 것을 확인하게 바꿨다 |
| 펌웨어 host test 전체 (`tools/test_*`) | **83개 프로그램, 실패 0.** 80개는 각 파일 머리말의 명령으로 빌드·실행했고, ESP-IDF 환경이 필요한 3개는 따로 실행했다(`test_power_base_ble_sec2_interop.py` 176, `test_gen_power_base_ble_config.py` 77, `test_pack_power_base_await_config.py` 18). 기록: `~/esp-builds/pogoautofix_20261008_results/host_tests_final.txt` |
| BLE 영수증 경로 `tools/test_power_base_ble_sec2_interop.py` | **176개, 실패 0** (169 → 176). ESP-IDF의 실제 protocomm·Security 2와 실제 BLE 서비스 코드(host harness)에 ESP-IDF의 공식 클라이언트로: 최종 record가 있어도 저장소에 네트워크가 있거나 읽지 못하면 `0x87`의 답이 "없음"·0, record 불변, 저장소가 깨끗해지면 같은 영수증 |
| 허브 `pogohome_api_unit_tests` (Mac host 빌드) | **1,360개, 실패 0** (1,342 → 1,360). 허브 코드는 바뀌지 않았다. 실제 허브 ↔ 실제 보드 클라이언트로 두 경우를 더했다 |
| 재현 `tools/test_power_base_release_session_openthread.sh` | **통과.** 검사 70개(49 → 70): OpenThread 30개(+3), PSA 40개(+18). 기록: `~/esp-builds/pogoautofix_20261008_release_session_repro.log` |

### link된 이미지 검사

`tools/check_power_base_ot_store.py --elf … --idf …`. 새로 보는 것: flash를 직접 읽고 지우는 것이
`pb_stored_network.c`뿐인가, `pb_network_store_erase()`가 스위치 상태를 묻고 flash 삭제를 부르는가, 부팅 경로가
최종 record를 대조하는가, BLE 이미지에서 영수증을 내주는 곳이 `release_receipt_source` 하나이고 저장소 확인을
넘기는가, 그리고 위의 소스 확인 넷.

| 이미지 | 결과 |
|---|---|
| `pogoautofix_20261008_power_base_ble` | 46개, 실패 0 |
| `pogoautofix_20261008_power_base_ble_reenroll_gpio9` | 46개, 실패 0 |
| `pogoautofix_20261008_power_base_default` | 45개, 실패 0 (BLE 등록이 없는 이미지: 영수증을 내주는 곳이 없음을 본다) |
| `pogoautofix_20261008_power_base_bench` | 45개, 실패 0 (같다) |
| 대조: 수정 전의 0.2.0 `ble` 이미지(`pogoauto_20261008_power_base_ble`) | **4개 실패**: 저장소 자체에서 키를 지우는 코드가 없다, `pb_network_store_erase()`가 스위치 상태를 묻지 않는다, 부팅 경로에 최종 record 대조가 없다, BLE 영수증 경로가 저장소 확인을 넘기지 않는다. 기록: `check_ot_store_prefix_0.2.0_ble_image.log` |

### 빌드와 산출물

**이 표의 산출물도 쓰지 않는다.** Network Key 자리의 키를 출처와 무관하게 받아들인다(아래 "Network Key
자리의 키의 출처"). 파일은 그대로 두었다. 쓸 것은 그 절의 `pogoautokey_20261008_*`다.

`tools/pogo_version.py build-firmware … --out <새 디렉터리>`. 모두 **개발 빌드**(커밋하지 않은 트리). 빌드
전후 입력이 같고 `manifest-verify --match-current-tree`를 통과했다. 어디에도 기록하지 않았다.

| 디렉터리 (`~/esp-builds/`) | 구성요소·버전·variant | 주 산출물, 크기, SHA-256 | manifest SHA-256 |
|---|---|---|---|
| `pogoautofix_20261008_power_base_default` | `power_base` 0.2.0 `default` | `power_base.bin` 889,888 B `01bc619d65da4ac8045a3187c02e274a3cda3d836b95e29d2116f2d8787f28b3` | `3b26c19ea9c94602d5283a6fe171258d1df34dd0b95675106e018e09e8783f22` |
| `pogoautofix_20261008_power_base_bench` | `power_base` 0.2.0 `bench` | `power_base.bin` 1,026,064 B `9e0a6bd328347515dd97351ae444997d128f3ebef798b5c2ff13c2227bd2c67a` | `1d86c7c879901bbcf96217aaf48492d0411ec4f6a46a4de5b0ec9a0bc8bb3585` |
| `pogoautofix_20261008_power_base_ble` | `power_base` 0.2.0 `ble` | `power_base.bin` 1,435,920 B `e14b359435ab480a6313a36ad95aa21de715b6c574fdc67a79612d3e1a552df7` | `967582631f1aaa4aafe0aa1eb2bd47a112f387d2d9a82438657961ee3296cddf` |
| `pogoautofix_20261008_power_base_ble_reenroll_gpio9` | `power_base` 0.2.0 `ble+reenroll_button_gpio9` | `power_base.bin` 1,440,192 B `b3be86f0fd496a74e0c24d1179bdd356056b7b42a9861fb9c11219dd3a224d8f` | `c492e997e58d4acc245dff9fe0cbbd35e817d6a9cb5fac06ca8eca1d06a32e59` |
| `pogoautofix_20261008_module_default` | `module` 0.1.0 `default` (회귀 빌드: `components/`가 바뀌어 지문이 달라졌다. 바뀐 두 component를 link하지 않는다) | `pogo_module.bin` 885,984 B `184146eae2265bc9553f875e8d27d2808553e1fb39a142a3d529c097808fd752` | `4c6f90db3d02ddcf985d04a47ff19b7f3d7cc4147edcc882da077feca8718bd7` |
| `pogoautofix_20261008_linux_hubctl_host_artifact` | `linux_bridge` 0.2.0, Mac host의 `pogohome-hubctl` (`macos-arm64`. **허브에 올릴 산출물이 아니다**) | `pogohome-hubctl` 187,840 B `2548e68636b5f7d2658f72ffd2eca15e308d45385f7dd318ca5400180a075f66` | `45fe4995cc93af04544eb5527e018c028f223c2a568789676bb599a1f3f88b01` |

- Power Base 네 이미지의 빌드 식별자는 `590e2a11d2cb+dirty.03479fbce5ed`, `module`은
  `590e2a11d2cb+dirty.f55d4d04aedb`, 허브는 `590e2a11d2cb+dirty.1b1bc214f04a`다(`pogohome-hubctl --version`이 그렇게
  답한다). toolchain과 대상은 위 절과 같다(ESP-IDF v6.1, `riscv32-esp-elf-gcc` 15.2.0, `esp32h2`).
- 크기: 수정 전의 0.2.0 대비 default·bench +2,256 B, ble·ble+reenroll +2,480 B. app 파티션(1,536,000 B)의 여유는
  ble 6.5%, ble+reenroll 6.2%다. `module`은 같다(885,984 B).
- `rcp`는 빌드하지 않았다(지문도 그대로다). **앱은 다시 빌드하지 않았다**: 앱 저장소는 이 작업에서 한 파일도
  바뀌지 않았고, `pogoauto_20261008_app_device_release_signed`가 지금의 앱 트리와 일치한다
  (`05375341f70d+dirty.553da7e84bb2`, `manifest-verify --match-current-tree`).
- 허브 host 산출물의 manifest는 `inputs_unchanged_during_build: false`다(위 절의 것과 같다): 이 도구는 CMake host
  빌드의 전후 지문을 재지 않는다. 바이너리에 든 빌드 식별자와 지금 트리의 값이 같다는 것은 확인했다.
- 그 밖의 새 디렉터리: `…_results`(로그, `before/`의 수정 전 재현, `fix_changes_since_start.patch`),
  `…_release_session_repro`(재현의 빌드), `…_host_ble_sec2`(host harness), `…_linux_api_host_final`·
  `…_linux_ingest_host`·`…_linux_core_host`(허브 host test), `…_linux_api_host`(작업 중의 빌드. 최종 트리보다
  앞선 것이다).

### 하지 않은 것

| 항목 | 상태 |
|---|---|
| 보드에 기록, 부팅, 삭제 | **미실행** |
| 재시작한 보드에서 "네트워크 없음"으로 읽히는가(BLE 영수증이 나오는가) | **미실행.** 위 "삭제하지 않은 부팅이 묻는 것" |
| 세션 안에서 flash를 읽는 것(attach 직후, 전송 전마다) | 미실행. Thread가 붙어 있는 동안 NVS를 여덟 번 읽는다. 걸리는 시간과 영향은 모른다 |
| 스위치 뒤의 flash 직접 삭제(`nvs_erase_all`, `psa_its_remove`) | 미실행. host에서는 시뮬레이터·파일 backend |
| BLE 영수증 요청이 NimBLE host task에서 저장소를 읽는 것 | 미실행. stack 여유를 재지 않았다 |
| 스위치가 실제로 우회되는 보드 | 만들 수 없다. 이 경로들은 그 경우를 위한 것이고 host에서는 가짜 저장소로 일으킨다 |
| 실제 전원 차단, Linux GN/CHIP 빌드, 운영 허브, iPhone | 미실행 (위 절과 같다) |

### 실물 시험에 더할 것 (준비만 했다)

위 절의 절차에 더한다. 이미지는 **`pogoautokey_20261008_power_base_ble`**(아래 절)이고, 그 절의 "실물 시험에
더할 것"을 함께 한다.

1. **가장 먼저.** 삭제를 마치고 재시작한 보드의 부팅 로그에
   `this board is released and no network is stored: its release receipt can be collected`가 나오는가.
   대신 `its network store holds a network or cannot be read -- erasing and checking`이 **매 부팅** 나오면
   중단한다: 실제 보드의 부팅이 저장소에 무엇인가를 남긴다는 뜻이고, 그러면 BLE "삭제 확인"이 "영수증
   없음"으로 끝난다.
2. 온라인 삭제의 세션 로그에 `the network store is NOT empty in the receipt session`이 **없어야** 한다. 있으면
   스위치가 경로에 없다는 뜻이다(보드는 영수증을 보내지 않고, 다시 지운 뒤 BLE 영수증으로 끝낼 수 있다).
3. 세션 중 전원 차단(위 절의 6) 뒤: 부팅 로그가 1의 첫 줄이고, "삭제 확인"(BLE)이 영수증을 받아 `completed`가
   되는가.
4. 자동 전달이 안 된 보드(허브를 끈 채 삭제)의 수동 "삭제 확인"이 이전처럼 되는가.

**복구.** 위 절과 같다. 더해지는 한 가지: 해제된 보드가 영수증을 내주지 않으면(1의 경우) 같은 보드를 다시
등록해 허브의 작업을 `superseded`로 정리하거나, 0.1.0 이미지로 되돌려 이전의 확인으로 끝낸다.

## Power Base 자동 삭제 확인: Network Key 자리의 키의 출처 (2026-10-08)

범위: 위 절의 수정을 검토하다 재현된 한 가지. `pb_stored_network_holds_no_network()`가 `0x20001`의 키를 출처와
무관하게 받아들였다. 바뀐 것은 Power Base 펌웨어(`components/power_base_ot_store`, `components/power_base_mgmt`,
`power_base/main`)와 test·검사·재현·문서다. 허브와 앱의 코드, 관리 프로토콜, BLE 메시지, record 형식, HTTP API는
바뀌지 않았다. 규약: [`power-base-management.md`](power-base-management.md) 4절 "OpenThread가 시작한 뒤에 묻는 것".

**이 절의 어느 것도 실물이 아니다.** 보드에 기록하지 않았고, 시리얼·BLE·Linux 장비에 접근하지 않았다.

### 기준

| | |
|---|---|
| 저장소 | `feature/unified-module-role-manager`, `590e2a11d2cb93f1cbc2b2b0eb094d50ea289a50`, 커밋하지 않은 작업 트리. 시작 시 변경 파일 124개(추적 64, 미추적 60), stage 0 — 검토에서 준 기준선과 같고, 위 절을 끝낸 상태와 파일 하나하나가 같다 |
| 작업 전 | `power_base` 0.2.0 `03479fbce5ed…58dc`, `linux_bridge` 0.2.0 `1b1bc214f04a…2a10`, `module` 0.1.0 `f55d4d04aedb…b39e`, `rcp` 0.1.0 `b1657c1a3148…2449` |
| 작업 후 | `power_base` 0.2.0 지문 `75d5caae752c6d4f81ce63e37c79b99821ea37fa27fbbde857c5e233057d1895`, `linux_bridge` 0.2.0 `fb760a37501df29ecbb337c0cb4deb4aa75726aa794d95d09e6587f99c1afd97`, `module` 0.1.0 `155745b4dc96d87faec28310c5608bf2312263260297a46a66b76ab3643311d9`, `rcp` 0.1.0 `b1657c1a3148f657f11584f9870fcb00516ad1a1b8145325432fa10444322449`(그대로) |
| 버전 | **유지.** 위 절과 같은 이유다: 0.2.0은 배포도 기록도 된 적 없는 개발 버전이고, 이 수정은 그 기능을 끝내는 일이며, 다른 구성요소가 보는 동작·형식은 그대로다. 어느 빌드인지는 빌드 식별자가 말한다 |
| 출력 | 모두 새 디렉터리 `~/esp-builds/pogoautokey_20261008_*`. 기존 산출물은 그대로다 |

앱 저장소(`main`, `05375341f70d…`, 변경 119개, stage 0)는 읽기만 했고 한 파일도 바뀌지 않았다.

### 결함과 실제 부팅 순서

OpenThread는 시작할 때(`thread_manager_init()` → `esp_openthread_start()` → `Instance::AfterInit()` →
`KeyManager::Init()`) `StoreNetworkKey(임의의 키, overwrite = false)`를 부른다. 그 자리(`0x20001`)에 키가 **없을
때만** 만들고, 있으면 그대로 둔다(ESP-IDF v6.1 소스, `tools/check_power_base_ot_store.py`가 확인). 위 절의 검사는
그 자리의 키를 "OpenThread가 부팅마다 만드는 것"으로 보고 통과시켰으므로, 삭제가 Network Key 하나만 지우지 못한
보드는 다음 부팅부터 통과했다(settings도 다른 키도 없다). 실제로 그 상태에 이르는 경로가 있다: 최종 record 아래
네트워크가 남은 보드(세션 중 전원 차단)의 부팅이 지우다가 그 키만 남기면, 그 부팅은 read-back으로 잡지만
**다음 부팅은 잡지 못했다.**

`power_base/main/app_main.c`의 순서는 NVS 초기화 → 설정 load → EUI-64 → `thread_manager_init()` → (mode 2)
`thread_manager_stop()` → 로컬 초기화 부팅 → `pb_mgmt_runtime_finish_pending_release()`다. 관리 부팅 경로가
저장소를 볼 때는 OpenThread가 이미 그 자리를 지나간 뒤다. 그래서 고칠 자리는 `thread_manager_init()` **앞**이다.

### 채택한 방식

"안전한 초기화 순서에서 지우고 확인"과 "출처 확인"을 둘 다 쓴다. 저장 위치로 추정하지 않는다.

1. **시작 전 삭제.** 최종 record를 가진 보드는 부팅마다 `thread_manager_init()` 앞에서 두 저장소 자체를 지우고
   다시 읽는다(`pb_mgmt_runtime_prepare_released_start()` → `pb_mgmt_released_prestart()`). NVS만 있으면 된다.
   남은 키가 누구의 것이든 없어지고, 그것을 read-back이 확인한다.
2. **생성 관찰.** 두 저장소가 비어 있음을 읽은 순간부터 `0x20001`의 쓰기·제거를 저장소 인터페이스에서 센다
   (모든 키 저장소 쓰기는 `--wrap`으로 그곳을 지난다). 첫 쓰기의 내용은 RAM에 두고, 두 번째 쓰기나 제거가
   보이면 지운다.
3. **대조.** `0x20001`의 키는 "이 부팅이 비어 있음을 읽었고, 그 뒤 쓰기 정확히 한 번·제거 없음, flash의 항목이
   그 쓰기와 바이트까지 같음"일 때만 OpenThread의 것으로 본다. 아니면 영수증을 내주지 않는다.

1만으로도 기존 키는 없어진다. 2·3은 "그 뒤 그 자리에 있는 것이 OpenThread가 방금 만든 바로 그것"임을 추정이
아니라 관찰로 말하게 한다. 1이 실패한 부팅은 OpenThread가 남은 키 위에서 시작하고(그대로 둔다), 부팅 경로의
대조가 통과하지 못해 OpenThread의 삭제로 한 번 더 지우고 read-back한다.

검토한 다른 방식: (가) 출처를 영속 표식으로 남기는 것 — 새 영속 상태가 생기고, 표식과 키가 어긋나는 전원 차단
경우를 다시 다뤄야 한다. (나) OpenThread 시작 **뒤**에 지우고 재시작 없이 계속 쓰는 것 — 지운 뒤의 instance로
같은 부팅에서 재등록까지 가는 경로를 실물로 확인할 수 없다. (다) 키 내용을 읽어 판별 — 임의의 키와 네트워크의
키는 내용으로도 구분되지 않는다.

### 수정 전의 실패와 수정 후

기록: `~/esp-builds/pogoautokey_20261008_results/`(`key_origin_before_after.txt`, `before/`).

| | 수정 전 | 수정 후 |
|---|---|---|
| 검토가 준 재현 파일(`tools/test_power_base_ot_store.c` + 검사 1개), 그대로 | 109개 통과, **1개 실패**: `REVIEW: a surviving old Network Key alone must not authorize a release receipt` | 그 검사를 같은 자리에 옮겼다(`[key origin review]`): 통과 |
| 새 `tools/test_power_base_ot_store.c`를 작업 시작 시점의 component에 대해 | **18개 실패**(옮긴 검사 포함). 이전 component에 없는 것(관찰, RAM reset)은 `before/make_prefix_tree.py`가 "아무것도 보지 못한 관찰"로 대신했고 검사문은 그대로다 | 156개 모두 통과 |
| 실제 BLE 서비스 코드로: 최종 record + 기존 Network Key 하나 | (수정 전 harness는 저장소 검사가 가짜였다. 실제 component를 붙인 것이 이번이다) | `0x87`의 답이 "없음"·0. 지우지 못하는 부팅, 읽지 못하는 부팅 뒤에도 "없음". 지워진 부팅부터 같은 영수증 |

재현 파일을 지금의 소스에 그대로 돌리면 그 검사는 통과하고(109개 통과), **다른 검사 하나가 실패한다**:
`but no network is stored: that is the question a later boot asks`. 그 파일이 품고 있는 위 절의 검사로,
아무도 보지 않은 채 그 자리에 놓인 키를 "받아들인다"고 확인하던 것이다: 이번에 고친 동작 그 자체다. 기록은
`before/reviewer_file_on_fixed_sources.log`.

### host test (실제 production 소스)

| 대상 | 결과 |
|---|---|
| 저장소 component `tools/test_power_base_ot_store.c` | **156개, 실패 0** (109 → 156). 새로: 시작 전 삭제 뒤 OpenThread의 키가 "쓰기 한 번, 제거 없음"으로 보이고 받아들여진다 / 재시작하면 같은 키도 출처를 모르는 키다 / 기존 Network Key 하나만 남은 저장소: OpenThread의 시작은 그대로 두고, 받아들이지 않는다 / 시작 전 삭제가 그 키를 지우고 새 키가 생긴 뒤에만 통과 / 지우지 못함, 읽지 못함(그 자리, 저장소 전체, 대조용 읽기) / 같은 부팅에서 바뀐 키(제거+쓰기, 두 번 쓰기, 인터페이스를 거치지 않은 변경, 길이만 다른 것) / 비어 있음을 읽지 않은 부팅의 쓰기 / 보관하기에 너무 긴 쓰기 |
| 보드 관리 코드 `tools/test_power_base_mgmt.c` | **397개, 실패 0** (355 → 397). 가짜 플랫폼의 부팅이 `app_main.c`의 순서를 따른다(실제 `pb_mgmt_released_prestart()` → "OpenThread 시작" → record 대조). 새로: 시작 전 단계는 최종 record에서만 지운다(등록됨·삭제 대기·record 없음은 아무것도 읽지도 지우지도 않는다) / 삭제 오류·키만 남기는 삭제·read-back 실패 / **최종 record + 기존 Network Key 하나**: 영수증 없음, OpenThread가 시작해도 없음, 부팅의 대조가 통과하지 않음 / 그 상태의 부팅들(지우지 못함, 읽지 못함, 시작 전 삭제 직후 전원 차단, 시작 전 삭제는 실패하고 OpenThread의 삭제는 되는 부팅) / 지워진 부팅부터 영수증 |
| 정상 자동 삭제, 응답 유실 후 재전송, 수동 확인, 전원 차단 복구 | 같은 test의 기존 검사가 통과한다. 달라진 것: 최종 record 아래 네트워크가 남은 보드의 **보통의** 다음 부팅은 이제 시작 전에 지우므로 곧바로 영수증이 있다(그 삭제가 안 되는 부팅에서만 "없음") |
| 로컬 초기화 `tools/test_power_base_reenroll.c` | 367개, 실패 0 (변경 없음) |
| 펌웨어 host test 전체 (`tools/test_*`) | **83개 프로그램, 실패 0.** 80개는 각 파일 머리말의 명령으로 빌드·실행했고, ESP-IDF 환경이 필요한 3개는 따로 실행했다(`test_power_base_ble_sec2_interop.py` 189, `test_gen_power_base_ble_config.py` 77, `test_pack_power_base_await_config.py` 18). 기록: `~/esp-builds/pogoautokey_20261008_results/host_tests_final.txt` |
| BLE 영수증 경로 `tools/test_power_base_ble_sec2_interop.py` | **189개, 실패 0** (176 → 189). host harness가 이제 **실제** `pb_stored_network.c`·`pb_volatile_store.c`를 link한다(가짜인 것은 키 저장소 표와 "OpenThread의 시작"뿐). ESP-IDF의 실제 protocomm·Security 2, 실제 BLE 서비스, 실제 `pb_mgmt_receipt_load()`, 실제 `pb_mgmt_released_prestart()`를 ESP-IDF 공식 클라이언트로 |
| 허브 `pogohome_api_unit_tests` (Mac host 빌드) | **1,360개, 실패 0** (변경 없음). ctest 3/3, ingest 8/8, core 1/1. 허브 코드와 허브 test는 바뀌지 않았다. `components/`가 허브의 빌드 입력이라 다시 빌드했다 |
| 재현 `tools/test_power_base_release_session_openthread.sh` | **통과.** 검사 90개(70 → 90): OpenThread 30개(그대로), PSA 60개(+20). 기록: `~/esp-builds/pogoautokey_20261008_release_session_repro.log`. 실제 PSA 키 저장소로: import 한 번에 그 자리의 쓰기 한 번·제거 없음 / 저장된 키 파일이 본 쓰기와 같다 / 있는 키 위의 import는 거부된다 / 재시작 뒤의 같은 키는 받아들이지 않는다 / 네트워크의 키가 남은 저장소에서 OpenThread의 시작은 import하지 않고 그 키가 그대로 export된다 / 시작 전 삭제 뒤에는 새 키가 import된다 / **키 저장소가 이미 읽어 들인 키는 파일을 지워도 RAM에서 계속 답한다**(시작 전 삭제가 맨 앞이어야 하는 이유) |

### link된 이미지 검사

`tools/check_power_base_ot_store.py --elf … --idf …`. 새로 보는 것: 키 저장소를 쓰고 지우는 것이 PSA 키
저장소뿐인가(`psa_save_persistent_key`, `psa_destroy_persistent_key`) / `pb_stored_network.c`가 관찰을 시작하고
항목을 대조하는가 / 시작 전 삭제가 link됐는가 / **`app_main`에서 그 호출 뒤의 다음 호출이 `thread_manager_init()`
인가**(제어 흐름을 따라간다: 컴파일러가 그 블록을 떼어 놓고 되돌아가는 jump로 이었다) / 소스 넷(있는 키는
그대로 둔다, 바꾸는 쪽은 먼저 지운다, import 한 번에 `psa_its_set` 한 번, 있는 키 위에는 쓰지 않는다).

| 이미지 | 결과 |
|---|---|
| `pogoautokey_20261008_power_base_ble` | 54개, 실패 0 |
| `pogoautokey_20261008_power_base_ble_reenroll_gpio9` | 54개, 실패 0 |
| `pogoautokey_20261008_power_base_default` | 53개, 실패 0 |
| `pogoautokey_20261008_power_base_bench` | 53개, 실패 0 |
| 대조: 이 수정 전의 `ble` 이미지(`pogoautofix_20261008_power_base_ble`) | **3개 실패**: 관찰·대조 코드가 없다, 시작 전 삭제가 없다, `app_main`에 그 호출이 없다. 기록: `check_ot_store_prefix_ble_image.log` |

`firmware_builds.txt`의 `ble ot_store check exit=1`은 빌드 직후, 호출 순서를 주소 순으로 보던 첫 검사의
결과다(컴파일러가 mode 2 블록을 `thread_manager_init()` 호출 뒤쪽 주소에 두고 되돌아가는 jump로 이었다).
제어 흐름을 따라가게 고친 뒤 네 이미지를 모두 다시 검사했다(`image_checks.txt`). 이미지는 다시 만들지 않았다:
검사 도구는 빌드 입력이 아니다.

### 빌드와 산출물

`tools/pogo_version.py build-firmware … --out <새 디렉터리>`. 모두 **개발 빌드**(커밋하지 않은 트리). 빌드
전후 입력이 같고 `manifest-verify --match-current-tree`를 통과했다. 어디에도 기록하지 않았다.

| 디렉터리 (`~/esp-builds/`) | 구성요소·버전·variant | 주 산출물, 크기, SHA-256 | manifest SHA-256 |
|---|---|---|---|
| `pogoautokey_20261008_power_base_default` | `power_base` 0.2.0 `default` | `power_base.bin` 891,600 B `11f3baa4c7b2ab50012fb6355154ab9346d2fa4192bb5d2361b7dec4d215e0aa` | `f89dec746c4ba56ba8b78278c7cf3b779776b8f381372c8fcfaa3c1a89872170` |
| `pogoautokey_20261008_power_base_bench` | `power_base` 0.2.0 `bench` | `power_base.bin` 1,027,776 B `d11b9b774416ff6847324c50ff8d830c20532510a804a72a2173dc0d9e66c03b` | `e02e05abec2925cecdb80f30586718c3398404d39e6abf01350807c7fdb22b3b` |
| `pogoautokey_20261008_power_base_ble` | `power_base` 0.2.0 `ble` | `power_base.bin` 1,437,632 B `a16399a1929794ccd7626a213f890f42ef0c0d71896fcc7461a81af21cdf68fb` | `4f1e8f331b3ac0b58715930ba4ed4bec61830b45d1eefa5692b0f79b0217970e` |
| `pogoautokey_20261008_power_base_ble_reenroll_gpio9` | `power_base` 0.2.0 `ble+reenroll_button_gpio9` | `power_base.bin` 1,441,904 B `d549478bcdf97c71c4bb8d5ddc0911ffa4b0f3cda9c45067f1623a20736da7c3` | `098fe412452719f43bcef64a17f9bbb4367fd492ee74ef98f56c61d5155cb26b` |
| `pogoautokey_20261008_module_default` | `module` 0.1.0 `default` (회귀 빌드: `components/`가 바뀌어 지문이 달라졌다. 바뀐 component를 link하지 않는다) | `pogo_module.bin` 885,984 B `1159d9dd82ab5215e6927a2056031c05b10463b9771736c584cf4a266ec8e5a3` | `83aa086a6f7271a549a4d920d3089287790b9f7f5333fbc603815856b9adce37` |
| `pogoautokey_20261008_linux_hubctl_host_artifact` | `linux_bridge` 0.2.0, Mac host의 `pogohome-hubctl` (`macos-arm64`. **허브에 올릴 산출물이 아니다**) | `pogohome-hubctl` 187,840 B `e321acf04958f2d23be503bca41b1819f9b5c282d2d649a94989b28bb15efbc1` | `b13b8bcfecdb543bad42b926bbebe996cbff01de0230c33f9c87847dfa814776` |

- Power Base 네 이미지의 빌드 식별자는 `590e2a11d2cb+dirty.75d5caae752c`, `module`은
  `590e2a11d2cb+dirty.155745b4dc96`, 허브는 `590e2a11d2cb+dirty.fb760a37501d`다(`pogohome-hubctl --version`이 그렇게
  답한다). toolchain과 대상은 위 절들과 같다(ESP-IDF v6.1, `riscv32-esp-elf-gcc` 15.2.0, `esp32h2`). 컴파일 경고 0.
- 크기: 위 절의 빌드 대비 네 구성 모두 +1,712 B. app 파티션(1,536,000 B)의 여유는 ble 6.4%, ble+reenroll
  6.1%다. `module`은 같다(885,984 B).
- `rcp`는 빌드하지 않았다(지문도 그대로다). **앱은 다시 빌드하지 않았다**: 앱 저장소는 한 파일도 바뀌지
  않았고 `pogoauto_20261008_app_device_release_signed`가 지금의 앱 트리와 일치한다
  (`05375341f70d+dirty.553da7e84bb2`, `manifest-verify --match-current-tree`).
- 허브 host 산출물의 manifest는 위 절들과 같이 `inputs_unchanged_during_build: false`다(이 도구는 CMake host
  빌드의 전후 지문을 재지 않는다). 바이너리에 든 빌드 식별자와 지금 트리의 값이 같다는 것은 확인했다.
- 그 밖의 새 디렉터리: `…_results`(로그, `before/`, `key_origin_before_after.txt`,
  `key_origin_changes_since_start.patch`), `…_release_session_repro`(재현의 빌드), `…_host_ble_sec2`(host
  harness), `…_linux_api_host_final`·`…_linux_ingest_host`·`…_linux_core_host`(허브 host test).

### 하지 않은 것

| 항목 | 상태 |
|---|---|
| 보드에 기록, 부팅, 삭제 | **미실행** |
| 시작 전 삭제가 실제 보드에서 되는가 | **미실행.** `thread_manager_init()` 앞에서 `nvs_entry_find`·`nvs_erase_all`·`esp_psa_its.c`의 `psa_its_remove`·`psa_its_get_info`를 부른다. 소스로는 NVS 초기화만 필요하다 |
| OpenThread가 시작하며 `0x20001`에 정확히 한 번 쓰는가 | **미실행.** 실제 PSA 키 저장소 core(TF-PSA-Crypto)로는 한 번이다. ESP-IDF의 `esp_psa_its.c`와 OpenThread의 ESP port를 거친 경로는 실행하지 않았다. 한 번이 아니면 해제된 보드가 영수증을 내주지 않는다(수동 확인이 "영수증 없음") |
| 그 항목을 다시 읽은 것이 쓴 것과 같은가 | **미실행.** 같은 이유. host의 저장소는 표와 파일 backend다 |
| 그 시점에 PSA 키 저장소가 그 키를 읽어 들이지 않았는가 | **미실행.** 소스로는 OpenThread만 그 키를 쓴다. 이미 읽어 들였다면(재현에 있다) OpenThread가 새 키를 만들지 않고 저장소는 빈 채로 남는다: 영수증은 나온다(저장소에는 아무것도 없다) |
| 해제된 보드의 부팅당 flash 쓰기 두 번 | 실물로 세지 않았다 |
| 실제 전원 차단, Linux GN/CHIP 빌드, 운영 허브, iPhone | 미실행 (위 절들과 같다) |

### 실물 시험에 더할 것 (준비만 했다)

위 두 절의 절차에 더한다. 이미지는 `pogoautokey_20261008_power_base_ble`.

1. **가장 먼저.** 삭제를 마치고 재시작한 보드의 부팅 로그에 다음 두 줄이 이 순서로, OpenThread의 시작 로그를
   사이에 두고 나오는가.
   `this board is released: its network stores are erased and read back empty before OpenThread starts`
   `this board is released and no network is stored: its release receipt can be collected`
   첫 줄 대신 `did NOT read back empty before OpenThread starts`가 나오면 시작 전 삭제가 실물에서 되지 않는
   것이고, 둘째 줄 대신 `holds a network or cannot be read -- erasing and checking`이 **매 부팅** 나오면 OpenThread의
   키가 "한 번 쓰기"로 보이지 않거나 다시 읽은 것이 다른 것이다. 어느 쪽이든 중단한다: BLE "삭제 확인"이 "영수증
   없음"으로 끝난다.
2. 그 보드를 두 번 더 껐다 켜도 1의 두 줄이 나오는가(부팅마다 지우고 새로 만든다).
3. 그 상태에서 수동 "삭제 확인"(BLE)이 영수증을 받아 허브가 `completed`가 되는가. 같은 보드를 다시 등록할 수
   있는가(해제된 보드의 OpenThread 키는 부팅마다 새 것이다).

**복구.** 위 절들과 같다. 해제된 보드가 영수증을 내주지 않으면 같은 보드를 다시 등록해 허브의 작업을
`superseded`로 정리하거나, 0.1.0 이미지로 되돌려 이전의 확인으로 끝낸다.

## 주소 복구와 센서 체인 재등록 복구 (2026-10-09)

범위 두 가지. ① 같은 허브의 Thread 수신 주소·포트가 바뀌었을 때 Power Base가 허브를 다시 찾는 기능(Power Base
0.3.0, Linux bridge 0.3.0. 규약: [`power-base-management.md`](power-base-management.md) 12절). ② Base를 삭제하고 다시
등록했을 때 계속 꽂혀 있던 아래 센서 모듈이 돌아오지 않던 결함의 수정(module 0.1.1.
[`state-machine.md`](state-machine.md) "UPSTREAM 세대 변경과 DOWNSTREAM 재설정"). 두 가지는 서로 다른 일이고 아래에서도
따로 적는다.

**이 절의 어느 것도 실물이 아니다.** 보드에 기록하지 않았고(flash·erase·시리얼 접근 없음), 실제 Thread 네트워크·
BLE·운영 Linux 허브·iPhone을 쓰지 않았다. 구현이 끝났다는 것과 실물에서 된다는 것은 다른 말이다: 지금 끝난 것은
**구현과 Mac에서의 검증**이고, 실물 검증은 하나도 하지 않았다.

### 기준

| | |
|---|---|
| 저장소 | `feature/unified-module-role-manager`, `590e2a11d2cb93f1cbc2b2b0eb094d50ea289a50`, 커밋하지 않은 작업 트리. 시작 시 status 126줄(추적 수정 64, 미추적 62), stage 0, stash 0 — 작업 지시의 `baseline.json`(파일 550개의 SHA-256, index 해시)과 모두 같았다 |
| 작업 전 | `power_base` 0.2.0 지문 `75d5caae752c6d4f81ce63e37c79b99821ea37fa27fbbde857c5e233057d1895`, `linux_bridge` 0.2.0 `fb760a37501df29ecbb337c0cb4deb4aa75726aa794d95d09e6587f99c1afd97`, `module` 0.1.0 `155745b4dc96d87faec28310c5608bf2312263260297a46a66b76ab3643311d9`, `rcp` 0.1.0 `b1657c1a3148f657f11584f9870fcb00516ad1a1b8145325432fa10444322449`. 첫 수정 전에 떠 둔 사본으로 수정 전 트리를 다시 만들어 네 값을 다시 계산했고 지시서의 값과 같았다 |
| 작업 후 | `power_base` **0.3.0** 지문 `0a4e8b76307a534b4946358528bd72d00a51c96f7cd716d6d36d9dc30f4e03a8`(빌드 식별자 `590e2a11d2cb+dirty.0a4e8b76307a`), `linux_bridge` **0.3.0** `dfeda036433e49e43314292d105212fab4bcaa87070c5f8c03d0eb44d7dd674a`(`…dirty.dfeda036433e`), `module` **0.1.1** `7ae0df18db6e8bb9e8323d564aed05415f1ef511c6df4b163a5a2d748f709f1d`(`…dirty.7ae0df18db6e`), `rcp` 0.1.0 `e180a4323e8deb0fb909349dc097eeb345d3fb88345f0304be91ce20b185df38` |
| 번호들 | 관리 규약 1, BLE 등록 규약 1, HTTP API v1, record 버전 1·2·3, 허브 상태 파일 schema 2, 재등록 journal 1: 모두 그대로. 늘어난 것은 관리 메시지 type `0x06`·`0x86`과 capability bit 두 개(`tools/pogo_version.py compat`) |
| 도구 | macOS 26.6.2, Apple clang 21.0.0, CMake, ESP-IDF v6.1(`~/.espressif/v6.1/esp-idf`), 그 ESP-IDF의 OpenThread `b678a4f63b6f` |
| 출력 | 모두 새 디렉터리 `~/esp-builds/pogolocate_20261009_*`. 기존 산출물과 실물 시험 준비물은 바꾸지 않았다 |

`rcp`의 지문이 달라진 것은 `versions.json`이 그 빌드 입력이기도 해서다. RCP의 소스는 바뀌지 않았고 새 RCP 이미지는
만들지 않았다. 설치돼 있는 RCP를 바꿀 이유가 없다.

### ② 센서 체인: 재현, 원인, 수정

**실물에서 본 것(사용자 보고와 열려 있던 모니터의 로그. 설치돼 있던 것은 10-03 이미지이고, 현재 소스와 그 이미지가
같은 바이너리인지는 확인하지 않았다).** Power Base → SGP41 → PMS5003에서 Base를 삭제하고 다시 등록하자 SGP만 표시됐고,
PMS는 뽑았다 꽂은 뒤에 나타났다. SGP의 로그: UPSTREAM이 끊겼다가 다시 연결되고 새 collector를 받아 Thread에 붙은 뒤,
PMS를 물리적으로 분리하기 전까지 DOWNSTREAM provisioning이 한 번도 시작되지 않는다. PMS의 로그: 그동안 sample과
topology 전달이 확인되지 않는다. 로그 앞부분의 `ESP_ERR_TIMEOUT`은 그 전에 해결된 PMS 센서 문제로, 이 결함과 무관하다.

**재현(실제 코드, 수정 전 소스).** `tools/test_module_upstream_regeneration.c`를 첫 수정 전에 떠 둔 소스 사본과 link해
실행했다. 실제 `pogo_port`(두 port)·`role_manager`·`module_wiring`·`child_service`·`parent_service`·`topology_reporter`·
`sensor_transport`가 돈다. test가 맡는 것은 양쪽 끝의 보드, Thread 대역(받은 Dataset을 간직한다), CoAP가 어디로
갔는지의 기록뿐이다.

| | 수정 전 소스 | 수정 후 |
|---|---|---|
| module 하나(9개 국면, 74개 검사) | **20개 실패.** 국면 1(최초 연결)·2(같은 세대)·8(role만 변경)은 통과하고, 국면 3에서 실물과 같은 모양으로 실패한다: UPSTREAM이 다시 provisioning된 뒤에도 꽂혀 있는 아래 보드에 IDENTIFY조차 가지 않는다 | 74개 통과 |
| 실제 module 세 개의 체인(프로세스 세 개, 27개 검사) | **11개 실패.** Base를 다시 provisioning하면 M1만 새 세대로 가고 M2·M3는 첫 세대의 collector와 Dataset에 그대로 남는다 | 27개 통과. 세대마다 각 module의 Thread join이 정확히 한 번씩이다 |

**원인.** `parent_service`는 DOWNSTREAM의 PEER_READY로만 provisioning을 시작하고, 끝나면 DETACHED/ERROR가 올 때까지
`JOINED`에 머문다. UPSTREAM이 새로 provisioning돼도 아래 보드가 물리적으로 그대로면 DOWNSTREAM port에는 event가
없다. `module_wiring`의 gate(`upstream_ready`)는 "지금 시작해도 되는가"만 말하고 "이미 준 것이 낡았는가"는 말하지
않는다. 지시서의 가설과 같다.

**수정.** [`state-machine.md`](state-machine.md) "UPSTREAM 세대 변경과 DOWNSTREAM 재설정". 요약: `child_service`의
세대 번호(JOINED가 될 때만 오름) → `parent_service`가 DOWNSTREAM session을 그 번호에 묶고, 다르면 다시 provisioning.

**수정 후 검사한 경계**(같은 test).

| 경계 | 국면 | 본 것 |
|---|---|---|
| 같은 세대에서는 불필요한 재설정 0 | 2, 체인 5 | ROUTER 승격·무선 detach와 재attach·시간 경과 뒤에도 새 session 0, 재join 0, 새 ATTACHED 0. 체인은 3초 동안 세 module 모두 재join·재등록 없이 sample만 보낸다 |
| UPSTREAM 물리 detach 뒤 새 세대 | 3, 체인 2·3 | 아래 보드가 정확히 한 번 다시 provisioning되고 새 collector·Dataset·depth·부모를 순서대로 받는다. DOWNSTREAM port event 0 |
| detach 없이 새 IDENTIFY_REQ(체인의 둘째 단계가 보는 것) | 4, 체인 4 | 같음. 자기 topology도 새 부모·depth로 다시 등록한다 |
| 3단 체인의 각 단계 | 체인 2·3·4 | M1(depth 1)·M2(2)·M3(3)가 모두 새 collector·Dataset·depth·부모로 옮기고 그곳으로 sample을 보낸다. M2·M3는 UPSTREAM port event를 하나도 보지 않는다 |
| 연속 변경, provisioning 도중 변경 | 5 | 앞 세대의 JOIN_RESULT 대기를 3초 timeout이 아니라 즉시 버리고 새 세대로 시작한다. 버려진 세대로 완료된 session 0 |
| 늦은 JOIN_RESULT | 6 | 이전 session의 JOIN_RESULT를 새 session의 대기 중에 다시 보내면 ACK만 다시 가고, 그 session은 그것으로 완료되지 않는다 |
| 재시도 실패 뒤 복구 | 7 | 응답 없는 보드에 3초 동안 5회 시도(고정 간격이면 약 13회). 응답을 시작하면 다음 시도에 provisioning된다. 그동안 이 module 자신의 경로는 정상 |
| role만 변경 | 8 | DOWNSTREAM을 뽑았다 꽂으면 link mode 재적용 2회, 새 session은 1회(물리 재연결 몫)뿐 |
| provisioning 도중 DOWNSTREAM 분리 | 9 | session을 버리고 빈 port로 아무것도 보내지 않는다. 다시 꽂으면 최신 세대로 provisioning |

**검사가 실제로 잡는지.** 안전장치를 하나씩 뺀 소스로 같은 test를 돌렸다: 이전 JOIN_RESULT 판별 제거 → 국면 6 실패,
session 도중의 세대 확인 제거 → 국면 5 실패, `topology_reporter`의 세대 확인 제거 → 국면 4 실패.

**이 test가 보이지 않는 것.** OpenThread(join은 곧바로 성공하는 대역이다), 실제 무선과 시간, NVS의 내용, 실제 센서,
실제 Power Base 펌웨어(Base 쪽은 같은 packer로 만든 대본이다). 기존 `tools/test_child_provisioning_session.c`는
`child_service`의 상태 기계를 옮겨 적은 모형이라 이 결함의 근거로 쓰지 않았다(그 test는 그대로 통과한다).

### ① 주소 복구: 검증한 것

| 검사 | 무엇을 실제 코드로 돌리는가 | 결과 |
|---|---|---|
| `tools/test_power_base_locate.c` | 보드의 `pb_mgmt_proto.c`·`pb_mgmt_client.c`·`power_base_enrollment.c` + NVS simulator. 네트워크(어느 주소·port가 무엇으로 답하는가)와 flash가 test다 | 140개 검사 통과. 내용은 [`power-base-management.md`](power-base-management.md) 12절 "확인한 것" |
| `linux/matter_bridge/app/api/tests`의 `EndpointLocate` | **실제 허브 코드**(`PowerBaseManagement`)와 **실제 보드 코드**를 서로 붙임 | API 단위 test 전체 1,399개 검사 통과(그 가운데 이 시나리오 29개), ctest 3/3 |
| `linux/matter_bridge/ingest/tests/test_loopback.cpp` | 실제 `run_server()`와 실제 UDP socket(loopback): locate 포트의 두 번째 socket | ingest ctest 8/8. locate 포트 검사 16개 |
| `test_interface_address.cpp` | `wpan0`의 주소 목록에서 Mesh-Local EID를 고르는 규칙 | 통과(주소 목록은 test가 준다. 실제 `wpan0`는 Mac에 없다) |
| `tools/test_linux_bridge_exporter_destination.c` | 실제 exporter·`power_base_transport`·registry | 통과. 전송 중 목적지 변경 포함 |
| `tools/test_power_base_locate_openthread.sh` | **ESP-IDF의 OpenThread core**(Nexus) 위에서 보드의 실제 client·record·**펌웨어의 힌트 수집 파일** | 31개 검사 통과(세 번 실행해 세 번 같음). 12절 "힌트의 출처" |

**요구한 경우와 그것을 본 곳.** 기존 endpoint 무응답 → 발견 → 신선한 인증 → 영속 반영 → 관리 poll 재개: locate
test의 주소·port 변경, `EndpointLocate`, OpenThread 재현. heartbeat·topology·sample 재개: exporter test(새 목적지로
heartbeat가 먼저, 이어 전체 topology와 모듈별 최신 sample). 다른 허브·키·세대·보드, 변조, 재생, 한 요청 늦은 답,
형식 오류: locate test의 "틀린 답"과 wire format, 허브 쪽은 `EndpointLocate`의 "누가 답을 받는가". 후보 여러 개와
한도: locate test, OpenThread 재현(다른 router의 다른 허브). 실패한 저장 세 가지와 read-back 실패, commit 직전과
전환 직후의 전원 차단, 재부팅 뒤 새 주소 복원: locate test. 삭제와의 경쟁(record가 `UNENROLLING`·`RELEASED`·없음·
다른 세대로 바뀐 경우 한 바이트도 쓰지 않음), 옮긴 뒤의 삭제: locate test, `EndpointLocate`. 오래된 callback:
exporter test의 "전송 중 변경". 구형 record(v1·v2)와 locate 없는 platform, 구형 허브(거부를 "여기가 아니다"로
받음): locate test.

**검사가 실제로 잡는지.** `pb_mgmt_client.c`에서 하나씩 뺐을 때 locate test가 실패한다: nonce 대조, MAC 검증,
보드·hub·세대 대조, 목적지에서의 재확인, 쓸 수 있는 목적지 검사, 저장 직전의 record 재확인, "쓰기 오류면 옮기지
않음". exporter의 목적지 세대 확인을 빼면 exporter test가 실패한다. (`LOCATE_ACK` type 검사 하나는 빼도 실패하지
않는다: 다른 type의 메시지는 목적지 필드가 0으로 읽혀 어차피 쓸 수 없는 목적지로 버려진다.)

**성공 bool만 stub한 곳은 없다.** 인증은 실제 HMAC이고(host의 SHA-256 구현, 허브 쪽은 OpenSSL. golden MAC은
Python `hmac`으로 따로 계산해 맞췄다), 저장은 NVS simulator의 실제 `set_blob`·`commit`·read-back이다.

### 기존 회귀 검사와 빌드

| | 결과 |
|---|---|
| `tools/test_*.c` 전체(각 파일 머리말의 명령 그대로) | **71개 프로그램, 실패 0**(기존 68개 + 새 3개: `test_module_upstream_regeneration` 101개 검사, `test_power_base_locate` 140, `test_linux_bridge_exporter_destination` 45). ②의 수정 직후에도 전체를 한 번 돌렸고(69개, 실패 0), 이것은 ①까지 끝난 최종 트리의 결과다. 기록: `~/esp-builds/pogolocate_20261009_results/host_tests_final/` |
| `tools/test_*.py` | **15개, 실패 0.** 12개는 그대로 실행. ESP-IDF 환경이 있어야 하는 3개는 따로 실행했다: `test_pack_power_base_await_config.py` 18개 검사, `test_power_base_ble_sec2_interop.py` 189(`tools/host_ble_sec2`를 새 디렉터리 `~/esp-builds/pogolocate_20261009_host_ble_sec2`에 다시 빌드), `test_pogo_version.py` 253. 마지막 것은 실제 저장소의 관리 메시지·capability 목록을 적어 둔 기대값 한 곳을 `0x06`·`0x86`과 새 capability 두 개가 든 목록으로 고쳤다(고치기 전에는 그 검사 하나가 실패했다: 목록이 늘어난 것을 test가 알아챈 것이다). 한꺼번에 돌린 첫 실행에서는 이 3개가 환경(PATH의 ESP-IDF Python, `protobuf`) 없이 돌아 실패·건너뜀으로 나왔고 그 기록도 `logs/py/`에 그대로 있다. 다시 돌린 기록은 `logs/py_rerun/` |
| 자동 삭제·erased-store·RAM-only 영수증 세션·E2(취소 뒤 부팅): `test_power_base_mgmt.c`, `test_power_base_ot_store.c`, `test_power_base_reenroll.c`, `tools/test_power_base_release_session_openthread.sh` | 모두 통과(위 71개에 포함. `test_power_base_reenroll.c` 367개 검사), OpenThread 재현 스크립트는 `PASS: both reproductions`. 삭제 명령·영수증·저장소 판정의 동작을 바꾸는 변경은 하지 않았다: 이 경로에서 달라진 것은 관리 client가 답 없는 poll을 세고 HELLO에 capability bit 하나를 더 싣는 것뿐이다 |
| Linux host: API(CMake) 단위 test와 ctest, ingest ctest, core ctest | API: 빌드 경고 0, 단위 test 1,399개 검사 실패 0, ctest 3/3. core: ctest 1/1. ingest: 빌드 경고 0, ctest 8/8. 빌드는 새 디렉터리 `~/esp-builds/pogolocate_20261009_linux_{api,core,ingest}_host` |
| 펌웨어 빌드(ESP-IDF v6.1, `tools/pogo_version.py build-firmware`) | Power Base 0.3.0 네 변형(`default`, `bench`, `ble`, `ble+reenroll_button_gpio9`)과 module 0.1.1 `default`: 빌드 성공, 다섯 산출물 모두 `manifest-verify --match-current-tree` 통과(최종 트리의 지문과 같다). Power Base 네 이미지는 `check_power_base_ot_store.py`도 통과. `power_base.bin` SHA-256: `ble+reenroll_button_gpio9` `ca7aeb7e1c7912e70cbf644bf3ddb9a8d37fbbcfb152b639c69e15c4b52f7aa1`(1,449,152바이트), `ble` `1ead30927e961de6063229d6df033479f039e72905a776d2ae1f68b9f13d0567`, `default` `c982980afb467812a2ceaa14cee380bbbee12d10942c3dbeea75b59092ee05f3`, `bench` `953db9de7d0e7766a215fe632a3f23ccafe2b405ccc9a5c67afbd4366ac96472`. `pogo_module.bin` `5cc1afb50e1f00c62cce839a60661fb53c6c6cf3f6031a6f76432260b4058f1d`(887,312바이트). 힌트 수집을 별도 파일로 옮기기 전에 만든 Power Base 빌드는 `~/esp-builds/pogolocate_20261009_superseded/`에 두었고 쓰지 않는다(지금의 트리와 지문이 다르다). RCP는 빌드하지 않았다. 어느 이미지도 보드에 기록하지 않았다 |

Linux용 `pogo_matter_bridge`의 GN/CHIP 빌드는 Mac에서 할 수 없어 하지 않았다. `BUILD.gn`에 새 소스 파일은 없다
(바뀐 것은 이미 들어 있는 파일들이다). 허브에서 실행할 소스 묶음과 지시는 실물 시험 준비물에 있다.

### 이전 기록의 판정 (2026-10-09에 덧붙임. 앞 절들의 글은 그 날짜에 적은 그대로 둔다)

위 "Power Base 자동 삭제 확인" 세 절은 각각 "이 절의 어느 것도 실물이 아니다"라고 적고 있다. 그 문장은 그 절이
기록한 **작업**(2026-10-08의 구현과 Mac 검증)에 대한 것으로 여전히 맞다. 그 뒤의 일을 여기에 적는다.

- **자동 삭제의 실물 시험은 사용자가 했다.** 근거는 사용자의 로그·화면·보고이고, 이 저장소의 작업이 보드나 허브에
  접근해 얻은 것이 아니다. 그 시험은 그때 설치돼 있던 빌드(Power Base 0.2.0, Linux bridge 0.2.0)의 시험이며 **이
  절의 새 빌드(0.3.0, 0.1.1)의 시험이 아니다.**
- **E2(영수증 전달 세션 도중의 차단).** 확인된 것은 여기까지다: RAM에만 있는 세션 도중 USB 연결이 끊겼고 → 다음
  부팅의 로그에 네트워크 저장소가 비어 있음과 Thread 비활성이 나왔고 → 사용자가 수동 BLE "삭제 확인"으로 삭제를
  완료했다고 보고했다. 로그에 남은 USB reset 원인만으로 **완전한 전원 차단**이었다는 것이 따로 증명된 것은 아니다.
- **F2-3(재등록한 등록의 센서·방 지정 보존).** 정상으로 보고된 것은 **PMS를 손으로 다시 연결한 뒤**의 10분과, 허브를
  다시 시작한 뒤의 5분이다. 즉 그 결과는 위 ②의 결함(다시 꽂아야 돌아옴)을 손으로 지나간 뒤의 것이고, ②의 자동
  복구를 본 것이 아니다. 둘은 따로 기록한다.
- 앱 1.2.1 (10)·(11)의 실물 시험은 사용자가 따로 진행하며, 그 결과는 여기에 적지 않았다(추정하지 않는다).

### 하지 않은 것

- **실물 전부.** 새 이미지의 flash, 실제 SGP41·PMS5003 체인의 삭제·재등록, 실제 OTBR의 주소·port 변경, 운영 허브의
  배포, Linux GN/CHIP 빌드.
- ①에서: 실제 보드의 ESP-IDF port와 radio 위의 힌트 수집, 실제 `wpan0`에서의 EID 판별, locator 주소로 온 datagram이
  Linux bridge의 socket에 닿는 것. OpenThread 재현의 허브는 OpenThread의 CoAP 서버다.
- ②에서: 실제 OpenThread join 시간 아래에서의 동작(JOIN_RESULT를 기다리는 20초 사이의 세대 변경은 대역의 3초로 봤다),
  세 module 실물 체인.
- 관리 task의 stack(6144바이트), `parent_service` task의 stack 여유는 보드에서 재지 않았다.

### 다음 실물 시험 (준비만 했고 실행하지 않았다)

(2026-10-09 후속에 덧붙임. 아래는 그때 적은 그대로다. 이 준비물과 그 안의 이미지는 쓰지 않는다: 아래 "후속 수정: 탐색
누락과 이전 JOIN_RESULT" 절의 준비물이 대신한다. 이 문단이 말하는 "주소 변경" 시험은 그 준비물에서 "준비되지 않음"으로
바뀌었다.)

준비물: `~/esp-builds/pogo_locate_pms_hwtest_20261009`. 두 기능을 따로 판정한다.

- **센서 체인.** SGP41(가운데 module)에 module 0.1.1을 app 영역만 갱신한다(cfg·NVS 그대로). PMS는 0.1.0 그대로여도
  된다. 두 센서를 계속 연결해 둔 채 Base 삭제 → 재등록 → PMS를 건드리지 않고 두 센서가 새 collector로 돌아오는지,
  topology와 값이 갱신되는지, 방 재지정 뒤 보존되는지, 재부팅 뒤, 반복.
- **주소 복구.** Power Base 0.3.0(app 영역만)과 Linux bridge 0.3.0(실행 파일만, `<state-dir>` 그대로). 같은 허브
  신원과 Thread Dataset을 보존한 port 변경과 주소 변경, 앱을 종료한 상태의 복구, 데이터 지속과 관리 삭제, 재부팅 뒤
  새 주소 복원. 주소를 바꾸는 명령은 사전 확인·백업·되돌리기·중단 조건이 있는 안내로만 준비했다(주소 변경은 OTBR의
  설정을 새로 만드는 절차라 이전 주소로 되돌릴 수 없다. 선택 항목으로 두었다). "이 허브가 아닌 것의 거부"는 실물로는
  `LOCATE`에 답하지 않는 0.2.0 bridge의 경우만 준비했다: 다른 hub_id의 허브를 같은 네트워크에 세우는 절차와 위조된
  답은 준비하지 않았고, 그 경우들의 근거는 위 host test와 OpenThread 재현뿐이다.

## 후속 수정: 탐색 누락과 이전 JOIN_RESULT (2026-10-09)

위 "주소 복구와 센서 체인 재등록 복구 (2026-10-09)"의 후보(Power Base 0.3.0 `…dirty.0a4e8b76307a`, module 0.1.1
`…dirty.7ae0df18db6e`, Linux bridge 0.3.0 `…dirty.dfeda036433e`)를 코드 검토한 결과 결함 두 건이 나왔다. 검토는 host에서
재현했고 실물 실패가 아니다: 그 후보는 어느 보드에도 허브에도 설치된 적이 없다. 이 절은 그 두 건의 수정과 Mac에서의
검증이다. 위 절의 글은 그 시점의 기록으로 그대로 둔다(그 절의 검사 수 140·101은 이 수정 전의 것이다).

**이 절의 어느 것도 실물이 아니다.** 보드에 기록하지 않았고 Linux 허브에 접속하지 않았다.

### 기준

| | |
|---|---|
| 지시와 검토 자료 | 검토 보고서, 두 재현 소스·실행 스크립트·로그, 기준선(`review-baseline.json`). 읽기만 했고 파일의 시각과 SHA-256이 작업 전후에 같다 |
| 저장소 | `feature/unified-module-role-manager`, `590e2a11d2cb93f1cbc2b2b0eb094d50ea289a50`, 커밋하지 않은 작업 트리. 시작 시 status 151줄(추적 수정 80, 미추적 71), stage 0, stash 0, index 해시와 기준선의 파일 559개(SHA-256)가 모두 기준선과 같았다 |
| 버전 | 그대로: `power_base` 0.3.0, `module` 0.1.1, `linux_bridge` 0.3.0, `rcp` 0.1.0. 근거는 [`versioning.md`](versioning.md) "버전의 단일 원본"의 2026-10-09 항목 |
| 작업 전 지문 | `power_base` `0a4e8b76307a534b4946358528bd72d00a51c96f7cd716d6d36d9dc30f4e03a8`, `module` `7ae0df18db6e8bb9e8323d564aed05415f1ef511c6df4b163a5a2d748f709f1d`, `linux_bridge` `dfeda036433e49e43314292d105212fab4bcaa87070c5f8c03d0eb44d7dd674a`, `rcp` `e180a4323e8deb0fb909349dc097eeb345d3fb88345f0304be91ce20b185df38` |
| 작업 후 지문 | `power_base` `03c2999127775354373eb11f6d54e1ca373352167c995569ed2ec930d0b37a50`(build id `590e2a11d2cb+dirty.03c299912777`), `module` `0a6fdc1ecc0d55e3bdcfd826677052ea425764a53dd22d76dff0d1a28cc1ab64`(`…dirty.0a6fdc1ecc0d`), `linux_bridge` `511c30f053bda47e1a9012b786a4a56d46b457369cc27aff198a671be0a2c5f8`(`…dirty.511c30f053bd`), `rcp` 그대로 |
| 형식·번호 | `tools/pogo_version.py compat`의 출력이 작업 전과 같다(POGO, 관리 규약과 메시지·capability, BLE, HTTP API, record, 상태 파일) |
| 출력 | 새 디렉터리 `~/esp-builds/pogolocfix_20261009_*`. 앞 후보의 `pogolocate_20261009_*`와 그 준비물은 바꾸지 않았다 |

`linux_bridge`의 소스(`linux/matter_bridge`)는 바뀌지 않았다. 지문이 달라진 것은 `components/`가 그 빌드 입력이어서다
(bridge의 host test가 보드의 `pb_mgmt_client.c`를 link한다). 기능 변경이 아니다.

### 검토의 두 재현: 수정 전 실패, 수정 후 통과

재현 소스와 스크립트는 **고치지 않았다.** 주소 재현은 소스 머리말의 명령에서 출력 경로만, 센서 재현은 스크립트의
사본에서 출력·실행 경로만 새 결과 디렉터리로 바꿔 현재 production 소스와 link했다.

| 재현 | 수정 전 소스 | 수정 후 소스 |
|---|---|---|
| `repro_candidate_starvation.c` (141개 검사) | **1개 실패**: "later bounded rounds eventually reach the 8th valid hint on its second port". 200번의 poll 뒤에도 옮기지 못함 | 141개 통과. 다음 round(poll 6번 뒤)에 옮긴다: 요청 합계 19개(첫 round 12 + 둘째 round 7) |
| `repro_stale_join_retry.c` (15개 검사) | **1개 실패**: "old result must not complete SECOND attempt either". 둘째 시도가 이전 결과로 `JOIN OK` | 15개 통과. 셋째 시도까지 이전 결과는 ACK만 받는다 |

### ① 주소 복구: 원인, 수정, 검사

**원인(코드로 확인).** `pb_mgmt_client.c`의 round는 매번 같은 후보 목록을 index 0부터 물었고 요청은 12개였다. 저장된
port가 5684가 아니면 힌트마다 후보가 둘이어서 힌트 6개에서 이미 목록(13)이 요청 수를 넘는다. 힌트 수집
(`enrollment_locate_hints.c`)은 매 호출 앞의 8개에서 멈췄다. 둘 다 고쳐야 했다: client의 순환만으로는 수집에서 잘린
후보에 닿지 못한다.

**수정.** [`power-base-management.md`](power-base-management.md) 12절 "round 사이의 진행". 요약: 힌트는 전부(상한 128,
Thread의 한도보다 많다), round는 지난 round가 마지막으로 물은 후보의 다음부터(주소·port로 기억), round의 마지막 요청에
온 답이 말한 목적지는 다음 round의 첫 요청으로(새 nonce, 같은 규칙). round당 요청 12개와 round 간격은 그대로다.

**검사(`tools/test_power_base_locate.c`, 199개. 수정 전 140개).** 검토의 검사 한 줄을 그 시나리오에 넣었고(다음 round에
7개의 요청으로 옮긴다는 것까지), `scenario_progress_from_round_to_round`를 더했다.

| 요구한 경우 | 본 것 |
|---|---|
| 실제 허브가 마지막 힌트, 저장 port ≠ 5684 | 힌트 20개(후보 40): 넷째 round에(무응답 poll 48번째. round는 6·12·24·48에 돌았다) 옮긴다. 그 앞의 후보 38개는 각각 정확히 한 번, 저장된 주소의 locate 포트는 round마다 한 번, 어느 round도 12개를 넘지 않는다. 문서의 상한 ⌈C/10⌉ 안 |
| 받는 수보다 많은 힌트 | 138개를 주면 128개를 받고, 128번째의 허브를 12번째 round에 찾는다(무응답 poll 726번째). 129번째 뒤는 묻지 않는다 |
| 요청 수의 끝에서 온 locator 답 + 직접 확인 | 12번째 요청에 허브의 locator가 답하면 그 round는 `UNCONFIRMED`로 끝나고 아무것도 저장하지 않는다. 다음 round의 **첫 요청**이 그 목적지로 가고, nonce는 앞의 어느 요청과도 다르며, 그 한 요청의 답으로 확인·저장된다 |
| 넘긴 목적지가 약점이 되지 않는가 | 그 목적지에 아무도 없음 / 지난 round의 답을 다시 보냄 / 다른 키 / 다른 허브 / 다른 세대 / 주소를 바꾼 답: 여섯 경우 모두 한 번 묻고 저장 없음, 그다음 round는 다시 묻지 않는다. 허브의 것이 아닌 답은 round의 마지막 요청에 와도 넘길 것을 남기지 않는다 |
| 목록 변경 | 앞에 router 셋이 늘고(하나는 두 번, 하나는 다른 prefix): 처음으로 돌아가지 않고 마지막으로 물은 후보의 다음에서 잇는다. 서 있던 후보의 router가 떠남: 그 뒤의 것부터, 빠뜨리지 않고. 허브의 router가 목록 맨 앞으로 이동: walk가 끝을 돌아 그 round에 만난다 |
| 저장 실패 | 둘째 round에서 확인 뒤 commit 실패 → record는 이전 것 그대로 → 셋째 round의 첫 요청이 그 목적지이고 저장된다 |
| 재부팅·삭제 상태 | 재부팅 뒤에는 넘긴 것이 없고 목록의 처음부터다. record를 다시 읽으면 잊는다. 인증된 poll 답이 오면 잊는다. 넘긴 목적지가 확인돼도 그 사이 `UNENROLLING`이 됐거나 초기화가 진행 중이면 한 바이트도 쓰지 않는다. record의 네트워크 밖 목적지는 넘겨져 있어도 묻지 않는다 |

기존 140개 검사(틀린 키·허브·세대·보드, 재생, 변조, 쓸 수 없는 주장, 전원 차단, record 변경 여섯 경우, v1·v2)는 그대로
통과한다.

**실제 OpenThread core(`tools/test_power_base_locate_openthread.sh`, 39개. 수정 전 31개).** 세 번째 실행을 더했다:
router 13개(OTBR과 12개)와 보드. **펌웨어의 수집 파일**이 13개의 locator를 모두 내고, 그 목록의 마지막 router로 옮겨 간
허브를 보드가 둘째 round에 찾는다(첫 round: 저장된 주소와 앞의 11개로 12개. 둘째 round: 4개). 같은 재현을 수정 전의
client와 수집 파일로 빌드하면 수집이 8개에서 멈춘다(그 검사와 뒤의 세 검사 실패).

**검사가 실제로 잡는지.** `pb_mgmt_client.c`에서 하나씩 바꾼 12가지에서 `test_power_base_locate`가 모두 실패한다: 항상
처음부터 / 자리 번호로만 기억 / 사라진 후보에서 한 칸 늦게 / 마지막 요청의 답을 버림 / 넘긴 목적지를 계속 보관 /
**넘긴 목적지를 묻지 않고 저장**(19개 실패) / 저장 실패를 넘기지 않음 / 응답 재개에도 진행을 보관 / record를 다시 읽어도
보관 / 힌트를 8개만 받음 / 넘긴 목적지의 네트워크 검사 생략 / 확인 요청에 한도를 적용하지 않음.

### ② 센서 체인: 원인, 수정, 검사

**원인(코드로 확인).** `run_provisioning_session()`은 시작할 때마다 이전 결과의 식별을 그 session의 node 기록
(`s_node.has_join_result`)에서 가져온 뒤 그 기록을 지웠다. 결과 없이 끝난 session 다음에는 가져올 것이 없었다.

**지금의 frame으로 보장되는 것을 먼저 확인했다.** JOIN_RESULT에는 child의 seq만 있다. `child_service`는 결과마다
seq를 올리고, 최신 결과만 같은 seq로 다시 보내며, 새 session의 IDENTIFY_REQ에서 그만둔다. 국면 10이 이 모듈 자신의
실제 `child_service`로 본다: ACK하지 않은 결과가 같은 seq로 다시 온다 / 새 IDENTIFY_REQ에 답한 뒤에는 그 결과가 더
오지 않는다 / 다음 결과는 다음 번호다. 그래서 "본 것 가운데 가장 새 seq"를 기억하는 것으로, 그 규칙을 따르는 child에
대해서는 이전 결과와 새 결과를 가를 수 있다. 가를 수 없는 경우(규칙을 따르지 않는 상대가 처음 보이는 이전 결과를 join
대기 중에 보냄)와 형식을 바꾸지 않은 이유는 [`state-machine.md`](state-machine.md)에 있다.

**수정.** 그 기억을 session이 아니라 handshake의 수명에 둔다.

**검사(`tools/test_module_upstream_regeneration.c`, 160개 = module 하나 133 + 체인 27. 수정 전 101개 = 74 + 27).**

| 요구한 경우 | 국면 | 본 것 |
|---|---|---|
| 같은 이전 결과가 두 번 이상의 실패·시간 초과·재시도에 걸쳐 도착 | 11 | 세 번의 시도 모두 이전 결과는 ACK만 받고 시간 초과한다. 그 사이 첫 단계에서 실패한 session 둘을 지나서도 같다. 자기 결과를 보내기 시작하면 다음 시도에 한 번 완료된다 |
| 결과를 한 번도 받기 전에 세대가 바뀌고, 버려진 session의 결과가 늦게 옴 | 12 | 다음 session의 앞 단계에서 도착한 그 결과는 ACK되지 않고, join 대기에서 다시 오면 ACK만 받는다. 그 session은 완료되지 않고, 다음 번호의 결과로 완료된다. 버려진 세대로 완료된 것은 0 |
| 연속 세대 변경, 초기 단계 실패 | 5, 7, 11 | 기존 국면 그대로 통과, 11에서 조합 |
| downstream 물리 detach·error·재시작 | 9, 15 | 뽑았다 꽂은 뒤와 port error 뒤: 다시 1부터 세는 child의 첫 결과(이전에 본 번호와 같은 seq 1)를 그대로 받는다. port가 알려 주지 않은 재시작: join timeout 한 번을 쓰고 둘째 session에서 완료(약 3.4초, session 2개). 그 이상 반복하지 않는다 |
| duplicate JOIN_RESULT 재ACK | 16 | session 밖에서 온, 처리한 결과의 재전송은 ACK한다. 처리한 적 없는 결과는 ACK하지 않되 기억해, 다음 session이 자기 것으로 받지 않는다 |
| seq가 넘어가는 자리 | 13 | 255로 완료 → 다음 session에 255가 다시 오면 ACK만 → 1로 완료 |
| SET_DATASET의 ACK 유실 | 14 | ACK보다 먼저 온 자기 결과를 이전 것으로 치지 않는다: 재전송된 것을 받아 한 session에 완료 |
| 같은 세대에서 재설정·쓰기 0, 최초 연결, 역할, 3단 체인 | 1, 2, 8, 16, 체인 | 그대로 통과. 국면 16의 끝에서 1.5초 동안 session 0, join 0 |

parent가 결과를 "받았는지"는 ACK만으로는 알 수 없다(받은 결과와 이전 결과 모두 ACK한다). 국면 14·15는 그 뒤 join
timeout이 지나도록 새 session이 시작되지 않는 것으로 판정한다.

conditioning, ROOT·INTERMEDIATE·LEAF, JOIN_RESULT 신뢰성, Power Base의 downstream은 기존 host test
(`test_conditioning_provisioning`, `test_role_manager_*`, `test_join_result_reliability`, `test_module_downstream_fault_isolation`,
`test_power_base_*`)가 그대로 통과한다(아래 전체 회귀).

**검사가 실제로 잡는지.** 수정 전의 `parent_service.c`로 같은 test를 돌리면 13개가 실패한다(국면 11의 둘째 시도부터).
수정한 파일에서 하나씩 바꾼 7가지에서도 모두 실패한다: SET_DATASET 대기 중의 결과까지 이전 것으로 기록(국면 14) /
handshake가 바뀌어도 잊지 않음(15) / session 밖의 결과를 기록하지 않음(16) / 앞 단계의 결과를 기록하지 않음(12) / 그 둘
다 / 아는 결과를 그대로 받음 / session을 시작할 때마다 잊음(검토가 찾은 결함과 같은 모양: 국면 11).

**이 test가 보이지 않는 것.** 위 절과 같다: OpenThread, 실제 UART와 시간, 실제 센서, 실제 Power Base 펌웨어. 아래 보드가
이전 결과를 join 대기 중에 다시 보내는 것은 대본이 만든 것이고, 실제 `child_service`는 그렇게 보내지 않는다(국면 10).

### 전체 회귀와 빌드 (최종 트리)

| | 결과 |
|---|---|
| `tools/test_*.c` 전체(각 파일 머리말의 명령 그대로) | **71개 프로그램, 실패 0.** `test_power_base_locate` 199개 검사, `test_module_upstream_regeneration` 160개 |
| `tools/test_*.py` | **15개, 실패 0** (ESP-IDF 환경에서. `test_power_base_ble_sec2_interop.py` 189개 검사는 `tools/host_ble_sec2`를 새 디렉터리에 다시 빌드, `test_pogo_version.py` 253) |
| 실제 OpenThread core의 재현 | `test_power_base_locate_openthread.sh` 39개 통과, `test_power_base_release_session_openthread.sh` `PASS: both reproductions` (새 작업 디렉터리에서 OpenThread와 TF-PSA-Crypto를 다시 빌드) |
| Linux host | API: 빌드 경고 0, 단위 test 1,399개 검사 실패 0, ctest 3/3. core ctest 1/1. ingest: 빌드 경고 0, ctest 8/8 |
| 펌웨어 빌드(ESP-IDF v6.1, riscv32-esp-elf-gcc 15.2.0, `tools/pogo_version.py build-firmware`) | Power Base 0.3.0 네 변형과 module 0.1.1 `default` 빌드 성공. 빌드 전후의 네 구성요소 지문이 같고 manifest의 `inputs_unchanged_during_build`가 true다. 다섯 산출물 모두 `manifest-verify --match-current-tree` 통과, Power Base 네 이미지는 `check_power_base_ot_store.py` 통과 |

산출물(`~/esp-builds/pogolocfix_20261009_<구성요소>_<변형>/artifact`. 모두 development, `deployed=false`,
`hardware_verified=false`):

- `power_base` `ble+reenroll_button_gpio9`: `power_base.bin` SHA-256 `59cbe31ec709f35188f193124964f92b2edb601bb307390acd465e8c39f6bbf3`, 1,450,224바이트
- `power_base` `ble`: `1d6cc1108608e820e195f01946fb3b5f71bb75e6482aa2175f383d68c39906c4`
- `power_base` `default`: `bf94d645f5c01e74e5b75bfaac0fb8b68b01ada36e86da058be7ad1e37d89397`
- `power_base` `bench`: `076673673e32fb3ed2c4e26d71fc46d2abc426fb1e20b8b8b6041a2f4dbc9a3f`
- `module` `default`: `pogo_module.bin` SHA-256 `3c3d0ba7e90789996d158ddc21c566a913ef2f9ac7dcf31c6b1f87026badd349`, 887,344바이트

RCP는 빌드하지 않았다(소스와 지문이 그대로다). Linux용 `pogo_matter_bridge`의 GN/CHIP 빌드는 Mac에서 할 수 없어 하지
않았다: 허브에서 실행할 소스 묶음(`pogo_linux_verify_590e2a11d2cb_511c30f053bd_prep3`)과 지시를 새 실물 시험 준비물에
두었다. 어느 이미지도 보드에 기록하지 않았다.

### 하지 않은 것

- **실물 전부.** 위 절의 "하지 않은 것"이 그대로다.
- ①에서: router가 많은 실제 Thread 네트워크, 그 안에서 round가 걸리는 실제 시간. 128개 상한은 Thread의 한도에서
  계산했고 그런 네트워크를 만들지 않았다(재현은 router 13개). `s_locate_hints`(2KB, 정적)가 보드의 RAM 여유에 미치는
  영향은 재지 않았다.
- ②에서: 실제 UART 위의 지연·중복. 규칙을 따르지 않는 상대가 처음 보이는 이전 결과를 join 대기 중에 보내는 경우는
  막지 못한다(형식을 바꾸지 않았다).

### 다음 실물 시험 (준비만 했고 실행하지 않았다)

준비물: `~/esp-builds/pogo_locfix_hwtest_20261009`(앞의 `pogo_locate_pms_hwtest_20261009`를 대신한다. 그 디렉터리는
그대로 두었다). 두 기능을 따로 판정한다.

- **센서 체인(P).** 위 절과 같다. 이미지만 새 빌드다.
- **주소 복구(L).** 실물로 보는 것은 **port 변경**까지다: 앱을 종료한 상태의 복구, 데이터 지속, 재부팅 뒤 새 port,
  옮긴 상태의 관리 삭제, port 되돌리기.
- **주소(EID) 변경 시험은 준비되지 않았고 하지 않는다.** 위 절이 선택 항목으로 적었던 "OTBR 초기화 뒤 Dataset 복원"은
  이전 주소로 되돌릴 수 없어 준비물에서 뺐다. 운영 네트워크와 분리된 시험 환경이나, 운영이 아닌 장치에서 먼저 확인한
  상태 복원 절차가 있어야 준비된다. 그때까지 "주소가 바뀐 경우"의 근거는 host test와 OpenThread 재현뿐이다.
- **다른 hub_id의 허브 거부도 실물로 준비되지 않았다.** `LOCATE`를 모르는 0.2.0 bridge와의 조합은 혼합 호환의
  확인으로 따로 두었고, 그 결과를 "다른 허브 거부"의 판정으로 쓰지 않는다.

## 허브 목록에서 제거 (2026-10-10)

돌아오지 않는 Power Base를 허브의 목록에서 지우는 기능이다([`power-base-management.md`](power-base-management.md)
13절). Linux bridge만 바뀌었고(`linux_bridge` 0.4.0), 보드 펌웨어와 관리·BLE 규약은 그대로다. 앱 쪽 기록은 앱
저장소의 `docs/validation-status.md` "허브 목록에서 제거 (2026-10-10, `1.3.0 (13)`)"에 있다.

**이 절의 어느 것도 실물이 아니다.** 보드에 접근하지 않았고, Linux 허브에 접속하지 않았고, Linux에서 빌드하지 않았다.

### 기준

| | |
|---|---|
| 저장소 | `feature/unified-module-role-manager`, `590e2a11d2cb93f1cbc2b2b0eb094d50ea289a50`, 커밋하지 않은 작업 트리. 시작 시 status 151줄(미추적 71), stage 0, stash 0, index SHA-256 `b53e4f1b…bf1d0`: 작업 지시가 적은 값과 같았다 |
| 버전 | `linux_bridge` 0.3.0 → **0.4.0**. `power_base` 0.3.0, `module` 0.1.1, `rcp` 0.1.0 그대로 |
| 작업 전 지문 | `linux_bridge` `511c30f053bda47e1a9012b786a4a56d46b457369cc27aff198a671be0a2c5f8`, `power_base` `03c2999127775354373eb11f6d54e1ca373352167c995569ed2ec930d0b37a50`, `module` `0a6fdc1ecc0d55e3bdcfd826677052ea425764a53dd22d76dff0d1a28cc1ab64` |
| 작업 후 지문 | `linux_bridge` `02727ebba79bb284769dcc36733a6716348bb8361a59dbb9f1d7d327bd04ac57`(build id `590e2a11d2cb+dirty.02727ebba79b`). `power_base`·`module`·`rcp`의 지문도 달라졌다: 소스는 같고 `versions.json`이 그들의 빌드 입력이기 때문이다. 2026-10-09의 펌웨어 산출물은 자기 manifest로는 그대로이고 `--match-current-tree`만 통과하지 않는다 |
| 형식·번호 | `tools/pogo_version.py compat`의 출력이 작업 전과 같다(POGO, 관리 규약과 메시지·capability, BLE, record). HTTP API는 추가만, 상태 파일은 schema 2에 선택 목록 `forgets`([`versioning.md`](versioning.md) "호환성 정책") |
| 출력 | `~/esp-builds/pogoforget_20261010_*`(빌드·결과), `~/esp-builds/pogo_forget_hwtest_20261010`(실물 시험 준비물). 2026-10-09의 `pogolocfix_20261009_*`와 `pogo_locfix_hwtest_20261009`는 바꾸지 않았다 |

### 무엇으로 시험했나

실제 production 코드다: `PowerBaseManagement`(상태 전이·저장), `ingest::Registry`와 `ingest::handle_request()`(데이터
경로), HTTP 서버(`Server`, TLS loopback, 실제 `DeviceAuth` 페어링), 설정 정리(`UnassignModuleRooms`), Matter 쪽의
`BridgeManager`·`EndpointStore`. 보드는 실제 펌웨어 client(`pb_mgmt_client.c`, `power_base_enrollment.c`)를 NVS
simulator에 올린 것이거나, 그 grant로 `pb_mgmt_proto.c`가 만든 메시지다. **주입한 것**: 시각(`now_unix_ms`), "들린 지
얼마나 됐나"의 기준 시각(`steady_now`), 상태 파일 쓰기 실패(`writer`), 설정 쓰기 실패. 실제 NTP 변경이나 전원 차단을
시험한 것이 아니다.

| 요구한 경우 | 어디서 | 본 것 |
|---|---|---|
| 오프라인 최신 등록의 목록 제거, 재시작 유지 | `ListRemoval` | 자격증명 삭제(파일에 키 없음), 차단, Registry에서 Base·모듈 제거, 기록. 새 프로세스(빈 Registry)에서도 차단·기록·미표시 |
| 이전 등록 방식(자격증명 없음)의 기록 정리 | `ListRemovalOfUnmanagedBase` | 허브가 지어낸 `record`로 제거, 재시작 뒤에도 차단, 삭제 요청은 여전히 `Unsupported` |
| 온라인 Base 거절 | `ListRemoval`, `ForgetApiTests`, ingest `test_power_base_block` | `Online`/`409 power_base_online`, 파일·Registry 그대로. Registry가 추적하지 않는 Base도 그 아래 node가 online이면 거절 |
| 확인창 뒤 재연결 | `ListRemovalOfUnmanagedBase` | 다시 들린 Base에 대한 오래된 요청: 거절, 변화 없음 |
| 확인창 뒤 새 등록 세대 | `ListRemoval`(8), `ListRemovalOfUnmanagedBase`, `ForgetApiTests` | 등록이 **시작만** 돼도 `record`가 바뀌어 오래된 요청은 `stale_record`. 새 등록은 그대로 완료된다 |
| 잘못된 인증·다른 허브·다른 UUID·stale·malformed | `ForgetApiTests`, `ListRemoval`(2), `ListRemovalOnAnotherHub` | 평문 HTTP `403`, 토큰 없음·모르는 토큰 `401`, 틀린 본문 `400`/`422`, 다른 허브의 `record` 거절. 어느 것도 아무것도 바꾸지 않는다 |
| 삭제 작업의 각 상태와의 충돌 | `ListRemovalAndRemovalJobs` | `pending` → 같은 쓰기에서 `cancelled`. `sent` → 작업·키 유지, Base만 목록에서 빠짐, 며칠 뒤에도 `sent`, 이후 인증된 accepted → released → **영수증으로만** `completed`. `released` → `UnknownPowerBase` |
| 중복 요청·응답 유실 | `ListRemoval`(6), `ForgetApiTests` | 같은 `request_id` → 같은 기록, 파일 그대로. 다른 Base에 쓴 `request_id` → 거절. 두 기기가 동시에 다른 `request_id`로 → 정확히 하나만 `201` |
| 저장 실패 시 선(先)효과 없음 | `ListRemoval`(3), ingest·core test | 쓰기 실패: 파일·Registry(차단 없음, Base·모듈 그대로)·자격증명·설정 정리·기록 모두 그대로, 그 Base의 데이터와 poll은 계속 받아들여진다 |
| 저장 직후·정리 도중 재시작 | `ListRemoval`(7), `ListRemovalLimitsAndStorage` | 파일만 있는 새 프로세스: 차단 적용. 설정 정리 실패 → 재시작 → 정리 목록 복원 → 재시도 |
| 설정 저장 실패 뒤 재시도와 새 등록 보호 | `ListRemovalLimitsAndStorage` | 재시도 전에 보드가 다시 등록되고 모듈 하나가 돌아오면, 재시도는 **아직 없는 모듈만** 정리한다 |
| 용량 한계 | `ListRemovalLimitsAndStorage` | 차단 64개가 찼을 때 거절(기존 차단 63개 그대로). `pending` 삭제가 잡아 둔 자리는 쓴다. 기록 64개가 찼을 때 `TooMany`, 보존 시간 뒤 가능 |
| 손상·잘린 파일, 모르는 schema | `ListRemovalLimitsAndStorage` | 여섯 가지 파일: 관리 기능이 시작되지 않고, 요청은 `Unavailable`, **파일은 그대로** |
| 이전 메시지로 되살아나지 않음 | `ListRemoval`(5) | heartbeat·topology·sample `4.03`, poll·HELLO·LOCATE(두 포트)·accepted·released·영수증 `4.01`, HTTP 영수증 `Rejected`. 실제 펌웨어 client는 등록된 상태 그대로다(보드는 지워지지 않았다) |
| 새로 인증된 등록의 복귀 | `ListRemoval`(8), `ForgetApiTests` | 자격증명 발급만으로는 풀리지 않고, 새 키의 첫 poll에서 차단 해제 |
| 늦은 이전 작업이 새 등록·방 지정을 지우지 않음 | `ListRemoval`(8), `ListRemovalLimitsAndStorage` | 이전 요청의 재전송과 이전 `record`의 새 요청 모두 새 등록을 건드리지 않는다 |
| 다른 Base, 다른 Base로 옮긴 모듈 | `ListRemoval`(4) | 옮긴 모듈은 정리 목록에 들어가지 않고 그쪽 node는 그대로 |
| Matter endpoint | core `test_power_base_removal` | 제거된 Base의 모듈에 `remove_endpoint`, 다른 Base의 모듈은 그대로, 늦은 보고는 endpoint를 만들지 않음, 재등록 뒤 **같은 endpoint id** |
| 자동 삭제의 영수증 의미 | 기존 `OnlineRemoval`·`ThreadReceipt` 등 그대로 통과, `ListRemovalAndRemovalJobs` | `completed`는 영수증 검증뿐 |
| 시계가 앞뒤로 바뀜 | `ListRemovalAndRemovalJobs` | 1년 뒤로·앞으로: `sent` 작업 그대로, 차단 그대로, 두 번 처리 없음 |
| 다른 허브 | `ListRemovalOnAnotherHub` | 이전 허브에서 제거해도 새 허브의 등록·모듈·poll은 그대로 |

### 검사가 실제로 잡는지

`~/esp-builds/pogoforget_20261010_results/tools/mutate_hub.py`: 저장소의 사본에서 보호 조건을 하나씩 뺀 19가지 변형을
만들어 같은 test를 돌렸다. **19가지 모두 실패한다**(`logs/mutations_hub.txt`): 온라인 검사 제거(추적되는 Base /
node만 있는 Base), 쓰기 실패 뒤에도 차단·제거, `record` 비교 생략, 시작된 등록을 `record`에 넣지 않음, 자격증명
유지, 차단 생략, `pending` 삭제를 취소하지 않음, `sent` 삭제까지 취소, 같은 요청을 두 번 처리, 진행 중인 등록 무시,
차단 용량·기록 용량 검사 생략, 차단된 Base를 계속 목록에 올림, 옮긴 모듈을 정리 목록에 넣음, 이미 차단된 Base를 다시
처리, 토큰 없이 경로 허용, 평문 HTTP 허용, 형식이 틀린 기록 항목 수용.

처음 돌렸을 때 넷은 잡히지 않았고, 그래서 test를 더했다: Registry 자체의 장면(ingest `test_power_base_block`), 정리
목록의 내용 검사, 작업을 남긴 Base에 대한 두 번째 요청, 그리고 변형 하나는 빌드되지 않아 고쳐 썼다.

### 이전 버전과의 파일 호환 (두 버전의 실제 코드)

`tools/xver/xver.cpp`를 작업 전 트리(0.3.0)와 작업 후 트리(0.4.0)에 각각 빌드해 네 조합을 돌렸다(`logs/xver.txt`).

| 쓴 쪽 → 읽은 쪽 | 결과 |
|---|---|
| 0.4.0 → 0.3.0 | 읽힌다. 세 Base 모두 차단 유지(heartbeat 거절). `sent` 작업을 남긴 Base는 0.3.0에서 **managed로 다시 보인다**. 0.3.0이 파일을 다시 쓰면 `forgets` 목록이 **없어진다**, 차단 3개는 남는다 |
| 0.4.0 → 0.4.0 | 차단 3, 기록 3, 목록에 없음. 다시 써도 유지 |
| 0.3.0 → 0.4.0 | 그대로 읽힌다. 0.4.0이 쓰면 빈 `forgets`가 생긴다 |
| 0.3.0 → 0.3.0 | (대조) |

### 전체 회귀 (최종 트리, `final_checks.txt`)

| | 결과 |
|---|---|
| `tools/test_*.c` 전체 | 71개 프로그램, 실패 0 |
| `tools/test_*.py` (ESP-IDF 환경) | 15개, 실패 0 |
| 실제 OpenThread core의 재현 | `test_power_base_locate_openthread.sh` 39개 통과, `test_power_base_release_session_openthread.sh` `PASS: both reproductions` |
| 2026-10-09 검토의 두 재현(소스를 고치지 않고 새 출력 디렉터리로) | `repro_candidate_starvation` 141개, `repro_stale_join_retry` 15개, 실패 0. 재현 소스의 SHA-256이 실행 전후 같다 |
| Linux API (Mac host) | 빌드 경고 0, 단위 test **1,539개 검사**(작업 전 1,399. 더한 것 140) 다섯 번 연속 실패 0, ctest 3/3 |
| Linux ingest | 빌드 경고 0, ctest 8/8 (`ingest_power_base_block_test` 44개 검사, 더한 것 9) |
| Linux core (Matter 쪽) | 빌드 경고 0, 194개 검사(더한 것 11), ctest 1/1 |
| Linux 검증 묶음의 스크립트 회귀 (가짜 OS, Mac. 풀어 놓은 묶음에서) | `linux_verify.sh`: 44개 검사, 실패 0. `linux_deploy.sh`·`linux_hub_checkpoint.sh`: 324개 검사 — 시뮬레이터 test 네 묶음과 동시에 돌린 첫 실행은 14개 실패(자기 시간 제한에 걸려 종료된 경우와 그 뒤따른 검사), **단독으로 다시 돌린 실행은 실패 0**. 두 실행 사이에 묶음은 바뀌지 않았다. `~/esp-builds/pogo_forget_hwtest_20261010/logs/linux_bundle_checks/` |

관찰: 변형 실행 도중(다른 빌드와 동시에 돌던 때) 기존 `test_otbr_helper.cpp`의 검사 하나("an already active
Commissioner is left as it is")가 한 번 실패했다. 이번 변경과 무관한 subprocess 시간 제한 test이고, 단독으로 돌린
위의 다섯 번과 그 전의 열두 번에서는 실패하지 않았다. 원인은 조사하지 않았다.

### 하지 않은 것

- **실물 전부**: 실제 보드·Thread·OTBR, 운영 허브, 실제 iPhone, 실제 Matter controller에서의 endpoint 제거.
- **Linux**: `pogo_matter_bridge`의 GN/CHIP 빌드와 실행. Mac에서는 할 수 없다. 허브에서 실행할 소스 묶음
  `pogo_linux_verify_590e2a11d2cb_02727ebba79b_prep4`와 그 절차를 준비물에 두었다. `linux_verify.sh`가 그 허브에서
  통과하기 전에는 배포 후보가 없다.
- 펌웨어 빌드: 보드 소스가 바뀌지 않아 새 이미지를 만들지 않았다.
- 허브 재시작 뒤 한 번도 다시 들리지 않은 Base를 제거할 때의 방 지정 정리: 하지 못하는 것이 설계의 한계다(13절
  "하지 않는 것"). (2026-10-10에 덧붙임: 0.4.1이 상태 파일의 `members`로 이 정리를 하게 됐다. 아래
  "목록 제거 후속 수정".)

### 이전 실물 결과와 이 변경 (2026-10-10에 덧붙임)

아래는 2026-10-10 작업 지시가 전한 **사용자의 실물 보고**다. 이 저장소의 작업이 보드·허브에 접근하거나 로그를 본
것이 아니고, 모두 **그때 설치돼 있던 빌드**(Power Base 0.3.0·module 0.1.1·Linux bridge 0.3.0의 2026-10-09 후보)의
결과이며 이 절의 0.4.0과 앱 1.3.0의 결과가 아니다.

| 항목 | 보고 |
|---|---|
| 주소 복구(L): port 전환, 복귀, 그 사이 앱 정상 동작 | 확인했다는 보고 |
| P5 | 통과 보고 |
| P6 | 사용자 선택으로 생략 |
| P4 | 자동 복구는 확인. PMS 표시가 간헐적으로 사라지거나 늦게 돌아오는 것은 **보류**(이번 작업에서 타임아웃·재시도·전원 동작을 바꾸지 않았다) |
| L5, 실제 주소(EID) 변경, 구버전과의 혼합 | 실행하지 않음. 통과가 아니다 |
| 재부팅 | RST 재부팅은 완전한 전원 차단 시험이 아니다 |
| 이번 0.4.0 허브·앱 1.3.0·목록 제거 | **실물 미검증** |
| (2026-10-10에 덧붙임) 0.4.1 허브·앱 1.3.1 | **실물 미검증.** 0.4.0은 설치하지 않는다: 독립 검토가 재현한 두 결함이 0.4.1에서 고쳐졌다(아래 절) |

## 목록 제거 후속 수정 (2026-10-10, Linux bridge 0.4.1)

"허브 목록에서 제거"(위 절, 0.4.0)의 독립 검토에서 실제 코드로 재현된 두 결함의 수정과, 0.4.0이 남겨 둔 것의 정리다
([`power-base-management.md`](power-base-management.md) 14절). Linux bridge만 바뀌었고(`linux_bridge` 0.4.1), 보드
펌웨어와 관리·BLE 규약은 그대로다. 앱 쪽 기록은 앱 저장소의 `docs/validation-status.md` "목록 제거 후속 수정
(2026-10-10, `1.3.1 (14)`)"에 있다.

**이 절의 어느 것도 실물이 아니고, Linux에서 실행한 것도 아니다.** 보드에 접근하지 않았고, Linux 허브에 접속하지
않았고, Linux에서 빌드하지 않았다. 아래의 경쟁 순서는 Mac host test에서 **결정적으로 주입한 것**이고, 실제 두 기기나
실제 네트워크 타이밍으로 만든 것이 아니다.

### 기준

| | |
|---|---|
| 저장소 | `feature/unified-module-role-manager`, `590e2a11d2cb93f1cbc2b2b0eb094d50ea289a50`, 커밋하지 않은 작업 트리. 시작 시 status 151줄(미추적 71), stage 0, stash 0, index SHA-256 `b53e4f1b…bf1d0`: 작업 지시가 적은 값과 같았고, 0.4.0 작업이 끝났을 때와 파일 하나 다르지 않았다 |
| 버전 | `linux_bridge` 0.4.0 → **0.4.1**. `power_base` 0.3.0, `module` 0.1.1, `rcp` 0.1.0 그대로 |
| 작업 전 지문 | `linux_bridge` `02727ebba79bb284769dcc36733a6716348bb8361a59dbb9f1d7d327bd04ac57` (0.4.0) |
| 작업 후 지문 | `linux_bridge` `2edef654f4514662ccb42db2aff8ce0aa8ce658608d4d0342cd79308d5671954`(build id `590e2a11d2cb+dirty.2edef654f451`). `power_base`·`module`·`rcp`의 지문도 달라졌다: 소스는 같고 `versions.json`이 그들의 빌드 입력이기 때문이다(0.4.0 때와 같다). 2026-10-09의 펌웨어 산출물은 자기 manifest로는 그대로이고 `--match-current-tree`만 통과하지 않는다 |
| 형식·번호 | `tools/pogo_version.py compat`의 출력이 작업 전과 같다. HTTP API는 선택 필드 하나(`list_removed`), 상태 파일은 schema 2에 선택 목록 `members`와 정리 항목의 선택 필드 `rooms`([`versioning.md`](versioning.md) "호환성 정책") |
| 출력 | `~/esp-builds/pogoforget2_20261010_*`(빌드·결과), `~/esp-builds/pogo_forget2_hwtest_20261010`(실물 시험 준비물). 0.4.0의 `pogoforget_20261010_*`와 `pogo_forget_hwtest_20261010`, 검토의 재현 디렉터리는 바꾸지 않았다: 시작 때의 165개 파일이 내용도 수정 시각도 그대로다. 새로 생긴 것은 검토의 원본 스크립트를 그대로 두 번 돌려서 그 스크립트가 자기 디렉터리에 만든 `run-ysauvxzz/`와 `run-zune3mts/`뿐이다 |

### 검토가 남긴 재현: 수정 전 실패 → 수정 후 통과

검토의 원본(`pogo-forget-review-20261010/`의 `repro_hub.cpp`, `repro_evidence.swift`, `run_review.py`)은 고치지 않았다.
SHA-256이 실행 전과 모든 실행 뒤에 같다(`repro/review_sources_sha256.*.txt`).

| 실행 | hub_build | evidence_build | snapshots | evidence | rooms | 기록 |
|---|---|---|---|---|---|---|
| 원본 그대로, **수정 전** 트리 | 0 | 0 | 0 | **1** | **1** | `repro/original_before_fix/` (검토의 `run-ysauvxzz/`의 사본). 스크립트 종료 코드 1 |
| 원본 그대로, 수정 후 트리 | **1 (빌드되지 않음)** | — | — | — | — | `repro/original_after_hub_fix.stdout.txt` (검토의 `run-zune3mts/`). 스크립트 종료 코드 2. **통과로 세지 않는다** |
| adapter, 수정 후 트리 (최종) | 0 | 0 | 0 | **0** | **0** | `repro/adapter_final.stdout.txt`, `repro/run-bo3jgfos/` (수정 직후의 첫 실행: `repro/run-u9xm5nw6/`) |
| adapter, 방 정리의 두 보호를 뺀 **사본** | 0 | 0 | 0 | 0 | **1** | `repro/adapter_on_mutated_tree.txt`, `repro/run-vbg5ckka/`. adapter가 결함을 여전히 잡는다는 대조 |

- **원본이 수정 후 빌드되지 않는 이유.** 원본의 방 정리 재현은 훅 `unassign_rooms`에 "모듈 UUID 목록만 받는 함수"를
  넣고, 그 안에서 `MakeRoomUnassigner()`가 준 함수를 UUID 목록만으로 부른다. 그 형식 — **UUID만으로 방을 지우는
  훅** — 이 이번 수정에서 없어졌다(대상과 "아직 이 정리의 것인가"라는 질문을 함께 받는다). 오류는 그 두 줄
  (`repro_hub.cpp:65`, `:72`)에서만 난다.
- **adapter** (`repro/adapter/`): `repro_hub_adapter.cpp`는 원본에서 그 **두 줄만** 바꾼 것이다(훅이 질문을 받아
  그대로 넘긴다. 경쟁 순서를 주입하는 부분은 한 글자도 다르지 않다: `repro_hub_adapter.diff`).
  `run_review_adapter.py`가 원본과 다른 곳(`run_review_adapter.diff`): 허브 소스로 adapter를 쓰고,
  `repro_evidence.swift`는 **원본 파일을 그대로** 쓰고, 의존 라이브러리는 0.4.0의 host 빌드 대신 지금 트리의 host
  빌드를 쓰고, 결과를 검토의 디렉터리가 아니라 이 작업의 결과 디렉터리에 쓰고, 시험할 트리와 라이브러리를 환경
  변수(`POGO_REVIEW_FW`, `POGO_REVIEW_DEPS`)로 바꿀 수 있다(보호를 뺀 사본에 같은 실행을 돌리기 위한 것이고,
  기본값은 저장소다).
- **두 수정은 각각 혼자서도 첫 재현을 통과시킨다** (`repro/evidence_matrix.txt`): 수정 전 허브가 만든 섞인
  snapshot을 수정 후 앱의 판정에 넣으면 통과(그 파일의 1번, 종료 코드 0), 수정 후 허브가 만든 snapshot을 수정 전 앱의
  판정에 넣으면 통과(2번, 종료 코드 0. 수정 후 허브의 "mixed" snapshot에는 옛 Base가 없다), 둘 다 수정 전이면 실패(3번,
  종료 코드 1. 대조).
- **adapter가 결함을 잡는지**: 저장소의 사본에서 `settings_store.cpp`의 두 보호(쓰기 직전의 질문, 방 결속)를 뺀 뒤
  같은 adapter를 그 사본에 돌리면 `rooms`가 원본의 수정 전 실행과 같은 문장으로 실패한다("new room preserved: NO",
  `repro/adapter_on_mutated_tree.txt`). 그 사본에서도 `evidence`는 통과한다: 앱이 수정된 것이기 때문이다.
- 원본의 SHA-256은 마지막 실행 뒤에도 시작 때와 같다(`repro/review_sources_sha256.final.txt`).

### 무엇으로 시험했나

실제 production 코드다: `PowerBaseManagement`, `ingest::Registry`, `SettingsStore`와 `UnassignModuleRooms()`·
`MakeRoomUnassigner()`·`MakeRoomReader()`, HTTP 서버(`Server`, TLS loopback, 실제 `DeviceAuth` 페어링). **주입한 것**:
시각, "들린 지 얼마나 됐나"의 기준 시각, 상태 파일과 설정 파일의 쓰기 실패, 그리고 두 군데의 순서 —
"Registry를 복사한 직후"(`ServerConfig::after_registry_copy`, test 전용)와 "설정을 읽은 뒤 쓰기 전"
(`UnassignModuleRooms()`의 `before_write`, 기존의 test 전용 자리).

| 요구한 경우 | 어디서 | 본 것 |
|---|---|---|
| GET의 실제 순서: Registry 복사 → (다른 요청의 목록 제거) → 관리 기록 읽기 | `SnapshotOrderTests` (실제 HTTP GET, 제거도 실제 `POST`) | 그 GET의 응답에 목록 제거 기록이 있고, 그 Base와 모듈은 **없다**. 다음 GET과 Base·모듈·작업·기록이 같다 |
| SSE의 첫 event와 그 뒤 event | `SnapshotOrderTests` (실제 `/api/v1/events`) | 첫 event, 그리고 관계없는 변화로 깨어난 loop의 event 모두: Base와 그 기록이 함께 나오는 event가 없다 |
| 삭제(release)의 같은 순서 | `SnapshotOrderTests`, `SnapshotDescription` | `released`(통지) / `completed`(통지 없이 영수증)와 "아직 목록에 있는 Base"가 함께 나오지 않는다. 복사가 "release 자신이 본 사본과 같은 generation"일 때도 |
| 복사 뒤에 제거 **그리고 재등록** | `SnapshotDescription` | 새 등록의 `record`로 Base가 관리 대상에 있고, 이전 등록의 모듈은 그 아래 보이지 않는다 |
| 관리 쪽 변화가 없었을 때 | `SnapshotDescription`, ingest `test_registry_snapshot` | 호출자의 사본을 그대로 쓴다(Registry가 스스로 배운 것은 다음 snapshot). 이미 본 사본보다 오래된 사본은 다시 뜬다 |
| 방 정리: 검토의 순서(판단 뒤·쓰기 전에 다른 Base에 붙고 새 방) | `RoomCleanupBinding` (실제 설정 저장소) | 옮겨 간 모듈은 새 방 유지, 남은 모듈은 방 해제. 이름·방 목록·집 이름·다른 모듈 그대로. 방을 모르는 정리(0.4.0이 적은 것)에서도 |
| 반대 순서(정리 먼저, 새 방 나중) | `RoomCleanupBinding` | 새 방 유지, 뒤따르는 정리 없음 |
| 읽기와 쓰기 사이의 저장: revision conflict → 재시도 | `RoomCleanupBinding`, 기존 `SettingsCleanupDurability` | 첫 쓰기 conflict, 둘째 시도가 다시 묻고 새 방을 보존. 관계없는 저장이 네 번 이기면 실패로 답하고 나중에 다시 |
| 옮겨 갔지만 방은 그대로인 모듈 | `RoomCleanupBinding` | 방 유지(질문이 막는다), 상태 파일에 새 Base의 member로 |
| 그 뒤에 받은 방(모듈은 어느 Base에도 없음) | `RoomCleanupBinding` | 방 유지(방이 다르다) |
| 자동 삭제(release)의 정리 | `RoomCleanupBinding` | 같은 두 순서에서 같은 결과 |
| 같은 Base의 새 등록 | `RoomCleanupBinding`, 기존 `SettingsCleanupDurability` | 새 등록 아래 보고된 모듈의 방 유지 |
| 정리의 재실행 | `RoomCleanupBinding` | 끝난 정리는 다시 묻지도 쓰지도 않는다(설정 revision 그대로) |
| 방이 없던 모듈 / 설정을 읽을 수 없던 순간 | `RoomCleanupBinding` | 정리 항목 없음 / 방을 모르는 채 적고, 수리 뒤 질문만으로 보호 |
| 재시작 뒤 다시 보고하지 않은 Base | `MembersAcrossRestart` | 목록 제거가 그 Base의 모듈 수를 알고, 시작 유예 뒤 방을 해제한다. 삭제(release)도 같다 |
| 재시작 전에 옮겨 간 모듈 / 유예 중에 나타난 모듈 / detach된 모듈 | `MembersAcrossRestart` | 셋 다 정리 대상이 아니고 방이 유지된다 |
| 이전 버전의 파일, 적지 못한 채 끝난 프로세스 | `MembersAcrossRestart`, `tools/xver` | 목록 제거는 되고(`affected_module_count` 0) **방은 지우지 않는다** |
| 저장 실패 | `MembersAcrossRestart` | 파일 그대로, 한 번 시도 뒤 간격을 두 배씩 늘려 재시도, 그동안의 목록 제거는 거절되고 아무것도 바뀌지 않음. 디스크가 돌아오면 적힌다 |
| 형식이 틀린 `members`·`rooms` | `MembersAcrossRestart` | 일곱 가지 파일: 관리 기능이 시작되지 않는다 |
| 상태가 파일 한도를 넘게 되는 쓰기 | `MembersAcrossRestart` | 그 요청은 저장 실패로 거절, 파일은 그대로이고 재시작이 읽는다 |
| `sent`/`accepted` 작업 + 목록 제거 | `ListRemovalAndRemovalJobs` | 작업은 `sent` 그대로, `list_removed`, 키 유지, `completed` 아님. `accepted`에서도 표시, `released`부터는 없음. 재등록 → `superseded`, 키 삭제, **새 등록에 새 삭제 작업이 만들어진다** |
| 자리를 비웠던 기기 | `SnapshotDescription` | 다섯 시간 뒤와 재시작 뒤에도 그 Base의 기록이 snapshot에 있고, 다시 등록되면 사라진다. 두 번 제거한 Base는 마지막 기록만 |

### 검사가 실제로 잡는지

`tools/mutate_hub.py`: 저장소의 사본에서 보호 조건을 하나씩 뺀 변형으로 같은 test를 돌렸다. 0.4.0의 19가지(바뀐
코드에 맞춰 옮김)와 이번의 29가지, 모두 48가지. **마지막 실행(단독)에서 48가지가 모두 잡혔고, 변형마다 처음 실패한 검사가 뺀 규칙에
대한 것이다** (`logs/mutations_hub.txt`).

이번의 변형: 오래된 Registry 사본을 그대로 씀 / 이미 본 사본보다 오래된 사본을 씀 / 목록 제거·release가 "목록이
바뀌었다"를 적지 않음 / 사본에 generation이 없음 / 쓰기 직전의 질문을 하지 않음 / 방 결속을 무시 / 질문이 예전의
Registry로 답함 / 정리 pass가 모듈의 위치를 묻지 않음 / 떠날 때 방을 읽지 않음 / 다시 등록된 모듈이 정리에 남음 /
`rooms`를 저장하지 않음 / `members`를 저장하지 않음·쓰지 않음 / 파일의 members를 유예 없이 정리 / 모듈이 옛 Base의
member로 남음 / detach된 모듈이 member로 남음 / 떠난 Base의 members가 남음 / 차단된 Base의 members를 읽어 들임 /
실패 직후 곧바로 다시 씀 / 두 Base에 든 모듈·아무 문자열의 방을 받아들임 / 한도를 넘는 상태를 씀 / 남은 작업에
표시가 없음 / 차단된 모든 작업에 표시 / Base가 목록 밖에 있는데 기록을 지움·내림 / 다시 등록된 뒤에도 기록을 남김 /
한 Base의 모든 기록을 남김.

세 번 돌렸다. 첫 실행(`logs/mutations_hub.run1.txt`, 44가지)에서 **하나가 잡히지 않았다**: "release가 목록 변화를 적지 않음". 그때의
test는 Registry 사본을 release보다 한참 먼저 떴기 때문에 "이미 본 사본보다 오래됨" 쪽으로 걸러졌다. 사본이 release
자신이 본 사본과 **같은 generation**인 경우의 test를 더했고(통지로, 영수증으로, 실제 GET으로), 그 변형은 잡힌다.
그 전에 변형으로 쓸모없음이 드러날 코드 두 군데를 지웠다(새 등록의 active에서 목록 변화를 적던 것, 재시작 때
자격증명 없는 Base의 members를 버리던 것).

둘째 실행(`logs/mutations_hub.run2_under_load.txt`, 48가지. 앱의 변형 실행과 **동시에** 돌았다)에서도 **하나가 잡히지
않았다**: "Base가 목록 밖에 있는데 기록을 지움". 그때의 test에서는 기록을 정리하는 쓰기가 일어나지 않았다. 차단된
동안 정리가 실제로 도는 쓰기를 넣고, 한 Base의 기록이 둘인 경우를 더했다. 같은 실행에서 "잡힘"으로 나온 둘은 처음
실패한 검사가 뺀 규칙과 무관한 시간 제한 검사였다(부하). 그래서 셋째 실행은 다른 작업 없이 단독으로 돌렸고, 위의
결과가 그것이다. 앞의 두 실행의 출력은 지우지 않았다(`mutations_hub_run1/`, `mutations_hub_recheck/`,
`mutations_hub_run2_under_load/`).

### 이전 버전과의 파일 호환 (세 버전의 실제 코드)

`tools/xver/xver.cpp`를 0.3.0 트리, 0.4.0 트리(이 작업의 수정 전 사본), 0.4.1 트리에 각각 빌드해 돌렸다
(`logs/xver.summary.txt`, 전체 출력 `logs/xver.txt`).

| 쓴 쪽 → 읽은 쪽 | 결과 |
|---|---|
| 0.4.1 → 0.4.0 | 읽힌다(`members`·`rooms` 무시). 차단·작업 그대로. 남은 정리를 **모듈 id만으로** 실행한다. 재시작 뒤 보고하지 않은 Base의 목록 제거: 모듈 0개. 0.4.0이 파일을 다시 쓰면 `members`와 `rooms`가 **없어진다** |
| 0.4.1 → 0.3.0 | 읽힌다. 위와 같고 `forgets`도 없어진다. `sent` 작업을 남긴 Base는 0.3.0에서 managed로 다시 보인다(0.4.0 때와 같다) |
| 0.4.1 → 0.4.1 | 남은 정리를 "그 방에 묶어, 아직 이 정리의 것일 때만" 실행. 재시작 뒤 보고하지 않은 Base의 목록 제거: 모듈 2개를 알고 정리한다. 남긴 작업에 `list_removed` |
| 0.4.0 → 0.4.1 | 그대로 읽힌다. 남은 정리는 "방을 모름, 아직 이 정리의 것일 때만". 보고하지 않은 Base: 모듈 0개, **정리하지 않는다**. 남긴 작업에 `list_removed`(차단에서 나온다) |
| 0.3.0 → 0.4.1 | 그대로 읽힌다 |
| 0.4.1 → 0.4.0(다시 씀) → 0.4.1 | 읽힌다. `members`는 비어서 시작한다 |

### 전체 회귀 (최종 트리, `final_checks.txt`)

| | 결과 |
|---|---|
| `tools/test_*.c` 전체 | 71개 프로그램, 실패 0 |
| `tools/test_*.py` (ESP-IDF 환경) | 15개, 실패 0 |
| 실제 OpenThread core의 재현 | `test_power_base_locate_openthread.sh` 39개 통과, `test_power_base_release_session_openthread.sh` `PASS: both reproductions` |
| 2026-10-09 검토의 두 재현(소스를 고치지 않고 새 출력 디렉터리로) | `repro_candidate_starvation` 141개, `repro_stale_join_retry` 15개, 실패 0. 재현 소스의 SHA-256이 실행 전후 같다 |
| Linux API (Mac host) | 빌드 경고 0, 단위 test **1,694개 검사**(0.4.0은 1,539. 더한 것 155) 다섯 번 연속 실패 0, ctest 3/3 |
| Linux ingest | 빌드 경고 0, ctest 8/8 (`ingest_registry_snapshot_test`에 사본의 generation 검사를 더했다) |
| Linux core (Matter 쪽) | 빌드 경고 0, 194개 검사, ctest 1/1 (바뀐 것 없음) |
| `tools/pogo_version.py check` / `compat` | `ok (linux_bridge 0.4.1, module 0.1.1, power_base 0.3.0, rcp 0.1.0)` / 출력이 작업 전 트리의 것과 같다(`logs/compat_before.json`, `logs/compat_after.json`) |
| `app/chip/main.cpp`의 바뀐 한 줄 | Mac에서는 그 파일을 빌드할 수 없다. 관리를 설정 저장소에 잇는 두 식만 같은 헤더로 문법 검사했다(`tools/main_wiring_check.cpp`, `-fsyntax-only`, 종료 코드 0). `main.cpp` 자체의 컴파일은 허브의 `linux_verify.sh`가 한다 |
| Linux 검증 묶음 (풀어 놓은 사본에서, 가짜 OS, Mac) | tar 체크섬 일치, 묶음 manifest 369개 파일 일치, 묶음 안 소스의 식별이 `0.4.1`·`590e2a11d2cb+dirty.2edef654f451`·`source-info-verified`. `linux_verify.sh` 회귀: 44개 검사, 실패 0. `linux_deploy.sh`·`linux_hub_checkpoint.sh` 회귀: **333개 검사, 실패 0**(prep4의 324개 + `members`를 세고 사라짐을 알리는 검사 9개. 다른 작업 없이 단독으로 돌렸고 첫 실행에서 통과). `~/esp-builds/pogo_forget2_hwtest_20261010/logs/linux_bundle_checks/` |
| 묶음·준비물의 민감 정보 검사 (`tools/scan_sensitive.py`) | 개인 키·기기 식별자·Matter 설정 payload·Thread dataset·이름 붙은 비밀 값의 형태를 찾았다. 걸린 것은 Git commit 해시, test의 가짜 방 id와 가짜 기기 id, 회귀 test가 쓰는 `FAKEPRIVATEKEYMATERIAL…`, 아이콘 파일 이름뿐이다(`logs/scan_sensitive.*.txt`) |

위 표의 첫 일곱 줄은 `final_checks.sh` 한 번의 결과이고(종료 코드 0), 변형 실행 뒤·앱 test 앞에 **단독으로** 돌렸다. 앱의
변형 실행과 동시에 시작했던 앞선 두 번은 load average가 400을 넘어 일부러 중단했고, 결과로 쓰지 않았다(`superseded/`).

### 하지 않은 것

- **실물 전부**: 실제 보드·Thread·OTBR, 운영 허브, 실제 iPhone, 실제 Matter controller.
- **Linux**: `pogo_matter_bridge`의 GN/CHIP 빌드와 실행. Mac에서는 할 수 없다. 허브에서 실행할 소스 묶음
  `pogo_linux_verify_590e2a11d2cb_2edef654f451_prep5`과 그 절차를 준비물에 두었다. `linux_verify.sh`가 그 허브에서 통과하기 전에는 배포 후보가 없다.
- 실제 재시작·전원 차단·디스크 고장: 재시작은 "같은 디렉터리 위의 새 manager와 새 Registry", 저장 실패는 주입한 것이다.
- 실제 두 기기의 네트워크 타이밍으로 만든 경쟁: 순서는 주입했다.
- 펌웨어 빌드: 보드 소스가 바뀌지 않아 새 이미지를 만들지 않았다.
- 끝난 삭제 작업(`completed`)의 기록이 10분 뒤 사라지는 것(자리를 비운 기기의 "복구 중"): 이번 범위에서 고치지 않았다
  (14절 "하지 않는 것, 남은 것"). (2026-10-11에 덧붙임: 0.4.2가 작업 이력과 별개로 "목록 밖의 Base"를 snapshot에 싣게
  됐다. 아래 "목록 제거 후속 수정 2".)
- (2026-10-11에 덧붙임) 위 "방 지정 정리의 결속"이 가리지 못한 순서가 독립 검토에서 재현됐다: 정리 대상 확인과 설정
  쓰기 **사이에** 모듈이 다른 Base로 옮겨 가고 아무도 설정을 저장하지 않은 경우, 그 모듈이 그대로 갖고 있던 방이
  지워졌다. 이 절이 그것을 "구별할 수 없는 순서"로 남긴 것은 틀린 판단이었다. 0.4.2에서 고쳤다(아래 절).

## 목록 제거 후속 수정 2 (2026-10-10, Linux bridge 0.4.2)

위 절(0.4.1)을 다시 독립 검토한 결과 남은 결함 둘의 수정이다([`power-base-management.md`](power-base-management.md)
15절). Linux bridge만 바뀌었고(`linux_bridge` 0.4.2), 보드 펌웨어와 관리·BLE 규약은 그대로다. 앱 쪽 기록은 앱 저장소의
`docs/validation-status.md` "목록 제거 후속 수정 2 (2026-10-10, `1.3.2 (15)`)"에 있다.

**이 절의 어느 것도 실물이 아니고, Linux에서 실행한 것도 아니다.** 보드에 접근하지 않았고, Linux 허브에 접속하지
않았고, Linux에서 빌드하지 않았다. 아래의 순서는 Mac host test에서 **결정적으로 주입한 것**이고, "16분 뒤"는 test가
관리의 시계를 옮긴 것이다. 실제 두 기기, 실제 네트워크 타이밍, 실제 대기로 만든 것이 아니다.

### 기준

| | |
|---|---|
| 저장소 | `feature/unified-module-role-manager`, `590e2a11d2cb93f1cbc2b2b0eb094d50ea289a50`, 커밋하지 않은 작업 트리. 시작 시 status 153줄(미추적 72), stage 0, stash 0, index SHA-256 `b53e4f1b…bf1d0`: 작업 지시가 적은 값과 같았고, 0.4.1 작업이 끝났을 때와 파일 하나 다르지 않았다 |
| 버전 | `linux_bridge` 0.4.1 → **0.4.2**. `power_base` 0.3.0, `module` 0.1.1, `rcp` 0.1.0 그대로 |
| 작업 전 지문 | `linux_bridge` `2edef654f4514662ccb42db2aff8ce0aa8ce658608d4d0342cd79308d5671954` (0.4.1) |
| 작업 후 지문 | `linux_bridge` `b70061c4bae7d597f58d6b69d60794d3b8d989ed70b1c6fb5848b87571546f55`(build id `590e2a11d2cb+dirty.b70061c4bae7`). `power_base`·`module`·`rcp`의 지문도 달라졌다: 소스는 같고 `versions.json`이 그들의 빌드 입력이기 때문이다. 2026-10-09의 펌웨어 산출물은 자기 manifest로는 그대로이고 `--match-current-tree`만 통과하지 않는다 |
| 형식·번호 | `tools/pogo_version.py compat`의 출력이 작업 전과 같다. HTTP API는 snapshot의 최상위 배열 하나(`power_base_departures`), 상태 파일은 schema 2에 차단 항목의 선택 키 둘(`reason`, `record`) |
| 출력 | `~/esp-builds/pogoforget3_20261010_*`(빌드·결과), `~/esp-builds/pogo_forget3_hwtest_20261010`(실물 시험 준비물). 0.4.1의 `pogoforget2_20261010_*`·`pogo_forget2_hwtest_20261010`과 그 전의 것들은 바꾸지 않았다. 두 검토의 재현 디렉터리도 원래 파일은 그대로다(검토의 실행 도구가 자기 디렉터리에 새 실행 디렉터리를 만든다: 아래) |

### 검토가 남긴 재현: 수정 전 실패 → 수정 후 통과

두 번째 검토의 재현(`pogo-forget2-review-20261010/`의 `repro_guard_commit_gap.cpp`, `run_guard_gap.py`)은 고치지
않았다. SHA-256이 시작 때와 모든 실행 뒤에 같다(`repro/review2_sources_sha256.*.txt`). 그 프로그램은 실제
`PowerBaseManagement`·`Registry`·`SettingsStore`·`UnassignModuleRooms()`를 쓰고, `UnassignModuleRooms()`의 test 전용
자리(`before_write`)에서 "모듈이 다른 Base 아래 목록에 오른다(방은 바꾸지 않는다)"를 끼워 넣는다.

| 실행 | 빌드 | 결함 검사 | 출력 | 기록 |
|---|---|---|---|---|
| 원본 그대로, **수정 전** 트리 | 0 | **1** | `existing room preserved after move: NO` | `repro/guard_gap_original_before_fix/`, `repro/guard_gap_original_before_fix.stdout.txt` |
| 원본 그대로, 수정 후 트리(최종) | 0 | **0** | `existing room preserved after move: yes` | `repro/guard_gap_original_after_fix.stdout.txt` |
| 원본 C++ + adapter 실행 도구(이 트리의 라이브러리) | 0 | **0** | `… yes` | `repro/guard_gap_adapter_after_fix.stdout.txt` |
| 대조: 같은 것, **사본**에서 "구간이 Registry를 보지 않고 판단"(이동 전에 본 것으로 판단: 0.4.1의 순서) | 0 | **1** | `… NO` | `repro/guard_adapter_on_mutated_tree.section_decides_without_looking.txt` |
| 대조: 같은 것, **사본**에서 "설정 쓰기가 구간을 지나지 않음" | 0 | **1** | `… NO` | `repro/guard_adapter_on_mutated_tree.settings_write_bypasses_the_section.txt` |
| 대조로 **쓰지 못한** 실행: **사본**에서 "구간이 받은 것을 그대로 씀" | 0 | 0 | `… yes` | `repro/guard_adapter_on_mutated_tree.txt` |

- **대조 실행(보호를 뺀 사본에서 재현이 다시 실패하는가).** 처음 고른 변형("구간이 받은 것을 그대로 씀")에서는 재현이
  **통과했다.** 기대(실패)와 달랐고, 이유는 변형 쪽에 있다: 검토의 시나리오는 모듈 하나짜리 정리이고, 구간이 Registry를
  보는 순간 그 정리가 통째로 끝나서, 구간이 "받은 것 가운데 무엇을 쓸지"를 고르는 줄에 이르지 않는다. 그 변형은 이
  시나리오에서 결함을 되살리지 못하므로 그 실행은 재현에 대해 아무것도 말하지 않는다(그 변형 자체는 `mutate_hub.py`에서
  이 저장소의 검사 9개로 잡힌다: `logs/mutations_hub.txt`). 그래서 이 시나리오가 실제로 지나는 보호 둘을 각각 뺀 사본으로 다시 했고, 둘 다 재현이 다시
  실패한다. 첫 실행의 기록은 지우지 않았다.
- **이번에는 원본이 수정 후에도 그대로 빌드된다.** `UnassignModuleRooms()`와 훅의 형식을 소스 호환으로 두었다
  (`RoomCleanupGuard`가 함수에서 타입으로 바뀌었지만 원본은 그것을 받아서 넘기기만 한다). C++ 소스의 adapter는 없다.
- **adapter 실행 도구를 따로 둔 이유.** 원본 실행 도구는 이 트리의 소스 넷을 다시 컴파일하면서 나머지는 **0.4.1 작업의
  host 빌드**(`~/esp-builds/pogoforget2_20261010_linux_api_host`)에서 가져다 붙인다. 그 빌드는 보존 대상이라 다시 만들지
  않았고, 두 버전의 object가 섞인 실행 하나만으로 통과를 말하지 않으려고 같은 C++ 소스를 **이 트리의 host 빌드**에
  붙이는 실행 도구를 더했다(`repro/adapter_guard/run_guard_gap_adapter.py`와 그 diff: 라이브러리 경로, 트리 경로를 환경
  변수로 바꿀 수 있음, 결과 디렉터리). 두 실행의 결과가 같다.
- **첫 번째 검토의 재현 둘**(0.4.1 작업의 adapter. 사본이 그때의 것과 바이트까지 같다:
  `repro/adapter_sources_sha256.*.txt`)도 최종 트리에서 다시 돌렸다: hub_build·evidence_build·snapshots·evidence·rooms
  모두 0(`repro/adapter_final.stdout.txt`). 두 번째 검토가 남긴 실행 도구(`run_adapter_review.py`, 고치지 않음)로도 같다
  (`repro/reviewer_adapter_runner.stdout.txt`).
- 검토의 실행 도구들은 **자기 디렉터리에** 실행마다 새 디렉터리를 만든다. 이 작업이 그렇게 만든 것:
  두 번째 검토의 디렉터리에 `guard-run-_v5qpa8o`(수정 전, 결함 검사 1), `guard-run-fm3j26xt`·`guard-run-zbr3uwsm`(개발 중의
  수정 후 실행, 0), `guard-run-ikfvp973`(최종, 0), `run-j0piwqtz`(검토의 adapter 실행 도구, 다섯 단계 모두 0). 첫 번째
  검토의 디렉터리에는 아무것도 만들지 않았다. 그 밖의 파일은 시작 때와 같다
  (`logs/preservation_reviews_and_earlier_outputs.log`).

### 무엇으로 시험했나

실제 production 코드다: `PowerBaseManagement`, `ingest::Registry`(`hold()`와 listings 포함), `SettingsStore`와
`UnassignModuleRooms()`·`MakeRoomUnassigner()`·`MakeRoomReader()`, HTTP 서버(실제 GET과 SSE). **주입한 것**: 관리의 시각,
"들린 지 얼마나 됐나"의 기준 시각, 상태 파일과 설정 파일의 쓰기(실패, 그리고 한 검사에서는 느린 쓰기), 그리고 순서 —
"설정을 읽은 뒤, 쓰기의 구간에 들어가기 전"(`UnassignModuleRooms()`의 `before_write`).

**방 지정 정리 (`RoomCleanupWriteSection`, API test 28개 검사)**

| 요구한 경우 | 본 것 |
|---|---|
| 검토의 순서: 설정을 읽은 뒤·쓰기 전에 모듈이 다른 Base로 가고, 아무도 저장하지 않음 (목록 제거) | 그 모듈은 방을 그대로 갖고, 함께 떠난 다른 모듈은 같은 쓰기로 해제된다. 한 번의 시도(conflict 없이). 이름·방 목록·집 이름·무관한 모듈 그대로. 상태 파일에 그 모듈이 새 Base의 member로. 나중에 그 모듈이 다시 어느 Base에도 없게 돼도 옛 정리는 돌아오지 않는다 |
| 정리의 모든 모듈이 그 틈에 옮겨 감 (검토 재현의 모양) | 설정을 쓰지 않는다(revision 그대로). 둘 다 방 유지, 정리 종료 |
| 방을 모르는 정리(이전 허브가 적은 것) | 같은 결과 |
| **자동 삭제(release)** 의 정리 | 같은 결과. 작업은 `released` 그대로이고 정리만 끝난다 |
| 그 틈에 다른 Base에 붙었다가 **다시 떼어짐**(쓰기 때는 어느 Base에도 없다) | 방 유지: Registry가 "정리가 적힌 뒤 목록에 오른 적이 있다"를 기억한다 |
| pass 전에 붙었다 떼어짐(관리가 한 번도 보지 못함), 같은 Base의 새 등록 아래에서도 | 방 유지. 한 번도 돌아오지 않은 모듈은 해제 |
| **설정 충돌 + 이동**: 첫 시도는 무관한 저장으로 conflict, 둘째 시도 직전에 이동 | 둘째 시도도 구간을 지난다: 기기의 저장은 남고, 옮겨 간 모듈은 방 유지, 남은 모듈만 해제 |
| 쓰기가 진행 중일 때 도착한 보고 (두 스레드, 느린 설정 writer) | 쓰기가 끝날 때까지 **적용되지 않고**, 끝난 직후 적용된다. 결과는 "정리가 먼저, 목록에 오른 것이 나중"이라는 온전한 순서 |
| Registry의 기록 표가 넘침 | 관리가 본 적 없는 기록이 버려졌으면 방을 **지우지 않는다**. round마다 관리가 봤으면(snapshot을 읽는 기기) 넘쳐도 정리는 그대로 실행된다 |
| 보고·기기의 설정 저장·snapshot 읽기·정리 pass를 **동시에** 12 round | 교착 없음(30초 감시). 모듈 2는 해제, 모듈 1은 통째로 유지되거나 통째로 해제. "모듈 1이 새 Base에 오른 뒤에 시작된 설정 쓰기가 그 방을 지운" 일 0회 |
| 정리와 새 방 저장의 두 순서, revision conflict, 정리 재실행, 다른 Base로 옮긴 모듈 (0.4.1의 검사) | `RoomCleanupBinding`·`MembersAcrossRestart`·`SettingsCleanupDurability`가 그대로 통과한다. test의 가짜 훅도 이제 구간(`commit()`)을 지나 묻는다 |

Registry 쪽(ingest `ingest_registry_snapshot_test`, 검사 17개 추가): 기록이 생기는 때(첫 보고, 다른
Base 아래의 첫 보고, detach 뒤의 첫 보고)와 생기지 않는 때(반복·변경·heartbeat·차단된 Base), detach와 Base 제거 뒤에도
남는 것, 표의 상한과 "어디까지 버렸나", 지금 노드가 있는 모듈의 기록을 먼저 버리지 않는 것, `hold()` 동안 다른
스레드의 보고가 적용되지 않는 것.

**목록 밖에 있는 Base (`Departures` 49개 검사, HTTP `DepartureSnapshotTests` 18개 검사)**

| 요구한 경우 | 본 것 |
|---|---|
| 일반 삭제 → `completed` → 16분 뒤, 5시간 뒤 | 작업은 목록에도 파일에도 없다. snapshot은 그 Base를 목록 밖으로 말한다(끝난 등록의 `record`, `removed`). 다른 Base는 나오지 않는다 |
| **실제 GET과 SSE** (관리의 시계를 16분 옮김) | `"power_base_removals":[]`, `"power_base_departures":[{…"reason":"removed"…}]`, Base와 모듈 없음. SSE의 첫 event도 같다 |
| 마지막 Base / 여러 Base 중 하나 | 마지막: 허브가 아무것도 나열하지 않으면서 그 Base를 목록 밖으로 말한다. 여럿 중 하나: 남은 Base와 모듈·`record`는 그대로 |
| 재시작 | 빈 Registry에서도 같은 내용(차단과 같은 파일 쓰기에 들어 있다) |
| 삭제 후 재등록, 이후 허브 재시작 | 새 등록이 시작만 된 동안은 여전히 목록 밖. active가 되는 순간 사라지고 Base가 새 `record`로 목록에. 그 뒤 재시작: 빈 Registry, Base는 등록만으로 목록에, 목록 밖이라는 말 없음 |
| 목록 제거 → 재등록 → 일반 삭제 | 마지막에 끝난 등록이 `removed`로 나온다. 16분 뒤 예전 목록 제거 기록은 붙들리지 않는다(0.4.1은 붙들었다) |
| `released`에서 멈춘 삭제 | 그때부터 목록 밖. 작업은 `released` 그대로(완료로 바꾸지 않음), 키 유지 |
| `sent` 작업을 남긴 목록 제거, 나중의 영수증 | `forgotten`으로 나오고 작업은 `sent`·`list_removed`·키 유지. 영수증으로 `completed`가 돼도 이유는 처음 그대로, 차단 유지 |
| 저장 실패 | 통지를 저장하지 못하면 받아들이지 않는다: 차단도, 목록 밖이라는 말도 없고 Base는 그대로 목록에 있다 |
| 이전 상태 파일(차단에 `reason`·`record`가 없음) | 읽힌다. 두 Base 모두 목록 밖으로 나오되 등록과 이유는 **없이**(JSON에서 생략, null도 아님). 목록 제거 기록은 이전 규칙으로 유지. 이 허브가 다음에 쓰는 파일에도 지어낸 이유가 없다 |
| 형식이 틀린 차단(모르는 `reason`, 문자열이 아닌 `reason`, hex가 아닌 `record`) | 세 파일 모두: 관리 기능이 시작되지 않는다 |
| 수의 상한 | 차단마다 하나. 차단은 64개가 상한이다(기존 `BlockCapacity`) |
| 관리가 없는 허브 | snapshot에 그 필드가 없다 |

### 검사가 실제로 잡는지

`tools/mutate_hub.py`: 저장소의 사본에서 보호 조건을 하나씩 뺀 변형 69가지(0.4.1까지의 46가지를 지금 코드에
맞춰 옮긴 것과 이번의 23가지). **마지막 전체 실행에서 69가지가 모두 잡혔고, 변형마다 실패한 검사에
뺀 규칙에 대한 것이 들어 있다** (`logs/mutations_hub.txt`).

전체 실행은 두 번, 그 사이에 재확인 한 번이다. 첫 전체 실행(`logs/mutations_hub.run1.txt`)에서는 **세 가지가 잡히지
않았다**:

- "다시 목록에 오른 모듈이 정리에 남음"(0.4.1의 변형): 이번에 더한 기록 규칙이 test의 모든 경우를 덮어서, 14절의 규칙
  ("지금 목록에 있으면 빠진다")만이 가릴 수 있는 경우가 test에 없었다. 그 경우 — 관리가 파일에서 정리를 넘겨받기 **전에**
  이미 목록에 올라 있던 모듈 — 의 검사를 더했다.
- "차단된 Base의 모든 목록 제거 기록을 남김"(0.4.1의 변형): 차단이 등록을 말하게 되면서 그 규칙은 이유를 모르는
  차단(이전 파일)에서만 일한다. 이전 파일에 같은 Base의 기록이 둘인 경우를 더했다.
- "나중의 삭제 뒤에도 예전 목록 제거 기록을 붙듦"(이번의 변형): test가 재등록 전에 시각을 5시간 옮겨서 그 기록이 이미
  지워진 뒤였다. 기록이 아직 있는 동안 재등록하고 삭제하도록 고쳤다.

그 셋과 관련 변형 넷을 다시 돌려 모두 잡았다(`logs/mutations_hub.recheck.txt`). 그 재확인의 일곱 실행 가운데 둘에서 새
동시 실행 검사가 **뺀 규칙과 무관하게** 함께 실패했다: 검사의 마지막 단계가 정리의 재시도 시각을 기다리지 않아서,
설정을 저장하는 스레드가 네 번 연속 이기면 정리가 (정당하게) 미뤄진 채로 판정했다. 제품이 아니라 test의 결함이고, 그
단계를 고쳤다. 고친 뒤 그대로 세 번, CPU 부하(busy loop 12개) 아래 세 번 돌려 여섯 번 모두 1,793개 통과였다
(`logs/linux_api_unit_tests.dev10_*`, `logs/linux_api_unit_tests.underload_*`). 그 뒤 전체 변형 실행을 처음부터 다시 한
것이 위의 결과다. 첫 전체 실행에서도 그 검사가 두 변형의 실행에 섞여 실패했는데, 두 실행 모두 뺀 규칙에 대한 검사도
실패했다. 앞의 출력은 지우지 않았다(`mutations_hub_run1/`, `mutations_hub_recheck/`).

이번의 변형: 설정 쓰기가 구간을 지나지 않음 / 구간이 받은 것을 그대로 씀 / 구간이 Registry를 보지 않고 판단 / `hold()`가
읽는 쪽을 부르기 전에 잠금을 놓음 / 기록을 적지 않음 / 버린 기록을 말하지 않음 / "적힌 뒤 오른 적이 있다"를 무시 / 버려진
기록을 "오른 적 없음"으로 봄 / 버려진 기록을 언제나 의심 / 정리가 적힌 generation을 갖지 않음 / 파일에서 읽은 정리를 0으로
적음 / release·목록 제거의 차단이 등록을 말하지 않음 / release의 차단을 forgotten이라 함 / 목록 밖의 Base를 snapshot에 넣지
않음(관리, HTTP, JSON 세 곳) / 차단의 이유·등록을 저장하지 않음 / 읽을 때 검사하지 않음 / 나중의 삭제 뒤에도 예전 목록 제거
기록을 붙듦 / 모르는 값을 빈 문자열로 내보냄.

0.4.1의 변형 둘은 없앴다: 쓰기 **전의 질문**에 대한 것이고, 그 질문 자체를 없앴기 때문이다(구간이 판단한다. 15절).

### 이전 버전과의 파일 호환 (네 버전의 실제 코드)

`tools/xver/xver.cpp`를 0.3.0·0.4.0·0.4.1 트리(각 작업의 수정 전 사본)와 0.4.2 트리에 빌드해 돌렸다
(`logs/xver.txt`).

| 쓴 쪽 → 읽은 쪽 | 결과 |
|---|---|
| 0.4.2 → 0.4.1 | 읽힌다(차단의 `reason`·`record` 무시). 차단·작업·남은 정리 그대로이고 정리는 0.4.1의 방식으로 실행된다. 0.4.1이 파일을 다시 쓰면 두 키가 **없어진다** |
| 0.4.2 → 0.4.0 | 읽힌다. 남은 정리를 모듈 id만으로 실행한다. 다시 쓰면 두 키와 `members`·`rooms`가 없어진다(0.4.1 때와 같다) |
| 0.4.2 → 0.3.0 | 읽힌다. 다시 쓰면 `forgets`도 없어진다(0.4.1 때와 같다) |
| 0.4.2 → 0.4.2 | 차단 셋이 모두 이유와 등록을 갖고 읽힌다: 삭제된 Base는 `removed`, 목록에서 제거된 둘은 `forgotten`. 남은 정리는 "방에 묶어, 쓰기 때 아직 이 정리의 것일 때만". 재시작 뒤 보고하지 않은 Base의 목록 제거: 모듈 2개를 알고 정리한다 |
| 0.4.1 → 0.4.2 | 그대로 읽힌다. 예전 차단 셋은 **Base만** 목록 밖으로 나오고 이유와 등록은 없다(지어내지 않는다). 0.4.2가 그 뒤에 제거한 Base는 `forgotten`과 등록을 갖는다. 남은 정리와 `members`는 0.4.1이 적은 대로 쓰인다 |
| 0.4.0 → 0.4.2 | 그대로 읽힌다. 차단은 위와 같다. 남은 정리는 "방을 모름, 쓰기 때 아직 이 정리의 것일 때만". 보고하지 않은 Base: 모듈 0개, 정리하지 않는다 |
| 0.3.0 → 0.4.2 | 그대로 읽힌다. 차단 하나가 이유 없이 목록 밖으로 나온다 |
| 0.4.2 → 0.4.1(다시 씀) → 0.4.2 | 읽힌다. 차단 넷이 모두 이유·등록 없이 목록 밖으로 나온다(Base는 여전히 말해진다). `members`는 비어서 시작한다 |

곧 이유와 등록은 0.4.2가 **자기가 만든 차단에만** 적고, 이전 버전이 파일을 다시 쓰면 잃는다. 앱이 근거로 삼는 것은
"그 Base가 목록 밖에 있다"는 것 하나이므로(이유와 등록은 근거의 조건이 아니다) 그 경우에도 옛 센서는 사라진다.

### 전체 회귀 (최종 트리, `final_checks.txt`)

| | 결과 |
|---|---|
| `tools/test_*.c` 전체 | 71개 프로그램. **첫 실행에서 하나가 실패했다**: `test_linux_bridge_exporter_rate_limit`의 검사 하나("the blocker settles, then both … exports follow"). 이번에 바뀌지 않은 펌웨어 코드의 시간 기반 test다. 그 test만 다섯 번: 다섯 번 통과(`host_tests_rate_limit_rerun_[1-5]/`). 71개 전체를 다른 작업 없이 다시: 71개 통과(`host_tests_final_rerun/`). 원인은 더 조사하지 않았고, 타이밍이라는 것은 재실행의 통과에서 나온 판단이다. 첫 실행의 기록(`host_tests_final/`)은 그대로 있다 |
| `tools/test_*.py` (ESP-IDF 환경) | 15개, 실패 0 |
| 실제 OpenThread core의 재현 | `test_power_base_locate_openthread.sh` 39개 통과, `test_power_base_release_session_openthread.sh` `PASS: both reproductions` |
| 2026-10-09 검토의 두 재현(소스를 고치지 않고 새 출력 디렉터리로) | `repro_candidate_starvation` 141개, `repro_stale_join_retry` 15개, 실패 0. 재현 소스의 SHA-256이 실행 전후 같다 |
| Linux API (Mac host) | 빌드 경고 0, 단위 test **1,793개 검사**(0.4.1은 1,694. 더한 것 99) 다섯 번 연속 실패 0, ctest 3/3 |
| Linux ingest | 빌드 경고 0, ctest 8/8 (`ingest_registry_snapshot_test`에 검사 17개를 더했다) |
| Linux core (Matter 쪽) | 빌드 경고 0, 194개 검사, ctest 1/1 (바뀐 것 없음) |
| `tools/pogo_version.py check` / `compat` | `ok (linux_bridge 0.4.2, module 0.1.1, power_base 0.3.0, rcp 0.1.0)` / 출력이 작업 전 트리의 것과 같고 0.4.1 작업 때의 것과도 같다(`logs/compat_before.json`, `logs/compat_after.json`) |
| `app/chip/main.cpp`의 연결부 | 이번에 바뀌지 않았다. Mac에서는 그 파일을 빌드할 수 없어서, 관리를 설정 저장소에 잇는 두 식을 바뀐 헤더로 다시 문법 검사했다(`tools/main_wiring_check.cpp`, `-fsyntax-only -Wall -Wextra`, 종료 코드 0, 출력 없음). `main.cpp` 자체의 컴파일은 허브의 `linux_verify.sh`가 한다 |
| Linux 검증 묶음 (풀어 놓은 사본에서, 가짜 OS, Mac) | tar 체크섬 일치, 묶음 manifest 369개 파일 일치, 묶음 안 소스의 식별이 `0.4.2`·`590e2a11d2cb+dirty.b70061c4bae7`·`source-info-verified`. `linux_verify.sh` 회귀: 44개 검사, 실패 0. `linux_deploy.sh`·`linux_hub_checkpoint.sh` 회귀: 333개 검사, 실패 0(다른 작업 없이 단독으로 돌렸고 첫 실행에서 통과. 스크립트가 prep5와 다른 것은 기대하는 식별 — 버전, build id, 지문, 묶음 이름, 준비 revision — 과 안내문뿐이다: 두 묶음을 diff로 확인했다). `~/esp-builds/pogo_forget3_hwtest_20261010/logs/linux_bundle_checks/` |
| 묶음·준비물의 민감 정보 검사 (`tools/scan_sensitive.py`) | 개인 키·기기 식별자·Matter 설정 payload·Thread dataset·이름 붙은 비밀 값의 형태를 찾았다. 걸린 것은 Git commit 해시, test의 가짜 방 id와 가짜 기기 id, 회귀 test가 쓰는 `FAKEPRIVATEKEYMATERIAL…`, 아이콘 파일 이름뿐이고, 묶음에서 걸린 종류는 0.4.1의 묶음과 같다(`logs/scan_sensitive.*.txt`) |

위 표의 첫 일곱 줄은 `final_checks.sh` 한 번의 결과다(변형 실행 뒤·앱 test 앞에 단독으로 돌렸다. 그 스크립트의 종료 코드
0은 끝까지 돌았다는 뜻일 뿐이고, 그 안의 C test 하나는 위에 적은 대로 실패였다). 나머지는 앱 test가 모두 끝난 뒤의
마지막 pass(`logs/final_pass.txt`)에서 하나씩 돌렸다. 변형 1차 직후에 시작했다가 test를 강화하려고 1분쯤에서 중단한
회귀는 결과로 쓰지 않았다(`superseded/`).

### 하지 않은 것

- **실물 전부**: 실제 보드·Thread·OTBR, 운영 허브, 실제 iPhone, 실제 Matter controller.
- **Linux**: `pogo_matter_bridge`의 GN/CHIP 빌드와 실행. 허브에서 실행할 소스 묶음 `pogo_linux_verify_590e2a11d2cb_b70061c4bae7_prep6`과 그 절차를
  준비물에 두었다. `linux_verify.sh`가 그 허브에서 통과하기 전에는 배포 후보가 없다. `app/chip/main.cpp`는 이번에 바뀌지
  않았다(0.4.1의 한 줄 그대로. 그 두 식의 문법 검사만 다시 했다).
- 구간이 잡혀 있는 동안의 대기 시간: 실제 저장 장치에서 재지 않았다.
- 실제 대기: "16분 뒤"와 "5시간 뒤"는 관리의 시계를 옮긴 것이다.
- 실제 두 기기의 네트워크 타이밍으로 만든 경쟁: 순서는 주입했다. 두 스레드 검사는 부하가 크면 결함을 놓칠 수는 있어도
  거짓으로 실패하지는 않는다.
- 펌웨어 빌드: 보드 소스가 바뀌지 않아 새 이미지를 만들지 않았다.
- (2026-10-11에 덧붙임) 이 절의 버전을 다시 독립 검토한 결과 둘이 나왔다. 일반 삭제 요청이 Base의 식별값만 실어서,
  응답을 놓친 요청의 재전송이 **다시 등록된 Base의 새 등록**을 삭제했다(이 절의 검사는 그 순서를 지나지 않았다).
  그리고 오래된 응답이 새 상태를 되돌리는 표시 문제(앱 저장소가 한계로 적어 둔 것)가 실제 코드로 재현됐다. 0.5.0에서
  고쳤다(아래 절).

## 삭제 요청의 등록 결속과 snapshot 순서 (2026-10-11)

위 절(0.4.2)을 다시 독립 검토한 결과 나온 둘의 수정이다([`power-base-management.md`](power-base-management.md) 16절).
Linux bridge만 바뀌었고(`linux_bridge` 0.5.0), 보드 펌웨어와 관리·BLE 규약은 그대로다. 앱 쪽 기록은 앱 저장소의
`docs/validation-status.md` "삭제 요청의 등록 결속과 snapshot 순서 (2026-10-11, `1.3.3 (16)`)"에 있다.

**이 절의 어느 것도 실물이 아니고, Linux에서 실행한 것도 아니다.** 보드에 접근하지 않았고, Linux 허브에 접속하지
않았고, Linux에서 빌드하지 않았다. "실제 HTTP"는 Mac에서 빌드한 이 저장소의 서버 코드에 Mac의 loopback 소켓으로 보낸
요청이다. 보드의 메시지는 test가 허브가 발급한 자격증명으로 만든 것이다.

### 기준

| | |
|---|---|
| 저장소 | `feature/unified-module-role-manager`, `590e2a11d2cb93f1cbc2b2b0eb094d50ea289a50`, 커밋하지 않은 작업 트리. 시작 시 status 153줄(미추적 72), stage 0, stash 0, index SHA-256 `b53e4f1b…bf1d0`: 작업 지시가 적은 값과 같았고, 0.4.2 작업이 끝났을 때와 파일 하나 다르지 않았다 |
| 버전 | `linux_bridge` 0.4.2 → **0.5.0** (0.y.z의 MINOR: 이전 클라이언트의 삭제 요청이 더는 받아들여지지 않는다). `power_base` 0.3.0, `module` 0.1.1, `rcp` 0.1.0 그대로 |
| 작업 전 지문 | `linux_bridge` `b70061c4bae7d597f58d6b69d60794d3b8d989ed70b1c6fb5848b87571546f55` (0.4.2) |
| 작업 후 지문 | `linux_bridge` `dbbfd045030040163ae270ba7ef845f94d7bf5cfaf91a7d6c16361047b44e04c` (build id `590e2a11d2cb+dirty.dbbfd0450300`). `power_base`·`module`·`rcp`의 지문도 달라졌다: 소스는 같고 `versions.json`이 그들의 빌드 입력이기 때문이다 |
| 형식·번호 | `tools/pogo_version.py compat`의 출력이 작업 전과 같다(BLE 등록 규약, 관리 규약, enrollment record, HTTP API 이름). HTTP API와 상태 파일의 변화는 아래 "호환" |
| 출력 | `~/esp-builds/pogoforget4_20261011_*`(빌드·결과), `~/esp-builds/pogo_forget4_hwtest_20261011`(실물 시험 준비물). 앞선 작업들의 결과·후보·준비물과 세 검토의 원본은 바꾸지 않았다(검토의 실행 도구가 자기 디렉터리에 새 실행 디렉터리를 만든다: 아래) |

### 검토가 남긴 재현: 수정 전 실패 → 수정 후

세 번째 검토(`pogo-forget3-review-20261011/`)의 원본 소스·스크립트·로그는 고치지 않았다. SHA-256이 시작 때와 모든 실행
뒤에 같다(`repro/review3_sources_sha256.*.txt`).

재현은 둘이다. 검토의 실행 도구(`run_expanded_repros.py`, 고치지 않음)에 트리와 **그 트리로 방금 만든** host 빌드
(`~/esp-builds/pogoforget4_20261011_linux_api_host_repro`)를 지정해 새 디렉터리에 돌렸다. 종료 코드 1은 결함이 있다는
뜻, 2는 준비·빌드 실패다.

| 실행 | 삭제 재현: 빌드 / 판정 | 순서 재현(앱): 빌드 / 판정 | 기록 |
|---|---|---|---|
| 원본 그대로, **수정 전** 트리 | 0 / **1** (`old request created a removal of the new registration: YES (BUG)`, `new registration received UNENROLL: YES (BUG)`) | 0 / **1** (`old GET after new registration SSE: new registration marked removed=true`) | `repro/expanded_original_before_fix/` |
| 원본 그대로, 수정 후 트리(최종) | **1 / 실행 안 됨** — 컴파일되지 않는다. **통과가 아니다** | 0 / **1** — 그대로 실패한다(아래) | `repro/expanded_original_after_fix/` (실행 도구 종료 2) |
| adapter, 수정 후 트리(최종) | 0 / **0** (`… created a removal of the new registration: no`, `… received UNENROLL: no`, `… polled once more: no command`) | 0 / **0** (`old GET after new registration SSE: left out (older than one already applied); new registration marked removed=false`) | `repro/adapters_after_fix/` |
| 대조: adapter, **사본**에서 "요청을 목록의 등록과 비교하지 않음" | 0 / 0 | — | `repro/control_hub_record.txt` |
| 대조: adapter, **사본**에서 "같은 요청을 알아보지 않음" | 0 / 0 | — | `repro/control_hub_request_id.txt` |
| 대조: adapter, **사본**에서 위 **둘 다** | 0 / **1** (`YES (BUG)` 둘, `… polled once more: UNENROLL (BUG)`) | — | `repro/control_hub_record_and_request_id.txt` |
| 대조: adapter, 앱 **사본**에서 "같은 실행의 더 작은 번호도 적용" / "허브의 순서를 읽지 않음" | — | 0 / **1**, 0 / **1** | `repro/control_app_older_number.txt`, `repro/control_app_order_not_read.txt` |

- **삭제 재현의 원본은 수정 후 컴파일되지 않는다**(오류 둘, 58행과 69행: `RequestRemoval(device, eui, out)` — Base만 싣는
  요청 — 을 부르는 두 줄). 그 호출이 없어진 것이 수정이다. 소스 호환으로 남기려면 "등록을 말하지 않는 요청"을 받는
  길을 남겨야 하고, 그것이 결함이다. 그래서 원본을 보존하고 **adapter**를 따로 두었다
  (`repro/adapter_retry/repro_removal_retry_new_registration_adapter.cpp`와 `.diff` 39줄): 같은 호출을 같은 순서로 하되,
  첫 요청이 그때 목록의 등록·serial과 요청 id를 싣고, **다시 보내는 요청은 그 셋을 그대로** 싣는다. 마지막에 새 등록의
  보드가 한 번 더 poll해서 명령이 없는 것을 본다. 실행은 `repro/run_adapters.py`(검토의 빌드 명령 JSON을 그대로 읽고
  소스 파일만 바꾼다).
- **순서 재현의 원본은 수정 후에도 판정 1이다. 숨기지 않는다.** 그 프로그램은 앱의 판정 함수
  (`PowerBaseReleaseEvidence`)를 직접 부르고, 수정은 그 **앞**의 수신 단계에 있다(판정 함수는 바꾸지 않았다). adapter
  (`repro/adapter_departure/repro_departure_order_adapter.swift`와 `.diff` 54줄)는 검토의 snapshot들에 허브의 순서
  필드를 붙이고, 같은 도착 순서로 실제 `HubSnapshotOrdering`을 지난 것만 판정 함수에 넣는다. 앱의 실제 수신 경로
  (`LiveSensorDataSource` → `AppModel.applyIncoming`)로 같은 순서를 넣은 XCTest는 앱 저장소의 기록에 있다.
- **대조 실행(보호를 뺀 사본에서 재현이 다시 실패하는가).** 미리 정한 기대 없이 돌렸다. 허브 쪽은 보호 하나를 빼서는
  결함이 돌아오지 않는다: 등록 비교만 빼면 같은 요청 id의 작업이 답이 되고, 요청 인식만 빼면 등록 비교가 거부한다.
  **둘을 함께 빼면** 검토의 문장 그대로 결함이 돌아온다. 앱 쪽은 하나만 빼도 돌아온다.
- **앞선 검토들의 재현.** 두 번째 검토의 방 정리 경쟁 재현(`repro_guard_commit_gap.cpp`, 고치지 않음)은 세 번째 검토가
  남긴 실행 도구(`run_guard_review.py`, 이 트리의 host 빌드를 지정)로 build 0 / 판정 0(`existing room preserved after
  move: yes`), 0.4.2 작업의 adapter 실행 도구로도 같다. 그 대조("구간이 Registry를 보지 않고 판단"한 사본)는 판정 1
  (`… NO`). 첫 번째 검토의 재현 둘은 0.4.1 작업의 adapter로 다섯 단계 모두 0, 두 번째 검토의 실행 도구로도 같다.
  2026-10-09 검토의 재현 둘: 141개·15개 검사, 실패 0. 기록: `repro/final_repros.txt`. 두 번째 검토의 원래 실행 도구
  (`run_guard_gap.py`)는 **돌리지 않았다**: 0.4.1 작업의 host 빌드를 이 트리의 소스와 섞어 붙이는 도구다.
- 개발 중에 `Server::SnapshotJson()`(private)의 형식을 한 번 바꿨다가 되돌렸다. 앞선 검토들의 C++ 재현이 그 함수의
  멤버 포인터를 쓰고 있어서, 바꾼 채로는 원본들이 컴파일되지 않았다. 지금의 형식은 0.4.2와 같다.
- 검토의 실행 도구들이 **자기 디렉터리에** 만든 실행 디렉터리: 세 번째 검토에 `guard-run-s5lpg4fz`, 두 번째 검토에
  `run-cg2va4nl`. 그 밖의 원본은 시작 때와 같다(`repro/review*_sources_sha256.*.txt`,
  `logs/preservation_reviews_and_earlier_outputs.log`).

### 무엇을 어떻게 확인했나

실제 HTTP인 것과 아닌 것을 나눠 적는다.

| 무엇 | 종류 | 결과 | 기록 |
|---|---|---|---|
| `tests/test_removal_api.cpp` — `RemovalBindingApiTests`, 본문 검증, `SnapshotSequenceTests` | **실제 HTTP**: 이 트리의 `Server`에 TLS·평문 loopback 소켓으로 요청. 보드의 메시지는 test가 발급된 자격증명으로 서명 | 아래 합계에 포함 | `logs/linux_api_unit_tests.final*.stdout.log` |
| `tests/test_power_base_management.cpp` — `RemovalRequestBinding` | 생산 클래스 직접 호출(HTTP 없음). 실제 상태 파일, 재시작(객체를 다시 만들어 파일에서 읽음), 시계 이동 | 〃 | 〃 |
| API host test 전체 | 위 둘 포함 | **1,942개 검사, 실패 0** — 다섯 번 연속 | 〃 |
| 앱의 실제 전송 코드 ↔ 이 트리의 서버 (시뮬레이터) | **실제 HTTP** (앱 저장소의 기록) | 통과 2, 알려진 실패 1(앱의 event stream 결함: 앱 저장소) | `logs/final_live_mini.*` |
| 같은 것, 허브 **사본**에서 등록 비교와 요청 인식을 뺌 | 실제 HTTP, 대조 | **실패**: 과거 요청이 새 등록의 작업을 만들고 새로 등록된 보드가 `unenroll`을 받는다 | `logs/final_live_control_mini.*` |

`RemovalRequestBinding`과 `RemovalBindingApiTests`가 지나는 순서: 응답 유실 → 완료 → 다른 기기의 재등록 → 같은 요청의
재전송(작업이 남아 있을 때, 보존 기간이 지나 정리된 뒤, 재시작 뒤), 첫 전달이 재등록 뒤로 늦어진 요청, 확인창이 열린
동안의 삭제·재등록, 등록이 시작만 된 동안의 요청, 취소한 요청의 재전송(남아 있을 때·재시작 뒤·정리된 뒤)과 새 확인,
두 기기의 동시 요청(스레드 24회)과 중복 요청, 저장 실패(작업도 serial도 명령도 없음), 다른 Base에 쓰인 요청 id, 이전
형식의 본문(`422`), 형식이 틀린 값들, 이전 버전의 파일과 손상된 파일.

`SnapshotSequenceTests`: 두 필드가 맨 끝에 있고 `GET`과 event가 한 줄로 센다, 설정만 바뀐 event도 더 큰 번호, 미리 뜬
사본으로 늦게 만들어지는 `GET`·event(관리가 있을 때와 없을 때), 관리가 호출자보다 새 사본으로 만든 snapshot 뒤에 그
사이의 사본을 가진 요청이 오는 경우, 실행마다 다른 stream.

| 그 밖의 검사 | 결과 |
|---|---|
| 변형(`tools/mutate_hub.py`: 사본에서 보호를 하나씩 뺌) | 98개 가운데 **96개**가 이 저장소의 검사로 잡혔다(`logs/mutations_hub.run2.txt`). 그 뒤 `Server::SnapshotBody()`의 세 줄과 검사 하나를 고쳤고, 최종 트리에서 `http_server.cpp`의 변형 전부와 영향받은 것 30개를 다시: **29개**(`logs/mutations_hub.final.txt`). 다시 돌리지 않은 67개는 그 수정 전의 트리에서 돌린 것이다(그 수정은 그 67개가 손대는 코드 밖이다) |
| 잡히지 않은 변형 둘 | ① "호출자의 사본만 기억" — 관리가 "마지막으로 받아들인 사본보다 오래된 사본은 받지 않는다"를 이미 하고 있어 결과가 같았다. 겹친 쪽을 지워 코드가 그 모양이 됐다(변형 목록에서 뺐다). ② `RemovalBinding()`이 active가 아닌 자격증명에도 답함 — 이 함수는 test와 재현 도구만 쓰고, 시작만 된 등록에는 목록의 등록이 없어 어차피 답하지 않는다. **잡히지 않은 채로 둔다** |
| C host test (`tools/test_*.c`) | 71개 모두 통과 |
| Python test (`tools/test_*.py`) | 15개 모두 통과 |
| OpenThread 재현 둘 | 39개 + `PASS: both reproductions` |
| core host test, ingest host test | 194개 검사 실패 0, ctest 8개 통과 |
| `tools/pogo_version.py check` / `compat` | `ok (linux_bridge 0.5.0, module 0.1.1, power_base 0.3.0, rcp 0.1.0)` / 출력이 0.4.2 트리의 것과 같다(`logs/compat_before.json`, `logs/compat_after.json`) |
| `app/chip/main.cpp` | 0.4.2 트리와 바이트까지 같다. 관리를 설정에 잇는 두 줄의 컴파일 확인: 통과 |
| Linux 검증 묶음 `pogo_linux_verify_590e2a11d2cb_dbbfd0450300_prep7` (풀어 놓은 사본에서, 가짜 OS, Mac) | tar 체크섬 일치(SHA-256 `435c8599…941e4b`), 묶음 manifest 369개 파일 일치, 묶음 안 소스의 식별이 `0.5.0`·`590e2a11d2cb+dirty.dbbfd0450300`·`source-info-verified`. `linux_verify.sh` 회귀 44개 검사 실패 0, `linux_deploy.sh`·checkpoint 회귀 333개 검사 실패 0. **Linux에서의 실행이 아니다** |

### 호환

`tools/xver/xver.cpp`를 0.3.0·0.4.0·0.4.1·0.4.2 트리(각 작업의 수정 전 사본)와 0.5.0 트리에 빌드해 돌렸다
(`logs/xver.txt`).

| 쓴 버전 → 읽은 버전 | 결과 |
|---|---|
| 0.5.0 → 0.5.0 | 삭제가 요청된 등록 셋의 serial과 작업 셋의 요청 id가 파일에 있고 읽힌다. 취소한 요청을 다시 보내면 **그 취소된 작업**이 답이다 |
| 0.5.0 → 0.4.2 / 0.4.1 / 0.4.0 / 0.3.0 | 읽힌다(두 키를 무시). 그 버전이 파일을 **다시 쓰면** 두 키가 없어진다 |
| 0.4.2 / 0.4.1 / 0.4.0 / 0.3.0 → 0.5.0 | 그대로 읽힌다. serial은 0, 요청 id 없음 |
| 0.5.0 → 0.4.2(다시 씀) → 0.5.0 | 읽힌다. 두 키는 없다(작업들은 요청 id 없이 남는다). 이 실행에서는 0.4.2 단계가 그 Base를 목록에서 제거하므로 취소했던 요청의 재전송은 Base를 찾지 못하는 것으로 끝난다: 아래 0.3.0의 줄이 serial이 사라진 효과를 보여 준다 |
| 0.5.0 → 0.3.0(다시 씀) → 0.5.0 | 읽힌다. **취소했던 요청(serial 0)을 다시 보내면 새 작업이 만들어진다**: 파일이 그 등록에 삭제가 요청된 적이 있다는 것을 더는 말하지 않는다. 되돌리기의 한계다(16절 "저장", 묶음의 `LINUX_README.md`) |

- HTTP: `schema_version` 1, `power_base_management` 1, `power_base_forget` 1 그대로. 삭제 요청의 본문이 바뀌었다(멤버 넷,
  이전 형식은 `422 removal_registration_required`). health에 `power_base_removal_request: 2`, Base 항목에
  `removal_serial`, 작업에 `request_id`, snapshot 끝에 `snapshot_stream`·`snapshot_sequence`.
- 앱 1.3.2 이하 + 0.5.0: 삭제 요청이 거부된다(아무것도 삭제되지 않는다). 앱 1.3.3 + 0.4.2 이하: 앱이 삭제를 요청하지
  않는다(실제 HTTP로 0.4.2의 서버 코드에 연결해 확인: 앱 저장소의 기록). 이전 앱 + 이전 허브: 결함이 그대로다.
- 관리 규약(CoAP), BLE 등록 규약, enrollment record: 바뀌지 않았다(`compat`의 출력이 같고, `components/`·`power_base/`·
  `module/`·`rcp/`에 바뀐 파일이 없다).

### 하지 않은 것, 남은 것

- Linux에서의 빌드·검증·배포, 보드, 실물 iPhone: 하지 않았다(금지된 범위). 준비물과 순서는
  `~/esp-builds/pogo_forget4_hwtest_20261011/public/`.
- `app/chip/main.cpp`는 바뀌지 않았고(0.4.2 트리와 바이트까지 같다) Mac에서 빌드되지 않는다. 관리를 설정에 잇는 두 줄이
  이 트리의 헤더에 맞는지만 컴파일로 봤다.
- 이전 앱 + 이전 허브의 조합에는 결함이 그대로다. 0.4.2 이하로 되돌려 그 버전이 상태 파일을 다시 쓰면 삭제 순번이 0부터
  다시 시작한다(16절 "저장", 묶음의 `LINUX_README.md` "되돌리기").
- "snapshot을 한 번에 하나씩 만든다"는 잠금을 뺀 변형은 결정적으로 잡을 방법이 없어 변형 목록에 없다.

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

BH1750 실물 시험에서 endpoint 3의 UniqueID가 module UUID `020000fffe65d357`과 일치했고,
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
| Power Base 등록 PSKd의 BLE 전달 | 펌웨어·통신 규약과 앱의 DEBUG 전용 등록 시험 화면까지 구현. 여분 보드 1대에서 등록 성공·취소·창 만료를 실물 확인(2026-10-02, "BLE 등록 실물 후속 검증"). 제품용(Release) 등록 화면과 삭제는 2026-10-05에 구현, 실물 미확인("Power Base 관리(일반 등록·삭제) 검증"). 기본 MTU 23과 인증 실패 잠금은 실물 미확인. 등록 뒤 재시작에서 BLE 등록을 다시 열지 않는 것은 주소 자동 전달 빌드의 RTS 재시작 1회에서 보드 로그로 확인(2026-10-05, "실물 후속 검증"). 물리 전원 재투입 뒤는 실물 미확인 |
| Power Base 등록의 허브 주소 자동 전달 | 구현(2026-10-03, 위 "허브 Thread 수신 주소 자동 전달"). host test·빌드 확인과 Linux 배포 검증(사용자 실행)에 더해, 2026-10-05에 여분 보드 1대에서 실물 확인("실물 후속 검증 (2026-10-05)"): 허브 → 앱 → 보드의 목적지 전달(`0x04` 수락은 보드 로그와 소스에 따른 간접 근거), record v2 저장, RTS 재시작 뒤의 복원과 새 heartbeat, 앱의 `heartbeat_received`. **실물 미확인**: 물리 전원 차단 뒤의 복원, 구형 start(`0x02`) 거부, 기본 MTU 23, 두 Base 동시 등록, 백그라운드 경쟁 조건, 세션의 `bridge_endpoint` 원문과 OTBR 주소의 직접 대조. 최초 전달과 재부팅 복원까지다. (2026-10-10에 덧붙임: 등록 뒤 같은 허브의 주소·port가 바뀌는 경우의 복구는 2026-10-09에 구현됐다. 아래 줄.) 제품용(Release) 등록 화면은 2026-10-05에 구현됐고 실물 미확인(아래 줄) |
| Power Base 일반 등록·삭제(관리 자격증명) | 구현(2026-10-05, 위 "Power Base 관리(일반 등록·삭제) 검증"). host test·시뮬레이터 test·빌드로만 확인. 실물 보드·실물 BLE·운영 Linux 허브·실제 iPhone 미실행, Linux GN/CHIP 빌드 미실행. record v1/v2로 등록된 기존 보드의 원격 삭제는 지원하지 않는다(재등록 필요) |
| Power Base의 BOOT 버튼 로컬 재등록(다른 허브로 이동) | 구현(2026-10-06, 위 "BOOT 버튼 로컬 재등록 검증"). 버튼 구성(`sdkconfig.reenroll_button_gpio9`)에서만 켜지고 GPIO9 배선은 미확인 가정이다. host test·상호운용 시험·시뮬레이터 test·빌드로만 확인. **실물 미실행**: 버튼, 재시작 창, 실제 삭제, 전원 차단, 새 네트워크 가입 |
| 등록 뒤 허브 주소가 바뀐 경우의 복구 | **구현(2026-10-09)**: 같은 허브의 Thread 수신 주소·port 변경 복구(위 "주소 복구와 센서 체인 재등록 복구", "후속 수정"). host test와 OpenThread 재현. port 변경은 사용자가 실물에서 확인했다는 보고가 있고(위 "이전 실물 결과와 이 변경"), 주소(EID) 변경과 다른 hub_id 거부는 실물 미실행 |
| 영구 오프라인 Base를 허브에서 빼는 것, 이전 허브의 기록 정리 | **구현(2026-10-10)**: "허브 목록에서 제거"(위 같은 이름의 절, Linux bridge 0.4.0)와 같은 날의 후속 수정(위 "목록 제거 후속 수정", Linux bridge 0.4.1: snapshot 일관성, 방 지정 정리의 결속, 재시작 뒤 다시 보고하지 않은 Base의 정리)과 그다음 후속 수정(위 "목록 제거 후속 수정 2", Linux bridge 0.4.2: 방 정리의 판단과 쓰기를 한 구간으로, 삭제를 통째로 놓친 기기를 위한 "목록 밖의 Base"). host test로만 확인. Linux GN/CHIP 빌드·운영 허브·실제 보드·실제 iPhone 미실행. 이전 허브의 기록은 앱을 그 허브에 연결해서 지운다: 닿지 않는 허브의 기록은 정리되지 않는다 |
| Power Base 등록의 QR 흐름 | 설정 코드의 **QR 입력**은 구현(2026-10-06, 위 "QR 설정 코드 입력 검증"). 실제 기기의 카메라 스캔은 미실행. QR 하나로 등록 전체를 진행하는 흐름은 없다 |
| 버전 체계의 release 빌드와 설치·실물 시험 기록 | 체계와 도구는 구현(2026-10-06, 위 "버전 관리 체계 검증"). 만든 산출물은 모두 개발 빌드이고 설치 기록은 없다. Linux GN/CHIP 빌드, 보드의 부팅 로그, 운영 허브의 health `build`, 실제 기기의 앱 정보 화면은 미확인 |

## 기록 원칙

새 결과를 추가할 때는 사용한 보드, 설정 이미지, 앱 커밋, 시험 경로를 함께 적는다. 로그를
직접 확인하지 못한 결과도 실물 시험 기록으로 남길 수 있지만, 확인 범위를 넓혀 쓰지 않는다.
host test와 빌드는 실물 동작의 대체 근거로 사용하지 않는다.
