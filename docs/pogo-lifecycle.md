# POGO 모듈 자동 등록·삭제 수명주기

모듈을 POGO로 붙이면 자동으로 등록되고, **물리 분리가 확정되면** Linux API·SSE·Matter에서
삭제된다. 통신이 끊기거나 보고가 오래 없으면 삭제하지 않고 offline으로만 표시한다.

## offline과 removed의 구분

| 원인 | Power Base topology registry | Linux 전송 | Linux 결과 |
|---|---|---|---|
| Power Base DOWNSTREAM `DETECT_IN` 분리(디바운스) | 해당 모듈과 하위 subtree 삭제 | `TOPOLOGY_REMOVED` | 삭제 |
| 모듈 자신의 `TOPOLOGY_REPORT_DETACHED` 보고 | 해당 모듈과 하위 subtree 삭제 | `TOPOLOGY_REMOVED` | 삭제 |
| 중간 모듈의 `downstream_connected` true→false | 하위 subtree 삭제 | `TOPOLOGY_REMOVED` | 삭제 |
| Power Base stale sweep(120 s) | offline 유지 | `TOPOLOGY_SNAPSHOT online=false` | offline, `Reachable=false` |
| Linux registry stale timeout(120 s) | 영향 없음 | 없음 | offline, `Reachable=false` |

stale timeout은 삭제 근거로 쓰지 않는다. 하드웨어로 확정된 분리만 삭제 근거가 된다.

## 전달 보장

`linux_bridge_exporter`는 2.xx 응답을 받을 때까지 `TOPOLOGY_REMOVED`를 주기 resync마다
다시 보낸다. 미확인 tombstone 목록은 RAM(`s_pending_removed_*`)과 함께 Power Base의 기본
`nvs` 파티션(namespace `pb_rm_journal`, key `uuids`)에 기록한다.

- 목록이 바뀌면 exporter task가 다음 loop 시작과 **모든 CoAP 전송 시도 직전**에 flash에
  commit한다. Linux가 응답하지 않아 resync가 길게 막혀 있어도 한 번의 시도·backoff 안에 기록된다.
- 같은 UUID가 다시 topology 등록되면 목록에서 지우고 그 변경도 다음 전송 전에 commit한다.
- 2.xx를 받으면 목록에서 지운다.
- 부팅 시 목록을 다시 읽어 첫 주기 resync(기본 5 s)부터 재전송한다. 그 사이 다시 등록된
  UUID는 보내지 않고 목록에서 지운다.
- flash 쓰기에 실패하면 다음 loop 또는 다음 전송 시도(최대 200 ms 뒤)에서 다시 쓴다.

따라서 분리 후 Linux가 tombstone을 받기 전에 통신이 끊기고 Power Base가 재부팅해도,
Linux는 결국 삭제 상태로 수렴한다. 남는 창은 분리 확정과 다음 commit 사이(보통 수 ms,
최대 한 CoAP attempt와 backoff, 약 0.5 s)에 Power Base 전원이 끊기는 경우뿐이다.

삭제된 모듈의 SAMPLE은 더 이상 내보내지 않는다. `sensor_root_aggregator`는 모듈의 마지막
sample을 계속 보관하므로, exporter는 topology registry에 있는 UUID의 sample만 resync한다.
이 조건이 없으면 삭제를 아직 적용하지 않은 Linux에서 같은 sample이 5 s마다 `duplicate`로
받아들여져 `online:true`와 멈춘 `sample_seq`가 계속 유지된다. 삭제가 적용된 Linux에서는 같은
sample이 와도 4.04로 거부되고 모듈이 다시 생기지 않는다.

Linux `ingest::Registry`의 removed marker는 (Power Base UUID, module UUID) 쌍으로 기록한다.
tombstone은 그 tombstone을 보낸 Power Base 아래의 항목만 지운다. 모듈을 Power Base A에서 떼어
B에 붙인 뒤 A의 tombstone이 늦게 도착해도 B 아래의 같은 모듈은 지워지지 않는다. Power Base가
하나일 때는 이전과 같다. removed marker는 최대 64개이며, 가득 차면 가장 오래된 marker를
버린다. 5.03으로 거절하면 Power Base가 영원히 재시도하고 모듈은 Linux에 남기 때문이다.

## 재도킹과 교차

- 다른 Power Base로 옮긴 모듈은 새 Base의 `online=true` 보고로 등록된다. 옛 Base가 stale
  sweep이나 tombstone으로 정리하기 전까지 Linux registry에는 두 Base 아래에 잠시 함께 있다.
  API는 UUID마다 한 항목만 보여 준다. online인 항목을 우선하고, 둘 다 online이면 가장 최근에
  online으로 보고된 쪽(`attach_order`)을 쓴다. 이름·방·Matter endpoint ID는 module UUID
  기준이라 Base를 옮겨도 유지된다.

