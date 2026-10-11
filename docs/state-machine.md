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

재시도 중인 포트는 DETECT_IN이 low이므로 `pogo_port_is_attached()`가 계속 true이고,
이전 `PEER_READY`에서 기록한 peer type도 남아 있다. 링크를 실제로 쓸 수 있는지는
`pogo_port_is_peer_ready()`(현재 `PEER_READY`인 동안만 true)로 판단한다.

cooldown 중에도 UART를 읽어 유효한 `PEER_HELLO_REQ`에는 `PEER_HELLO_RESP`로 답하고 곧바로
자신의 handshake를 시작한다. 양쪽이 같은 주기(handshake 약 1.2초 + cooldown 3초)로 재시도하므로,
cooldown 동안 듣지 않으면 두 쪽의 재시도 시작이 약 1~3초 어긋났을 때(시차를 두고 체결했거나,
연결된 상태에서 한 방향만 잠시 끊겨 양쪽의 `LIVENESS_LOST`가 약 1.3초 벌어진 경우) 한쪽의
handshake 구간이 매번 상대의 cooldown 안에 들어가 배선이 정상이어도 다시 연결되지 않았다.

- 응답만으로는 `PEER_READY`가 되지 않는다. 자신이 보낸 `PEER_HELLO_REQ`에 대한 응답을 받아야
  `PEER_READY`가 된다.
- version, peer type, BASE_ID, capability 검사는 handshake와 같다. 검사에 실패하면 같은 영구
  오류로 바뀌고 detach 전까지 재시도하지 않는다.
- 다른 frame, CRC·길이가 맞지 않는 frame, 이전 cycle의 `PEER_HELLO_RESP`는 버리고 cooldown을
  그대로 채운다.
- 한 번에 최대 20ms만 기다리므로 DETACHED 처리 지연과 재시도 주기는 이전과 같다
  (`tools/test_pogo_port_cooldown_hello.c`).

DETECT level은 ISR event 외에도 주기적으로 다시 읽어 내부 상태와 맞춘다.

## role_manager

AUTO 모드의 입력은 두 port의 attach, peer type, 오류 종류다. 위에서부터 먼저 맞는 줄을
적용한다.

```text
UPSTREAM 오류(종류 무관)                          -> ROLE_ERROR
DOWNSTREAM 영구 오류                              -> ROLE_ERROR
UPSTREAM not ready (분리 또는 handshake 미완료)   -> UNASSIGNED
UPSTREAM ready + DOWNSTREAM 분리                  -> LEAF
UPSTREAM ready + DOWNSTREAM 일시 오류·재시도 중   -> 직전 LEAF/INTERMEDIATE 유지 (없으면 LEAF)
UPSTREAM ready + DOWNSTREAM module ready          -> INTERMEDIATE
UPSTREAM ready + DOWNSTREAM handshake 진행 중     -> 직전 역할 유지
```

- 영구 오류: `PEER_TYPE_CONFLICT`, `PROTOCOL_VERSION_ERROR`, `CAPABILITY_MISMATCH`, 그리고
  분류되지 않은 오류 코드
- 일시 오류: `DOCK_TIMEOUT`, `LIVENESS_LOST`

역할 변화 callback은 다음 작업을 연결한다.

- `child_service_on_role_change()`
- Thread link mode 재적용
- parent service 허용 여부 갱신
- topology report generation 갱신

AUTO 센서 모듈은 ROOT를 선택하지 않는다. DEV_ROOT/DEV_LEAF는 초기 회귀 시험 fixture다.

### DOWNSTREAM 일시 오류 격리

DOWNSTREAM의 `DOCK_TIMEOUT`과 `LIVENESS_LOST`는 하위 링크의 문제이며 자신의 UPSTREAM 관계와
Thread 가입에는 영향을 주지 않는다. 이 두 오류는 역할을 바꾸지 않으므로 역할 변화
callback이 호출되지 않는다.

