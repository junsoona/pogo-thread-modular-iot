# 소프트웨어 구조

이 저장소에는 세 종류의 ESP32-H2 펌웨어와 Linux 애플리케이션이 있다.

- `module/`: 모든 센서 모듈이 공유하는 펌웨어
- `power_base/`: POGO 체인의 시작점이 되는 전원 모듈 펌웨어
- `rcp/`: Linux OTBR용 USB Radio Co-Processor
- `upper/`, `lower/`: 초기 MVP 회귀 시험용 펌웨어
- `linux/matter_bridge/`: 센서 데이터를 Matter endpoint로 변환하는 Linux 앱

Power Base의 고정 애플리케이션 역할과 OpenThread의 `otDeviceRole`은 분리되어 있다.
Power Base는 항상 POGO 체인의 루트지만 Thread에서는 CHILD, ROUTER, LEADER 중 하나가 될 수
있다. 센서 모듈의 LEAF와 INTERMEDIATE도 POGO 토폴로지 역할이며 Thread 역할 이름이 아니다.

## 공통 센서 모듈

모든 센서 보드에는 같은 `pogo_module.bin`을 플래시한다. 센서 종류와 핀 배치는
`module_config.bin`으로 정한다.

```text
pogo_module.bin
  + module_config.bin
  + 실제 POGO 연결 상태
  = 센서 드라이버, LEAF/INTERMEDIATE 역할, Thread 가입 정보
```

보드 위치를 바꿔도 앱 바이너리는 바뀌지 않는다. AUTO 모드에서 모듈은 UPSTREAM 연결과
DOWNSTREAM peer 상태를 보고 LEAF 또는 INTERMEDIATE를 선택한다. ROOT는 Power Base의
애플리케이션 역할이며 AUTO 센서 모듈에는 할당하지 않는다.

## 컴포넌트

### POGO 계층

- `pogo_proto`: frame, CRC16, message payload codec
- `pogo_port`: UART와 DETECT GPIO, debounce, HELLO, PING/PONG, hot-swap 복구
- `role_manager`: 포트 상태에서 LEAF/INTERMEDIATE를 계산
- `child_service`: UPSTREAM 프로비저닝 응답과 자신의 Thread join
- `parent_service`: DOWNSTREAM 자식 프로비저닝
- `node_calc`: polling period와 UUID 기반 backoff 계산

UPSTREAM과 DOWNSTREAM은 독립된 `pogo_port` instance다. 한 모듈이 위쪽에는 child,
아래쪽에는 parent로 동시에 동작할 수 있다.

### Thread와 데이터 계층

- `thread_manager`: OpenThread 초기화, Dataset 적용, 역할별 link mode
- `collector_endpoint`: Power Base UUID, Mesh-Local EID, CoAP port 캐시
- `sensor_channel`: 센서 sample wire format
- `sensor_transport`: module에서 Power Base `/s`로 sample 전송
- `topology_proto`: topology report wire format
- `topology_reporter`: module에서 Power Base `/t`로 상태 전송
- `topology_gate`: topology 등록 전 sample 전송 차단
- `coap_response_timing`: SED를 고려한 공통 CoAP 응답 대기 계산

센서 sample의 목적지는 현재 Thread Leader가 아니다. POGO `SET_COLLECTOR`로 전달받은
Power Base endpoint를 사용한다.

### 센서 계층

- `module_config`: NVS 설정 로드와 GPIO 검증
- `sensor_provider`: 센서 driver interface와 registry
- `sensor_service`: 측정 직렬화, 주기 측정, sample sequence 관리
- `sht45`, `bh1750`, `sgp40`, `sgp41`, `pms5003`: 실제 driver
- `sensirion_crc8`: SGP4x word CRC
- `sensirion_gas_index_algorithm`: Sensirion 공식 Gas Index 알고리즘

한 모듈은 한 센서와 영구적으로 짝을 이룬다. 같은 driver registry를 쓰지만 실행 중에는
설정의 `driver_id` 하나만 초기화한다.

### Power Base 계층

- `power_base_config`: POGO 포트와 전원 설정
- `power_base_thread_config`: STANDALONE/EXTERNAL_OTBR 설정
- `power_base_bridge_config`: Linux bridge 목적지
- `power_base_transport`: CoAP `/s`, `/t` 수신
- `topology_registry`: UUID 기반 topology 상태
- `sensor_root_aggregator`: 최신 sample 저장과 중복 제거
- `linux_bridge_exporter`: Linux ingest로 SAMPLE/TOPOLOGY/HEARTBEAT 전송
- `bridge_export_proto`: Linux 북향 wire format

Power Base에는 센서 driver와 `child_service`가 없다. `parent_service`는 context callback으로
의존성을 주입받아 센서 INTERMEDIATE와 Power Base 양쪽에서 같은 구현을 사용한다.