- 재도킹 보고(`online=true`)는 removed marker를 지우고 새 모듈처럼 등록한다.
- endpoint ID는 `endpoint_map.bin`의 UUID 예약을 그대로 쓰므로 같은 UUID는 같은 endpoint를
  받는다. Bridged Device Basic Information의 UniqueID와 EndpointUniqueID도 UUID에서 만든다.
- 같은 endpoint ID로 다시 만들기 때문에 컨트롤러가 기록한 `NodeLabel`/`DeviceLocation`
  (endpoint ID 기준으로 SDK가 저장)이 다시 로드된다. 제거 시 `kClusterShutdown`을 써서 이
  데이터를 지우지 않는다. PogoHome iOS 앱의 이름·방 설정은 module UUID 기준 SwiftData에
  저장되므로 같은 UUID가 다시 보이면 적용된다.
- tombstone과 재도킹이 교차하면 최신 물리 상태가 이긴다. 재도킹 뒤 오래된 tombstone은
  전송 직전 registry 재확인으로 버린다. 이미 전송 중이던 tombstone이 늦게 도착해도 다음
  주기 resync의 `online=true`가 다시 등록한다.
- Matter 동적 endpoint slot은 비어 있는 가장 낮은 index를 재사용하므로 분리·재도킹을
  반복해도 `CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT`(64)를 소모하지 않는다.

## 진단 로그

전이나 실패가 있을 때만 UUID별로 한 줄씩 남긴다. 성공한 일반 sample은 기록하지 않는다.

| 경계 | 로그 |
|---|---|
| Power Base 삭제 | `W (power_base_transport) removed uuid=..xxxx (power base DOWNSTREAM detach cascade)` 또는 `(confirmed detach reported by ..yyyy)` |
| exporter 미동작 | `W (power_base_transport) no bridge observer registered -- N removal(s) will NOT be reported to Linux` |
| pending 등록 | `I (linux_bridge_exporter) removal ..xxxx: tombstone pending for Linux` |
| flash 기록 | `I (linux_bridge_exporter) removal journal: N undelivered tombstone(s) now on flash` |
| 전달 성공 | `I (linux_bridge_exporter) removal ..xxxx: delivered, Linux answered 2.04` (중복이면 2.03) |
| 무응답 | `W (linux_bridge_exporter) removal ..xxxx: no response from Linux after 3 attempts -- kept pending ...` |
| Linux 거부 | `W (linux_bridge_exporter) removal ..xxxx: REJECTED by Linux with 4.00 -- kept pending ...` |
| 재결합 취소 | `I (linux_bridge_exporter) removal ..xxxx: pending tombstone cancelled -- re-attached ...` |
| 재부팅 복원 | `I (linux_bridge_exporter) removal ..xxxx: undelivered tombstone restored from flash` |
| Linux 수신 | `[topology-removed] pb=..xxxx mod=..xxxx removed (confirmed POGO detach)` / `duplicate (already removed)` |

실패 로그는 결과가 바뀔 때만 다시 찍는다. 5 s마다 재시도해도 같은 4.00이 반복되면 한 번만 남는다.
TOPOLOGY_REMOVED(msg_type 4)를 모르는 이전 Linux ingest는 이 메시지를
`[malformed] uri=pogo/v1/topology payload_len=22 -- rejected (4.00)`로 기록한다.

## 테스트

| 테스트 | 내용 |
|---|---|
| `tools/test_linux_bridge_exporter_reboot_removal.c` | 재부팅 전후 삭제 수렴, stale은 삭제 아님, 재부팅과 재도킹 교차, journal 쓰기 실패 재시도, 긴 resync 중 분리 |
| `tools/test_linux_bridge_exporter_detach_paths.c` | Power Base 직접 분리 연쇄 삭제, 말단만 분리, 말단 자체 보고, 무응답→4.00→2.04 재시도, 빠른 재결합 취소, 빈 slot 위 노드 resync, 삭제 뒤 sample 미전송 |
| `tools/test_linux_bridge_exporter_rapid_reattach.c` | 한 부팅 안의 빠른 분리·재도킹 |
| `linux/matter_bridge/app/core/tests/test_removal_convergence.cpp` | 실제 Registry → BridgeManager: offline→삭제→같은 endpoint 복원, 늦은 tombstone 교차 |
| `linux/matter_bridge/app/api/tests/test_http_server.cpp` | SSE에서 stale offline → 삭제 → 재등록 |
| `linux/matter_bridge/ingest/tests/test_registry_removal.cpp` | removed marker, 64개 초과 삭제 |
| `linux/matter_bridge/ingest/tests/test_registry_and_session.cpp` | 삭제 뒤 같은 sample 재전송은 4.04, 모듈·generation 불변 |
