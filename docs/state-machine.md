# 상태 머신

이 문서는 POGO 체결부터 Thread 데이터 전송까지의 상태 전이를 정리한다.

## pogo_port

UPSTREAM과 DOWNSTREAM은 서로 독립된 instance다.

```text
DISABLED
  -> WAIT_ATTACH
  -> ATTACH_SETTLE
  -> HANDSHAKE
  -> PEER_READY
  -> ERROR
```

- `DISABLED`: task와 UART가 비활성
- `WAIT_ATTACH`: DETECT_IN 연결 대기
- `ATTACH_SETTLE`: 접점이 안정될 때까지 대기
- `HANDSHAKE`: UART flush, parser reset, PEER_HELLO
- `PEER_READY`: provisioning frame과 PING/PONG 처리
- `ERROR`: 오류 종류에 따라 cooldown 또는 물리적 재체결 대기

`DOCK_TIMEOUT`과 `LIVENESS_LOST`는 DETECT_IN이 계속 low면 cooldown 뒤 새 cycle을
시작한다. version, capability, peer type 충돌은 detach 전까지 유지한다.

DETECT level은 ISR event 외에도 주기적으로 다시 읽어 내부 상태와 맞춘다.

## role_manager

AUTO 모드의 입력은 두 port의 attach와 peer type이다.

```text
UPSTREAM ready + DOWNSTREAM module ready -> INTERMEDIATE
UPSTREAM ready + DOWNSTREAM not ready    -> LEAF
UPSTREAM not ready                       -> UNASSIGNED
conflict                                 -> ROLE_ERROR
```

역할 변화 callback은 다음 작업을 연결한다.

- `child_service_on_role_change()`
- Thread link mode 재적용
- parent service 허용 여부 갱신
- topology report generation 갱신

AUTO 센서 모듈은 ROOT를 선택하지 않는다. DEV_ROOT/DEV_LEAF는 초기 회귀 시험 fixture다.

## thread_manager

### INTERMEDIATE

```text
rx-on-when-idle = true
FTD
router eligible = true
```

### LEAF

```text
rx-on-when-idle = false
router eligible = false
poll period = CONFIG_THREAD_MANAGER_LEAF_DATA_POLL_PERIOD_MS
```

역할에 맞는 link mode를 적용할 때 필요하면 Thread를 잠시 disable한 뒤 설정하고 다시
enable한다. 센서 polling period는 `otLinkSetPollPeriod()`에 사용하지 않는다.

역할 변경 callback에서는 자신의 Extended Address와 `otDeviceRole`을 기록한다. CHILD이면
parent Extended Address와 RLOC16도 기록한다. sample 전송 시에는 실제 CoAP send의
OpenThread lock 안에서 parent를 다시 확인하고, 처음 관찰하거나 바뀌었을 때만
`sample_seq`와 함께 로그를 남긴다.

## child_service

child service는 UPSTREAM 포트만 처리한다.

```text
IDLE
  -> IDENTIFIED
  -> UUID_CONFIRMED
  -> SLOT_ASSIGNED
  -> PARENT_INFO_SET
  -> SENSOR_REPORTED
  -> POLL_SET
  -> COLLECTOR_SET
  -> DATASET_STORED
  -> JOINING
  -> JOINED
```

허용된 순서가 아닌 write message는 NACK한다. 같은 sequence가 다시 오면 이미 적용한
부작용을 반복하지 않고 캐시한 응답을 다시 보낸다.

### SENSOR_HELLO

`sensor_service_measure_once()`로 실제 센서 응답 시간과 status를 얻는다.
`CONDITIONING`은 PMS5003/SGP41의 정상 예열 상태이므로 정보를 반환하고 다음 단계로
진행한다. 다른 오류 상태는 provisioning을 중단한다.

### POLL_CONFIG

받은 주기와 sensor driver의 최소 주기 중 큰 값을 실제 측정 주기로 사용한다.
이 값은 Thread SED data poll 주기와 별개다.

### SET_COLLECTOR

Power Base UUID, IPv6 address, port, generation을 검증해 cache에 저장한다.
Dataset보다 먼저 받아야 한다.

### SET_DATASET

Active Dataset TLV를 검증하고 NVS에 저장한 뒤 역할에 맞는 Thread link mode로 가입한다.
성공 또는 실패를 `JOIN_RESULT`로 보낸다.

### JOIN_RESULT

parent의 ACK를 받을 때까지 같은 sequence로 제한된 재전송을 한다.
ACK를 받으면 provisioning session이 완전히 끝난다.

UPSTREAM DETACHED/ERROR에서는 session, collector, topology gate를 지우고 Thread 가입
상태를 해제한다.

## parent_service

parent service는 DOWNSTREAM 포트만 처리한다. Power Base와 INTERMEDIATE가 같은 구현을
사용한다.

```text
PEER_READY
  -> IDENTIFY
  -> UUID
  -> ASSIGN_SLOT
  -> ASSIGN_PARENT_INFO
  -> SENSOR_HELLO
  -> POLL_CONFIG
  -> SET_COLLECTOR
  -> SET_DATASET
  -> JOIN_RESULT 대기
  -> JOIN_RESULT_ACK
```

각 request는 ACK timeout과 `CONFIG_POGO_MAX_RETRY`를 사용한다. retry는 같은 sequence를
보낸다. DETACHED/ERROR는 어느 단계에서든 현재 session을 취소한다.

