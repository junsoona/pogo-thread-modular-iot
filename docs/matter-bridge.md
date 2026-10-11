# Linux Matter Bridge

> 공개용 기록: 보드 식별값·계정·포트·주소는 예시로 바꿨습니다. 명령 실행 시 자신의 실제 값으로 바꾸세요. 원본 근거는 비공개로 보존했습니다.


`pogo_matter_bridge`는 Power Base가 보낸 CoAP 데이터를 받아 Matter 동적 endpoint로
공개하는 Linux 애플리케이션이다. `linux/matter_bridge/ingest`와 connectedhomeip
v1.6.0.0 Matter stack을 한 프로세스에서 실행한다.

이 구현은 졸업작품용 bridge다. Matter 인증 제품을 목표로 하지 않는다.

## 데이터 흐름

```text
sensor module
  -> Thread CoAP /s, /t
Power Base
  -> bridge_export_proto over CoAP/UDP
Linux wpan0
  -> ingest::Registry
BridgeManager
  -> CHIP dynamic endpoint
Apple Home / Google Home / chip-tool
```

Linux ingest는 SAMPLE, TOPOLOGY_SNAPSHOT, HEARTBEAT를 검증한 뒤 UUID 기반 registry에
적용한다. Matter 쪽 watcher는 registry generation이 바뀌면 snapshot을 읽고 필요한 endpoint
생성과 attribute 갱신을 Matter thread에 예약한다.

## 프로토콜 수신

ingest server는 `--interface`로 지정한 IPv6 interface와 UDP port에서 수신한다.

- Confirmable POST만 상태를 변경한다.
- Non-confirmable 요청은 응답 없이 버린다.
- `IPV6_RECVPKTINFO`로 실제 수신 interface와 목적지 주소를 확인한다.
- ACK는 `sendmsg()`와 `IPV6_PKTINFO`를 사용해 요청이 도착한 로컬 주소를 source로 쓴다.
- 잘린 datagram이나 ancillary data는 registry에 전달하지 않는다.
- CRC는 전송 오류 검출용이며 인증 수단이 아니다.

CoAP 응답 코드는 다음과 같다.

| 응답 | 의미 |
|---|---|
| 2.04 | 새 상태 적용 |
| 2.03 | 정확한 중복 |
| 4.00 | 잘못된 payload |
| 4.02 | 알 수 없는 critical option |
| 4.04 | topology에 없는 UUID |
| 4.05 | Confirmable이지만 POST가 아님 |
| 4.12 | offline, bitmap 불일치, 오래된 generation/sequence |
| 5.03 | 저장소 busy/full |

bridge port는 Thread 보안 경계 안에서만 열어 둔다. 외부 LAN이나 인터넷에 공개하지 않는다.

## 식별자와 endpoint

module UUID가 bridged device의 영구 식별자다. `power_base_uuid`는 데이터 출처와 충돌
검사용 보조 키이며 Matter device identity가 아니다.

endpoint 구조:

- endpoint 0: Matter root
- endpoint 1: Aggregator
- endpoint 2: ZAP 코드 생성을 위한 비활성 placeholder
- endpoint 3 이상: module UUID당 동적 endpoint 하나

`EndpointStore`는 `<state-dir>/endpoint_map.bin`에 UUID와 endpoint ID를 저장한다.
쓰기 순서는 임시 파일, `fsync`, rename, directory `fsync`다. CRC가 맞지 않으면 endpoint를
새로 배정하지 않고 시작을 거부한다. 프로세스 재시작 뒤에도 같은 UUID가 같은 endpoint를
사용한다.

같은 UUID가 두 Power Base에서 동시에 online으로 보이면 충돌로 처리하고 갱신을 보류한다.

## 센서와 Matter cluster

| 센서 채널 | Matter cluster | ID |
|---|---|---:|
| Temperature | Temperature Measurement | 0x0402 |
| Humidity | Relative Humidity Measurement | 0x0405 |
| PM1.0 | PM1 Concentration Measurement | 0x042C |
| PM2.5 | PM2.5 Concentration Measurement | 0x042A |
| PM10 | PM10 Concentration Measurement | 0x042D |
| Illuminance | Illuminance Measurement | 0x0400 |
| VOC/NOx Gas Index | 공개하지 않음 | - |

각 동적 endpoint에는 Descriptor, Bridged Device Basic Information, Identify를 두고,
센서 bitmap에 맞는 measurement cluster를 추가한다. PMS5003 endpoint에는 Air Quality
Sensor device type의 필수 Air Quality cluster도 추가한다.

### Temperature

wire 값은 milli-degree Celsius다. Matter의 0.01°C 단위로 반올림한다.

```text
matter_value = round(milli_celsius / 10)
```

허용 범위 밖이거나 scale이 다르면 적용하지 않는다.

### Humidity

wire 값은 milli-%RH다. Matter의 0.01%RH 단위로 반올림한다. 0~10000 범위를 벗어나면
적용하지 않는다.

