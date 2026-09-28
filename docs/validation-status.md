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
| hot-swap 복구 | 확인 | 확인 | timeout/liveness 손실 뒤 cooldown 재시작 |
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

## 기록 원칙

새 결과를 추가할 때는 사용한 보드, 설정 이미지, 앱 커밋, 시험 경로를 함께 적는다. 로그를
직접 확인하지 못한 결과도 실물 시험 기록으로 남길 수 있지만, 확인 범위를 넓혀 쓰지 않는다.
host test와 빌드는 실물 동작의 대체 근거로 사용하지 않는다.