session이 실패했지만 peer가 계속 연결 상태라면 PING interval 뒤 처음부터 다시 시도한다.

### polling period

| response time | multiplier |
|---|---:|
| 0 | 오류 |
| 1..49ms | 15 |
| 50..99ms | 10 |
| 100..499ms | 8 |
| 500..1000ms | 3 |
| 1001..10000ms | 1.5 |
| 10001ms 이상 | 1 |

계산은 64비트 중간값을 사용하고 경계값은 `tools/test_polling_calc.c`에서 검사한다.
sensor driver가 더 긴 최소 주기를 요구하면 child가 최종 적용값을 올린다.

## topology_reporter와 topology_gate

새 provisioning session에서는 gate를 닫는다. module이 Thread에 가입하면
`topology_reporter`가 `ATTACHED`를 `/t`로 보낸다. Power Base가 CoAP 2.xx로 받았을
때만 gate를 연다.

```text
Thread joined
  -> ATTACHED report
  -> CoAP 2.xx
  -> topology_gate OPEN
  -> /s sample 허용
```

4.04 또는 4.12 응답은 gate를 닫고 재등록을 요청한다. reporter task의 다음 poll tick에서
재등록한다. busy loop를 만들지 않는다.

DETACHED, role UNASSIGNED/ROLE_ERROR, 새 provisioning 시작에서도 gate를 닫는다.

## sensor_service

하나의 mutex로 provisioning 측정과 주기 측정을 직렬화한다.

```text
periodic timer
  -> measure_once
  -> sample_seq 증가
  -> role과 Thread joined 확인
  -> sample callback
```

Thread가 detached여도 측정은 계속한다. 무선 callback만 건너뛴다.

## sensor_transport

gate가 열려 있고 collector endpoint가 있을 때 Confirmable POST `/s`를 보낸다.

한 logical publish는 초기 attempt 1회와 최대 retry 3회다. OpenThread 자체 CoAP
retransmit은 0으로 두어 application retry와 겹치지 않게 한다.

응답 직후 gate를 다시 확인한다. 4.04/4.12가 gate를 닫았다면 backoff를 기다리지 않고
남은 retry를 중단한다. 이 경우 `delivery unconfirmed` 경고도 남기지 않는다.

일반 timeout은 UUID hash 기반 backoff를 사용한다.

```text
retry 1: 0..255ms
retry 2: 0..511ms
retry 3: 0..1023ms
```

같은 logical sample은 같은 `sample_seq`를 사용한다.

## SED CoAP timing

초기 구현에서는 sensor measurement period를 SED data poll에도 사용해, PMS5003의 수 초
단위 주기가 CoAP 응답 대기보다 길어지는 문제가 있었다. Power Base는 sample을 저장했지만
LEAF는 ACK를 받기 전에 timeout으로 판단했다.

현재는 세 값을 분리한다.

| 항목 | 기본값 |
|---|---:|
| sensor measurement period | POGO에서 협상 |
| Thread SED data poll | 500ms |
| CoAP ACK timeout | 2000ms |
| 계산된 response wait | 5000ms |

`coap_response_timing_wait_ms()`가 overflow-safe 계산을 제공하고 sensor transport와
topology reporter가 같은 Kconfig를 사용한다. timeout 로그는 데이터가 실제로 유실됐다고
단정하지 않고 `delivery unconfirmed`로 표현한다.

## Power Base 수신

`/t` topology report가 registry에 먼저 적용되어야 `/s` sample을 받는다.

- unknown UUID: 4.04
- parent offline, bitmap/세대 불일치: 4.12
- duplicate sample: 2.03
- 새 sample 저장: 2.04
- aggregator busy/full: 5.03

depth>1 보고는 parent가 online이고 downstream 연결 상태일 때만 적용한다. 링크 분리 뒤
늦게 도착한 자식 heartbeat가 subtree를 되살리지 못한다.

## detach 처리

### UPSTREAM detach

- child session 취소
- collector cache clear
- topology gate close
- Thread stop
- 새 POGO handshake와 Dataset을 받기 전 자동 복귀하지 않음

### DOWNSTREAM detach

- 자식 provisioning session 취소
- 해당 child/subtree offline
- 자신의 Thread 가입은 유지
- INTERMEDIATE에서 LEAF로 바뀌면 같은 Dataset으로 link mode만 재적용

## 오류와 복구

| 오류 | 처리 |
|---|---|
| CONFIG_ERROR | 부팅 중단 |
| DOCK_TIMEOUT | cooldown 뒤 재시도 |
| LIVENESS_LOST | cooldown 뒤 재시도 |
| PEER_TYPE_CONFLICT | detach 전까지 ERROR |
| PROTOCOL_VERSION_ERROR | detach 전까지 ERROR |
| CAPABILITY_MISMATCH | detach 전까지 ERROR |
| FRAME_CRC_ERROR | parser 재동기화, request timeout/retry |
| SENSOR_ERROR | NACK, session 중단 |
| POLL_CONFIG_ERROR | NACK, session 중단 |
| DATASET_ERROR | NACK, fallback 없음 |
| THREAD_JOIN_TIMEOUT | 실패 JOIN_RESULT, peer가 남으면 새 session 가능 |
| storage failure | NACK, Thread join 시작하지 않음 |

최신 실물 검증 상태는 [`validation-status.md`](validation-status.md)를 기준으로 한다.
