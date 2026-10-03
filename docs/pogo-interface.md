# POGO 인터페이스

센서 모듈은 UPSTREAM과 DOWNSTREAM 두 포트를 가진다. Power Base는 센서 모듈의
DOWNSTREAM과 같은 전기 구성을 가진 포트 하나를 사용한다.

각 POGO 포트는 다음 신호로 구성된다.

- UART TX
- UART RX
- DETECT_OUT
- DETECT_IN
- GND
- BASE_ID 또는 전원 접점

현재 프로토타입에서는 POGO 5V를 사용하지 않는다. 전원 접점은 최종 하드웨어 단계에서
추가하며 각 보드는 USB로 따로 전원을 받는다.

## GPIO

### 센서 모듈

| 기능 | UPSTREAM | DOWNSTREAM |
|---|---:|---:|
| UART controller | UART0 | UART1 |
| TX | GPIO4 | GPIO10 |
| RX | GPIO5 | GPIO11 |
| DETECT_IN | GPIO12 | GPIO23 |
| DETECT_OUT | GPIO2 | GPIO3 |
| BASE_ID | GPIO14 | -1 |

### Power Base

| 기능 | GPIO |
|---|---:|
| UART controller | UART1 |
| TX | GPIO10 |
| RX | GPIO11 |
| DETECT_IN | GPIO23 |
| DETECT_OUT | GPIO3 |

Power Base에서 첫 모듈로 연결할 때 TX/RX와 DETECT_IN/OUT을 교차한다.

```text
Power Base GPIO10 TX        -> module GPIO5 RX
Power Base GPIO11 RX        <- module GPIO4 TX
Power Base GPIO3 DETECT_OUT -> module GPIO12 DETECT_IN
Power Base GPIO23 DETECT_IN <- module GPIO2 DETECT_OUT
GND                         -- GND
BASE_ID GND contact         -- module GPIO14
```

중간 모듈과 다음 모듈도 같은 방식으로 DOWNSTREAM에서 UPSTREAM으로 교차한다.

## DETECT 신호

`DETECT_OUT`은 open-drain active-low 출력이다.

- 분리 상태: release
- 연결 상태: low
- `DETECT_IN`: 내부 pull-up 입력
- active-high 설정은 config validation에서 거부

ISR은 현재 level을 읽어 queue에 넣는 일만 한다. debounce와 상태 전이는 task에서 처리한다.
queue overflow나 ISR 누락에 대비해 주기적으로 실제 GPIO level과 내부 상태를 다시 맞춘다.

연결을 감지하면 짧은 settle 시간을 둔 뒤 UART parser를 초기화하고 RX buffer를 flush한
다음 HELLO를 시작한다. 분리 level이 확인되면 진행 중인 handshake와 cooldown을 즉시
취소하고 `WAIT_ATTACH`로 돌아간다.

## BASE_ID

BASE_ID는 첫 센서 모듈이 Power Base에 직접 연결됐는지 판단하는 UPSTREAM 전용 입력이다.
Power Base 커넥터가 GPIO14를 GND에 연결한다.

BASE_ID만으로 역할을 확정하지 않는다. `PEER_HELLO`의 peer type과 함께 확인한다.
BASE_ID는 Power Base를 뜻하는데 peer가 MODULE이라고 응답하거나 그 반대이면
`PEER_TYPE_CONFLICT`로 처리한다.

DOWNSTREAM에는 BASE_ID가 없으며 config 값은 항상 `-1`이다.

## PEER_HELLO

체결 뒤 두 포트는 `PEER_HELLO_REQ`와 `PEER_HELLO_RESP`를 교환한다. payload에는
protocol version, peer type, capability bitmap이 들어 있다.

확인 항목:

- protocol version 일치
- 상대 peer type이 MODULE 또는 POWER_BASE
- BASE_ID와 peer type 일치
- 필수 capability 지원

HELLO가 끝나야 `PEER_READY` event를 보낸다. provisioning service는 UART의 임의 첫
frame이 아니라 `PEER_READY`부터 시작한다.

## 포트 상태

```text
DISABLED
  -> WAIT_ATTACH
  -> ATTACH_SETTLE
  -> HANDSHAKE
  -> PEER_READY
  -> ERROR / WAIT_ATTACH
```

### 일시적 오류

- `DOCK_TIMEOUT`
- `LIVENESS_LOST`

AUTO 모드에서 DETECT_IN이 계속 연결 상태라면 cooldown 뒤 새 DETECT edge 없이 handshake를
다시 시작한다. 한 cycle 안에서는 `CONFIG_POGO_MAX_RETRY`만큼 재시도하고, cycle이
끝나면 cooldown을 둔다. busy loop를 만들지 않는다.

cooldown 중에도 상대의 유효한 `PEER_HELLO_REQ`에는 응답하고 바로 새 handshake를 시작한다.
이렇게 하지 않으면 양쪽 재시도 주기가 같아 어긋난 채로 계속 엇갈린다. 응답만으로
`PEER_READY`가 되지는 않으며, HELLO 검사와 영구 오류 처리는 handshake 중과 같다.

### 영구 오류

- `PEER_TYPE_CONFLICT`
- `PROTOCOL_VERSION_ERROR`
- `CAPABILITY_MISMATCH`