- 자신의 Thread를 중지하지 않고 `upstream_ready`, collector, topology gate를 그대로 둔다.
  측정과 `/s` 전송이 계속된다.
- 하위 쪽은 같은 `ERROR` event로 따로 정리한다. parent service는 진행 중인 provisioning
  session을 취소하고, topology reporter는 `downstream_connected=false`를 보고한다.
- 하위 module이 물리적으로 붙어 있는 동안에는 빈 포트로 보지 않는다. INTERMEDIATE를 LEAF로
  내리지 않으므로 SED link mode로 바뀌지 않고, 복구 때 다시 올리는 재적용도 없다.
- 한 번도 handshake를 마치지 못한 하위 module은 ready module이 아니다. 그 상태에서는 LEAF를
  유지하고, UPSTREAM이 나중에 ready가 된 경우에도 LEAF로 시작한다.
- 역할이 INTERMEDIATE로 유지되어도 하위 링크가 연결된 것은 아니다. 하위 연결 여부는 포트가
  현재 `PEER_READY`인지로 판단한다.

복구 조건은 DOWNSTREAM `PEER_READY`다. pogo_port가 기존 cooldown과 재시도 횟수로 handshake를
다시 시도하고, 성공하면 다음 순서로 이어진다. 양쪽의 재시도 시점이 어긋난 경우는 pogo_port
절의 cooldown 설명을 참고한다.

- LEAF였다면 INTERMEDIATE로 바뀌고 같은 Dataset으로 link mode를 한 번 재적용한다.
- INTERMEDIATE였다면 역할 변화가 없고 자신의 Thread 가입도 그대로다.
- parent service가 하위 provisioning을 처음부터 다시 시작한다.

DOWNSTREAM `DETACHED`는 실제로 빈 포트이므로 LEAF가 된다.

UPSTREAM 오류는 격리하지 않는다. 하위 오류 중에 UPSTREAM이 `DETACHED` 또는 `ERROR`가 되면
아래 "UPSTREAM detach"의 정리를 그대로 수행하고, 그 뒤 DOWNSTREAM이 `PEER_READY`로 돌아와도
자신의 가입과 gate는 열리지 않는다.

role_manager는 격리 로그를 오류 종류가 바뀔 때 한 번만 남긴다. pogo_port의 cycle별 로그
주기는 그대로다.

역할 판정과 Thread 중지 여부는 `tools/test_role_manager_downstream_isolation.c`가,
포트·provisioning·gate·전송을 포함한 전체 경로는
`tools/test_module_downstream_fault_isolation.c`가 실제 소스를 링크해 검사한다.
일시 오류 격리와 cooldown 중 HELLO 응답은 host 확인만 했고 실물에서는 확인하지 않았다
([`validation-status.md`](validation-status.md) "POGO 체인 분리·재연결 실물 시험").

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

가입 성공은 OpenThread의 현재 role(CHILD/ROUTER/LEADER)로 판정한다. 가입 호출 직후 Thread는
DETACHED이므로 그 뒤에 보이는 attached role은 이번 가입의 결과다. role 변화 queue는 가입 중에만
비워지므로 이전 가입 때의 변화(INTERMEDIATE의 ROUTER 승격, Thread 중지의 DISABLED, LEAF의 재연결)가
남아 있을 수 있고, 가득 차면 새 변화가 버려진다. 그래서 queue의 event만으로는 판정하지 않는다
(`tools/test_child_service_stale_role_events.c`).

역할이 INTERMEDIATE가 아니면 LEAF link mode로 가입한다. DOWNSTREAM handshake가 끝나기 전에
`SET_DATASET`이 도착해 역할이 아직 UNASSIGNED인 경우도 여기에 포함된다. 가입 뒤에는 실제로
적용한 link mode가 바뀔 때(LEAF ↔ INTERMEDIATE)만 같은 Dataset으로 다시 적용한다. 다시
적용하면 Thread를 껐다 켜므로, 역할이 UNASSIGNED에서 LEAF로 확정되는 것만으로는 다시 적용하지
않는다(`tools/test_child_service_link_mode_reapply.c`).

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