## module 부팅 흐름

`module/main/app_main.c`는 다음 순서로 초기화한다.

1. NVS 초기화
2. sensor driver registry 등록
3. `module_config`와 dev mode 읽기
4. OpenThread 초기화
5. 설정에 맞는 sensor provider 초기화
6. `topology_gate`, `sensor_transport`, sample callback 초기화
7. `role_manager` 초기화
8. UPSTREAM/DOWNSTREAM `pogo_port` 초기화
9. `child_service`, `parent_service` 초기화
10. `topology_reporter` 초기화
11. 설정에서 enabled인 포트만 활성화

초기화 뒤에는 port event와 Thread state callback으로 동작한다.

UPSTREAM 연결이 끊기면 현재 provisioning 상태와 collector cache를 지우고 Thread를
중지한다. DOWNSTREAM 연결이 끊기면 자식 세션만 정리하며 자신의 Thread 가입은 유지한다.

## 역할별 Thread 설정

### INTERMEDIATE

- always-on
- FTD
- `rx-on-when-idle=true`
- router eligible
- downstream parent service 활성 가능

### LEAF

- SED
- `rx-on-when-idle=false`
- router eligible=false
- 별도 `otLinkSetPollPeriod()` 사용

센서 측정 주기와 SED data poll 주기는 서로 다른 설정이다. 측정 주기는 POGO
`POLL_CONFIG`, data poll은 `thread_manager` Kconfig 기본값을 사용한다.

## Power Base 부팅 흐름

Power Base는 고정 설정과 Dataset 설정을 읽고 always-on FTD로 Thread를 시작한다.
attach supervisor가 CHILD/ROUTER/LEADER를 확인하면 CoAP transport, parent service,
POGO port, stale sweep, 선택적 Linux exporter를 한 번만 시작한다.

STANDALONE은 자체 Dataset으로 네트워크를 시작하고, EXTERNAL_OTBR는 Linux OTBR의
Active Dataset으로 기존 네트워크에 가입한다. 센서 모듈은 두 경우 모두 POGO
`SET_DATASET`으로 같은 정보를 받는다.

자세한 내용은 [`power-base.md`](power-base.md)와 [`otbr-rcp.md`](otbr-rcp.md)를
참고한다.

## 센서 sample 흐름

```text
sensor_service periodic task
  -> measure_once()
  -> sample_seq 증가
  -> topology_gate 확인
  -> sensor_transport Confirmable POST /s
  -> Power Base topology registry 확인
  -> sensor_root_aggregator 저장
  -> linux_bridge_exporter pending 표시
  -> Linux ingest Registry
  -> Matter Bridge attribute 갱신
```

Thread가 detached된 동안에도 센서 측정 task는 계속 실행하지만 무선 handoff는 하지 않는다.
재가입 뒤 다음 sample부터 전송한다.

Power Base가 4.04 또는 4.12로 topology 상태가 유효하지 않음을 알리면 module은 gate를 닫고
재등록을 요청한다. 같은 sample의 남은 retry는 즉시 중단한다.

## topology 흐름

```text
module role/parent/depth 상태
  -> topology_reporter
  -> Confirmable POST /t
  -> Power Base topology_registry
  -> topology_gate open
  -> Linux topology snapshot
  -> Matter Reachable
```

Power Base는 depth>1 노드의 parent가 online이고 downstream이 연결된 경우에만 보고를
받는다. 상위 링크가 끊기면 해당 subtree를 offline으로 바꾼다.

## POGO 프로토콜 확장

초기 `upper/`, `lower/` MVP 이후 다음 메시지를 추가했다.

- `PEER_HELLO`
- `SENSOR_HELLO` / `SENSOR_INFO_ACK`
- `POLL_CONFIG`
- `ASSIGN_PARENT_INFO`
- `SET_COLLECTOR`
- `JOIN_RESULT_ACK`

`ASSIGN_SLOT`에는 `topology_depth`가 추가됐다. 기존 참조 프로젝트도 같은
`pogo_proto`를 빌드하며 추가 필드를 명시적으로 채운다.

## 오류 처리 원칙

- 고정 설정이 잘못되면 센서, POGO, Thread를 시작하지 않는다.
- wire CRC 오류는 parser가 다음 SOF에서 재동기화한다.
- timeout과 liveness 손실은 제한된 재시도와 cooldown을 사용한다.
- protocol version, capability, peer type 충돌은 물리적 재체결 전까지 유지한다.
- Dataset 검증 실패는 fallback 없이 중단한다.
- host test 성공과 실물 성공을 문서에서 구분한다.

현재 검증 범위는 [`validation-status.md`](validation-status.md)에 정리한다.