### PM

PMS5003 값은 이미 µg/m³ 단위 정수다. Matter Concentration Measurement의
`MeasuredValue` float로 전달하고 medium은 air, unit은 µg/m³로 설정한다.

### Illuminance

BH1750 wire 값은 milli-lux다. Matter Illuminance Measurement는 lux 자체가 아니라
로그 인코딩을 사용한다.

```text
lux < 1      -> 0
lux >= 1     -> round(10000 * log10(lux)) + 1
```

인코딩 범위는 `1..65534`이며 0은 “측정 가능한 범위보다 낮음”을 뜻한다.
`value_mapping.hpp`의 `kIlluminanceMeasuredValueMin/Max`를 변환과 cluster 설정에서 함께
사용해 범위가 달라지지 않게 했다.

### VOC와 NOx

SGP40/41의 VOC_INDEX, NOX_INDEX는 0~500 범위의 Sensirion Gas Index다. 실제 농도 단위가
아니므로 VOC Concentration cluster에 넣지 않는다. manufacturer-specific cluster도 현재
범위에는 없다. SGP40/41만 가진 모듈은 Matter endpoint를 만들지 않는다.

### 센서 오류

channel status가 `OK`가 아니면 마지막 숫자를 새 측정값처럼 다시 쓰지 않고 Matter
attribute를 null로 만든다. topology가 offline으로 바뀌면 endpoint를 삭제하지 않고
Bridged Device Basic Information의 `Reachable=false`만 적용한다. endpoint는 확정된 POGO
분리(`TOPOLOGY_REMOVED`)에서만 삭제한다. [`pogo-lifecycle.md`](pogo-lifecycle.md) 참고.

## Device type과 필수 cluster

사용하는 device type:

| 센서 | device type | 필수 cluster 처리 |
|---|---:|---|
| SHT45 | Temperature Sensor + Humidity Sensor | Identify 포함 |
| BH1750 | Light Sensor 0x0106 | Identify 포함 |
| PMS5003 | Air Quality Sensor 0x002C | Identify + Air Quality 포함 |

PMS5003의 PM 값만으로 Good/Fair/Poor 등급을 임의 계산하지 않는다. Air Quality attribute는
유효한 표준 값인 `Unknown(0)`으로 유지한다.

Identify command는 동적 endpoint에서 성공하도록 timer delegate와 effect handler를 둔다.
졸업작품에는 물리 LED 표시를 연결하지 않았으므로 IdentifyTime과 command 성공 여부만 확인한다.

## 센서 교체 감지

한 UUID에 생성된 Matter endpoint의 cluster 구성은 처음 등록된 sensor bitmap으로 정한다.
bridge가 실행 중인 상태에서 같은 보드의 설정 이미지만 바꿔 센서 종류가 달라지면 기존 endpoint
구성과 새 bitmap이 맞지 않는다.

`BridgeManager::SensorBitmapMismatches()`가 이 상태를 감지하며, endpoint에 없던 channel을
계속 적용하려 하지 않고 보류한다. 시험 중 센서를 바꿀 때는 bridge 프로세스만 재시작한다.
`endpoint_map.bin`과 CHIP KVS는 지우지 않는다. 같은 endpoint ID에 새 cluster 구성이
생성된다.

## 갱신 속도와 thread model

ingest server thread는 registry만 갱신한다. CHIP API는 Matter thread에서만 호출한다.

UUID별로 Matter work item 하나만 in-flight 상태가 될 수 있다. 일반 SAMPLE 변화는 기본
1000ms 간격으로 coalescing한다. 새 endpoint 생성과 Reachable 변화는 즉시 처리한다.

attribute 적용이 성공한 필드만 `CommitApplied()`로 기록한다. 실패한 필드는 다음
reconcile에서 다시 시도한다. Matter thread가 실제 적용할 때 최신 registry snapshot을
다시 읽으므로 오래된 queued 값을 쓰지 않는다.

## 빌드

Matter SDK는 connectedhomeip v1.6.0.0을 사용한다.

```bash
cd ~
git clone --branch v1.6.0.0 --depth 1 \
  https://github.com/project-chip/connectedhomeip.git
cd ~/connectedhomeip
./scripts/checkout_submodules.py --shallow --platform linux
source scripts/bootstrap.sh
```

bridge 저장소에서 빌드한다.

```bash
cd ~/pogo-thread-modular-iot
export CHIP_ROOT="$HOME/connectedhomeip"

PW_ACTIVATE_SKIP_CHECKS='' \
  tools/build_matter_bridge.sh "$HOME/pogo-builds/pogo-matter-bridge"
```

생성 파일:

```text
~/pogo-builds/pogo-matter-bridge/pogo_matter_bridge
```

SDK 비의존 테스트:

```bash
tools/test_matter_bridge.sh
```

이 스크립트는 ingest와 `app/core` 테스트를 실행한다. 실제 CHIP 링크는 Linux 호스트에서
`tools/build_matter_bridge.sh`로 확인한다.