session이 실패했지만 peer가 계속 연결 상태라면 처음부터 다시 시도한다. 다시 시도하기 전의 대기는 PING
interval(기본 1초)에서 시작해 실패가 이어질 때마다 두 배가 되고 16배(16초)에서 멈춘다. 성공, 새 handshake
(PEER_READY), DETACHED/ERROR, 새 UPSTREAM 세대가 대기를 처음으로 되돌린다. 대기는 기한으로 두므로 그동안에도
task는 port event(특히 DETACHED)를 처리한다. 2026-10-09 이전에는 간격이 고정(PING interval)이었고 task가 그
시간 동안 멈춰 있었다.

### UPSTREAM 세대 변경과 DOWNSTREAM 재설정

module이 아래 module에 넘겨주는 것(collector endpoint, Dataset, 자기 depth, 자기 uuid)은 모두 자기 UPSTREAM
provisioning session에서 온다. 그 session이 **새로** 끝까지 진행되면(Thread join까지) 아래 module이 가진 것은
전부 낡은 것이 된다. 아래 module이 물리적으로 계속 꽂혀 있으면 DOWNSTREAM port에는 아무 event도 없으므로,
이것은 port event가 아니라 **세대**로 다룬다.

- `child_service`가 UPSTREAM 세대 번호를 가진다(`child_service_get_upstream_generation()`). session이 JOINED가
  되는 그 순간에만 1 오른다. `upstream_ready`가 true가 되기 **전에** 오른다. role 변경(LEAF ↔ INTERMEDIATE의
  link mode 재적용: 같은 Dataset으로 다시 join), OpenThread의 ROUTER 승격, 무선 detach·재attach, 같은 요청의
  재전송은 번호를 바꾸지 않는다.
- `parent_service`는 DOWNSTREAM session을 시작할 때의 번호를 기억한다(`get_upstream_generation`,
  `module_wiring.c`가 연결한다). `JOINED`는 "그 번호에 대해 joined"라는 뜻이다. 현재 번호와 다르면:
  - `JOINED`였으면 `IDLE`로 돌아가 꽂혀 있는 child를 IDENTIFY부터 다시 provisioning한다. 새 PEER_READY나 물리
    재연결이 필요 없다.
  - session이 진행 중이었으면(JOIN_RESULT를 기다리는 중 포함) 그 자리에서 버리고 새 번호로 곧바로 다시
    시작한다. 실패 대기를 적용하지 않는다. 기다리는 동안 100ms마다 번호를 본다.
  - 번호는 **비교**한다. `upstream_ready`가 false였다가 true가 되는 것을 지켜보는 방식이 아니므로, 그 사이가
    아무리 짧아도, 아무리 늦게 보아도 한 번은 처리되고, 번호가 같은 동안에는 아무 일도 하지 않는다.
