# 프로젝트 개발 메모

이 문서는 POGO 기반 Thread 센서 시스템을 설계하면서 정한 기준을 정리한 것이다.
세부 구현과 최신 검증 상태는 `docs/`의 문서를 기준으로 한다.

## 목표

센서 모듈을 물리적으로 이어 붙이면 별도 설정 화면 없이 연결 순서를 파악하고, 센서 종류에
맞는 설정을 적용해 Thread 네트워크로 데이터를 전달하는 구조를 만든다. 최종 시연에서는
전원 모듈, 중간 센서 모듈, 말단 센서 모듈을 한 체인으로 연결하고 Linux OTBR 및 Matter
Bridge까지 이어지는 전체 경로를 보여준다.

## 기본 설계

### 한 종류의 센서 모듈 펌웨어

모든 센서 모듈은 `module/` 바이너리를 공유한다. 센서 드라이버, 버스 핀, POGO 포트,
타임아웃은 `cfg` NVS 이미지로 선택한다. 보드를 바꿀 때 앱을 다시 만드는 대신 설정 이미지만
교체할 수 있도록 분리했다.

### 고정된 하드웨어 역할과 동적인 Thread 역할

Power Base는 POGO 체인의 루트이자 프로비저닝 담당 장치다. 이 역할은 Thread의 Leader,
Router, Child 상태와 별개다. OpenThread가 네트워크 상황에 따라 무선 역할을 결정하고,
Power Base 서비스는 연결된 상태라면 세 역할에서 모두 동작한다.

센서 모듈의 LEAF와 INTERMEDIATE는 POGO 토폴로지에서의 역할이다. `otDeviceRole` 이름과
혼동하지 않는다. INTERMEDIATE는 downstream 모듈을 프로비저닝할 수 있고, LEAF는 말단
센서로 동작한다.

### 영구 UUID

모듈 UUID는 재부팅이나 재프로비저닝 뒤에도 바뀌지 않는다. 토폴로지 레지스트리, 센서
샘플 중복 제거, Linux registry, Matter endpoint ID의 기준은 모두 이 UUID다.

### POGO 프로토콜

POGO 프레임은 길이와 CRC16을 포함한다. 프로비저닝은 HELLO, UUID 확인, 슬롯·부모 정보,
센서 정보, polling 설정, collector 설정, Dataset 전달, Thread join 결과 순으로 진행한다.
각 단계는 ACK와 제한된 재시도를 사용한다.

물리적 분리와 통신 실패는 구분한다. 프로토콜 버전이나 capability 충돌은 다시 체결하기 전까지
오류로 유지하고, timeout이나 liveness 손실은 cooldown 뒤 새 handshake를 시작한다.

### 데이터 경로

센서 모듈은 현재 Thread Leader 주소를 목적지로 쓰지 않는다. Power Base의 Mesh-Local EID와
CoAP 포트를 `SET_COLLECTOR`로 받아 `/s` 센서 샘플과 `/t` 토폴로지 보고를 전송한다.
Power Base는 topology 등록이 끝난 UUID의 데이터만 저장한다.

Linux로 내보낼 때는 최신 샘플만 유지하고 UUID별 publish 간격을 제한한다. 센서의 실제 측정
주기는 바꾸지 않고 북향 전송량만 줄인다.

## 센서 정책

- SHT45: 온도와 습도
- BH1750: 조도
- SGP40: VOC raw와 Gas Index
- SGP41: VOC/NOx raw와 Gas Index, 초기 conditioning 상태 구분
- PMS5003: PM1, PM2.5, PM10, UART terminal LEAF 전용

SGP40/41의 Gas Index는 농도값이 아니다. Matter 농도 클러스터에 억지로 넣지 않는다.
PMS5003의 warm-up과 SGP41의 conditioning은 오류와 구분해 프로비저닝을 계속 진행한다.

## 전원과 절전

현재 프로토타입은 각 보드에 USB 전원을 공급한다. POGO 5V 전원 회로는 최종 하드웨어 단계에서
추가할 예정이다. 외부 USB-C 전원을 Power Base와 POGO 레일에 분배하되, 전류 제한과 역급전
방지를 회로에서 처리해야 한다.

Deep-sleep과 자동 Light-sleep은 적용하지 않았다. Matter에서 항상 온라인 상태를 유지해야 하고,
UART wake만으로는 이미 들어온 POGO 프레임 전체를 보존할 수 없기 때문이다. Thread LEAF의 SED
동작은 유지하지만 애플리케이션 수준의 수면 모드는 사용하지 않는다.

## Matter 범위

Linux Matter Bridge는 다음 값을 표준 클러스터로 공개한다.

- SHT45: Temperature Measurement, Relative Humidity Measurement
- BH1750: Illuminance Measurement
- PMS5003: PM1, PM2.5, PM10 Concentration Measurement

각 모듈 UUID는 하나의 동적 Matter endpoint에 대응한다. endpoint ID는 파일에 저장해 프로세스가
재시작되어도 유지한다. Apple Home과 Google Home은 표준 클러스터를 모두 같은 방식으로 표시하지
않으므로, 실제 값 확인은 `chip-tool` 결과를 기준으로 한다.

## 검증 원칙

검증 결과는 다음 세 단계로 구분한다.

1. 소스 검토 또는 정적 검사
2. host test와 ESP-IDF/Linux 빌드
3. 실제 보드, OTBR, Matter controller를 사용한 실물 시험

host test가 통과해도 실물 동작을 확인한 것으로 기록하지 않는다. 실물 로그가 없는 기능은
`docs/validation-status.md`에 미검증으로 남긴다. OpenThread가 선택한 라우팅 결과도 POGO의
물리적 depth와 따로 기록한다.

## 저장소 운영

- 비밀 Dataset과 로컬 로그는 저장소에 넣지 않는다.
- 동작 코드와 설정 스키마가 바뀌지 않은 문서 수정은 펌웨어 재빌드 대상으로 보지 않는다.
- 코드 변경은 영향을 받는 host test와 실제 프로젝트 빌드로 확인한다.
- 실물에서 확인한 보드, 설정 이미지, 경로를 기록해 다른 시험 결과와 섞이지 않게 한다.