## 실행

state directory를 먼저 만든다.

```bash
sudo install -d -o "$USER" -g "$USER" -m 700 \
  /var/lib/pogo-matter-bridge
```

직접 실행:

```bash
stdbuf -oL -eL \
  "$HOME/pogo-builds/pogo-matter-bridge/pogo_matter_bridge" \
  --interface wpan0 \
  --port 5684 \
  --state-dir /var/lib/pogo-matter-bridge \
  --KVS /var/lib/pogo-matter-bridge/chip-kvs
```

운영 환경에서는 `pogo-matter-bridge.service`가 `otbr-agent.service`와 함께 재시작되도록
설정한다. 상태와 로그:

```bash
sudo systemctl status otbr-agent pogo-matter-bridge --no-pager -l
sudo journalctl -u pogo-matter-bridge.service -f -o cat
```

5684 port를 이미 `ingest_server`가 쓰고 있으면 Matter Bridge가 시작하지 못한다. 독립
ingest smoke server를 종료한 뒤 하나만 실행한다.

**`--port`를 5684가 아닌 값으로 줄 때 (Linux bridge 0.3.0부터).** 관리 기능이 켜져 있으면(`--api-enroll-helper`)
bridge는 5684에도 socket을 하나 더 연다. 등록된 Power Base가 "허브가 지금 어디 있는가"를 묻는 `LOCATE`에만
답하는 socket이다([`power-base-management.md`](power-base-management.md) 12절 "locate 포트"). 시작 로그에
`ingest: locate port=5684 (answers a Power Base's LOCATE only)`가 나온다. 5684를 다른 프로그램이 쓰고 있으면
`the locate port 5684 could not be opened`를 남기고 그 socket 없이 돈다: 데이터 수신은 그대로지만, 이미
등록된 Power Base는 새 port를 스스로 찾지 못한다(재등록이 필요하다). `--port 5684`(기본)에서는 새로 여는
것이 없다.

## commissioning과 확인

프로세스가 처음 시작할 때 출력하는 QR code 또는 manual pairing code로 commissioner에
등록한다. 개발용 기본 manual code는 로그에 표시되지만, 최종 시연용 자격 증명은 명시적으로
설정하는 편이 좋다.

`chip-tool`에서 endpoint 목록을 확인한다.

```bash
CHIP_TOOL="$HOME/connectedhomeip/out/linux-x64-chip-tool/chip-tool"

"$CHIP_TOOL" descriptor read parts-list 1 1
```

endpoint의 UUID와 값을 읽는다.

```bash
"$CHIP_TOOL" bridgeddevicebasicinformation read unique-id 1 <ENDPOINT_ID>
"$CHIP_TOOL" temperaturemeasurement read measured-value 1 <ENDPOINT_ID>
"$CHIP_TOOL" relativehumiditymeasurement read measured-value 1 <ENDPOINT_ID>
"$CHIP_TOOL" illuminancemeasurement read measured-value 1 <ENDPOINT_ID>
"$CHIP_TOOL" pm25concentrationmeasurement read measured-value 1 <ENDPOINT_ID>
"$CHIP_TOOL" airquality read air-quality 1 <ENDPOINT_ID>
"$CHIP_TOOL" identify read identify-time 1 <ENDPOINT_ID>
"$CHIP_TOOL" identify identify 2 1 <ENDPOINT_ID>
```

`<ENDPOINT_ID>`는 꺾쇠까지 입력하는 문자열이 아니라 실제 숫자로 바꾼다.

## 확인된 실물 결과

Linux 호스트에서 connectedhomeip v1.6.0.0으로 최종 링크에 성공했다. 기존 Matter fabric과
KVS를 유지한 채 서비스를 다시 시작했고 endpoint ID도 유지됐다.

BH1750:

- endpoint 3
- UniqueID `020000fffe65d357`
- `MeasuredValue`가 센서를 가리고 밝히는 동안 0~22369 범위에서 변화
- `IdentifyTime: 0`
- Identify command status `SUCCESS`

PMS5003:

- endpoint 4
- UniqueID `020000fffe65d07a`
- `AirQuality: 0` (`Unknown`)
- `IdentifyTime: 0`
- Identify command status `SUCCESS`
- Illuminance read는 `UNSUPPORTED_CLUSTER`로 정상 거부

Apple Home과 Google Home에서도 일부 센서 값을 확인했지만 앱별 UI 지원 범위가 다르다.
cluster 구현 자체를 판단할 때는 `chip-tool` 값을 기준으로 한다.

## 남은 범위

- SHT45 endpoint의 Identify 실물 command
- TriggerEffect 동작
- sensor bitmap mismatch 실물 교체 절차
- Matter certification
- 모든 Apple Home/Google Home UI 표현
- SGP40/41 Gas Index용 별도 앱 또는 사용자 정의 데이터 모델