- **이전 session의 JOIN_RESULT (2026-10-09 후속 수정).** JOIN_RESULT에는 child가 매긴 seq만 있고, 그것이 어느
  session의 결과인지는 실려 있지 않다. parent는 "꽂혀 있는 child에게서 지금까지 본 가장 새 JOIN_RESULT의 seq"를
  기억하고, 그 seq의 JOIN_RESULT가 session의 join 대기 중에 오면 ACK만 다시 보내며(보낸 쪽이 ACK를 못 받았을 수
  있다) 그 session의 결과로 쓰지 않는다.
  - **이 기억의 주인은 session이 아니라 child와의 handshake다.** session이 시작·실패·시간 초과·세대 교체로
    버려져도 그대로 남고, ATTACHED/DETACHED/ERROR/PEER_READY에서만 버린다(재시작한 child는 1부터 센다). 처음
    구현은 이것을 session의 node 기록에 두어, 결과 없이 끝난 session 다음의 재시도에서는 이전 결과를 알아보지
    못했다(같은 이전 결과가 둘째 시도를 성공시켰다).
  - 어디서 오든 기록한다: join 대기, session의 앞 단계(자기 Dataset을 주기 전에 온 것은 이전 session의 것이다),
    session 밖(자기 session이 포기한 뒤에 늦게 온 결과). session 밖에서 온, 어느 session도 처리한 적 없는
    결과는 ACK하지 않는다(이전과 같다). session이 처리한 결과의 재전송은 session 밖에서도 ACK한다(이전과 같다).
  - **지금의 frame으로 보장되는 것과 그 근거.** child(`child_service`)는 ① 결과마다 seq를 1씩 올리고(0은 쓰지
    않는다) ② 가장 최근 결과만, 같은 seq로, ACK를 받을 때까지 다시 보내며 ③ 새 session의 IDENTIFY_REQ를 받으면
    그 재전송을 그만둔다. ④ 이 모두를 task 하나에서 하고 UART와 event queue는 순서를 지킨다: IDENTIFY_REQ에
    답하기 전에 보낸 것은 그 답보다 먼저 도착한다. 따라서 이전 session의 결과로 올 수 있는 것은 "지금까지 본
    가장 새 것"뿐이고, session 자신의 결과는 항상 다른 번호다. ①~③은 실제 `child_service`로 확인했다
    (`tools/test_module_upstream_regeneration.c` 국면 10).
  - **보장되지 않는 것.** parent가 한 번도 본 적 없는 이전 결과가 그 session의 SET_DATASET이 ACK된 뒤에 처음
    도착하는 경우는 지금의 frame으로 가려낼 수 없다. `child_service`를 돌리는 module은 그렇게 보낼 수 없다(위
    ③④). 그 규칙을 따르지 않는 상대까지 막으려면 JOIN_RESULT에 session 번호가 있어야 하고, 그것은 POGO 메시지
    형식의 변경이다. 하지 않았다.
  - SET_DATASET의 ACK가 유실되어 child의 결과가 재전송된 ACK보다 먼저 온 경우, 그 결과는 이 session의 것이다.
    이전 것으로 기록하지 않고, child가 다시 보낼 때 받는다.
  - **port가 알려 주지 않은 child 재시작.** 꽂힌 채 재시작한 child의 새 HELLO_REQ에 `pogo_port`는 event 없이
    답한다. 그 child의 첫 결과(seq 1)가 parent가 기억하는 번호와 우연히 같으면 한 번 이전 결과로 취급되어 join
    timeout 한 번을 쓰고, 다음 시도(seq 2)에서 완료된다. 그 이상 반복되지 않는다.
  - seq는 255 다음에 1이다. 기억하는 것은 번호 하나이고 같은지만 보므로 넘어가는 자리에 따로 규칙이 없다.
- 받는 쪽은 기존 동작 그대로다: JOINED인 child가 새 IDENTIFY_REQ를 받으면 이전 session을 버리고 새로 시작하고
  (collector를 지우고 `upstream_ready`를 false로), 새 Dataset으로 join한 뒤 **자기** 세대 번호를 올린다. 그래서
  3단 이상의 체인에서는 각 단계가 차례로 아래를 다시 설정한다. 물리 detach를 보는 것은 첫 module뿐이다.
- `topology_reporter`도 같은 번호를 쓴다. `ATTACHED` 등록은 그것을 만든 세대에만 유효하다. 번호가 달라지면 gate를
  닫고 새 collector로 `ATTACHED`를 다시 보낸다(reporter task가 전송 재시도 안에 있는 동안 `upstream_ready`가
  내려갔다 올라온 것을 놓쳐도 된다).
- Power Base는 이 번호를 쓰지 않는다(`get_upstream_generation = NULL`). Base가 삭제·재등록되면 downstream port가
  꺼지고 칩이 재시작하므로 첫 module은 물리 detach를 본다.