이 오류는 물리적 detach/reattach 전까지 유지한다. 시간이 지난다고 호환성 문제가
해결되지는 않기 때문이다.

## PING/PONG liveness

`PEER_READY` 뒤 유휴 상태에서는 PING/PONG으로 연결 상태를 확인한다. PING/PONG도 같은
frame parser와 UART 송신 경로를 사용하며 별도 byte parser를 두지 않는다.

연속 timeout이 한도를 넘으면 `LIVENESS_LOST`를 발생시킨다. DETECT pin이 연결 상태면
cooldown 뒤 새 handshake를 시작하고, 분리 상태면 즉시 `WAIT_ATTACH`로 돌아간다.

## frame 형식

POGO frame은 SOF, version/type, sequence, payload length, payload, CRC16으로 구성한다.
parser는 partial input을 누적하고 잘못된 길이 또는 CRC에서 다음 SOF를 찾아 재동기화한다.

request retry는 같은 sequence를 사용한다. child service는 write message의 중복 sequence를
인식해 이미 적용한 동작을 다시 실행하지 않고 캐시한 응답을 보낸다.

## 역할 판정

AUTO 센서 모듈은 다음 규칙을 사용한다.

| UPSTREAM | DOWNSTREAM | 결과 |
|---|---|---|
| Power Base 또는 Module에 연결 | Module 연결 | INTERMEDIATE |
| Power Base 또는 Module에 연결 | 분리 | LEAF |
| Power Base 또는 Module에 연결 | 연결됐지만 `DOCK_TIMEOUT`/`LIVENESS_LOST` 재시도 중 | 직전 LEAF/INTERMEDIATE 유지 (없으면 LEAF) |
| 분리 | 임의 | UNASSIGNED |
| 오류(종류 무관) | 임의 | ROLE_ERROR |
| 임의 | peer type/version/capability 충돌 | ROLE_ERROR |

우선순위와 DOWNSTREAM 일시 오류 격리 규칙은 [`state-machine.md`](state-machine.md)의
role_manager 절을 따른다.

센서 모듈 AUTO 경로에서 ROOT를 만들지 않는다. Power Base가 POGO 체인의 고정 루트다.
INTERMEDIATE와 LEAF는 Thread의 ROUTER/CHILD 이름과 별개다.

개발 회귀용 `dev_root.json`, `dev_leaf.json`은 DETECT와 BASE_ID가 없는 단일 UART
fixture를 위해 남겨 두었다. 실제 최종 프로토타입에는 AUTO 설정을 사용한다.

## GPIO 선택 기록

초기 시험에서 UPSTREAM DETECT_IN을 GPIO13으로 검토했지만 SuperMini의 onboard LED와
겹쳐 제외했다. GPIO6도 후보였으나 해당 패키지에서 외부로 나오지 않아 사용할 수 없었다.

그 뒤 GPIO2를 DETECT_IN으로 사용했지만 terminal LEAF의 wake 후보를 남기기 위해 최종적으로
GPIO12와 GPIO2의 역할을 바꿨다.

```text
현재: DETECT_IN=GPIO12, DETECT_OUT=GPIO2
```

GPIO12는 ESP32-H2 RTC GPIO 조건을 만족한다. 이 선택은 wake 가능성만 확보한 것이며
Deep-sleep 코드가 구현됐다는 뜻은 아니다.

DOWNSTREAM DETECT_OUT은 처음 GPIO22였지만 SuperMini header에 나오지 않아 납땜이
필요했다. 보드 손상 위험과 조립 복잡도를 줄이기 위해 GPIO3으로 옮겼다.

```text
현재: DOWNSTREAM DETECT_IN=GPIO23, DETECT_OUT=GPIO3
```

GPIO3은 출력 전용으로 사용하며 wake source가 아니다.

## GPIO 검증

`module_config`와 config packer는 같은 ESP32-H2 정책을 검사한다.

- 허용 범위: `-1` 또는 GPIO0..27
- strapping pin 제외
- flash pin 제외
- USB Serial/JTAG pin 제외
- 패키지에서 나오지 않은 pin 제외
- 같은 GPIO 중복 사용 금지
- enabled port는 UART0 또는 UART1
- AUTO port는 DETECT_IN/OUT 필수
- AUTO UPSTREAM은 BASE_ID 필수
- DOWNSTREAM BASE_ID는 항상 `-1`

정확한 목록은 `components/module_config/gpio_profile.c`에 있고
`tools/pack_module_config.py`가 같은 값을 사용한다. parity test가 둘의 차이를 검사한다.

## PMS5003 예외

PMS5003은 센서 자체가 UART를 사용하므로 terminal LEAF 전용이다.

- sensor UART1
- MCU TX GPIO0
- MCU RX GPIO1
- 9600 8N1
- downstream POGO 비활성

UPSTREAM POGO는 UART0 GPIO4/5를 그대로 쓴다. GPIO0/1은 다른 I2C 센서의 SDA/SCL 위치와
맞춰 조립 혼동을 줄였지만, PMS5003 설정에서는 I2C가 아니라 UART로 동작한다.

## 콘솔

POGO가 UART0과 UART1을 모두 사용하므로 일반 UART console을 쓰지 않는다. 개발 로그와
플래시는 ESP32-H2 USB Serial/JTAG를 사용한다. USB D-/D+ 핀은 POGO나 센서 기능에
할당하지 않는다.
