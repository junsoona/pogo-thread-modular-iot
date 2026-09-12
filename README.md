# POGO 기반 Thread 자동 페어링 -- MVP

요구사항/현재 상태는 `CLAUDE-CODE-BOOTSTRAP.md` 참고. 이 문서는 빌드·플래시 절차만 다룬다.

## 구조

```
components/pogo_proto/   상위·하위 공용: UART transport, 프레임 codec(CRC16), 프로토콜 payload
                          보드 설정(UART 번호/핀/보드레이트/타임아웃)은 전부 여기 Kconfig 한 곳에 있음
upper/                    상위 모듈 (Thread Leader/Router, FTD)
lower/                    하위 모듈 (Thread SED, MTD)
```

`upper`, `lower`는 각각 독립된 ESP-IDF 프로젝트이며, `EXTRA_COMPONENT_DIRS`로 `components/`를 공유한다.

## 중요: 빌드 디렉터리는 반드시 이 저장소 밖(ASCII 경로)으로 지정할 것

이 저장소 경로에 한글 디렉터리명(`학교`, `캡스톤`)이 포함되어 있는데, ESP-IDF v6.1의 picolibc
spec 파일 생성 스크립트가 이 경로를 손상시켜(한글 구간이 개행 문자로 깨짐) 빌드가
`cannot read spec file` 에러로 실패한다. 이는 이 프로젝트 코드의 문제가 아니라
ESP-IDF 툴체인이 비-ASCII(CJK) 절대 경로를 다루는 방식의 버그다.

**해결책:** 소스는 지금 위치 그대로 두고, 빌드 디렉터리만 `-B`로 ASCII 경로에 지정한다.

```bash
source ~/.espressif/v6.1/esp-idf/export.sh

cd upper
idf.py -B ~/esp-builds/pogo_upper set-target esp32h2
idf.py -B ~/esp-builds/pogo_upper build

cd ../lower
idf.py -B ~/esp-builds/pogo_lower set-target esp32h2
idf.py -B ~/esp-builds/pogo_lower build
```

이후 `build`, `flash`, `monitor`, `flash monitor` 등 모든 `idf.py` 명령에 매번
동일한 `-B ~/esp-builds/pogo_upper` / `-B ~/esp-builds/pogo_lower`를 붙여야 한다.
(각 프로젝트 폴더 안에 남는 `sdkconfig`, `sdkconfig.defaults`는 정상이며 문제와 무관하다.)

이미 두 프로젝트 모두 위 명령으로 빌드 성공을 확인했다 (아래 "빌드 결과" 참고).

## 플래시 (사용자가 실행)

포트는 재연결 시 바뀔 수 있으니 `ls /dev/tty.usbmodem*`로 먼저 확인.
현재 확인된 값: 하위 `/dev/tty.usbmodem113301`, 상위 `/dev/tty.usbmodem113401`.

```bash
# 하위 먼저 (상위가 하위를 찾는 구조가 아니라 PING을 계속 보내므로 순서는 상관없음)
idf.py -B ~/esp-builds/pogo_lower -p /dev/tty.usbmodem113301 flash monitor

# 다른 터미널에서 상위
idf.py -B ~/esp-builds/pogo_upper -p /dev/tty.usbmodem113401 flash monitor
```

`monitor`는 `Ctrl+]`로 종료.

## 배선 (POGO UART MVP)

| 상위 | 하위 |
|---|---|
| GPIO10 (TX) | GPIO11 (RX) |
| GPIO11 (RX) | GPIO10 (TX) |
| GND | GND |

두 보드 모두 각자 USB로 전원 공급 중일 때는 5V/3.3V 선은 연결하지 않는다 (GND·TX·RX만).
핀/UART 번호/보드레이트를 바꾸려면 `idf.py -B <build-dir> menuconfig` -> `Component config`
-> `POGO UART Protocol` (양쪽 프로젝트 모두 동일 항목).

## 기대 로그

**하위 (`lower_main`, `lower_thread`, `lower_pairing`, 태그 기준)**
```
I (..) lower_thread: OpenThread MTD/SED ready (poll period 3000ms)
I (..) lower_main: lower module ready -- waiting for POGO commands (PING/IDENTIFY/...)
I (..) lower_pairing: dataset stored (N bytes) -- starting Thread join as SED    # 도킹 후, N은 실제 TLV 길이
I (..) lower_pairing: Thread join succeeded, otDeviceRole=2                      # role=2 == CHILD
```

**상위 (`upper_main`, `upper_thread`, `upper_pairing`)**
```
I (..) upper_thread: no dataset in NVS -- forming a new Thread network from Kconfig defaults
I (..) upper_main: upper module ready -- waiting for POGO dock (PING/PONG)
I (..) upper_pairing: PONG received -> DOCKED                # 배선 연결 후
I (..) upper_pairing: lower identified: mac=...
I (..) upper_pairing: uuid+slot assigned (slot=1)
I (..) upper_pairing: dataset accepted (.. bytes) -- waiting for join result
I (..) upper_pairing: JOIN OK -- lower otDeviceRole=2
```

Network Key가 포함된 Dataset 원문, UUID 값은 로그에 절대 출력되지 않는다 (요구사항).

## 빌드 결과 (이번 세션에서 확인됨)

- `idf.py -B ~/esp-builds/pogo_upper build` -> 성공, `pogo_upper.bin` 생성 (앱 파티션의 약 48% 여유)
- `idf.py -B ~/esp-builds/pogo_lower build` -> 성공, `pogo_lower.bin` 생성 (앱 파티션의 약 54% 여유)
- 두 빌드 모두 ESP-IDF v6.1 / esp32h2 타겟, 경고 없이 링크 완료.

## 아직 실물 보드에서 확인되지 않은 항목

- 실제 POGO UART 배선 상태에서의 PING/PONG 도킹 감지 동작.
- 하위가 상위로부터 받은 Active Dataset TLV로 실제 Thread network에 join하는 것
  (Leader 쪽 dataset 형성 자체는 ESP-IDF OpenThread 표준 API 호출이며, 이 조합이
  두 보드 사이 무선으로 성립하는지는 미검증).
- 하위가 SED(rx-off-when-idle, MTD)로 유지되며 Router로 승격되지 않는지.
- CRC 오류/조각난 UART 스트림에 대한 프레임 파서의 실제 재동기화 (단위 테스트 없이
  코드 검토 + `pogo_crc16_selftest()` 부팅 시 자체 점검만 확인됨).
- 재도킹/재부팅 시나리오 (특히 이미 JOINED 상태에서 다시 SET_DATASET을 받는 경우의
  OpenThread 쪽 동작) -- 이번 범위(1~5단계, 최초 1회 도킹->자동 접속)에서는 다루지 않음.