- UPSTREAM이 끊겨 있는 동안(role UNASSIGNED, Thread 정지)에는 번호가 바뀌지 않고 아래로 아무것도 보내지 않는다.
  저장된 예전 정보로 다시 가입하지 않는다는 규칙은 그대로다: 새 세대는 새 handshake와 새 SET_DATASET 뒤에만
  생긴다.

실물에서 본 증상(2026-10-09 이전 이미지): Power Base → SGP41 → PMS5003에서 Base를 삭제하고 다시 등록하면 SGP는
돌아오고 PMS는 뽑았다 꽂아야 돌아왔다. SGP의 `parent_service`가 이전 세대의 `JOINED`에 머물러 PMS에게 새
collector를 주지 않았기 때문이다. 재현과 검증: `tools/test_module_upstream_regeneration.c`(module 하나를 실제
코드로 돌리는 부분과, 실제 module 세 개를 프로세스 세 개로 이은 체인). 이전 JOIN_RESULT에 관한 규칙은 같은 test의 국면 6과 10~16이 본다.

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

`downstream_connected`는 DOWNSTREAM 포트가 현재 `PEER_READY`이고 peer가 module일 때만
true다. DETECT_IN이 low이거나 이전 peer type이 module이라는 이유만으로 true가 되지 않는다.
값이 바뀔 때만 `UPDATED`를 보내므로 하위 재시도가 반복되어도 보고는 한 번이다.

DETACHED, role UNASSIGNED/ROLE_ERROR, 새 provisioning 시작에서도 gate를 닫는다. UPSTREAM 세대 번호가 등록 때와
달라졌을 때도 닫고 다시 등록한다(위 "UPSTREAM 세대 변경과 DOWNSTREAM 재설정").

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

### DOWNSTREAM 일시 오류 (DOCK_TIMEOUT, LIVENESS_LOST)

- 자식 provisioning session 취소, cooldown 뒤 handshake 재시도
- `downstream_connected=false` 보고, Power Base registry에서 해당 subtree 제거
- 자신의 Thread 가입, collector, topology gate, 역할은 유지
- `PEER_READY`로 돌아오면 자식 provisioning 재개

## 오류와 복구

| 오류 | 처리 |
|---|---|
| CONFIG_ERROR | 부팅 중단 |
| DOCK_TIMEOUT (UPSTREAM) | cooldown 뒤 재시도, ROLE_ERROR, Thread 중지 |
| LIVENESS_LOST (UPSTREAM) | cooldown 뒤 재시도, ROLE_ERROR, Thread 중지 |
| DOCK_TIMEOUT (DOWNSTREAM) | cooldown 뒤 재시도, 하위 링크만 무효, 자신의 역할과 Thread 유지 |
| LIVENESS_LOST (DOWNSTREAM) | cooldown 뒤 재시도, 하위 링크만 무효, 자신의 역할과 Thread 유지 |
| PEER_TYPE_CONFLICT | detach 전까지 ERROR, 어느 포트든 ROLE_ERROR |
| PROTOCOL_VERSION_ERROR | detach 전까지 ERROR, 어느 포트든 ROLE_ERROR |
| CAPABILITY_MISMATCH | detach 전까지 ERROR, 어느 포트든 ROLE_ERROR |
| FRAME_CRC_ERROR | parser 재동기화, request timeout/retry |
| SENSOR_ERROR | NACK, session 중단 |
| POLL_CONFIG_ERROR | NACK, session 중단 |
| DATASET_ERROR | NACK, fallback 없음 |
| THREAD_JOIN_TIMEOUT | 실패 JOIN_RESULT, peer가 남으면 새 session 가능 |
| storage failure | NACK, Thread join 시작하지 않음 |

최신 실물 검증 상태는 [`validation-status.md`](validation-status.md)를 기준으로 한다.
