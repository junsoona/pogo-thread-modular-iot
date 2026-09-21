# 센서 모듈 설정

`module_config`는 센서 모듈의 고정 하드웨어 정보를 `cfg` NVS 파티션에서 읽는다.
Power Base는 별도의 `power_base_config`를 사용한다.

## 고정 정보와 동적 정보

| 고정 `cfg` | 실행 중 결정 |
|---|---|
| sensor driver와 channel bitmap | LEAF/INTERMEDIATE |
| I2C/SPI/UART와 GPIO | Thread CHILD/ROUTER/LEADER |
| address, clock, calibration | parent UUID, slot, depth |
| measurement timeout | Dataset, polling period, session |
| UPSTREAM/DOWNSTREAM 포트 | online/offline 상태 |

센서와 ESP32-H2의 조합은 조립할 때 정하고 실행 중에 자동 탐색하지 않는다. 네트워크를
초기화해도 센서 설정이 사라지지 않도록 동적 `nvs`와 고정 `cfg`를 분리했다.

```text
nvs  : OpenThread Dataset, child/parent 동적 상태
cfg  : module_cfg 고정 blob, dev_cfg 회귀 시험 override
```

## blob 형식

`module_config_load()`는 `cfg` 파티션의 `module_cfg` namespace에서 두 값을 읽는다.

- `blob`: schema v1 90바이트 또는 schema v2 104바이트, big-endian
- `crc`: blob 전체의 CRC16/CCITT-FALSE

schema v1은 I2C/SPI 센서용이다. schema v2에는 센서 UART의 TX, RX, baud rate, mode,
warm-up 시간이 추가됐다. 정확한 byte offset은
`components/module_config/module_config.c`의 구조 설명을 기준으로 한다.

다음 조건은 pack 단계와 runtime validation에서 검사한다.

- 지원하는 `bus_type`: I2C, SPI, UART
- `measurement_timeout_ms > 0`
- enabled POGO port의 TX/RX GPIO 존재
- UPSTREAM과 DOWNSTREAM의 UART controller 중복 금지
- GPIO 범위와 중복 사용
- open-drain active-low DETECT_OUT
- driver ID와 channel bitmap 정합성
- PMS5003 terminal port 소유 규칙

오류가 있으면 `CONFIG_ERROR`를 남기고 센서, POGO, Thread를 시작하지 않는다.
부분 설정이나 driver 자동 fallback은 없다.

`dev_cfg` namespace에는 회귀 시험용 `dev_mode` 한 바이트가 들어간다. namespace가 없으면
AUTO가 기본값이다.

## 설정 이미지 생성

```bash
source ~/.espressif/v6.1/esp-idf/export.sh

bash tools/gen_config.sh build/cfg_images
```

개별 생성도 가능하다.

```bash
python3 tools/pack_module_config.py \
  configs/prototype_sht45.json \
  build/cfg_images/prototype_sht45_config.bin
```

출력 NVS image 크기는 `0x3000`(12288바이트)이며 `module/partitions.csv`의 `cfg`
크기와 같아야 한다. 실제 blob은 NVS image보다 작다.

## ESP32-H2 GPIO 검증

할당 가능한 값은 `-1` 또는 GPIO0..27이다. `-1`은 사용하지 않는 기능을 뜻한다.

제외하는 핀:

- strapping: GPIO8, GPIO9, GPIO25
- 패키지에서 외부로 나오지 않음: GPIO6, GPIO7
- internal flash: GPIO15..21
- USB Serial/JTAG: GPIO26, GPIO27

추가 규칙:

- 같은 GPIO를 둘 이상의 기능에 할당하지 않음
- enabled port의 `uart_num`은 0 또는 1
- DOWNSTREAM `base_id_gpio=-1`
- AUTO enabled port는 DETECT_IN/OUT 필수
- AUTO UPSTREAM은 BASE_ID 필수

정책 원본은 `components/module_config/gpio_profile.c`다.
`tools/pack_module_config.py`가 같은 목록을 사용하고
`tools/test_gpio_profile_parity.py`가 차이를 검사한다.

현재 공통 POGO 배치:

| 기능 | UPSTREAM | DOWNSTREAM |
|---|---:|---:|
| UART | 0 | 1 |
| TX | GPIO4 | GPIO10 |
| RX | GPIO5 | GPIO11 |
| DETECT_IN | GPIO12 | GPIO23 |
| DETECT_OUT | GPIO2 | GPIO3 |
| BASE_ID | GPIO14 | -1 |

GPIO12는 future wake 후보로 남겼지만 Deep-sleep은 구현하지 않았다.

## driver와 channel 이름

driver ID는
`components/sensor_provider/include/sensor_provider/sensor_provider.h`에 정의한다.
channel type은
`components/sensor_channel/include/sensor_channel/sensor_channel.h`에 정의한다.

JSON에서는 이름을 사용할 수 있다.

```json
{
  "driver_id": "SHT45",
  "sensor_type_bitmap": ["TEMPERATURE", "HUMIDITY"]
}
```

packer의 `DRIVER_IDS`와 `CHANNEL_BITS`는 C header와 같은 값을 가져야 한다.
관련 Python test가 이름, 숫자, packed blob의 정합성을 검사한다.

## 프로토타입 설정

| 설정 | driver | bus/address | channel | timeout |
|---|---|---|---|---:|
| `prototype_sht45.json` | SHT45 | I2C `0x44` | Temperature, Humidity | 200ms |
| `prototype_bh1750.json` | BH1750 | I2C `0x23` | Illuminance | 300ms |
| `prototype_sgp40.json` | SGP40 | I2C `0x59` | VOC raw/index | 200ms |
| `prototype_sgp41.json` | SGP41 | I2C `0x59` | VOC/NOx raw/index | 200ms |
| `prototype_pms5003.json` | PMS5003 | UART1 GPIO0/1, 9600 | PM1/2.5/10 | UART timeout |

SHT45, BH1750, SGP40, SGP41 설정은 같은 POGO port block을 사용한다.
`tools/test_prototype_port_blocks_match.py`가 JSON과 packed NVS blob을 함께 검사한다.

BH1750은 breakout의 ADDR pin이 high면 주소가 `0x5C`가 되므로 실제 보드를 확인한다.

## SGP40과 SGP41

두 driver는 Gas Index Algorithm을 위해 최소 sampling period를 1000ms로 제한한다.
`measurement_timeout_ms`는 한 번의 I2C 측정이 멈췄을 때 빠져나오기 위한 상한이며
sampling period와 다른 값이다.

SGP40:

- fixed address `0x59`
- VOC_RAW, VOC_INDEX
- 고정 보상값 50%RH, 25°C

SGP41:

- fixed address `0x59`
- 첫 약 10초는 VOC conditioning command 사용
- conditioning 중 VOC 채널만 `SENSOR_CHANNEL_STATUS_CONDITIONING`
- 이후 VOC_RAW, VOC_INDEX, NOX_RAW, NOX_INDEX
- 고정 보상값 50%RH, 25°C

다른 모듈의 SHT45 값을 가져와 보상하지 않는다. 따라서 Gas Index를 실제 온습도 보정
농도값으로 해석하지 않는다.

## PMS5003

PMS5003은 terminal LEAF 전용 예외다.

- schema v2
- sensor UART1
- MCU TX GPIO0, RX GPIO1
- 9600 8N1
- passive read 지원
- warm-up 30초
- DOWNSTREAM POGO 완전 비활성

packer와 runtime loader가 이 소유 규칙을 검사한다.

## mock sensor

`SENSOR_DRIVER_ID_MOCK`은 DEV_ROOT/DEV_LEAF 회귀 시험에서만 사용할 수 있다.
AUTO 설정과 함께 쓰면 `CONFIG_ERROR`로 부팅을 중단한다.

## cfg만 다시 플래시

앱 바이너리를 바꾸지 않고 설정만 기록할 수 있다.

```bash
python -m esptool --chip esp32h2 -p "$MODULE_PORT" write-flash \
  0x187000 build/cfg_images/prototype_sht45_config.bin
```

offset은 현재 `module/partitions.csv` 기준이다. 파티션 구성을 바꾸면 새 offset을 다시
확인한다.
