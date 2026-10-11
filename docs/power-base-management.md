# Power Base 관리: 일반 등록과 삭제·등록 해제

앱에서 Power Base를 등록하고, 등록된 Power Base를 삭제(등록 해제)하는 기능의 규약이다. 보드 펌웨어
(`power_base/`, `components/power_base_enrollment`, `components/power_base_mgmt`), Linux 허브
(`linux/matter_bridge`), 앱(PogoHome)이 이 문서를 같이 따른다. 기존 등록 경로의 세부는
[`power-base.md`](power-base.md) "BLE 등록 전달 경로", HTTP API는
[`pogohome-api-v1.md`](pogohome-api-v1.md), 검증 범위는 [`validation-status.md`](validation-status.md)에 있다.

**2026-10-05 후속 수정.** 처음 구현은 보드가 저장된 네트워크를 지우기 **전에** 보낸 증명으로 허브가
삭제를 `completed`로 처리했다. 지금은 그 메시지를 "등록 사용 중지" 통지로만 다루고, `completed`는 보드가
삭제를 끝내고 검증한 뒤에 만든 **영수증**을 허브가 확인했을 때만 된다(4·6·7·8절). 함께 고친 것: 차단
목록이 찼을 때 오래된 차단을 버리던 동작(7절 "차단 용량"), 방 배정 정리가 실패하거나 중단되면 다시
하지 않던 동작(7절 "설정 정리").

**2026-10-08 자동 삭제 확인.** 삭제를 끝낸 보드가 영수증을 **스스로** 허브에 보낸다. 그전에는 영수증이
BLE로만 나왔고, 앱이 보드를 찾아 설정 코드로 인증한 뒤 읽어 전달해야 `completed`가 됐다. 지금은 보드가
삭제·검증·최종 record 저장을 마친 **뒤에** flash에 아무것도 쓰지 않는 마지막 Thread 세션을 한 번 열어
같은 영수증을 관리 resource로 보낸다(4절 "영수증 전달 세션"). `completed`의 조건은 바뀌지 않았다: 허브가
그 영수증의 MAC을 검증했을 때뿐이다. BLE 영수증과 "삭제 확인" 화면은 그대로 있고, 자동 전달이 되지 않은
경우와 이전 펌웨어 보드의 경로다. 함께 고친 것 두 가지: 삭제 검증이 Network Key가 들어 있는 PSA 키
저장소를 읽지 않던 것(4절 "삭제 확인이 읽는 것"), 부팅 경로의 통지가 CoAP socket을 열지 않아 나가지 못하던
것(4절 E). 버전: Power Base 0.2.0, Linux bridge 0.2.0([`versioning.md`](versioning.md)).

**2026-10-09 같은 허브의 주소·포트 변경 복구.** 등록된 Power Base가, **같은 허브**의 Thread 수신 주소나
포트가 바뀌었을 때 그 허브를 스스로 다시 찾아 통신을 잇는다(12절). 앱은 관여하지 않는다. 허브의 Wi-Fi/LAN
주소를 앱이 다시 찾는 기능과는 다른 것이다. 관리 메시지 `LOCATE`/`LOCATE_ACK`와 capability 두 개가
늘었고(규약 번호 1 그대로), record·BLE·HTTP API의 형식은 바뀌지 않았다. 버전: Power Base 0.3.0, Linux bridge
0.3.0. **실물로는 실행하지 않았다**(12절 "확인한 것과 하지 않은 것").

**2026-10-10 허브 목록에서 제거.** 돌아오지 않는 Power Base를 **이 허브의 목록에서** 지운다(13절). 삭제가
아니다: 보드에 저장된 연결 정보는 지워지지 않고, 어떤 삭제 작업도 이것으로 `completed`가 되지 않는다. 허브가
처리하는 순간에 그 Base가 오프라인인지 다시 확인하고, 요청이 가리킨 등록(`record`)이 지금의 등록일 때만 한다.
보드 펌웨어와 관리 규약(CoAP·BLE)은 바뀌지 않았고, HTTP API에 경로와 필드가 더해졌다. 버전: Linux bridge 0.4.0.
**실물로는 실행하지 않았다.**

**2026-10-10 목록 제거의 후속 수정.** 독립 검토가 실제 코드로 재현한 두 결함 — snapshot에 "제거 전 목록"과
"제거 뒤 기록"이 섞이는 것, 늦게 실행된 방 지정 정리가 다른 Base로 옮긴 모듈의 새 방까지 지우는 것 — 을 고치고,
13절이 남겨 둔 것(재시작 뒤 다시 들리지 않은 Base의 방 정리, 목록에서 빠진 Base의 삭제 작업)을 정리했다(14절).
HTTP API와 상태 파일에는 선택 항목만 더해졌다. 버전: Linux bridge 0.4.1. **실물로도 Linux에서도 실행하지 않았다.**

**검증 상태.** 이 문서의 내용은 2026-10-05에 추가됐고 host test, 시뮬레이터 test, 빌드로만 확인했다.
실물 보드·실물 BLE·운영 Linux 허브·실제 iPhone으로는 실행하지 않았고, Linux용 `pogo_matter_bridge`
(GN/CHIP) 빌드도 하지 않았다. 그 전에 실물로 확인된 것(목적지 전달, record v2 복원, heartbeat 등록
확인)은 이 변경 **이전** 코드의 결과이며 이 변경에 소급되지 않는다. 실행한 것과 하지 않은 것의 목록은
[`validation-status.md`](validation-status.md) "Power Base 관리(일반 등록·삭제) 검증"에 있다. 2026-10-08의
자동 삭제 확인도 실물로는 실행하지 않았다. 그 근거와 한계는 같은 문서의 "Power Base 자동 삭제 확인 검증
(2026-10-08)"에 있다.

## 1. 용어와 신뢰 경계

| 용어 | 뜻 |
|---|---|
| 설정 코드 | 보드마다 한 번 만든 26자리 코드. BLE Security 2의 password. 변하지 않는다 |
| PSKd | 등록 세션마다 앱이 만드는 일회용 Thread Joiner 암호 |
| 관리 키 | 등록 세대마다 **허브가** 만드는 32바이트 난수. 삭제 명령, 등록 사용 중지 통지, 삭제 영수증의 HMAC 키. PSKd·Network Key·설정 코드와 무관하다 |
| 등록 세대(generation) | 허브가 등록 세션마다 만드는 16바이트 난수. 관리 키와 1:1 |
| hub_id | 허브의 고정 identity 16바이트(`<state-dir>/hub_id`의 32 hex) |
| 삭제 작업(removal) | 허브가 영속 저장하는, 한 Base의 한 등록 세대에 대한 등록 해제 요청 |
| 등록 사용 중지 통지 | 보드의 `UNENROLL_DONE`/`RELEASED`. "이 등록을 다시는 쓰지 않는다고 저장했다"는 뜻이다. 저장된 Thread 가입 정보(Dataset)는 **아직 지우지 않았다**. 완료가 아니다 |
| 삭제 영수증(release receipt) | 보드의 `UNENROLL_DONE`/`ERASED`. 보드가 저장된 네트워크를 지우고, 지워진 것을 다시 읽어 확인하고, 최종 record를 저장한 **뒤에만** 만드는 메시지. 허브의 `completed`는 이것을 확인했을 때만 된다 |
| 영수증 전달 세션 | 최종 record를 저장한 뒤 보드가 영수증을 허브에 직접 보내려고 한 번 여는 Thread 세션. 삭제 직전에 RAM에 읽어 둔 Dataset으로 붙고, OpenThread의 영속 저장소 두 곳을 RAM으로 돌려 둔 상태라 flash에는 아무것도 쓰이지 않는다(4절). 재시작하면 사라진다 |

신뢰 경계:

- Thread 네트워크에 가입했다는 사실, 송신 IP, EUI-64, CRC는 아무것도 증명하지 않는다. 기존
  `pogo/v1/{sample,topology,heartbeat}`는 그대로 미인증 데이터 경로이고, 그 경로에 파괴적 명령을
  추가하지 않았다.
- 삭제 명령과 그 결과는 관리 키의 HMAC-SHA-256으로만 인증한다. 키를 아는 것은 그 등록 세대의 보드와
  허브뿐이다. 앱은 등록할 때 한 번 메모리로 전달만 하고 저장하지 않는다.
- 앱 ↔ 허브는 기존 그대로 TLS + 인증서 핀 + 페어링된 기기의 bearer token이다. 앱 ↔ 보드는 기존 그대로
  BLE 위의 protocomm Security 2(설정 코드)다.
- 허브는 보드에게 먼저 말을 걸지 않는다. 보드가 인증된 poll을 보내고, 허브는 그 CoAP 응답으로만
  명령을 준다. 미인증 heartbeat의 주소로 명령을 보내는 일은 없다.

## 2. capability와 버전 호환

| 위치 | 표시 | 뜻 |
|---|---|---|
| 보드 `proto-ver`의 `pb_enroll.cap` | `mgmt_v1` | `pb-start` `0x06`(목적지 + 관리 자격증명)을 받고, 등록 뒤 관리 poll을 한다 |
| 보드 `proto-ver`의 `pb_enroll.cap` | `release_receipt_v1` | `pb-status`의 `0x07`로 삭제 영수증을 내준다(5절) |
| 보드 `proto-ver`의 `pb_enroll.cap` | `local_reset_v1` | 재등록 버튼이 있는 보드다. `pb-start`의 `0x09`(로컬 초기화 명령)와 `pb-status`의 `0x08`(그 상태)을 받는다. 원격 삭제와는 다른 경로다([`power-base.md`](power-base.md) "BOOT 버튼 로컬 재등록") |
| 허브 `GET /api/v1/health` | `"power_base_management": 1`, `"power_base_removal": "available"`/`"unavailable"` | 등록 세션의 `management_version: 1`과 삭제 API를 아는 허브다. `unavailable`은 지금은 쓸 수 없다는 뜻이다(TLS identity 없음, 등록 기능 꺼짐, 관리 파일 손상) |
| 허브 snapshot의 `power_bases[]` | `"management": "supported"`/`"unsupported"` | 그 Base에 대해 허브가 **유효한 관리 자격증명**을 갖고 있는지 |
| 보드의 관리 메시지 `HELLO` (6절) | capability `PB_MGMT_BOARD_CAP_THREAD_RECEIPT` (bit 0) | 이 펌웨어는 삭제를 끝낸 뒤 영수증을 스스로 Thread로 보낸다. 그 등록의 관리 키로 인증된 메시지다. 보내지 않는 보드(이전 펌웨어)는 이 capability가 없는 것으로 다룬다 |
| 허브의 관리 메시지 `HELLO_ACK` (6절) | capability `PB_MGMT_HUB_CAP_THREAD_RECEIPT` (bit 0) | 이 허브는 관리 resource로 온 영수증으로 삭제를 완료하고, 같은 영수증이 다시 오면 같은 확인을 다시 준다. 보드는 이 값으로 아무것도 결정하지 않는다(아래 표) |
| 보드의 관리 메시지 `HELLO` (6절) | capability `PB_MGMT_BOARD_CAP_ENDPOINT_LOCATE` (bit 1) | 이 펌웨어는 저장된 허브 주소가 답하지 않으면 허브를 다시 찾아 옮긴다(12절). 허브에게는 로그용 정보일 뿐이고 아무것도 이 값에 달려 있지 않다 |
| 허브의 관리 메시지 `HELLO_ACK` (6절) | capability `PB_MGMT_HUB_CAP_ENDPOINT_LOCATE` (bit 1) | 이 허브는 `LOCATE`에 답한다. 보드는 이 값을 기다리지 않고 묻는다: 물어야 하는 때는 허브와 연락이 끊겼을 때다 |
| 허브 snapshot의 `power_bases[]`, 삭제 작업 JSON | `"erase_confirmation": "automatic"`/`"manual"` | 그 Base(또는 그 작업)의 삭제 확인이 어떻게 올 것으로 **예상되는지**. `automatic`: 보드가 위 capability를 알렸다. `manual`: 알리지 않았거나, 알렸지만 제시간에 오지 않았다(7절). 표시용 값이며 상태를 바꾸지 않는다. 이 필드가 없는 허브는 이전 허브다 |

원격 해제가 가능한 조건은 둘 다다: ① 보드 펌웨어가 `mgmt_v1`이고 ② 그 보드의 현재 등록이 `0x06`으로
이뤄져 보드와 허브가 같은 세대의 키를 갖고 있다. 펌웨어만 올린 기존 보드(record v1/v2)는 키가 없으므로
`unsupported`다. 그런 보드는 한 번 초기화한 뒤 새 등록 화면으로 다시 등록해야 원격 해제가 된다.

| 조합 | 동작 |
|---|---|
| 새 앱(제품 등록) + 보드에 `mgmt_v1` 없음 | 허브 세션을 만들기 전에 "보드 펌웨어 업데이트 필요". 아무 start도 보내지 않는다 |
| 새 앱(제품 등록) + 허브가 `management_version`을 모름 | 허브가 `400 invalid_request`. 앱은 "허브 업데이트 필요"로 끝내고 옵션 없이 다시 요청하지 않는다 |
| 새 앱(제품 등록) + 새 허브가 관리를 지금 쓸 수 없음 | `503 enrollment_unavailable`(`"reason":"management"`). 세션을 만들지 않는다. `management_version`이 1이 아니면 `422 unsupported_management_version`, `bridge_endpoint_version` 없이 `management_version`만 보내면 `400 invalid_request` |
| 구형 앱·DEBUG 시험(`0x04`) + 새 보드 | 이전 그대로 등록된다(record v2, 관리 키 없음 → 원격 해제 미지원) |
| 새 보드 + 구형 허브 | 관리 poll에 허브가 `4.00`으로 답한다(관리가 꺼진 새 허브는 `4.04`). 보드는 인증된 답이 아니므로 아무것도 하지 않고 다음 주기(10초)에 다시 poll한다. 데이터 경로는 영향 없다 |
| 삭제 요청 + 키 없는 Base | 허브가 `409 power_base_management_unsupported`. 앱은 "지원하지 않음"을 표시한다. 목록에서만 숨기는 로컬 삭제로 대체하지 않는다 |
| 삭제된(차단된) EUI-64를 관리 없는 세션으로 재등록 | 허브가 `409 management_required`. 차단은 새 **인증된** 등록 세대만 풀 수 있다 |

| 삭제 확인 + `release_receipt_v1` 없는 보드 | 앱은 영수증을 요청하지 않는다. 삭제 작업은 `released`로 남는다 |
| 삭제 확인 + 영수증 경로가 없는 허브 | `404`. 앱은 "허브 업데이트 필요"를 표시한다 |
| 재등록 화면 + `local_reset_v1` 없는 보드 | 앱은 `0x08`·`0x09`를 보내지 않고 다른 message로 대신하지도 않는다. "펌웨어 업데이트 필요"로 끝난다. 아무것도 지워지지 않는다 |

자동 삭제 확인(2026-10-08)의 신·구 조합. 버전 문자열로 판단하는 곳은 없다. 보드의 capability(`HELLO`)와
상대의 실제 응답으로만 갈린다.

| 보드 | 허브 | 앱 | 동작 |
|---|---|---|---|
| 0.2.0 | 0.2.0 | 1.2.0 | `HELLO`로 허브가 capability를 알고, 작업은 `erase_confirmation: automatic`. 앱은 삭제 요청 뒤 '삭제 대기'/'삭제 중'만 보여 주고, 영수증이 도착하면 '삭제 완료'. 사용자가 더 할 일이 없다 |
| 0.2.0 | 0.2.0 | 1.1.1 이하 | 허브는 똑같이 영수증으로 완료한다. 이전 앱은 새 필드를 읽지 않으므로 `released` 동안 "등록 해제됨 · 삭제 확인 필요"를 보여 주다가 완료되면 "삭제 완료"가 된다 |
| 0.2.0 | 0.1.0 | 어느 것이든 | 이전 허브는 `HELLO`를 모르는 type으로 거부한다(`4.00`). 보드는 그것을 "capability 없음"으로 받고 그 부팅에서 다시 보내지 않는다. **영수증 전달 세션은 그래도 한다**: 0.1.0 허브도 관리 resource로 온 영수증을 검증해 완료한다(같은 영수증이 다시 오면 키가 없어 `4.01`로 거부하고, 보드는 거기서 멈춘다). 허브가 `erase_confirmation`을 주지 않으므로 새 앱도 이전처럼 "삭제 확인 필요"를 보여 주다가 완료를 본다 |
| 0.1.0 | 0.2.0 | 1.2.0 | 보드가 `HELLO`를 보내지 않는다. 작업은 처음부터 `erase_confirmation: manual`이고 앱은 이전과 같이 "삭제 확인"(BLE)을 제공한다 |
| 0.1.0 | 0.1.0 | 1.2.0 | 필드가 없다. 앱은 `manual`과 같이 다룬다(이전 동작 그대로) |

주소·포트 변경 복구(2026-10-09)의 신·구 조합. 이것도 버전 문자열이 아니라 상대의 실제 응답으로 갈린다.
앱은 어느 조합에서도 관여하지 않으므로 앱 버전(1.2.1 (10)·(11) 포함)과 무관하다.

| 보드 | 허브 | 동작 |
|---|---|---|
| 0.3.0, record v3 | 0.3.0 | 허브의 주소나 포트가 바뀌면 보드가 약 1분 30초 뒤부터 허브를 찾아 새 목적지를 저장하고 옮긴다(12절) |
| 0.3.0, record v3 | 0.2.0 이하 | 평소 동작은 0.2.0과 같다. 허브가 옮겨 가면 보드는 묻지만 이전 허브는 `LOCATE`를 모르는 type으로 거부한다(`4.00`). 보드는 그것을 "여기는 내 허브가 답하는 곳이 아니다"로 받고 아무것도 바꾸지 않는다. 복구는 재등록이다(이전과 같다). 보드의 `HELLO`가 싣는 새 bit는 이전 허브가 값 그대로 저장하고 쓰지 않는다 |
| 0.2.0 이하 | 0.3.0 | 보드가 묻지 않는다. 허브가 옮겨 가면 이전과 같이 끊기고, 복구는 재등록이다. 허브의 새 capability bit와 locate 포트는 그 보드에게 보이지 않는다 |
| 0.3.0, record v1·v2 (관리 키 없음) | 어느 것이든 | 묻지 않는다. 답을 인증할 키가 없기 때문이다. record는 읽기만 하고 다시 쓰지 않는다. 허브가 옮겨 가면 재등록해야 한다 |

보안 요구를 낮춘 자동 fallback은 없다.

**이전 형식과의 구분.** `UNENROLL_DONE`의 result 1(`RELEASED`)은 처음부터 "영속 해제 기록을 저장했다"는
뜻이었고 그 바이트와 MAC은 그대로다. 달라진 것은 허브가 그것을 더 이상 완료로 부르지 않는다는 점이다.
완료는 새 result 2(`ERASED`)로만 된다. `RESULT_ACK`는 이제 자기가 확인하는 result를 한 바이트로
되돌려 준다(이전 형식의 ACK는 그 자리가 0이라 새 보드가 확인으로 받지 않는다). 허브의 상태 파일은
schema 2이고, schema 1 파일의 `completed` 작업은 읽을 때 `released`로 바뀐다(그때의 "완료"는 삭제를
확인한 것이 아니었다). 이 형식들은 한 번도 배포되지 않았다.

## 3. 관리 자격증명의 수명

1. **생성.** 앱이 `POST /api/v1/power-base-enrollments`에 `"management_version": 1`을 넣으면, 허브가
   세션을 만들 때 CSPRNG(`RAND_bytes`)로 세대 16바이트와 키 32바이트를 만든다. 세션 worker를 시작하기
   전에 `provisional` 자격증명으로 영속 저장한다. 저장에 실패하면 세션을 만들지 않는다.
2. **전달.** 키는 그 `POST`의 `202` 응답에만 들어 있다(TLS, 페어링된 기기, `Cache-Control: no-store`).
   이후의 `GET`·`DELETE` 응답과 다른 기기의 응답에는 세대만 있고 키는 없다. 앱은 이 값을 메모리에만
   두고, Security 2 세션 안의 `pb-start` `0x06`으로 보드에 넘긴 뒤 버린다. 화면·로그·오류·클립보드·저장소에
   나오지 않는다.
3. **보드의 보관.** 등록이 끝나면(attach + 완전한 Dataset) 완료 표식·목적지와 **한 blob**(record v3)으로
   저장한다. 등록에 실패하면 RAM의 키를 지운다.
4. **활성화.** 보드의 첫 인증된 관리 poll이 허브에 도착하면 그 세대가 `active`가 된다. 같은 EUI-64의
   이전 세대 자격증명은 이때 폐기되고, 이전 세대에 대한 끝나지 않은 삭제 작업(`released` 포함)은
   `superseded`로 끝나며, 그 EUI-64의 ingest 차단이 풀린다.
5. **교체.** 재등록은 항상 새 세대·새 키다. 보드는 새 시도를 시작할 때 기존 record를 먼저 지우고, 허브는
   `provisional`을 새로 쓴다(이전 `provisional`은 대체). 이전 세대의 명령·응답은 키와 세대가 달라 새
   등록에 적용되지 않는다.
6. **폐기.** 보드는 **최종 record를 쓸 때** flash의 키를 지운다. 등록 사용 중지 통지와 삭제 영수증을
   만드는 데 키가 필요하기 때문에, 그 전(`RELEASED`/삭제 대기)까지는 record에 키가 남아 있다. 영수증 전달
   세션 동안에는 허브의 확인(`RESULT_ACK`)을 검증하려고 RAM에만 남겨 두고, 세션이 끝나면 지운다(그 뒤
   재시작한다). 허브는 영수증을 확인해 작업을 `completed`로 만드는 같은 저장에서 그 세대의 키를 지운다.
   그때 자기가 준 확인의 MAC을 작업에 남겨, 같은 영수증이 다시 오면 키 없이 같은 확인을 다시 준다. 영수증을
   받지 못한 `released` 작업의 키는 영수증을 검증하기 위해 보관하고, 새 세대가 활성화될 때 지운다.

허브 저장 위치: `<state-dir>/pogohome/power_base_mgmt.json`(0600, 0700 디렉터리, 임시 파일 + fsync +
rename). 파일이 손상되면 관리 기능만 `503`으로 끄고 파일을 덮어쓰지 않는다.

## 4. 보드의 record와 상태 전이

`pb_enroll/done` key 하나에 세 버전이 있다. 크기와 버전 바이트가 맞아야 한다. 버전 1(36바이트)과
버전 2(56바이트)의 형식·검사·부팅 동작은 바뀌지 않았고, 읽는 것만으로 다른 버전으로 다시 쓰지 않는다.

**버전 3 (200바이트, 관리 자격증명 포함)**

| offset | 길이 | 내용 |
|---|---|---|
| 0 | 4 | magic `PBE1` |
| 4 | 1 | version 3 |
| 5 | 1 | flags: 0x01 = 목적지 있음. 최종 `RELEASED` record에서는 0 |
| 6 | 1 | state: 1 `ENROLLED`, 2 `UNENROLLING`, 3 `RELEASED` |
| 7 | 1 | release flags (`RELEASED`만): 0x01 삭제 대기(이것만 단독), 또는 최종 record의 0x04 삭제 확인됨(항상) + 0x02 통지 확인받음(선택). 그 밖에는 0 |
| 8 | 8 | EUI-64 |
| 16 | 8 | Extended PAN ID. 최종 record에서는 0 |
| 24 | 8 | Mesh-Local Prefix. 최종 record에서는 0 |
| 32 | 16 | bridge IPv6 주소. 최종 record에서는 0 |
| 48 | 2 | port, big-endian. 최종 record에서는 0 |
| 50 | 1 | 관리 형식 버전 = 1 |
| 51 | 1 | 0 |
| 52 | 16 | hub_id |
| 68 | 16 | 등록 세대 |
| 84 | 32 | 관리 키. 최종 record에서는 모두 0 |
| 116 | 16 | 명령 id. `ENROLLED`에서는 모두 0 |
| 132 | 32 | 최종 record만: 삭제 영수증(`UNENROLL_DONE`/`ERASED`)의 MAC |
| 164 | 32 | 0 |
| 196 | 4 | CRC-32(LE, `[0..195]`) |

최종 record(release flags의 0x01이 꺼짐)에 남는 것은 영수증을 다시 만들 수 있는 값뿐이다: EUI-64, hub_id,
세대, 명령 id, MAC. 비밀은 없다. 그 전 상태의 record는 관리 키를 담는다. 로그에 내용을 출력하지 않는다.

**삭제의 다섯 단계.** 서로 다른 일이고, 어느 하나를 다른 것으로 보고하지 않는다.

| 단계 | 뜻 | 허브에 전하는 것 |
|---|---|---|
| 명령 수락 | 명령이 모든 검사를 통과했고 journal에 적혔다(`UNENROLLING`) | `UNENROLL_ACCEPTED` |
| 등록 사용 중지 | record가 `RELEASED`(삭제 대기)다. 어떤 부팅도 이 등록으로 서비스를 재개하지 않는다. 저장된 Dataset은 그대로다 | `UNENROLL_DONE`/`RELEASED` (Thread, 아직 켜져 있다). 완료가 아니다 |
| 저장 가입 정보 삭제 완료 | Dataset을 RAM에 읽어 두고, Thread를 끄고, OpenThread 저장소를 지우고, 두 저장소를 다시 읽어 비어 있음을 확인하고, 최종 record를 쓰고 다시 읽어 확인했다 | 이 단계 **안에서** 영수증(`UNENROLL_DONE`/`ERASED`)을 만든다. 아직 아무것도 보내지 않았다 |
| 영수증 전달 | 최종 record가 저장된 뒤, RAM에만 있는 Thread 세션을 연다. 붙은 뒤와 **보낼 때마다 그 직전에** flash의 두 저장소를 다시 읽고, 비어 있을 때만 영수증을 보낸다. 끝나면 Thread를 끄고 한 번 더 읽는다 | 영수증 그대로(`UNENROLL_DONE`/`ERASED`). 전달되지 않아도 보드의 삭제는 끝난 것이고, 영수증은 최종 record에 남아 BLE로 읽을 수 있다. 저장소가 비어 있지 않으면 아무것도 보내지 않는다 |
| 허브가 완료를 확인 | 허브가 영수증의 MAC을 그 등록 세대의 키로 검증했다 | 보드가 보낸 것이든 앱이 BLE로 읽어 전달한 것이든 같은 영수증이다(5·6·7절) |

**삭제 뒤의 Thread 보고가 풀어야 하는 것.** ESP-IDF v6.1의 OpenThread 소스로 확인했고, 그 소스를 host에서
실행해 재현했다(`tools/test_power_base_release_session_openthread.sh`, [`validation-status.md`](validation-status.md)).

- `otInstanceErasePersistentInfo()`는 Thread가 꺼져 있을 때만 동작한다(`Instance::ErasePersistentInfo()`가
  MLE가 disabled가 아니면 `INVALID_STATE`를 돌려준다). 그래서 지우려면 먼저 Thread를 꺼야 하고, 그 순간
  보드의 유일한 네트워크가 없어진다.
- 지운 뒤 다시 붙으면 **지운 것이 돌아온다.** 부모의 Child ID Response에 실린 Active Dataset을 `Mle`가
  `ActiveDatasetManager::Save()`로 flash에 저장하고, Network Key는 영속 PSA 키로 다시 import된다.
  재현에서도 그랬다: 스위치 없이 다시 붙은 보드의 저장소에 Dataset이 돌아왔고, 재시작한 뒤 지운 네트워크에
  다시 붙었다.
- 지우기 전에 서명한 메시지를 먼저 보내는 것은 삭제의 증명이 아니다. 그렇게 보내는 것은 위 표의
  "등록 사용 중지" 통지뿐이고, 허브는 그것으로 `completed`를 만들지 않는다.

**영수증 전달 세션.** 그래서 영수증은 "저장하지 못하는" Thread 세션으로 보낸다
(`power_base/main/enrollment_mgmt.c`의 `op_receipt_network_up`, `components/power_base_ot_store`).

1. 삭제 단계가 Thread를 끄기 **전에** Active Dataset을 RAM으로 읽어 둔다(완전한 Dataset일 때만). 그 뒤
   저장소 삭제 → 두 저장소 read-back → 영수증 계산 → 최종 record 저장·read-back까지는 이전과 같다. 여기까지
   실패하면 세션은 없다.
2. 세션을 열기 직전에 flash를 한 번 더 읽는다. 비어 있지 않으면 열지 않는다.
3. **OpenThread의 영속 저장소 두 곳을 RAM으로 돌린다**(`pb_volatile_store_engage()`). 되돌리는 방법은
   재시작뿐이다. 두 곳은 OpenThread settings(`otPlatSettingsGet/Set/Add/Delete/Wipe`, NVS namespace
   `openthread`)와 PSA 키 저장소(`psa_its_get_info/get/set/remove`, NVS namespace `psa_its`)다. 펌웨어를
   `-Wl,--wrap=<함수>`로 link해 아홉 함수의 모든 호출이 이 component를 지나게 했고, 돌린 뒤에는 platform의
   함수가 한 번도 불리지 않는다(읽기도, 지우기도). 스위치가 실제로 경로에 있는지는 켤 때 두 번의 읽기로
   확인하고, 아니면 Thread를 켜지 않는다. link된 이미지에서도 확인한다(`tools/check_power_base_ot_store.py`).
4. extended address를 새 난수로 바꾼다. 삭제가 frame counter를 0으로 되돌리므로, 이전 주소를 아직 기억하는
   router는 같은 주소의 메시지를 재생으로 보고 버린다(재현: 같은 주소로는 30초 안에 붙지 못했다).
5. router 자격 없이, RAM의 Dataset으로 Thread를 켠다. 아무 router의 child로 붙는다. 서비스·exporter·POGO·
   관리 poll은 켜지 않는다. 45초 안에 붙지 못하면 보내지 않는다.
6. 영수증을 record의 허브 주소로 보낸다. 최대 5회, 2·4·8·16초 간격. **보내기 직전마다 flash의 두 저장소를
   다시 읽고(`network_erased`), 비어 있을 때만 보낸다.** 붙는 것이야말로 Dataset을 다시 저장하는 동작이고,
   허브는 처음 받은 영수증으로 `completed`를 만들기 때문이다. 스위치가 지켜 주리라고 믿고 보내지 않는다.
   비어 있지 않거나 읽지 못하면 그 뒤로는 보내지 않는다. 허브의 `RESULT_ACK`(`ERASED`)를 RAM의 키로
   검증하면 멈춘다. 허브가 거부(`4.xx`)해도 멈춘다.
7. Thread를 끄고, RAM의 Dataset과 키를 지우고, flash의 두 저장소를 다시 읽는다. 6이나 여기서 한 번이라도
   "비어 있음"이 아니었으면 **flash를 직접** 다시 지우고 읽는다(아래 "스위치 뒤의 삭제"). 그래도 비어 있지
   않으면 record를 **삭제 대기로 되돌린다**: 저장소에 네트워크가 있는 보드는 영수증을 내주지 않는다. 그 뒤
   재시작한다.

**스위치 뒤의 삭제.** 스위치를 켠 뒤에는 OpenThread 자신의 삭제(`otInstanceErasePersistentInfo()`)가 flash에
닿지 않는다. 그 호출의 `otPlatSettingsWipe()`와 `psa_its_remove()`도 스위치가 RAM에서 답하기 때문이다
(재현: 실제 PSA 키 저장소에서 `psa_destroy_key()`는 성공을 돌려주고 저장소의 키 파일은 그대로 남았다).
2026-10-08의 첫 구현은 세션 뒤의 재삭제를 그 호출로 했으므로 실제 보드에서는 아무것도 지우지 못했을
것이다(read-back이 "비어 있지 않음"으로 잡아 삭제 대기로 돌아가기는 했다). 지금은 스위치가 켜져 있으면
`pb_network_store_erase()`가 `pb_stored_network_erase()`로 **두 저장소 자체를** 지운다: NVS namespace
`openthread`를 별도 handle로 `nvs_erase_all` + `commit`하고, OpenThread의 영속 키 일곱 개를 저장소 함수
(`psa_its_remove`)로 스위치를 거치지 않고 지운다. 그 뒤의 확인은 여전히 read-back이다.

이 세션이 보장하는 것과 아닌 것:

- 세션 동안 Dataset과 Network Key는 **RAM에** 있다(최대 약 90초). 영수증이 말하는 것은 영속 저장소다:
  영수증이 나가는 시점에 flash에는 네트워크도 관리 키도 없고, 어느 시점에 전원이 끊겨도 다음 부팅에는
  네트워크가 없다.
- 세션 중이나 직전에 전원이 끊기면 그 영수증은 자동으로 전달되지 않는다. 다음 부팅에는 붙을 네트워크가
  없기 때문이다. 영수증은 최종 record에 남아 있고, 앱의 "삭제 확인"(BLE)이나 같은 보드의 재등록이 전달한다.
- **최종 record만으로는 영수증이 나가지 않는다.** 세션이 저장소에 무엇을 쓴 채(스위치가 경로에 없었던
  경우) 전원이 끊기면, 다음 부팅은 최종 record와 저장된 네트워크를 함께 보게 된다. 그래서 (1) 최종 record를
  가진 보드는 부팅마다 OpenThread를 시작하기 **전에** 두 저장소를 지우고 read-back하며(아래 "OpenThread가
  시작한 뒤에 묻는 것"), (2) 시작한 뒤 record를 저장소와 대조해(`pb_mgmt_client_check_final_release()`) 통과하지
  못하면 OpenThread의 삭제로 한 번 더 지우고 read-back하고, (3) BLE로 영수증을 달라는 **요청마다** 저장소를
  다시 읽어 네트워크가 있거나, Network Key 자리에 출처를 모르는 키가 있거나, 읽지 못하면 "없음"으로 답한다
  (`pb_mgmt_receipt_load()`의 인자). 지워지지 않는 동안에는 영수증이 없고, 그 보드는 계속 해제 상태이며
  재개되지 않는다.
- 순서로 막을 수 없는 경우가 하나 남는다: 영수증을 보내기 직전의 read-back 뒤에 저장소가 쓰이고, 그것이
  다시 지워지지도 않는 경우다. 허브는 보낼 때 참이었던 영수증으로 이미 `completed`이고, 보드는 삭제 대기로
  돌아가 부팅마다 지운다. 스위치를 우회하는 쓰기와 flash 삭제 실패가 함께 있어야 한다.
- 허브가 꺼져 있거나 닿지 않으면 마찬가지다. 허브는 `released`에서 기다리다가 4분 뒤 "직접 확인 필요"로
  알린다(7절).

**삭제 확인이 읽는 것.** `pb_network_store_is_empty()`(→ `components/power_base_ot_store/pb_stored_network.c`)는
OpenThread나 PSA에게 묻지 않고 두 저장소를 직접 읽는다. **둘 다** 비어 있어야 한다.

- NVS의 `openthread` namespace에 항목이 하나도 없다. ESP-IDF의 `otPlatSettingsWipe()`는 `nvs_erase_all()`의
  결과를 확인하지 않고, `otDatasetIsCommissioned()`는 wipe로 지워지지 않는 RAM의 flag로 답한다.
- OpenThread의 영속 PSA 키 일곱 개(`0x20001`~`0x20007`: Network Key, PSKc, Active·Pending Dataset의 사본,
  ECDSA 키)가 저장소에 없다. ESP-IDF v6.1은 OpenThread를 key reference로 빌드하므로 **Network Key와 PSKc는
  settings가 아니라 여기에 있다**(settings의 Dataset에는 그 두 값이 비어 있다). 삭제는 이 키들을
  `psa_destroy_key()`로 지우지만 결과를 확인하지 않는다. 저장소 함수(`psa_its_get_info`)를 스위치를 거치지
  않고 직접 불러, 일곱 개 모두 "없음"일 때만 통과한다. 읽지 못한 것도 통과가 아니다.
- **2026-10-08 이전에는 첫째만 읽었다.** settings는 지워졌는데 Network Key가 남은 보드를 "지워졌다"고 하고
  영수증을 만들 수 있었다. host test `tools/test_power_base_ot_store.c`가 그 경우를 다룬다.
- 이 확인은 **삭제와 같은 부팅에서만** 뜻이 있다. OpenThread는 instance를 만들 때마다 Network Key가 없으면
  임의의 키를 하나 만들어 저장한다(`Instance::AfterInit()` → `KeyManager::Init()`). 어느 네트워크의 키도
  아니지만 항목(`0x20001`)은 생기고, 그 뒤로는 계속 남는다. 그래서 "비어 있는가"는 같은 부팅의 삭제 뒤에만
  묻는다: 삭제 직후, 세션을 열기 직전, 세션 안에서 영수증을 보내기 직전마다, 세션을 내린 뒤.

**OpenThread가 시작한 뒤에 묻는 것: Network Key 자리의 키는 누구의 것인가.** 최종 record를 가진 보드의
부팅과 BLE 영수증 요청은 "비어 있는가"를 물을 수 없다(`0x20001`에 OpenThread의 키가 있다). 그렇다고 그 자리의
키를 OpenThread의 것으로 쳐 줄 수도 없다. OpenThread는 시작할 때 그 자리가 **비어 있을 때만** 임의의 키를
만들고(`KeyManager::Init()` → `StoreNetworkKey(키, overwrite = false)`), 무엇인가 있으면 그대로 둔다. 그래서
삭제가 Network Key만 지우지 못한 보드에서는 **그 네트워크의 키가 그 자리에 그대로 남고, OpenThread의 키와
같은 자리에 같은 모양으로 있다.** 저장소를 들여다보는 것으로는 둘을 가를 수 없다. 자리로 추정하지 않는다.

가르는 것은 그 부팅이 한 일이다.

1. **OpenThread를 시작하기 전에 지운다.** record가 최종 `RELEASED`인 보드는 부팅마다, `thread_manager_init()`
   보다 먼저, 두 저장소 자체를 지우고(`pb_stored_network_erase()`) 다시 읽어 비어 있음을 확인한다
   (`pb_mgmt_released_prestart()`, `power_base/main/app_main.c`의 4b). 그 자리에 있던 것이 이전 부팅의 OpenThread
   키든 지우지 못한 네트워크의 키든 여기서 없어지고, 없어졌다는 것을 read-back이 확인한다. 이 시점에는 NVS만
   있으면 된다: OpenThread도, 그 키를 읽어 들인 PSA 키 저장소도 아직 없다. 다른 record(등록됨, 삭제 대기,
   없음)에서는 아무것도 읽지도 지우지도 않는다. 그 보드들은 저장된 것이 필요하다.
2. **그 뒤에 생기는 키를 저장소 인터페이스에서 본다.** 두 저장소를 다시 읽어 비어 있음을 확인한 순간부터
   `0x20001`에 대한 쓰기와 제거를 센다(`pb_volatile_store_its_watch_*`). 이미지의 모든 키 저장소 쓰기는
   `-Wl,--wrap`으로 이 component를 지나므로, OpenThread가 시작하며 자기 키를 넣는 것이 여기서 보인다. PSA 키
   저장소는 영속 키 하나를 import할 때 저장소에 정확히 한 번 쓰고, 이미 있는 키 위에는 import를 거부한다
   (바꾸려면 먼저 지워야 한다). 그 한 번의 쓰기는 내용을 RAM에 보관해 두고, 두 번째 쓰기나 제거가 보이면
   지운다.
3. **물을 때 대조한다.** `pb_stored_network_holds_no_network()`는 다음이 모두 참일 때만 통과한다.
   - `openthread` namespace에 항목이 하나도 없다.
   - 영속 키 `0x20002`~`0x20007`이 없다(저장된 Active Dataset은 자기 Network Key 사본 `0x20003`을 반드시 둔다).
   - `0x20001`은 **없거나**, 있다면 (a) 이 부팅에서 두 저장소가 비어 있음을 읽은 적이 있고 (b) 그 뒤로 그
     자리에 쓰기가 정확히 한 번, 제거는 없었으며 (c) 지금 flash에 있는 항목이 그 쓰기와 **바이트까지 같다.**
   - 어느 읽기든 "있음"/"없음"이 아닌 답(읽지 못함)은 통과가 아니다.

그래서 재시작 직후의 저장소는, 1이 끝나기 전에는 통과하지 못한다. 이전 부팅이 만든 OpenThread의 키도 이
부팅에게는 출처를 모르는 키다. 1이 실패한 부팅(지우지 못했거나 읽지 못함)은 OpenThread가 남은 키 위에서
시작하고, 부팅 경로의 대조(`pb_mgmt_client_check_final_release()`)가 통과하지 못해 OpenThread의 삭제로 한 번 더
지우고 read-back한다. 그것도 안 되면 그 부팅은 영수증을 내주지 않고 다음 부팅이 다시 한다. 지워진 뒤에는
OpenThread가 새 키를 만들고 영수증이 다시 나온다: 수동 "삭제 확인"은 기존 키가 실제로 없어진 다음에만 된다.

- 대가: 해제된 보드는 부팅마다 flash를 두 번 쓴다(이전 부팅의 키 제거, 새 키 저장). 해제된 채 전원이 들어와
  있는 여분 보드에만 해당한다.
- 2026-10-08의 두 번째 구현까지는 3의 `0x20001` 조건이 "있어도 된다"였다. 삭제가 Network Key 하나만 남기고
  실패한 뒤의 **다음** 부팅은 settings도 다른 키도 없으므로 통과했고, 그 키가 남은 채 영수증을 내줬다.
- 확인한 것: `tools/check_power_base_ot_store.py`가 ESP-IDF 소스에서(시작 때의 키 생성은 그 자리 하나이고 있는
  키는 건드리지 않는다, 바꾸는 쪽은 먼저 지운다, PSA 키 저장소는 import 한 번에 `psa_its_set` 한 번이고 있는 키
  위에는 쓰지 않는다, 시작은 settings에 쓰지 않는다) 그리고 link된 이미지에서(키 저장소를 쓰고 지우는 것은 PSA
  키 저장소뿐이다, 1의 호출이 `app_main`에서 `thread_manager_init()`보다 앞에 있다). 실제 PSA 키 저장소로 한
  재현은 import 한 번에 그 자리의 쓰기가 한 번임을, 저장된 키 파일이 본 쓰기와 같음을, 그리고 **키 저장소가
  이미 읽어 들인 키는 파일을 지워도 RAM에서 계속 답한다**는 것(1이 맨 앞이어야 하는 이유)을 보인다.
- **실제 보드에서는 확인하지 못했다.** 해제된 보드의 부팅 로그에 `network stores are erased and read back
  empty before OpenThread starts`와 `released and no network is stored`가 차례로 나와야 한다.

재현으로 확인한 것은 OpenThread core와 PSA 키 저장소의 동작이다. ESP-IDF의 settings port·NVS·radio와 실제
보드는 실행해 보지 않았다.

**상태 전이와 저장 시점** (각 전이는 같은 key에 대한 `set_blob` + `commit` 한 번이다)

```
(미등록) --0x06 등록 완료--> ENROLLED --인증된 명령--> UNENROLLING --> RELEASED(삭제 대기)
                                             --삭제 + 확인--> RELEASED(최종, 영수증 보관) --새 등록 시도--> (record 삭제)
```

| 단계 | 하는 일 | 이 직후 전원이 끊기면 |
|---|---|---|
| A. 명령 검증 | MAC·EUI-64·hub_id·세대·nonce 확인. CoAP 응답 callback은 worker에 넘기기만 한다 | 아무 변화 없음(`ENROLLED`) |
| B. 신규 작업 차단 | downstream provisioning 거부, collector 제공 중지, exporter 정지, POGO downstream 비활성화 | RAM 상태뿐. 다음 부팅은 `ENROLLED`로 재개하고 허브가 같은 명령을 다시 준다 |
| C. journal | state = `UNENROLLING`, 명령 id 기록. 그 뒤 `UNENROLL_ACCEPTED`를 한 번 보낸다 | 다음 부팅은 서비스를 시작하지 않고 D부터 이어서 한다 |
| D. 등록 사용 중지 | state = `RELEASED`(삭제 대기). 키는 그대로. 다시 읽어 확인 | 다음 부팅은 재개하지 않는다. E부터 한다 |
| E. 통지 | `UNENROLL_DONE`/`RELEASED`를 허브에 보낸다(제한된 재시도, backoff). Thread는 아직 켜져 있다 | 다음 부팅이 저장된 Dataset으로 **통지만을 위해** Thread를 잠시 켜고 E를 다시 한다. 서비스·exporter·POGO는 켜지 않는다 |
| F. 삭제와 확인 | Dataset을 RAM에 읽어 둠 → Thread 정지 → OpenThread 저장소 삭제 → 두 저장소를 다시 읽어 비어 있음 확인 → 영수증 계산 → 최종 record 저장(키·네트워크 값 삭제, 영수증 MAC 기록) → 다시 읽어 확인 | 어느 지점에서 끊기든 record는 `RELEASED`(삭제 대기)다. 다음 부팅이 E(네트워크가 남아 있으면)와 F를 다시 한다 |
| G. 영수증 전달 | 저장소를 RAM으로 돌림 → 새 extended address → RAM의 Dataset으로 Thread → (flash를 다시 읽어 비어 있으면 영수증 전송)×최대 5회 → Thread 정지 → 두 저장소를 다시 읽음 → 재시작 | record는 이미 최종이다. 다음 부팅은 그 record를 저장소와 대조한다. 네트워크가 없으면 미등록 안전 대기이고 붙을 네트워크가 없으므로 **다시 보내지 않는다**(영수증은 BLE로 읽을 수 있다). 네트워크가 있으면 지우고 read-back한 뒤 재시작하고, 그때까지 영수증을 내주지 않는다 |

- **F의 어느 하나라도 실패하면 완료가 아니다.** 삭제 호출의 오류, 삭제는 오류 없이 끝났는데 저장소가 비어
  있지 않은 경우, 영수증 계산 실패, 최종 record 저장 실패, 다시 읽은 값의 불일치는 모두 record를 "삭제
  대기"로 둔다. 영수증은 만들어지지 않고, 아무것도 삭제됐다고 보고하지 않는다.
- **펌웨어의 반환값이 단계의 결과다.** poll은 F까지 끝났을 때만 `PB_MGMT_POLL_UNENROLLED`를 돌려주고,
  D까지는 됐지만 F가 끝나지 않으면 `PB_MGMT_POLL_RELEASED_NOT_ERASED`, journal이나 D의 저장이 실패하면
  `PB_MGMT_POLL_UNENROLL_INTERRUPTED`를 돌려준다. 부팅 경로는 `PB_MGMT_RELEASE_FINAL` /
  `PB_MGMT_RELEASE_INCOMPLETE`다. 어느 경우든 호출자는 재시작하고, 다음 부팅이 저장된 state에서 이어 간다.
- **영수증은 F 안에서만 만들어진다.** 그 전에는 어디에도 없다(RAM에도, record에도). G는 그 영수증을
  그대로 보낼 뿐이고, F가 끝나지 않으면 시작하지 않는다.
- **G는 결과를 바꾸지 않는다.** 전달됐든 아니든 반환값은 F의 것이다(`PB_MGMT_POLL_UNENROLLED`,
  `PB_MGMT_RELEASE_FINAL`). 단 하나의 예외는 세션 안이나 뒤의 read-back이 "비어 있음"이 아니었고 다시
  지워지지도 않는 경우로, 그때는 record를 삭제 대기로 되돌리고 `…RELEASED_NOT_ERASED`/`…INCOMPLETE`를
  돌려준다. **세션이 붙으면서 저장소를 썼다면 영수증은 나가지 않았으므로 허브는 `released`에 머문다**
  (2026-10-08의 첫 구현은 세션이 끝난 뒤에야 저장소를 읽었고, 그때는 허브가 이미 `completed`였다).
- E의 재시도: 한 부팅에서 최대 6회 보낸다. 2~6번째 시도 앞에 2·4·8·16·30초를 기다린다. 부팅당 한
  묶음이며, 묶음이 끝나면 확인을 받지 못했어도 F로 간다(통지를 못 받은 허브도 나중에 영수증으로 전부
  알게 된다). 그 뒤 보드는 미등록 안전 대기로 돌아가고 BLE 등록이 다시 열린다(BLE 빌드).
- E는 부팅 경로에서도 나간다. 2026-10-08 이전에는 그렇지 못했다: 요청을 보내는 CoAP socket은 서비스가
  시작될 때 열리는데(`power_base_transport_init()`), 끝나지 않은 해제를 마치는 부팅은 서비스를 시작하지
  않으므로 OpenThread가 전송을 `INVALID_STATE`로 거부했다. 지금은 통지와 영수증을 보내기 전에 socket을
  연다(이미 열려 있으면 아무 일도 없다). 통지가 나가지 못해도 F로 가도록 돼 있었기 때문에 삭제 자체는
  그때도 끝났다.
- G가 끝난 뒤의 재시작은 서비스 task·CoAP resource·callback의 수명을 한 번에 정리하기 위한 것이다. bootloader,
  partition table, phy, `cfg` 파티션(배선·센서 설정·`pb_ble_auth`의 salt/verifier)은 건드리지 않는다.
  설정 코드는 그대로 유효하다.

**부팅 규칙(버전 3)**

| record | 동작 |
|---|---|
| `ENROLLED` + 일치하는 완전한 Dataset | 버전 2처럼 재개. 서비스 시작 뒤 관리 poll 시작 |
| `ENROLLED`인데 Dataset 없음·불일치 | 버전 2와 같이 안전 대기 |
| `UNENROLLING` | 서비스 미시작. D → E → F → G |
| `RELEASED`(삭제 대기) | 서비스 미시작. E(저장된 네트워크가 있을 때만) → F → G(그 부팅의 F가 Dataset을 읽어 둘 수 있었을 때만). F가 또 실패하면 그 부팅은 안전 대기로 가고 다음 부팅이 다시 한다 |
| `RELEASED`(최종) | **OpenThread를 시작하기 전에** 두 저장소를 지우고 read-back한다. 그 뒤 OpenThread가 시작해 자기 키를 만들고, record를 저장소와 대조한다. 통과하면 미등록 안전 대기: BLE 등록 가능, 영수증을 내줄 수 있다. 통과하지 못하면(시작 전의 삭제가 실패했거나 읽지 못함) OpenThread의 삭제로 한 번 더 지우고 read-back한다: 통과하면 재시작(OpenThread가 Dataset을 읽어 들였을 때), 못 하면 안전 대기로 가되 영수증을 내주지 않고 다음 부팅이 다시 한다. 어느 경우에도 Thread를 켜지 않고 영수증을 Thread로 다시 보내지 않는다 |
| 손상·다른 보드용 | 버전 2와 같이 안전 대기 |

`pb-status`의 상태·결과 값(0~3, 0~18)은 늘리지 않았다. 해제 중인 보드는 BLE를 열지 않으므로 그 상태가
BLE로 보일 일이 없고, 해제가 끝난 보드는 `waiting` / `none`으로 보고한다.

## 5. BLE: `pb-start` `0x06`과 삭제 영수증 `0x07`

**`pb-start` `0x06` (목적지 + 관리 자격증명).** `0x02`·`0x04`의 바이트와 의미는 그대로다. `0x06`은 같은
endpoint의 별도 메시지다(130 + `pskd_len` = 136~162바이트).

| offset | 길이 | 내용 |
|---|---|---|
| 0 | 1 | version = 1 |
| 1 | 1 | type = 0x06 |
| 2 | 1 | `pskd_len` (6~32) |
| 3 | 1 | 0 |
| 4 | 8 | 대상 EUI-64 |
| 12 | 16 | request id |
| 28 | 16 | challenge |
| 44 | 1 | 목적지 형식 버전 = 1 |
| 45 | 1 | 0 |
| 46 | 2 | port, big-endian |
| 48 | 16 | IPv6 주소 |
| 64 | 1 | 관리 형식 버전 = 1 |
| 65 | 1 | 0 |
| 66 | 16 | hub_id (모두 0이면 안 됨) |
| 82 | 16 | 등록 세대 (모두 0이면 안 됨) |
| 98 | 32 | 관리 키 (모두 0이면 안 됨) |
| 130 | `pskd_len` | PSKd |

응답은 4바이트 `[1]`=**0x86**. code는 `0x04`와 같고 `INVALID_MANAGEMENT`(0x1e)가 더 있다: 관리 형식
버전 ≠ 1, 또는 hub_id·세대·키가 0. 길이·예약 바이트 오류는 `MALFORMED`. request id의 재전송 판정
digest에는 관리 block 전체가 들어가므로, 같은 request id에 다른 세대·키를 보내면 `REQUEST_CONFLICT`다.
복호화된 요청 버퍼, admission의 slot, worker의 stack 사본은 모든 경로에서 지운다.

golden vector(보드 `tools/test_power_base_ble_admission.c`, 앱 `CodecTests.swift`에 같은 값):

```
대상 6055f9fffe001234, request id a0..af, challenge c0..cf, port 5684,
주소 fd11:2233:4455:0:8c1f:2e3d:4c5b:6a79, hub_id 10..1f, 세대 20..2f, 키 30..4f, PSKd "J01NME7K"
요청 01060800 6055f9fffe001234 a0a1…af c0c1…cf 0100 1634 fd112233445500008c1f2e3d4c5b6a79
     0100 101112…1f 202122…2f 303132…4f 4a30314e4d45374b
응답 01860100 (ACCEPTED), 01861e00 (INVALID_MANAGEMENT)
```

**삭제 영수증 `0x07` (`pb-status` endpoint의 두 번째 메시지, capability `release_receipt_v1`).** "지웠고
확인했다"는 영수증을 Security 2 세션 안에서 읽는다. 보드가 스스로 보낸 영수증(4절 G)과 같은 바이트이고,
그 전달이 되지 않았을 때와 이전 허브·펌웨어 조합에서 쓰는 경로다. 읽는 것은 보드의 아무것도 바꾸지 않고
challenge도 필요 없다. GATT의 endpoint 구성은 그대로다. 이 메시지와 capability는 바뀌지 않았다.

요청(12바이트): `[0]` version `[1]` 0x07 `[2]` 0 `[3]` 0 `[4..11]` 대상 EUI-64.

응답(112바이트):

| offset | 길이 | 내용 |
|---|---|---|
| 0 | 1 | version = 1 |
| 1 | 1 | type = 0x87 |
| 2 | 1 | code: `OK`, `UNSUPPORTED_VERSION`, `MALFORMED`, `WRONG_TARGET` |
| 3 | 1 | 0 |
| 4 | 8 | 이 보드의 EUI-64 |
| 12 | 1 | 영수증 형식 버전 = 1 |
| 13 | 1 | 1 = 영수증 있음, 0 = 없음 |
| 14 | 2 | 0 |
| 16 | 96 | 영수증: 관리 메시지 `UNENROLL_DONE`/`ERASED` 전체(6절). 없으면 0 |

보드에 영수증이 있는 것은 record가 최종 `RELEASED`이고 **요청을 받은 그때 저장소에 네트워크가 없는**
동안이다: 삭제가 확인된 뒤부터 새 등록 시도가 수락돼 record가 지워질 때까지. 최종 record가 있어도
OpenThread settings에 항목이 있거나, Dataset·PSKc 키가 남아 있거나, **Network Key 자리에 이 부팅이 만드는 것을
보지 못한 키가 있거나**, 저장소를 읽지 못하면 응답은 "없음"(`[13]` = 0, 영수증 자리는 0)이다(4절 "OpenThread가
시작한 뒤에 묻는 것"). 메시지 형식과 값은 그대로다. 그래서 같은 보드를 다시 등록하는 앱은 `pb-start`를 보내기 **전에**
영수증을 읽어 허브에 전달한다(10절). 영수증에는 비밀이 없고(식별자와 MAC), 그 등록의 키를 가진 허브만
진위를 판단할 수 있다. 앱은 형식만 확인하고 그대로 전달한다. capability가 없는 보드는 이 type에
`pb-status`의 다른 알 수 없는 type처럼 답한다(40바이트, `MALFORMED`).

## 6. 관리 통신: CoAP `pogo/v1/mgmt` (규약 v1)

보드가 허브의 ingest 포트에 Confirmable POST를 보내고, 허브가 그 ACK에 payload를 실어 답한다.
`bridge_export_proto`와는 별개의 형식이다(`components/power_base_mgmt/pb_mgmt_proto.h`). 모든 정수는
big-endian이고 모든 메시지는 고정 길이다.

**공통 header (44바이트)와 MAC**

| offset | 길이 | 내용 |
|---|---|---|
| 0 | 1 | version = 1 |
| 1 | 1 | type |
| 2 | 1 | flags (type별. 정의되지 않은 bit는 0) |
| 3 | 1 | 0 |
| 4 | 8 | 보드 EUI-64 |
| 12 | 16 | hub_id |
| 28 | 16 | 등록 세대 |
| 44 | … | body |
| 끝 32 | 32 | `HMAC-SHA-256(관리 키, "pogo-pb-mgmt-v1\0" ‖ 메시지의 MAC 앞 전부)` |

| type | 방향 | body | 전체 길이 |
|---|---|---|---|
| 0x01 `POLL` | 보드 → 허브 | board_nonce(16) ‖ hub_nonce_echo(16) ‖ state(1) ‖ 0(3) | 112 |
| 0x81 `IDLE` | 허브 → 보드 | board_nonce(16) ‖ hub_nonce(16). flags 0x01 = 명령 대기 중 | 108 |
| 0x82 `UNENROLL` | 허브 → 보드 | board_nonce(16) ‖ 명령 id(16) | 108 |
| 0x02 `UNENROLL_ACCEPTED` | 보드 → 허브 | 명령 id(16) | 92 |
| 0x03 `UNENROLL_DONE` | 보드 → 허브 | 명령 id(16) ‖ result(1) ‖ 0(3). result 1 = `RELEASED`(통지), 2 = `ERASED`(영수증) | 96 |
| 0x83 `RESULT_ACK` | 허브 → 보드 | 명령 id(16) ‖ 확인한 type(1) ‖ 확인한 result(1) ‖ 0(2) | 96 |
| 0x05 `HELLO` | 보드 → 허브 | capability(4, big-endian bitmask) ‖ 0(4) | 84 |
| 0x85 `HELLO_ACK` | 허브 → 보드 | capability(4, big-endian bitmask) ‖ 0(4) | 84 |
| 0x06 `LOCATE` | 보드 → 허브(일 수 있는 곳) | board_nonce(16) ‖ 0(4) | 96 |
| 0x86 `LOCATE_ACK` | 허브 → 보드 | board_nonce(16) ‖ IPv6 주소(16) ‖ port(2, big-endian) ‖ 0(2) | 112 |

`LOCATE`·`LOCATE_ACK`는 2026-10-09에 더했다(12절). `HELLO` 때와 같은 방식이다: version 바이트는 1 그대로이고,
앞의 여덟 메시지의 바이트와 뜻은 바뀌지 않았으며, 두 type을 모르는 허브는 `4.00`으로 답한다. 가장 긴 메시지는
여전히 112바이트다. `LOCATE_ACK`의 port 0과 주소 `::`는 형식 오류다. 주소가 보드가 쓸 수 있는 것인지(자기
네트워크의 Mesh-Local EID인지)는 형식이 아니라 보드의 검사다.

`HELLO`·`HELLO_ACK`는 2026-10-08에 더했다. **version 바이트는 1 그대로다**: 위 여섯 메시지의 바이트와 뜻은
하나도 바뀌지 않았고, 두 type을 모르는 상대는 다른 모르는 type과 같이 답한다(허브는 `4.00`). 보내는 쪽은
그것을 "capability 없음"으로 받는다. 받는 쪽은 모르는 capability bit를 무시한다(거부하지 않는다). flags는
없고 뒤의 네 바이트는 0이어야 한다.

- 보드는 부팅마다, 허브가 poll에 처음 답한 직후(그 세대가 `active`인 것이 분명해진 뒤) `HELLO`를 한 번
  보낸다. 알릴 capability가 없는 펌웨어는 보내지 않는다. 답이 없으면 뒤의 poll과 함께 최대 3회까지 다시
  보내고, 거부(`4.xx`)당하면 그 부팅에서는 다시 보내지 않는다. poll과 명령의 nonce에는 영향이 없다.
- 허브는 `active` 세대의 `HELLO`만 받는다(`provisional`·`released`는 `4.01`). 값을 자격증명과 함께 저장한
  뒤에 `HELLO_ACK`를 준다. 저장하지 못하면 `5.03`이고 그 값을 믿지 않는다.
- nonce가 없다. 다시 보낸 `HELLO`는 그때의 capability를 다시 말할 뿐이고, 그것으로 바뀌는 것은 표시용
  값(`erase_confirmation`)뿐이다. 삭제를 완료시키지 못한다.

`UNENROLL_DONE`의 두 result는 result 바이트가 다르므로 MAC도 다르다. 키 없이는 하나를 다른 하나로 바꿀 수
없다. `RESULT_ACK`의 "확인한 result"는 `UNENROLL_DONE`을 확인할 때 그 result이고, `UNENROLL_ACCEPTED`를
확인할 때는 0이다. 보드는 자기가 보낸 통지의 result를 되돌려 준 ACK만 그 통지의 확인으로 받는다.

golden MAC(EUI-64 6055f9fffe001234, hub_id 10..1f, 세대 20..2f, 키 30..4f, 명령 id e0..ef. 펌웨어·허브·
Swift·Python test에 같은 값. Python `hmac`으로 따로 계산했다):

```
UNENROLL_DONE / RELEASED  02f00a8d63fba56ff747911889aae31a29b60d053178db827a8e8730a05b7eba
UNENROLL_DONE / ERASED    efd7833baf44c006d3f980b77630e2250e2d7f61328cceee83ac30a4a41fe01b
RESULT_ACK (RELEASED)     b49c98d6300315bc13e54178bd1e8600401dd5c0304858516fdf469a33a84112
RESULT_ACK (ERASED)       174d78c5ec891b13dad88c9eaf93c5c4d9b21602eafeab955569673dbf8085e4
RESULT_ACK (ACCEPTED)     33e83b068d5d9ff3975fae6cd65eccb1c5458c957816eb2af45475d826bac23d
HELLO (capability 1)      5ce6b6e36eb4f9ce77dade4eb4b03fded2243fe4af50ef92169d2d2142decb21
HELLO_ACK (capability 1)  9fb3c6c60ba32ecda5ca9efafdadbb32e0c2ac6771eb9cb51e5f35e820d9c314
LOCATE (nonce b0..bf)     5ceec372200fa66bf2904adb8d47502e1b314bbee8ef110d19789272f4989231
LOCATE_ACK (nonce b0..bf, fd11:2233:4455:0:8c1f:2e3d:4c5b:6a79, port 5684)
                          6fa4e266b7ef48249a301bb5639a81953cb0cd7494dcd29b69d3eaa271faf831
```

CoAP 응답 코드: 인증된 답은 `2.05` + payload. 형식 오류 `4.00`, MAC·세대·hub 불일치 `4.01`(payload
없음, 왜 틀렸는지 구분해 알려 주지 않는다), 인증은 됐지만 허브가 모르는(또는 취소·대체된) 명령 id의
`UNENROLL_ACCEPTED`·`UNENROLL_DONE`은 `4.04`, 저장 실패나 관리 기능이 꺼진 상태는 `5.03`. 삭제돼 차단된
Base의 데이터 메시지(`sample`·`topology`·`heartbeat`·removal)는 `4.03`이다(7절). 관리 resource는 차단과
무관하다: 차단된 Base의 통지와 영수증은 계속 받는다. `LOCATE`는 `provisional`·`active` 자격증명에만 `2.05`로
답하고, `released`·대체된 세대·모르는 보드·다른 허브·틀린 MAC은 다른 메시지와 같이 `4.01`, 허브가 자기
Thread 주소를 지금 알 수 없으면 `5.03`이다(12절).

**영수증이 관리 resource로 올 때.** 허브는 그 세대의 키로 MAC을 검증하고, 작업의 명령 id가 맞으면 완료한 뒤
`RESULT_ACK`(`ERASED`)로 답한다(7절 "`completed`가 될 때"). 완료하면서 키를 지우므로, **같은 영수증이 다시
오면** 키 없이 답해야 한다: 완료된 작업이 기억하는 영수증의 MAC과 같고 보드·세대·명령 id가 같은 메시지는 그
영수증과 바이트가 같은 메시지이므로, 그때 준 확인을 저장해 둔 MAC으로 다시 만들어 준다. 아무것도 저장하지
않는다. 그 밖의 것(다른 MAC, 다른 명령 id, 그 등록의 통지나 poll)은 키 없는 메시지로 `4.01`이다. 완료된
작업이 보관 기간(10분)을 지나 지워진 뒤에도 `4.01`이다.

**재생 방지**

- 보드는 `POLL`마다 새 board_nonce(16바이트 CSPRNG, RAM)를 만든다. `IDLE`·`UNENROLL`은 진행 중인
  poll의 nonce를 그대로 담아야 한다. 재부팅하면 nonce가 사라지므로 이전 부팅에 대한 응답은 쓸 수 없다.
- 허브는 `IDLE`마다 hub_nonce(16바이트, RAM, 60초, 1회용)를 준다. 허브는 직전에 준 hub_nonce를 그대로
  돌려준 `POLL`에만 `UNENROLL`을 싣는다. 캡처한 `POLL`을 다시 보내도 `IDLE`만 받는다.
- 보드는 poll마다 직전 `IDLE`의 hub_nonce를 돌려준다. 그래서 평소(10초마다 poll하는 보드)에는 삭제
  작업이 생긴 뒤의 **다음 poll 하나**에 명령이 실린다. poll이 두 번 필요한 것은 돌려줄 유효한 nonce가
  없을 때뿐이다: 보드가 막 부팅했거나, 허브가 다시 시작했거나, nonce가 60초를 넘겼을 때. 그때 허브는
  `IDLE`에 flags 0x01(명령 대기 중)을 세우고, 그것을 본 보드는 주기를 기다리지 않고 바로 다시 poll한다.
- 보드는 MAC, 자기 EUI-64, record의 hub_id·세대, nonce, 그리고 record state가 `ENROLLED`인 것을 모두
  확인한 명령만 받는다. 다른 보드·다른 허브·이전 세대·변조·재생된 명령은 버린다(상태 변화 없음).
- `UNENROLL_ACCEPTED`·`UNENROLL_DONE`·`RESULT_ACK`에는 nonce가 없다. 같은 (EUI-64, hub, 세대, 명령 id,
  result)에 대해 항상 같은 바이트이고, 다시 받아도 결과가 같다(멱등). 영수증도 같은 이유로 몇 번을 읽어
  전달해도 같은 바이트다.

**보드의 poll 주기.** attach 상태에서 10초마다. 응답이 없거나 인증되지 않으면 아무것도 하지 않고 다음
주기를 기다린다. 데이터 경로(exporter)와는 별도 task다.

## 7. 허브의 삭제 작업

**API** (TLS + 페어링된 기기의 bearer token. 평문은 `403 tls_required`, 토큰 없음은 `401`. 모든 응답
`Cache-Control: no-store`. 등록 세션의 `DELETE`와는 다른 경로다)

| 요청 | 응답 |
|---|---|
| `POST /api/v1/power-base-removals` `{"power_base_uuid":"<16 hex>"}` (본문 ≤ 256바이트) | `202` + 작업. 같은 대상의 끝나지 않은 작업이 있으면 `200` + 그 작업 |
| `GET /api/v1/power-base-removals` | `200` `{"removals":[…]}` |
| `GET /api/v1/power-base-removals/<removal_id>` | `200` + 작업, 없으면 `404 removal_not_found` |
| `DELETE /api/v1/power-base-removals/<removal_id>` | `200` + 작업(`cancelled`). 취소할 수 없으면 `409 removal_not_cancellable` + 작업 |
| `POST /api/v1/power-base-removals/receipt` `{"receipt":"<192 hex>"}` (본문 ≤ 512바이트) | `200` + 작업(`completed`). 같은 영수증을 다시 보내도 같은 답 |

오류: `404 unknown_power_base`(허브가 모르는 Base), `409 power_base_management_unsupported`(유효한 관리
자격증명 없음), `409 enrollment_in_progress`(같은 EUI-64의 등록 세션이 진행 중), `422 invalid_power_base_uuid`,
`503 removal_unavailable`(저장 불가·시작 실패), `503 too_many_removals`(보드를 기다리는 작업 32개),
`409 removal_capacity_full`(차단 용량이 없음. 아래 "차단 용량"). 영수증: `422 invalid_receipt`(192자리 소문자
hex가 아니거나, `UNENROLL_DONE`/`ERASED` 형식이 아님. 통지를 넣은 경우 포함), `409 receipt_rejected`(형식은
맞지만 이 허브의 삭제 작업에 대해 검증할 수 없음: 다른 허브, 모르는·대체된 세대, 취소된 작업, MAC 불일치.
어느 것인지 구분해 알려 주지 않는다).

작업 JSON:

```json
{"removal_id":"<32 hex>","power_base_uuid":"<16 hex>","generation":"<32 hex>",
 "state":"pending","result_unknown":false,"cancellable":true,
 "created_at_unix_ms":0,"updated_at_unix_ms":0,"completed_at_unix_ms":null,
 "requested_by_device_id":"<16 hex>","affected_module_count":2,
 "settings_cleanup_pending":false,"erase_confirmation":"automatic"}
```

`erase_confirmation`(2026-10-08, 선택 필드)은 끝나지 않은 작업에만 있고, 맨 뒤에 붙는다. 끝난 작업의 JSON은
이전과 같다.

| 값 | 조건 |
|---|---|
| `automatic` | 그 등록의 보드가 `HELLO`로 `THREAD_RECEIPT`를 알렸고, `result_unknown`이 아니며, `released`라면 그렇게 된 지 4분이 지나지 않았다. 아무도 할 일이 없다 |
| `manual` | 보드가 알리지 않았다(이전 펌웨어). 또는 알렸지만 `released` 뒤 4분 안에 영수증이 오지 않았다. 또는 `result_unknown`이다. 누군가 BLE로 영수증을 읽어 전달하거나 같은 보드를 다시 등록해야 한다 |

4분은 보드가 영수증을 보내는 데 걸릴 수 있는 시간(통지의 답이 유실된 경우의 재시도 약 75초 + 세션 약
90초)에 여유를 둔 값이다. **표시용이다**: 상태를 바꾸지 않고, 그 뒤에 도착한 영수증도 그대로 작업을
완료한다. 허브를 다시 시작해도 `released`가 된 시각에서 센다.

삭제 작업은 허브의 Base 목록처럼 페어링된 모든 기기가 같이 본다. 두 기기가 같은 Base를 삭제하면 같은
작업을 받는다. 영수증은 페어링된 어느 기기가 전달해도 된다. 기기의 token은 누가 전달하는지만 말해 주고,
영수증의 내용은 MAC이 맞을 때만 믿는다.

**상태**

| state | 뜻 | 취소 |
|---|---|---|
| `pending` | 저장됨. 명령이 한 번도 나가지 않았다(보드가 오프라인이면 여기서 기다린다: "삭제 대기") | 가능 |
| `sent` | 인증된 poll에 명령을 실어 보냈다. 도착 여부는 모른다 | 불가 |
| `accepted` | 보드의 인증된 `UNENROLL_ACCEPTED`를 받았다 | 불가 |
| `released` | 보드의 인증된 `UNENROLL_DONE`/`RELEASED`를 받았다. 그 보드는 이 등록을 다시 쓰지 않는다. 허브는 Base를 목록에서 뺐다(아래). **완료가 아니다**: 보드의 저장 정보 삭제는 확인되지 않았다. 보드가 스스로 영수증을 보내는 경우 보통 몇 초~수십 초 머문다 | 불가 |
| `completed` | 보드의 인증된 삭제 영수증(`UNENROLL_DONE`/`ERASED`)을 확인했다. 보드가 관리 resource로 보낸 것이든 앱이 전달한 것이든 | — |
| `cancelled` | `pending`에서 취소됐다 | — |
| `superseded` | 완료되기 전에 같은 EUI-64의 새 등록 세대가 활성화됐다 | — |

`result_unknown`은 `sent`·`accepted`가 5분 넘게 진전이 없을 때 `true`가 되는 표시다("결과 확인 불가").
상태를 바꾸지 않으며 늦게 도착한 통지나 영수증은 그대로 작업을 진행시킨다. 타임아웃, heartbeat 중단,
재시작, 통지만으로는 `completed`가 되지 않는다. `released`는 영수증이 오거나 새 등록이 대체할 때까지
`released`로 남는다(시간이 지나도 지워지지 않는다).

- 작업은 수락된 순간 `power_base_mgmt.json`에 저장된다. 앱이 닫히거나 요청한 기기가 사라져도 유지되고,
  허브가 다시 시작하면 그대로 복구된다(`sent`·`accepted`는 그 상태로 남고 보드의 다음 poll에 같은 명령
  id를 다시 준다).
- 허브의 "재시도"는 보드의 poll에 대한 답이다. poll마다 한 번, 같은 명령 id로만 준다. 허브가 스스로
  패킷을 보내는 일은 없다.
- 보드를 기다리는 작업(`pending`·`sent`·`accepted`)은 32개까지다. 끝난 작업은 32개까지, 끝난 지 10분 동안
  보관한다. 설정 정리가 남은 작업은 그것이 끝날 때까지 보관한다.

**`released`가 될 때 허브가 하는 일** (한 번. 통지 없이 영수증이 먼저 도착하면 `completed`가 될 때 같이 한다)

한 번의 파일 저장에 작업 상태, 차단, 설정 정리 대상을 함께 쓰고, 저장이 성공한 뒤에 Registry를 바꾼다.

1. 그 EUI-64를 ingest 차단 목록에 넣는다(영속). 이후 그 Base의 heartbeat·topology·sample·removal은
   `4.03`으로 거부되고 Registry를 바꾸지 못한다.
2. Registry에서 그 Base와 **그 Base 아래에 등록된** 노드를 지운다. Registry의 노드 key가 (Base, 모듈)이므로
   이미 다른 Base로 옮겨 그 Base 아래에 등록된 같은 모듈은 지워지지 않는다.
3. Matter endpoint는 기존 Reconcile 경로가 지운다(snapshot에서 사라진 모듈 → `remove_endpoint`).
   `endpoint_map.bin`의 UUID→endpoint id 기록은 지우지 않으므로 같은 모듈이 다시 나타나면 같은 id를 쓴다.
   다른 모듈의 endpoint, KVS, 페어링, TLS identity, hub_id는 건드리지 않는다.
4. 방 배정을 정리할 모듈(2에서 지운 모듈 중 다른 Base 아래에 남아 있지 않은 것)을 상태 파일에 적는다.
   실제 정리는 아래 "설정 정리"가 한다.

`pending`·`sent`·`accepted` 동안에는 차단하지 않는다. 보드는 아직 등록된 상태이고 데이터는 계속
들어온다. `cancelled`·`superseded`는 아무것도 지우거나 차단하지 않는다.

**`completed`가 될 때.** 영수증의 형식(`UNENROLL_DONE`, result `ERASED`), hub_id, 그 세대의 키로 계산한
MAC, 작업의 명령 id를 확인한다. 맞으면 작업을 `completed`로 저장하고 그 세대의 키를 지운다. 완료된 작업은
영수증의 MAC과, 그 영수증에 준 확인(`RESULT_ACK`)의 MAC을 기억하므로 같은 영수증이 다시 와도 같은 답을
한다(키 없이): HTTP로는 같은 작업을, 관리 resource로는 같은 확인을. 차단은 그대로 남는다. 영수증이 어느
길로 왔는지는 기록하지 않는다(같은 메시지다).

보드가 `HELLO`로 capability를 알린 것, 통지를 보낸 것, 시간이 지난 것은 어느 것도 `completed`를 만들지
않는다.

**상태 파일의 추가 필드(schema 2 그대로).** 자격증명에 `board_capabilities`(정수, `HELLO`의 값. 없으면 0),
완료된 작업에 `receipt_ack_mac`(64 hex. 없으면 0). 둘 다 선택 필드다: 이전 허브가 쓴 파일은 그대로 읽히고
(그 보드는 "알린 적 없음", 그 작업의 영수증은 관리 resource에서 다시 확인해 주지 않음), 새 허브가 쓴 파일을
이전 허브가 읽으면 두 필드를 무시한다. HELLO가 실을 수 없는 값은 손상된 파일로 다룬다.

**차단 용량.** 차단은 자리를 만들기 위해 버려지지 않는다. 시간, 완료, 완료 이력의 정리, 허브 재시작 어느
것도 차단을 풀지 않는다. 차단을 푸는 것은 한 가지뿐이다: 그 EUI-64의 새 등록 세대가 활성화되는 것(새
키로 인증된 poll).

- 차단 목록은 64개까지다. 차단이 될 자리는 **삭제 요청을 받을 때** 잡아 둔다: 쓰이는 자리 = 차단된 Base
  수 + 차단으로 이어질 수 있는 작업(`pending`·`sent`·`accepted`) 수. 이것이 64이면 새 요청은
  `409 removal_capacity_full`로 거부되고, 작업이 만들어지지 않으므로 그 보드에는 어떤 명령도 가지 않는다.
- 잡아 둔 자리는 취소(`cancelled`)하거나 새 등록이 대체(`superseded`)하면 돌아온다. 별도의 counter 없이
  저장된 작업과 차단 목록에서 매번 계산하므로 재시작해도 같다. 같은 Base에 대한 중복 요청은 같은
  작업이라 자리를 더 쓰지 않는다. 저장에 실패한 요청은 자리를 잡지 않는다.
- 요청을 받아들였으면 보드가 통지할 때 자리가 있다. 그래도 목록이 가득한 것이 발견되면(파일을 손으로
  고친 경우) 기존 차단을 버리지 않고 그 통지를 `5.03`으로 거부한다.
- 자격증명은 96개까지다. 새 등록 세션은 가장 오래된 `provisional`만 밀어낼 수 있고, 그것이 없으면
  거부된다. `active`와, 영수증을 기다리는 `released` 작업의 자격증명은 밀려나지 않는다. 차단 64개가 모두
  영수증을 기다리는 동안에는 등록된 Base를 32개까지 더 둘 수 있다. 영수증으로 완료되면 그 자격증명의
  자리가 돌아온다.
- 64개가 모두 찬 상태를 푸는 방법은 삭제했던 보드를 다시 등록하는 것뿐이다. 운영자가 차단을 직접 지우는
  명령은 만들지 않았다.

**설정 정리(방 배정).** 삭제된 Base와 함께 빠진 모듈의 방 배정을 공유 설정(`settings.json`)에서 미배정으로
바꾼다. 이름은 남긴다(모듈 설정은 모듈 UUID 기준이라 다시 연결하면 이름이 돌아온다). 보드의 등록 해제와는
별개의 일이고, 작업의 `settings_cleanup_pending`으로 따로 보인다.

- 정리 대상과 진행 상태는 상태 파일의 `cleanups`에 있다: 작업 id, EUI-64, 세대, 모듈 목록, 시도 횟수,
  다음 시도 시각. `released`를 저장하는 **같은 저장**에 들어가므로, 그 직후 프로세스가 끝나도 다음
  시작에서 남아 있다.
- 실제 설정 저장은 전용 worker thread가 한다. 허브나 Registry의 mutex를 잡지 않고, ingest·HTTP thread에서
  하지 않는다. worker는 `released` 직후와 5초마다 돈다.
- 시도할 때마다 대상 모듈이 **지금** 어디에 있는지 Registry에서 다시 본다. 어느 Base 아래에든 등록돼 있는
  모듈(삭제 전에 옮겨졌거나, 그 뒤 다른 Base에 끼웠거나, 같은 Base가 새 세대로 다시 등록된 경우)은 그
  방 배정이 더 이상 삭제된 Base의 것이 아니므로 건드리지 않고 정리 대상에서 영구히 뺀다.
- 재시작 직후에는 Registry가 비어 있다. 파일에서 읽은 정리는 120초를 기다린 뒤에 시도한다(Base들이 다시
  보고해 모듈의 현재 위치가 보일 때까지).
- 실패(설정 파일 저장 실패, 다른 기기와의 revision 충돌이 네 번 연속, 설정 파일 손상)는 로그만 남기고
  끝내지 않는다. 5초부터 두 배씩 늘려 최대 10분 간격으로 계속 다시 시도한다. 성공하면 `cleanups`에서
  지운다.
- 정리는 성공했는데 "끝났다"는 표식의 저장이 실패하면 pending으로 남고 다시 한다. 이미 방이 없는 모듈을
  다시 정리해도 설정은 바뀌지 않고 새 revision도 생기지 않는다(멱등).
- 중복 통지, 영수증, 여러 번의 재시작은 정리를 다시 만들지 않는다.
- 설정은 기존 revision 규칙으로 저장하므로 다른 기기는 평소처럼 새 revision 하나를 받는다. 대상이 아닌
  모듈의 이름·방, 방 목록, 집 이름, 그사이 다른 기기가 저장한 변경은 그대로다.
- 한계: 정리가 pending인 동안 그 모듈이 어느 Base에도 없는 채로 사용자가 방을 다시 지정하면, 다음 시도가
  그 지정을 지운다.

**경쟁**

| 상황 | 결과 |
|---|---|
| 같은 Base에 삭제 요청 두 개(같은 기기·다른 기기) | 같은 작업 |
| 등록 세션이 진행 중인 EUI-64에 삭제 요청 | `409 enrollment_in_progress` |
| 삭제 작업이 끝나지 않은 EUI-64에 새 등록 세션 | 허용. 새 세대가 활성화되면 그 작업은 `superseded`. (등록된 보드는 BLE를 열지 않으므로, 이 경우는 보드가 이미 해제됐거나 따로 초기화된 것이다) |
| 다른 Base의 등록·삭제 | 서로 막지 않는다(등록 세션은 기존대로 허브당 하나) |
| 차단 용량의 마지막 자리에 요청 여러 개 | 자리 수만큼만 받아들이고 나머지는 `409 removal_capacity_full` |

## 8. '삭제 완료'가 보장하는 것

`completed`는 **그 보드가, 그 등록 세대에 대해, 저장된 Thread 가입 정보(OpenThread의 영속 저장소: settings와
영속 PSA 키)를 지우고, 지워진 것을 다시 읽어 확인하고, 최종 record를 저장한 뒤에 만든 영수증을 허브가
검증했다**는 뜻이다. 영수증이 보드에서 직접 왔든 앱을 거쳐 왔든 같다.

`released`는 그보다 적은 것을 보장한다: 그 보드가 이 등록을 다시 쓰지 않겠다고 영속 저장했고(재부팅해도
이 등록으로 서비스를 재개하지 않는다), 허브가 그 통지를 검증해 Base를 목록에서 뺐다. 저장 정보가 지워졌는지는
말하지 않는다.

`completed`가 뜻하지 **않는** 것:

- 센서 모듈의 flash나 Dataset을 지웠다는 뜻이 아니다. 센서는 이 Base를 통한 허브 서비스에서 빠질 뿐이고,
  다시 연결하면 기존 POGO provisioning이 그대로 동작한다.
- 그 순간 보드의 RAM에 네트워크가 없다는 뜻이 아니다. 보드가 스스로 영수증을 보낼 때는 그 세션 동안 Dataset과
  키가 RAM에 있고(최대 약 90초), 세션이 끝나면 지우고 재시작한다. 영수증이 말하는 것은 flash다.
- 보드가 물리적으로 초기화됐다는 뜻이 아니다. 펌웨어, `cfg`, 설정 코드는 그대로다.
- 방 배정 정리가 끝났다는 뜻이 아니다. 그것은 `settings_cleanup_pending`이 따로 말한다.
- 실물에서 확인됐다는 뜻이 아니다. 영수증이 보장하는 것은 펌웨어가 위 순서를 지켰다는 것이고, 그 순서의
  각 호출이 실제 보드에서 기대대로 동작하는지는 실물 시험의 몫이다(이 문서 머리말의 검증 상태).

## 9. 실패·응답 유실·재부팅 정리

| 경우 | 보드 | 허브 | 앱 표시 |
|---|---|---|---|
| 보드 오프라인 | — | `pending` 유지 | 삭제 대기(취소 가능) |
| 명령 응답 유실 | 상태 변화 없음. 다음 poll | `sent` | 삭제 진행 중(취소 불가) |
| 통지(`RELEASED`) 유실 | 재시도 뒤 삭제로 진행, 영수증 보관 | `sent`/`accepted` → 5분 뒤 `result_unknown` | 결과 확인 불가. 영수증을 전달하면 바로 `completed`, 같은 보드를 다시 등록하면 `superseded` |
| 통지 도착, 삭제 실패 | `RELEASED`(삭제 대기). 부팅마다 다시 시도. 영수증 없음 | `released` | 등록 해제됨 · 삭제 확인 필요. "삭제 확인"은 "영수증 없음"으로 끝난다 |
| 삭제 성공, 최종 record 저장 실패 | `RELEASED`(삭제 대기). 다음 부팅이 다시 지우고 확인한 뒤 저장 | `released` | 위와 같음 |
| 삭제까지 완료, 영수증 자동 전달 | 최종 record(영수증 보관), 재시작 | `released` → `completed` | 삭제 중 → 삭제 완료. 사용자가 할 일 없음 |
| 삭제까지 완료, 영수증의 답 유실 | 같은 영수증을 다시 보낸다(최대 5회) | 첫 번째에 `completed`, 다시 온 것에는 같은 확인 | 삭제 완료 |
| 삭제까지 완료, 영수증 미전달(세션 중 전원 차단, 허브 꺼짐·불통, 부모 없음) | 최종 record(영수증 보관). 다음 부팅은 다시 보내지 않는다 | `released`, 4분 뒤 `erase_confirmation: manual` | 삭제 중 → 등록 해제됨 · 삭제 확인 필요 → "삭제 확인"으로 `completed` |
| 세션이 붙으면서 저장소를 씀(스위치가 경로에 없었던 경우) | 보내기 전의 read-back이 잡는다. **영수증을 보내지 않는다.** flash를 직접 다시 지우고 확인. 통과하면 최종 record 그대로(영수증은 BLE로), 안 되면 record를 삭제 대기로 되돌림(영수증 없음) | `released`. 4분 뒤 `erase_confirmation: manual` | 삭제 중 → 등록 해제됨 · 삭제 확인 필요. 다시 지워졌으면 "삭제 확인"으로 `completed`, 아니면 "영수증 없음"이고 다음 부팅이 다시 지운 뒤 그 부팅의 세션이 보낸다 |
| 영수증을 보낸 **뒤에** 저장소가 쓰임 | 다음 전송 전이나 세션을 내린 뒤의 read-back이 잡는다. 더 보내지 않고 flash를 직접 다시 지우고 확인. 안 되면 record를 삭제 대기로 되돌림 | 이미 `completed`(보낼 때 참이었던 영수증) | 삭제 완료. 보드는 부팅마다 다시 지운다. 순서로 막을 수 없는 유일한 경우다 |
| 그 세션 중 전원 차단(최종 record + 저장된 네트워크) | 다음 부팅이 OpenThread를 시작하기 전에 지우고 read-back한다. 그 삭제가 안 되는 동안 **BLE 영수증 "없음"** | 영수증이 가기 전이면 `released` | 등록 해제됨 · 삭제 확인 필요. 지워지기 전의 "삭제 확인"은 "영수증 없음" |
| 삭제가 Network Key만 남기고 실패(최종 record + 그 키 하나) | OpenThread는 시작하며 그 키를 그대로 둔다. 그 키는 OpenThread의 것으로 치지 않는다: **BLE 영수증 "없음"**. 부팅마다 시작 전에 지우고 read-back, 안 되면 OpenThread의 삭제로 한 번 더. 지워진 부팅부터 영수증 | 영수증이 가기 전이면 `released` | 등록 해제됨 · 삭제 확인 필요. 지워지기 전의 "삭제 확인"은 "영수증 없음", 지워진 뒤에는 된다 |
| 이전 펌웨어 보드(영수증을 보내지 않음) | 최종 record(영수증 보관) | `released`, 처음부터 `erase_confirmation: manual` | 등록 해제됨 · 삭제 확인 필요 → "삭제 확인"으로 `completed` |
| C~F 사이 전원 차단 | 4절의 표대로 다음 부팅이 이어서 한다 | 그대로 | 그대로 |
| 허브 재시작 | 영향 없음 | 작업·자격증명·차단·설정 정리 복구 | 그대로 |
| 해제 뒤 늦은 heartbeat·topology·sample | — | `4.03`, Registry 불변 | 목록에 다시 나타나지 않음 |
| 해제 뒤 같은 보드 재등록 | 새 record(새 세대·새 키). 영수증은 새 시도 전에 앱이 읽어 간다 | 영수증이 먼저 오면 `completed`, 아니면 새 세대 활성화 때 `superseded`. 차단 해제 | 새 Base로 등록 완료 |
| 이전 세대의 명령·통지·영수증이 새 등록에 도착 | MAC·세대 불일치로 버림 | `4.01` / `409 receipt_rejected` | — |
| 완료된 이전 삭제의 영수증이 재등록 뒤 다시 도착 | — | 그 작업이 아직 보관 중이면 그때의 확인을 다시 줄 뿐, 상태 파일은 한 바이트도 바뀌지 않는다. 새 등록의 자격증명·차단 해제·Registry는 그대로이고, 새 등록의 삭제 작업은 그 영수증으로 진행되지 않는다 | — |
| 차단 용량이 가득 참 | 명령을 받지 않음 | `409 removal_capacity_full`, 작업 없음 | 삭제할 수 없다는 안내 |
| 방 배정 정리 실패 | — | `settings_cleanup_pending: true`, 계속 재시도 | 관리 화면에 정리 대기 표시 |

### 원격 삭제와 로컬 초기화 (BOOT 버튼 재등록, 2026-10-06)

이전 허브가 없어진 보드는 그 허브의 삭제 명령을 받을 수 없다. 그런 보드를 미등록으로 되돌리는 것은 이 문서의
원격 삭제가 아니라 [`power-base.md`의 "BOOT 버튼 로컬 재등록"](power-base.md)이다. 둘은 다른 것이고, 서로의
증명을 만들지 않는다.

| | 원격 삭제 (이 문서) | 로컬 초기화 |
|---|---|---|
| 누가 승인하나 | 허브의 인증된 명령(관리 키의 MAC) | 물리 버튼으로 연 창 + 설정 코드(Security 2) + 사용자의 명시적 확인 |
| 보드의 기록 | 등록 record의 상태(`UNENROLLING` → `RELEASED` → 최종) | 별도 journal(`pb_reenroll`/`op`: `APPROVED` → `VERIFIED`) |
| 끝난 뒤 남는 것 | 최종 record와 삭제 영수증 | 등록 record 없음, journal `VERIFIED`. **영수증 없음** |
| 허브의 삭제 작업 | 영수증을 검증하면 `completed` | 바뀌지 않는다. 허브는 아무것도 듣지 못한다 |
| 이전 허브의 목록·방 배정·차단 | 허브가 정리한다 | **그대로 남는다**(별도 작업) |

우선순위는 다음과 같이 고정했고 host test로 확인했다(`tools/test_power_base_reenroll.c`).

- 원격 삭제가 진행 중인 보드(`UNENROLLING`, 또는 `RELEASED`·삭제 대기)에서는 버튼의 창이 열리지 않고 로컬
  초기화 명령은 `BUSY`다. 부팅은 이전처럼 원격 해제를 먼저 마친다.
- 최종 `RELEASED` record(영수증)는 버튼으로도, 인증·확인된 초기화 명령으로도 지워지지 않는다. 그런 보드는
  이미 초기화된 보드이므로 명령은 `NOT_ENROLLED`이고, 영수증은 지금처럼 새 등록 시도가 수락될 때까지 남는다.
- journal로 기록된 로컬 초기화가 끝나지 않은 보드는 다른 무엇보다 그것을 먼저 끝낸다.
- 창 부팅에서는 관리 poll이 시작되지 않으므로, 창과 원격 명령이 같은 부팅에서 경쟁하지 않는다.

## 10. 등록 화면이 지키는 순서 (제품용)

기존 "앱이 지켜야 할 순서"([`power-base.md`](power-base.md))에 다음만 더한다.

1. 보드의 `proto-ver`에 `bridge_endpoint_v1`과 `mgmt_v1`이 모두 있어야 한다. 없으면 허브 세션을 만들지
   않는다.
2. 광고 이름의 EUI-64는 Security 2의 username 후보일 뿐이다. 인증된 `pb-status`의 EUI-64가 다르면 허브
   세션을 만들지 않는다.
3. `POST`에 `bridge_endpoint_version: 1`과 `management_version: 1`을 넣는다. `202`에 관리 자격증명이
   없으면 그 세션을 취소하고 끝낸다.
4. 보드가 `release_receipt_v1`을 알리면, 인증과 EUI-64 확인 뒤, 허브 세션을 만들기 전에 `0x07`로 삭제
   영수증을 읽는다. 영수증이 있으면 `POST /api/v1/power-base-removals/receipt`로 전달한다(보드가 이미 스스로
   전달해 완료된 작업이면 허브는 같은 작업으로 답한다). 허브가 그것을
   받았는지는 등록을 막지 않는다(못 받았으면 이전 삭제 작업은 새 등록이 활성화될 때 `superseded`가 된다).
   `pb-start`가 수락되면 보드의 record와 함께 영수증도 지워지므로 순서가 중요하다.
5. `pb-start`는 `0x06` 하나만 쓴다. `0x04`나 `0x02`로 바꿔 보내지 않는다.
6. 성공은 허브 세션의 `heartbeat_received`뿐이다. 보드가 등록됐는데 허브 확인을 받지 못한 경우(세션
   `expired`)는 "부분 완료"로 표시하고 보드를 초기화했다고 하지 않는다.

## 11. 앱 화면 (PogoHome)

**등록: "기기 추가" → "Power Base 추가"** (`PowerBaseSetupView`)

- 설정의 '허브 찾기'로 고르고 이 기기가 페어링한 실제 허브에서만 진행한다. 시뮬레이션 데이터, 직접 입력한
  주소, 페어링하지 않은 허브에서는 이유와 "설정 열기"만 보여 준다. 허브는 고를 때의 Bonjour 이름으로
  표시한다(이름이 없으면 `hub_id`의 끝 네 자리).
- Bluetooth는 사용자가 "주변에서 Power Base 찾기"를 누른 뒤에만 쓴다(권한 요청도 그때). 목록은 광고된
  EUI-64마다 한 줄(신호가 가장 센 것)이고, EUI-64가 없는 주변 기기는 보여 주지 않는다. Bluetooth 꺼짐·권한
  없음·찾지 못함은 각각 안내한다.
- 입력은 설정 코드 26자뿐이다. EUI-64, IPv6 주소, port, PSKd는 입력받지도 보여 주지도 않는다. 붙여넣기는
  시스템 `PasteButton`을 눌렀을 때만 읽고, 앱이 클립보드를 스스로 읽지 않는다. 코드는 화면의 메모리에만
  있고 허브가 등록을 확인하면 지운다.
- 진행 표시는 "찾는 중 / 보드 확인 중 / 연결 중 / 허브 확인 중 / 완료" 다섯 단계다. 보드가 스스로
  `enrolled`라고 답하면 "허브 확인 중"까지만 간다. 끝난 화면에는 보드의 마지막 진행 상태(`joining`,
  `attaching`)를 보여 주지 않는다.
- 결과는 네 가지로 구분한다: 등록됨(허브의 `heartbeat_received`), 끝까지 확인하지 못함(보드에 전달됐지만
  허브가 확인하지 못함. 보드를 초기화했다고 하지 않고, 초기화하라고도 하지 않는다), 등록하지 못함(설정
  코드 불일치, 보드 잠김, 다른 보드, 보드·허브 업데이트 필요, 허브 연결 실패, 시간 초과 등 이유별 문구),
  취소(보드가 취소를 확인했는지에 따라 문구가 다르다).
- 화면을 잠깐 벗어나는 것(`onDisappear`)만으로는 취소하지 않는다. 기존 controller의 수명 규칙을 그대로 쓴다.

**삭제: 기기 탭의 Power Base 제목 옆 "…" → "Power Base 관리"** (`PowerBaseManagementSheet`)

- 이름(`Power Base ··<끝 네 자리>`), 연결 상태, 연결된 센서 수를 보여 준다.
- "Power Base 삭제"는 snapshot이 그 Base를 `"management": "supported"`로 알려 줄 때만 누를 수 있다. 확인
  대화상자에 이름, 영향받는 센서 수, 영향(기기 목록·Apple 홈(Matter)에서 사라짐, 방 지정 해제), 오프라인이면
  "삭제 대기"가 된다는 것을 적는다. 마지막 문단은 허브가 그 Base에 대해 말한 것에 따른다: `erase_confirmation`이
  `automatic`이면 "삭제를 누르면 나머지는 자동으로 진행됩니다 …", 아니면 이전과 같이 Bluetooth 확인이
  필요하다는 문구다.
- **한 번 확인하면 끝이다(2026-10-08).** 사용자가 하는 일은 "Power Base 삭제" → 확인 한 번이다. 그 뒤는 허브와
  보드가 하고, 앱은 허브가 알려 주는 상태를 보여 줄 뿐이다. 보드 검색, 설정 코드(QR) 입력, "삭제 확인"은
  자동 확인이 되지 않았을 때(허브가 `manual`로 알릴 때)와 이전 허브·펌웨어에서만 나온다. 앱이 꺼져 있거나
  백그라운드에 있거나 iPhone이 멀리 있어도 삭제는 진행되고 완료된다: 영수증은 보드에서 허브로 간다.
- 허브가 관리를 모르거나(`power_base_removals` 없음) Base가 `unsupported`이면 "지원하지 않음"과 해야 할
  일(허브 업데이트, 펌웨어 업데이트 뒤 재등록)을 보여 준다. 목록에서만 숨기는 삭제는 없다.
- 작업 상태 표시: `pending` → 삭제 대기(취소 가능), `sent`·`accepted` → 삭제 중(취소 불가),
  `result_unknown` → 결과 확인 불가, `released` + `automatic` → 삭제 중(할 일 없음), `released` + 그 밖 →
  등록 해제됨 · 삭제 확인 필요, `completed` → 삭제 완료, `cancelled`, `superseded`. 끝나지 않은 상태는 기기
  목록에도 표시된다. `completed`만 "삭제 완료"라고 부른다. "삭제 중"은 2026-10-08 이전의 "삭제 진행 중"이다.
- `settings_cleanup_pending`이면 관리 화면에 "방 지정을 아직 정리하지 못했습니다. 허브가 자동으로 다시
  시도합니다"를 작업 상태와 따로 보여 준다.
- 차단 용량이 가득 차 거부된 요청(`409 removal_capacity_full`)은 "잠시 뒤 다시 시도"가 아니라, 왜 안
  되는지와 자리가 생기는 방법(삭제했던 Power Base를 다시 등록)을 알려 준다. 아무것도 삭제되지 않았다는
  것도 함께 적는다.
- 앱은 목록에서 아무것도 스스로 지우지 않는다. Base와 그 센서는 허브의 snapshot에서 사라질 때 사라진다.
  허브가 더 이상 목록에 넣지 않는 Base의 작업 가운데 **끝나지 않은 것**은 기기 목록의 "Power Base 삭제 요청"
  구역에 남는다. `completed`가 되면 그 구역에서 빠져 "최근 삭제한 Power Base" 구역에 기록으로 남는다(허브가
  알려 주는 동안. 허브는 끝난 작업을 10분 동안 알려 준다). 허브의 차단 목록과 완료된 작업의 영수증·확인 MAC은
  이 표시와 무관하게 7절대로 보존된다.
- 요청의 답을 받지 못하면 "도착했는지 알 수 없음"으로 표시한다. 같은 요청을 다시 보내도 같은 작업이다.

**삭제 확인: 관리 화면의 `released` 작업, 또는 결과를 확인하지 못한 작업 → "삭제 확인"**
(`PowerBaseReleaseConfirmationView`)

- 진입 조건: 작업이 `released`이고 허브가 자동 확인을 예상하지 않는 경우(`erase_confirmation`이 `automatic`이
  아니다: `manual`이거나, 필드가 없는 이전 허브다), 또는 `sent`·`accepted`인 채로
  `result_unknown`이 된 경우다(`PowerBaseRemoval.offersEraseConfirmation`). `released` + `automatic`인 동안에는
  제공하지 않는다(할 일이 없다). 허브가 4분 뒤 `manual`로 바꾸면 제공된다. 뒤의 경우는 `RELEASED` 통지가
  유실됐을 수 있고 보드가 영수증을 가지고 있을 수 있기 때문이다. `pending`, 아직 답을 기다리는 `sent`·
  `accepted`, 끝난 작업(`completed`·`cancelled`·`superseded`)에서는 제공하지 않는다. 버튼을 제공하는 것이
  작업의 표시(결과 확인 불가 / 등록 해제됨)를 바꾸지는 않고, 화면의 설명만 두 경우가 다르다.
  이전 문서의 "`released` 작업에서만"은 그 수정 전의 설명이다.
- `released`인 Base는 허브의 목록에서 빠지므로, 그 작업은 기기 목록의 "Power Base 삭제 요청" 구역에 남고
  거기서 관리 화면을 연다. 영수증이 전달될 때까지 시간이 지나도 사라지지 않는다.
- 백그라운드에서는 새 BLE 명령이나 허브 요청을 시작하지 않는다(연결, opening의 각 message, 상태, 영수증
  요청, 허브 전달, "허브에 다시 보내기" 직전에 각각 확인한다). 화면을 벗어나거나 허브가 바뀌거나 새 실행이
  시작된 뒤에는 예약돼 있던 명령을 보내지 않는다.
- 순서: "주변에서 이 Power Base 찾기"(이때 처음 Bluetooth 사용) → 광고된 EUI-64가 삭제한 Base와 같은
  보드만 표시 → 설정 코드 → 보드 인증(Security 2) → 세션 안에서 보드가 말한 EUI-64가 같은지 확인 →
  영수증 읽기 → 허브에 전달 → 허브가 `completed`로 답했을 때만 "삭제 완료".
- 앱은 영수증의 형식만 보고 그대로 전달한다. 진위는 허브가 판단한다. 영수증은 메모리에만 둔다.
- 읽기는 보드의 아무것도 바꾸지 않으므로 화면을 벗어날 때 정리할 것이 없다. 영수증을 읽었는데 허브에
  전달하지 못했으면 "허브에 다시 보내기"로 Bluetooth 없이 다시 보낸다.
- 실패는 이유별로 구분한다: 설정 코드 불일치, 다른 보드, 펌웨어가 영수증을 지원하지 않음, 영수증
  없음(보드가 아직 다 지우지 못했거나 그사이 다른 등록을 시작함), 허브에 전달 실패, 허브가 받아들이지 않음.
- 보드는 전원을 켠 뒤 15분 동안만 BLE로 응답한다. 찾지 못하면 전원을 껐다 켜라고 안내한다.
- 같은 Base를 다시 등록하는 경우에는 등록 화면이 이 확인을 대신 한다(10절 4번).

**재등록: "기기 추가" → "다른 허브에 연결하기"** (`PowerBaseReenrollView`, 2026-10-06)

- 앞부분(보드 인증과 로컬 초기화)은 `PowerBaseLocalResetController`가 하고, **허브에 아무것도 요청하지
  않는다.** 이전 허브가 꺼져 있거나 페어링이 없어도 진행된다. 뒷부분(새 허브 등록)은 기존 "Power Base
  추가" 화면 그대로이고, 새 허브 선택·페어링 조건과 `heartbeat_received` 판정도 그대로다.
- 화면 맨 위에 "새로 연결할 허브"를 먼저 보여 준다. 준비되지 않았으면(시뮬레이션, 직접 입력 주소, 페어링
  없음) 이유와 "설정 열기", 그리고 "허브 없이도 지울 수는 있지만 그동안 어느 허브에도 연결되지 않는다"는
  안내를 보여 준다. 초기화를 막지는 않는다. 초기화가 확인된 뒤의 "새 허브에 등록" 버튼만 허브가 준비돼야
  눌린다.
- 화면이 구분해 보여 주는 것: BOOT 버튼 조작 안내 → 재등록 창의 보드 발견(스캔 목록) → 설정 코드 인증
  ("설정 코드 확인 중") → 기존 등록 정보 삭제의 명시적 확인("지우기 전 확인", 설명 + 확인 대화상자) →
  초기화 진행("기존 등록 정보 지우는 중") → 결과 확인 불가("지우기 결과를 확인하지 못했습니다") → 초기화
  검증 완료와 새 허브 등록 준비("기존 등록 정보를 지웠습니다" / "새 허브에 등록") → (다음 화면) 새 허브의
  등록 확인.
- 확인 문구는 기존 연결이 끊기고 되돌릴 수 없다는 것, **이전 허브의 기기 목록·방 배정 같은 기록은 남을
  수 있다**는 것, 센서 모듈과 설정 코드는 바뀌지 않는다는 것을 적는다. 키·주소·PSKd·EUI-64는 입력받지도
  보여 주지도 않는다.
- 인증만으로는 아무것도 보내지 않는다(challenge도 요청하지 않는다). 명령은 확인 화면에서만 보낼 수 있고,
  확인 값 없이는 codec이 명령을 만들 수 없다. `local_reset_v1`이 없는 보드에는 다른 message로 대신하지 않고
  "펌웨어 업데이트 필요"로 끝낸다.
- 작업 ID는 한 번 만들고, 답 유실·연결 끊김·화면 이탈 뒤에도 유지한다. 그 뒤의 연결은 **같은 작업의 상태를
  조회**할 뿐이고 초기화 명령을 다시 보내지 않는다. 이미 보낸 명령은 화면 이탈·허브 변경 뒤에도 "취소"가
  아니라 "결과 확인 불가"로 표시한다. 보드가 그 작업을 모른다고 답하면 "시작하지 않음"으로 끝내고, 새
  시도는 사용자가 다시 확인해야 시작된다(새 작업 ID).
- 연결 직전, opening의 각 message, 상태, 초기화 상태 조회, challenge, 초기화 명령, 그 뒤의 조회마다
  foreground와 실행 유효성을 확인한다. 같은 보드에 다시 연결할 때는 기존 등록 화면과 같은 BLE 수명
  관리(`PowerBaseEnrollmentBLELifecycle`: 이전 연결의 종료를 확인한 뒤 연결)를 쓴다.
- 설정 코드는 화면의 메모리에만 있고, "새 허브에 등록"으로 넘어갈 때 등록 화면의 입력란에 그대로 넘긴다
  (저장·로그·클립보드 없음).

**Release와 DEBUG의 구분**

| 구성 | 들어 있는 것 |
|---|---|
| Release·Debug 공통 | BLE 통신 패키지(`PowerBaseBLEKit`, `#if DEBUG` 없음), 등록 controller·transport·BLE 수명 관리, 제품용 등록 화면, 관리 화면, 삭제 controller·transport, 재등록 화면과 로컬 초기화 controller(`PowerBaseReenrollView`, `PowerBaseLocalResetController`), 제품 문구(`Localizable.xcstrings`의 ko/en/es), Bluetooth 권한 문구(iPhone·iPad·Mac Catalyst) |
| Debug에만 | "BLE 등록 시험" 화면, USB 콘솔 시험(controller·구역), PSKd 표시·복사용 클립보드, 시험 화면 문구(`PowerBaseEnrollmentDebug.xcstrings`), 세션·정리 결과의 원문 진단 |

Release 빌드 점검은 `pogo-sensor-ios/PogoHome/Scripts/check_release_enrollment_surface.sh <.app> release`로
한다. 예전 기준("Release 바이너리에 `PowerBaseEnrollment` 문자열 0건")은 공용 등록 코드가 제품 코드가
되면서 더 이상 맞지 않는다. 지금의 기준은 "시험 화면·USB 콘솔 시험·PSKd 표시/복사 경로·DEBUG 문구 표가
없고, 제품 등록·삭제 코드와 Bluetooth 권한 문구는 있다"이다. 같은 스크립트의 `debug` 모드는 Debug
빌드에서 시험 전용 항목을 **찾을 수 있음**을 확인한다(없다는 것만 말할 수 있는 점검은 근거가 아니다).

## 12. 같은 허브의 주소·포트 변경 복구 (LOCATE, 2026-10-09)

등록할 때 보드는 허브의 수신 목적지(OTBR의 Mesh-Local EID와 ingest port)를 한 번 받아 record에 저장한다. 그
목적지는 세 곳이 쓴다: record(부팅 때 복원), 관리 poll, exporter(heartbeat·topology·sample). 2026-10-09 이전에는
이 값을 고치는 것이 없었다. OTBR의 Mesh-Local EID가 바뀌거나(예: RCP 동글을 바꾸고 같은 Dataset을 다시 넣은
경우. OpenThread의 설정 파일이 새로 만들어져 EID의 뒤 64비트가 새 난수가 된다) bridge를 다른 `--port`로 다시
시작하면, 보드는 같은 Thread 네트워크에 붙어 있으면서도 허브와 끊겼고 복구 방법은 재등록뿐이었다.

지금은 관리 자격증명을 가진 보드(record v3, `ENROLLED`)가 **같은 허브**를 스스로 다시 찾는다. 앱이 꺼져 있어도
되고, 앱은 이 과정에 없다.

**이것이 아닌 것.** 앱이 허브의 Wi-Fi/LAN 주소를 Bonjour로 다시 찾는 기능은 앱과 허브 사이의 일이고 그대로다.
그 광고는 LAN의 mDNS이며 Thread의 보드는 그것을 조회하지 않는다. 이 절은 보드 → 허브의 Thread 쪽 목적지만
다룬다. 센서 모듈이 값을 보내는 곳(collector)은 Power Base 자신의 주소이므로 허브의 주소와 무관하다.

### 범위

| 경우 | 동작 |
|---|---|
| 같은 hub_id·등록 세대·관리 키, 같은 Thread 네트워크에서 허브의 EID 또는 port(또는 둘 다)가 바뀜 | 보드가 새 목적지를 찾아, 허브가 그 목적지에서 직접 확인해 준 뒤 저장하고 옮긴다 |
| 허브 프로세스 재시작·일시 중단, OTBR 재시작(주소는 그대로) | 찾기는 돌지만 바뀐 것이 없으므로 아무것도 쓰지 않는다. 허브가 다시 답하면 그대로 이어진다. 센서·방·Matter endpoint·페어링은 건드리지 않는다 |
| 보드 재부팅 | 저장된(옮긴 뒤라면 새) 목적지로 부팅한다. 재부팅 뒤에 허브가 옮겨 가 있으면 같은 방식으로 찾는다 |
| OTBR 장치(RCP·호스트)를 바꿨고 **Dataset과 허브의 `<state-dir>`(hub_id, `power_base_mgmt.json`)를 보존**함 | 위 첫 줄과 같다: 같은 네트워크, 같은 허브 신원과 키 |
| 다른 hub_id의 허브 | 답을 받아들이지 않는다. 기존 재등록 절차 |
| 허브가 관리 키나 세대를 잃음(`power_base_mgmt.json` 소실, 새 등록 세대로 대체) | 그 허브는 이 보드의 `LOCATE`에 `4.01`로 답한다. 보드는 아무것도 바꾸지 않는다. 기존 재등록 절차 |
| 새 Dataset/Network Key의 네트워크(네트워크를 새로 만든 OTBR) | 보드는 그 네트워크에 붙지도 못한다. 자동 이전 대상이 아니다. 기존 재등록 절차 |
| record v1·v2(관리 키 없음), `UNENROLLING`·`RELEASED`·미등록·로컬 초기화 중 | 찾지 않는다 |

### 발견 → 인증 → 저장 → 사용

구현은 `components/power_base_mgmt/pb_mgmt_client.c`("locate")이고, 관리 task가 poll과 poll 사이에 실행한다
(별도 task가 없다. callback에서 실행하지 않는다).

1. **언제 찾는가.** poll이 **인증된 답 없이** 6번 연속 끝났을 때(무응답, 거부, 인증되지 않는 답을 가리지 않는다).
   poll은 붙어 있는 동안 10초마다이고 답 없는 poll 하나는 최대 5초를 기다리므로 약 1분 30초다. 인증된 답이
   하나라도 오면 처음부터 다시 센다.
2. **힌트(발견).** 보드가 **자기 Thread Network Data와 router table에서** 읽는다. 아무것도 보내지 않는다.
   on-mesh prefix·external route를 낸 노드(border router), service를 낸 노드, leader, router들의 RLOC16을
   모아 각자의 routing locator 주소(`<Mesh-Local Prefix>:0000:00ff:fe00:<RLOC16>`)로 만든다. 자기 자신은 뺀다.
   **가진 것을 전부** 준다(가능성이 높은 것부터: Network Data의 순서, leader, router id 순. 네트워크가 그대로면
   부를 때마다 같은 순서다). 상한은 128개이고 Thread 네트워크는 그보다 많이 낼 수 없다(router는 32개까지,
   Network Data 254바이트가 이름을 댈 수 있는 노드는 82개까지). 힌트는 "물어볼 곳"일 뿐이고 인증되지 않았다:
   그 주소 뒤에는 네트워크의 어느 노드든 있을 수 있다. round마다 새로 읽고 보관하지 않는다.
3. **후보.** ① 저장된 주소의 locate 포트(5684): 매 round 맨 먼저. ② 각 힌트 주소의 저장된 포트와 locate
   포트(저장된 포트가 5684면 하나): 힌트의 순서대로 한 줄로 세운 목록. 중복은 한 번만, record의 Mesh-Local
   Prefix 밖의 힌트는 후보가 아니다. 한 round가 다 묻지 못하는 목록은 round를 이어 가며 묻는다(아래 "round
   사이의 진행").
4. **질문.** 후보마다 `LOCATE`(새 nonce, 그 등록의 MAC)를 Confirmable POST로 `pogo/v1/mgmt`에 한 번 보낸다.
5. **주장(인증).** 답이 `LOCATE_ACK`이고, 그 등록의 키로 MAC이 맞고, 이 보드·이 hub_id·이 세대이며, **방금 보낸
   그 질문의 nonce**를 담고 있을 때만 허브의 말로 받는다. 그 안의 주소와 port가 허브가 말하는 자기 목적지다.
   Network Key로 네트워크에 들어와 있다는 것, 힌트에 있었다는 것, hub_id가 같다는 것만으로는 아무것도 아니다.
6. **쓸 수 있는 목적지인가.** 주장된 목적지는 등록 때와 같은 검사를 통과해야 한다: record의 Mesh-Local Prefix
   안의 Mesh-Local EID(locator·link-local·multicast·global·다른 prefix는 아님), port ≠ 0.
7. **확인.** 주장은 **그 목적지에서 온 것일 때만** 쓴다. 답을 받은 곳과 주장된 목적지가 같으면 확인된 것이고,
   다르면(보통의 경우다: 힌트는 locator이고 목적지는 EID다) 주장된 목적지로 `LOCATE`를 한 번 더 보내 **같은
   목적지를 말하는 인증된 답**을 받아야 한다. 닿지 않거나 그 사이 다른 곳을 말하면 그 round에서는 아무것도
   바꾸지 않는다. 보드가 스스로 닿아 보지 못한 목적지는 저장되지 않는다.
8. **저장.** 확인된 목적지가 저장된 것과 같으면 아무것도 쓰지 않는다. 다르면: flash의 record를 **다시 읽어**
   그것이 여전히 이 등록(`ENROLLED`, 같은 네트워크·hub_id·세대·키·목적지)인지 보고, 그 record에서 목적지만
   바꿔 **blob 하나로** 쓰고(`set_blob` + `commit` 한 번), 다시 읽어 대조한다. 쓰기가 오류 없이 끝났고 새
   record가 그대로 읽힐 때만 "옮겼다"이다.
9. **사용.** 그 뒤에야 관리 poll이 새 목적지를 쓰고, exporter가 새 목적지로 옮겨 heartbeat → 전체 topology →
   모듈별 최신 sample → 전달되지 않은 삭제 표식을 주기를 기다리지 않고 다시 보낸다. poll은 바로 다시 한다.

**round 사이의 진행 (2026-10-09 후속 수정).** 처음 구현은 매 round 같은 목록을 처음부터 물었고 힌트도 앞의
8개만 받았다. 후보가 한 round의 요청 수(12)보다 많으면 뒤쪽 후보는 몇 round를 돌아도 묻지 않았다(저장된 포트가
5684가 아닐 때는 6번째 힌트의 둘째 포트부터, 5684일 때는 수집 상한 때문에 9번째 힌트부터). 지금은:

- 힌트는 전부 받는다(위 2). round의 요청 수는 그대로 12다. 한도를 늘려 해결하지 않았다.
- round는 ① 지난 round가 넘긴 목적지(아래) ② 저장된 주소의 locate 포트 ③ 후보 목록을 **지난 round가 마지막으로
  물은 후보의 다음부터** 이어서 묻는다. 끝에 닿으면 처음으로 돌아가고, 한 round에 한 바퀴를 넘지 않는다. 그래서
  한 round에 들어가는 목록은 매 round 처음부터 끝까지 순서대로 묻고(가능성이 높은 것 먼저), 긴 목록은 연이은
  조각으로 덮는다.
- "마지막으로 물은 후보"는 **주소와 port로** 기억한다(자리 번호가 아니다). 그 앞에 후보가 늘거나 줄어도 walk는
  그 후보의 다음에서 이어진다. 그 후보가 사라졌으면(그 router가 네트워크를 떠남) 그것이 있던 자리에서 잇는다:
  뒤에 있던 것이 그 자리로 당겨져 있다. 순서가 바뀐 후보는 walk가 한 바퀴 도는 사이에 만난다.
- **도달 상한.** 후보가 C개이고 목록이 그대로이면, 어느 연이은 ⌈C/10⌉ round 안에 모든 후보를 한 번 이상 묻는다
  (round당 새 후보 10개 이상: 12에서 ②와 ①을 뺀 것. 두 번째 요청이 필요한 답이 없을 때). C는 힌트 수 × 포트 수
  (저장된 포트가 5684가 아니면 2)다.

  | 힌트(다른 router·border router) | 저장 포트 ≠ 5684일 때 후보 | 모두 묻는 데 드는 round | 무응답 시작부터의 시간(10~15초 poll) |
  |---|---|---|---|
  | 5개 이하 | 10개 이하 | 1 (매 round 전부) | 1~1.5분 |
  | 10개 | 20 | 2 | 2~3분 |
  | 20개 | 40 | 4 | 8~12분 |
  | 32개 (router가 가득 찬 네트워크) | 64 | 7 | 46~69분 |
  | 128개 (상한) | 256 | 26 | 5.5~8.3시간 |

  round의 시각은 무응답 poll 6·12·24·48·96번째, 그 뒤 90번마다이다(아래). 가정의 네트워크(router 몇 개)는 첫
  줄이다. 목록이 round마다 계속 바뀌는 동안에는 이 상한을 말할 수 없다: 그때 보장하는 것은 walk가 같은 앞부분으로
  되돌아가지 않는다는 것이다.
- **요청이 남지 않았을 때의 답.** round의 마지막 요청에 허브가 (locator에서) 답하면, 그 답이 말한 목적지를 직접
  물어 볼 요청이 없다. 그 목적지(주소와 port만. 답 자체나 그 MAC이 아니다)를 기억해 **다음 round의 첫 요청**으로
  묻는다: 새 `LOCATE`, 새 nonce, 다른 후보와 똑같은 규칙(그 목적지에서 온, 그 목적지를 말하는 인증된 답일 때만
  확인). 기억한 것은 그 한 round에만 쓰고 버린다. 지난 round의 답은 아무것도 인증해 주지 않는다. 확인까지 했는데
  저장에 실패한 목적지도 같은 방식으로 다음 round의 첫 요청이 된다(walk는 이미 그 후보를 지나갔다).
- round 사이에 남는 것은 주소 두 개뿐이고(RAM), 신뢰되는 것은 없다. 관리 poll에 인증된 답이 오거나, 옮겼거나,
  record를 다시 읽으면(재부팅 포함) 잊고 다음 탐색은 목록의 처음부터다.

**한 round의 한도와 간격.** round 하나는 `LOCATE`를 최대 12개 보낸다(각각 최대 5초). 허브를 찾지 못하면 다음
round는 6·12·24·48·90번의 poll 뒤다(poll 하나가 10초, 답이 없어 기다리면 최대 15초이므로 약 1~1.5분·2~3분·4~6분·
8~12분, 그 뒤로는 15~22분마다). 횟수의 끝은 없다: 하루 꺼져 있던
허브도 다시 찾아야 하기 때문이다. 대신 한도가 정해져 있다(15분에 unicast 요청 12개 이하). multicast나
broadcast는 쓰지 않고, 찾는 동안 flash에 쓰는 것은 없다. 관리 task만 기다리고 Thread task·센서 수집·POGO는
영향받지 않는다.

**복구 시간의 기준.** 허브가 새 목적지에서 이미 답하는 상태라면: 연락이 끊긴 뒤 약 1분 30초에 첫 round가 시작되고,
허브의 locator가 앞쪽 힌트이면 그 round에서 몇 초 안에 옮긴다. 답 없는 후보가 앞에 많으면 그 round가 최대
1분쯤 걸린다. 허브의 locator가 첫 round의 요청 수 밖에 있으면(router가 많은 네트워크) 위 표의 round에서다.
허브가 그때 꺼져 있었다면 다음 round(위 간격)에서다.

### 힌트의 출처: Thread에서 닿는 길

| 검토한 길 | 판단 |
|---|---|
| LAN의 mDNS/Bonjour 광고 | Thread의 보드가 조회하지 않는다. OTBR의 discovery proxy가 있더라도 돌려주는 것은 허브의 LAN 주소이고, record가 받는 것은 Mesh-Local EID뿐이다. 쓰지 않는다 |
| 이전 주소에 새 주소를 묻기 | 이전 주소가 죽었을 때가 문제이므로 이것만으로는 안 된다. port만 바뀐 경우를 위해 "저장된 주소의 locate 포트"는 후보에 넣었다 |
| Thread Network Data에 허브 전용 service를 올리기(`ot-ctl service add`) | OTBR의 제어 socket 권한(helper에 새 명령)과 OTBR 재시작마다 다시 올리는 감시가 필요하다. 아래 길로 충분해 넣지 않았다 |
| realm-local multicast로 묻기 | 네트워크 전체에 뿌려진다. 아래 길로 충분해 넣지 않았다 |
| **보드 자신의 Network Data와 router table** | **쓴다.** Linux OTBR은 border routing을 하면 Network Data에 prefix·route를 자기 RLOC16으로 내고, SRP 서버를 켜면 service도 낸다. 아무것도 내지 않더라도 네트워크를 만든 OTBR은 보통 leader이거나 router다. 보드는 이미 갖고 있는 정보를 읽을 뿐이라 보내는 것이 없다 |

이 길이 닿지 못하는 경우: OTBR이 Network Data에 아무것도 내지 않으면서(border routing·SRP 모두 꺼짐) router도
아닌 경우(child로 붙음). 그때는 port만 바뀐 것은 찾고(저장된 주소의 locate 포트), 주소가 바뀐 것은 찾지 못한다.

**실제 OpenThread로 실행한 것.** `tools/test_power_base_locate_openthread.sh`가 ESP-IDF v6.1이 펌웨어에 넣는
OpenThread core(commit `b678a4f6…`)를 Mac에서 빌드해(Nexus: 한 프로세스 안의 instance 네 개, 모의 radio, 가상
시간) 보드의 실제 관리 client·record 코드·**펌웨어의 힌트 수집 파일**(`power_base/main/enrollment_locate_hints.c`,
펌웨어가 link하는 바로 그 파일)을 그 위에서 돌린다. 허브는 그 네트워크의 한 노드에 올린 CoAP resource다.
거기서 본 것: ① 보드의 실제 Network Data와 router table에서 읽은 힌트에 OTBR의 locator가 들어 있다(OTBR이
on-mesh prefix를 낸 경우에는 맨 앞, 아무것도 내지 않은 경우에도 router로서). ② OTBR의 locator로 보낸 `LOCATE`가
그 노드에 locator 주소 앞으로 도착하고, locator에서 나간 답이 보드의 CoAP client에 그 요청의 답으로 돌아온다.
③ OTBR 장치를 같은 Dataset의 새 장치로 바꾸면(다른 extended address·EID·RLOC16, leader도 바뀜) 보드가 여섯
번째 poll 뒤의 round에서 새 EID를 찾아 저장하고 옮긴다(요청 3~4개). ④ 같은 네트워크의 다른 router에 있는,
다른 키의 허브는 이 보드의 `LOCATE`를 거부한다. 이 재현은 Linux가 아니다: 허브 쪽은 OpenThread의 CoAP 서버이고
`wpan0`와 bridge의 socket이 아니다.

**소스에서 확인한 것**(같은 OpenThread): 보드 쪽 API
`otNetDataGetNextOnMeshPrefix`·`otNetDataGetNextRoute`·`otNetDataGetNextService`·`otThreadGetLeaderRloc`·
`otThreadGetRouterInfo`·`otThreadGetMaxRouterId`·`otThreadGetMeshLocalPrefix`가 있고 이 보드의 FTD 빌드에
link된다(펌웨어 빌드로 확인). OTBR 쪽은 같은 OpenThread의 posix platform(`src/posix/platform/netif.cpp`)이
OpenThread의 모든 주소 변화를 Linux 인터페이스에 그대로 반영한다(`otIp6SetAddressCallback` →
`processAddressChange`). 즉 `wpan0`에는 EID와 함께 locator 주소도 올라가고, locator로 온 datagram은 호스트의
socket에 닿을 것이다. **이것은 소스를 읽은 것이고, 운영 중인 허브의 OTBR에서 확인한 것은 아니다**(이 작업은 운영 Linux를 건드리지 않는다).
실물 시험 준비물의 사전 확인에 `ip -6 addr show dev wpan0`와 `ot-ctl netdata show`를 넣었다.

### locate 포트

보드가 아는 port는 등록 때 받은 것 하나다. bridge가 다른 port로 옮겨 가면 그 port에는 아무도 없으므로, 묻는
곳이 따로 정해져 있어야 한다: **5684**(`PB_MGMT_LOCATE_PORT`, ingest의 기본 port).

- bridge가 `--port 5684`(기본)로 돌면 ingest socket 하나가 `LOCATE`를 포함한 모든 것을 받는다. 새로 여는 것이
  없다.
- 다른 port로 돌면 5684에 socket을 하나 더 연다. 같은 인터페이스 제한(datagram마다 `wpan0`인지 확인)을 받고,
  **`pogo/v1/mgmt`로 온 Confirmable POST의 `LOCATE`에만** 답한다. 그 밖의 것(POLL, heartbeat, sample, 다른
  method, Non-confirmable)에는 아무 답도 하지 않고 Registry에 닿지 않는다.
- 5684를 열 수 없으면(다른 프로그램이 씀) 경고를 남기고 그 socket 없이 돈다. 그 허브로 옮겨 간 port는 보드가
  찾지 못한다.

### 허브의 답

`PowerBaseManagement::HandleLocateLocked`.

- **누구에게 답하는가.** 그 보드·세대의 자격증명이 `provisional`이거나 `active`이고 MAC이 맞을 때. `released`는
  키가 남아 있어도 거부한다(영수증 확인용으로만 둔 키다). 대체된 세대, 모르는 보드, 다른 hub_id는 다른
  메시지와 같이 `4.01`이고 어느 것인지 알려 주지 않는다.
- **무엇을 답하는가.** 지금의 ingest 목적지: `wpan0`의 Mesh-Local EID와 실제로 연 ingest port. EID는 답할 때마다
  인터페이스의 주소 목록에서 읽는다(`getifaddrs`, 권한 불필요, helper를 부르지 않는다). 규칙: `fd00::/8` 안의
  /64 가운데 locator 꼴(`…:0000:00ff:fe00:xxxx`) 주소가 있는 것이 Mesh-Local Prefix이고, 그 /64에서 locator가
  아닌 주소 하나가 EID다. 그런 /64가 없거나 둘이거나, 후보가 없거나 둘이면 추정하지 않고 `5.03`으로 답한다.
  요청이 도착한 주소를 되돌려 주는 것이 아니다(그 주소는 locator일 수 있다).
- **무엇도 바꾸지 않는다.** 상태 파일, nonce, 자격증명의 상태(`provisional`이 `active`가 되지 않는다)를 건드리지
  않는다. 같은 `LOCATE`를 다시 받으면 같은 답을 한다. 답이 알려 주는 것은 허브의 Thread 주소뿐이고, 받는 쪽은
  이미 그 허브의 키를 가진 보드다.
- 허브의 로그에 요청마다 한 줄이 남는다(어느 보드가 물었는지, port).

### 저장, 전원 차단, 경쟁

- **세 곳이 같은 값을 쓴다.** 순서가 그것을 지킨다: record(쓰고 다시 읽음) → 관리 client의 사본 → exporter.
  쓰기가 실패하면 뒤의 둘은 바뀌지 않는다.
- **쓰기 실패.** 쓰기가 오류를 돌려줬거나 다시 읽은 값이 새 record가 아니면 "옮기지 않음"이다. 이전 record가
  그대로 읽히면 그대로 두고, 그렇지 않으면(오류가 났는데 무엇인가 쓰였거나 읽히지 않음) 이전 record를 다시
  써서 이 부팅이 쓰는 것과 flash를 맞춘다. 다음 round가 다시 시도한다.
- **전원 차단.** record는 key 하나의 blob이고 쓰기는 `set_blob` + `commit` 한 번이므로, 어느 순간에 끊겨도
  다음 부팅이 읽는 것은 이전 record 전체이거나 새 record 전체다(host test는 commit 직전과 exporter 전환 직후를
  끊는다). 둘 다 같은 키·세대·상태의 `ENROLLED`이고 CRC가 맞다. 이전 record로 부팅했으면 다시 찾는다.
  손상된 record는 지금까지와 같이 안전 대기이며 성공으로 치지 않는다.
- **이전 목적지의 늦은 답.** 관리 통신은 요청 하나에 token 하나이고 지난 token의 답은 버려진다. 그것을 지나도
  nonce가 다르다. exporter는 목적지 세대 번호를 요청마다 기억해, 전송 중에 목적지가 바뀐 요청의 답은 무엇이든
  "전달됨"으로 치지 않고 새 목적지로 다시 보낸다(삭제 표식은 **새** 목적지가 확인할 때까지 남는다).
- **삭제가 우선한다.** 찾기와 삭제 명령 처리는 같은 task의 같은 poll 안에서 갈린다: 인증된 답이 온 poll은
  명령을 처리하고, 오지 않은 poll만 찾기로 간다. 저장 직전에 flash의 record를 다시 읽으므로, 그 사이
  record가 `UNENROLLING`·`RELEASED`(삭제 대기·최종)·없음(로컬 초기화)·다른 세대(재등록)로 바뀌어 있으면 **한
  바이트도 쓰지 않는다.** 확인된 답이 삭제된 record를 `ENROLLED`로 되돌리는 일은 없다. platform도 저장 직전에
  한 번 더 묻는다(원격 삭제 진행 중, 등록·BLE 진행 중이면 쓰지 않음). `RELEASED` 뒤의 보드는 poll하지 않으므로
  찾지도 않고, 영수증 전달 세션(4절 G)은 record의 목적지를 그대로 쓴다. 키·등록 상태·영수증은 이 기능이 쓰는
  record에서 바뀌지 않는다(목적지 18바이트와 CRC만 다르다).
- **옮긴 뒤의 삭제.** 허브에 삭제 작업이 기다리고 있었다면 옮긴 직후의 poll이 명령을 받고, 통지와 영수증은 새
  목적지로 간다.

### 호환성

- 관리 규약 번호 1, BLE 규약, HTTP API v1, record 버전(1·2·3)과 그 형식, 허브의 상태 파일 schema는 **바뀌지
  않았다.** 늘어난 것: 관리 메시지 type 두 개(`0x06`, `0x86`), capability bit 두 개, 허브의 locate 포트.
  기존 메시지의 바이트와 뜻은 그대로이고 golden MAC도 그대로다.
- record는 같은 v3이고 목적지 필드의 값만 바뀐다. 0.2.0 펌웨어로 되돌려도 그대로 읽는다.
- 2절의 조합 표 참조. 기존 자동 삭제, 수동 영수증(BLE), BOOT 버튼 로컬 재등록, QR 설정 코드 경로는 그대로다.

### 확인한 것과 하지 않은 것

**확인한 것 (Mac의 host test와 빌드. [`validation-status.md`](validation-status.md) "주소 복구와 센서 체인 재등록
복구 (2026-10-09)")**

- 보드 쪽 실제 코드(`pb_mgmt_proto.c`, `pb_mgmt_client.c`, `power_base_enrollment.c`)를 NVS simulator와 대본
  허브에 연결한 `tools/test_power_base_locate.c`: 정상 허브에서는 찾지 않음, 짧은 중단, 긴 중단의 round 간격과
  한도, 주소 변경, port 변경(세 방향), 둘 다, 틀린 답 여덟 가지, 재생, 한 요청 늦은 답, 쓸 수 없는 주장 다섯
  가지, 닿지 않는 주장, 묻는 중 변경, 후보 다수와 한도, 쓰기 실패 세 가지와 read-back 실패, 전원 차단 두 지점,
  record가 그 사이 바뀐 여섯 경우, 옮긴 뒤의 삭제, v1·v2와 locate 없는 platform. 2026-10-09 후속 수정으로
  더한 것: 한 round보다 많은 후보(마지막 힌트의 허브, 힌트 20개와 128개), round마다의 요청 수와 간격 유지,
  목록이 round 사이에 늘고·줄고·순서가 바뀌는 경우, round의 마지막 요청에 온 답과 다음 round의 직접 확인(새
  nonce), 그 넘긴 목적지에 허브가 아닌 것이 답하는 여섯 경우, 뒤쪽 round의 저장 실패, 재부팅·응답 재개·삭제
  진행 중에 넘긴 것이 쓰이지 않는 것.
- 실제 허브 코드(`PowerBaseManagement`)와 실제 보드 코드를 맞물린
  `linux/matter_bridge/app/api/tests/test_power_base_management.cpp`의 `EndpointLocate`.
- 실제 UDP socket의 locate 포트(`linux/matter_bridge/ingest/tests/test_loopback.cpp`), EID 고르기
  (`test_interface_address.cpp`), exporter의 목적지 전환(`tools/test_linux_bridge_exporter_destination.c`).
- 실제 OpenThread core 위의 재현(`tools/test_power_base_locate_openthread.sh`. 위 "힌트의 출처"): 힌트 수집, locator로
  보낸 요청의 왕복, OTBR 교체 뒤의 이전. 후속 수정으로 더한 것: router 13개의 네트워크에서 펌웨어의 수집이
  13개를 모두 내고, 그 목록의 **마지막** router에 있는 허브를 보드가 둘째 round에 찾는 것.

**하지 않은 것**

- **실물 전부.** 실제 보드, 실제 OTBR·`wpan0`, 실제 Thread, 운영 허브, Linux용 `pogo_matter_bridge`(GN/CHIP)
  빌드. Mac에는 Thread 인터페이스가 없으므로 `getifaddrs`로 실제 `wpan0`를 읽는 것, locator 주소로 온 datagram이
  bridge의 socket에 닿는 것, 그 답이 보드에 돌아가는 것은 실행하지 못했다.
- 보드의 힌트 수집(`power_base/main/enrollment_locate_hints.c`)은 실제 OpenThread core 위에서 실행했지만 ESP-IDF의
  port·radio·실제 보드에서는 아니다. API lock을 잡는 `enrollment_mgmt.c`의 호출부와 exporter 전환 호출은 컴파일과
  link만 확인했다.
- 관리 task의 stack 여유(6144바이트로 늘렸다)는 보드에서 재지 않았다.


## 13. 허브 목록에서 제거 (2026-10-10)

삭제(7절)는 보드가 응답해야 끝난다. 보드가 돌아오지 않으면 — 고장, 분실, 버튼으로 초기화해 다른 허브에 등록 —
그 Power Base는 허브의 목록에 오프라인으로 남고, 삭제 요청은 `pending`으로 기다릴 뿐이다. 이 절의 기능은 그런
Power Base를 **이 허브의 목록에서** 지우는 것이다.

**삭제가 아니다.** 허브가 자기 기록을 지우는 것이고, 보드에 대해서는 아무것도 말하지 않는다.

| | 삭제 (7절) | 허브 목록에서 제거 (이 절) |
|---|---|---|
| 대상 | 관리 자격증명이 있는 Base | 허브가 **오프라인**으로 보는 Base (자격증명 유무와 무관) |
| 보드에 저장된 연결 정보 | 보드가 지우고, 허브가 영수증을 검증한다 | **지워지지 않는다. 아무도 지워졌다고 말하지 않는다** |
| 끝나는 조건 | `completed` = 인증된 영수증 | 요청을 처리한 그 순간. 작업(job)이 없다 |
| API의 상태 | `pending` … `completed` | `removed_from_hub` 하나 |
| 되돌리기 | `pending`일 때 취소 | 없다. 앱으로 다시 등록하면 돌아온다 |

응답이 없다는 이유로, 오래 오프라인이라는 이유로, 또는 이 기능을 썼다는 이유로 삭제 작업이 `completed`가 되는
일은 없다(아래 "삭제 작업과 만날 때").

### 허브가 하는 일

`PowerBaseManagement::ForgetPowerBase()` (`linux/matter_bridge/app/api/src/power_base_management.cpp`). 한 번의
요청이 아래를 하거나, 아무것도 하지 않고 거절된다.

1. 그 등록의 자격증명을 모두 버린다(진행 중이던 등록의 것 포함. 예외는 "삭제 작업과 만날 때").
2. Base를 차단한다. 삭제의 `released`와 **같은 차단**이다(7절 "차단 용량"): heartbeat·topology·sample·모듈 삭제
   표식이 `4.03`으로 거절되고 Registry에 아무것도 남기지 않는다. 차단은 **그 보드의 새 등록 세대가 active가
   될 때만** 풀린다(앱으로 다시 등록하고, 보드가 새 키로 인증된 첫 poll을 보냈을 때).
3. Base와 그 아래 등록된 모듈을 Registry에서 지운다. 다른 Base 아래에도 등록돼 있는 모듈(옮겨 간 모듈)의 그쪽
   node는 그대로다. Matter bridge는 평소처럼 Registry의 변경을 보고 그 모듈의 endpoint를 내린다.
4. 떠난 모듈의 방 지정을 정리할 것으로 적어 둔다(7절 "설정 정리"와 같은 목록, 같은 재시도). 정리할 때 다시
   확인한다: 그 시점에 어느 Base 아래에든 등록돼 있는 모듈은 건드리지 않는다.
5. 기록을 남긴다(`forgets`. 아래 "저장").

1·2·4·5는 **한 번의 파일 쓰기**다. 3은 그 쓰기 직후, 같은 Registry 잠금 안에서 일어난다(다음 문단). Matter
endpoint와 설정(방 지정)은 그 뒤에 각자의 경로로 따라온다. 네 저장소(관리 상태 파일, Registry, Matter, 설정 파일)에
걸친 원자성은 없다. 있는 것은 순서와 복구다:

| 중단된 곳 | 남는 상태 | 복구 |
|---|---|---|
| 파일 쓰기 전, 또는 쓰기 실패 | 아무것도 바뀌지 않음. 요청은 `503` | 같은 요청을 다시 보낸다 |
| 파일 쓰기 뒤, Registry 반영 전에 프로세스 종료 | 파일에는 차단·기록·정리 목록이 있다 | 시작할 때 차단을 Registry에 건다. 새 프로세스의 Registry는 비어 있다 |
| Matter endpoint를 내리기 전에 종료 | 위와 같다 | 새 프로세스는 Registry에 있는 모듈의 endpoint만 만든다 |
| 설정 정리 실패(설정 파일을 쓸 수 없음) | 정리 목록이 파일에 남는다. 기록의 `settings_cleanup_pending`이 true | 허브가 스스로 다시 시도한다(시작 뒤 유예 포함) |

**오프라인 확인은 처리하는 순간에, 허브가 한다.** 앱이 보여 준 "연결 끊김"은 근거가 아니다. 판정·저장·반영은
Registry의 한 단계다(`ingest::Registry::block_and_remove_power_base_if_offline()`): Registry의 잠금 안에서 그 Base가
stale timeout 안에 들린 적이 있는지(또는 그 아래 node가 online인지) 보고, 아니면 상태 파일을 쓰고, 쓰기가
성공했을 때만 차단하고 지운다. 그 사이에 그 Base의 메시지가 끼어들 수 없다. 들리는 Base면 파일을 쓰지 않고
`409 power_base_online`.

### 어느 등록을 지우는가: `record`

요청은 지울 **등록**을 이름으로 가리켜야 한다. snapshot의 Power Base마다 `record`(32자리 hex, 비밀 아님)가 있다.

| Base | `record` |
|---|---|
| 자격증명이 있음 | 허브가 그 보드에 가장 최근에 발급한 등록의 generation. **시작만 된 등록(provisional)도 센다** |
| 자격증명 없이 데이터로만 알려짐(이전 등록 방식) | 이 허브 프로세스가 그 Base를 처음 목록에 올릴 때 지어낸 값. 재시작하면 사라진다(그 항목 자체가 그렇다) |
| 차단됨 | 없음(목록에 없다) |

요청의 `record`가 지금 허브가 가진 값과 다르면 `409 stale_record`이고 아무것도 바뀌지 않는다. 그래서 확인창을 연
뒤에 그 보드가 다시 등록됐거나 등록이 시작됐다면, 오래된 확인은 새 등록을 지우지 못한다. 등록 세션이 진행 중이면
`409 enrollment_in_progress`. 확인창을 연 뒤 Base가 다시 연결됐다면 `409 power_base_online`.

### 삭제 작업과 만날 때

그 Base에 끝나지 않은 삭제 작업이 있을 때의 정책이다. 허브의 판단은 모두 관리 잠금 안에서 일어난다(관리 메시지
처리와 같은 잠금).

| 삭제 작업 | 목록 제거 요청 | 근거 |
|---|---|---|
| 없음, 또는 끝난 것만 | 처리 | |
| `pending` | 처리. 그 작업은 같은 파일 쓰기에서 `cancelled`가 된다. 기록의 `cancelled_removal_id` | `pending`은 명령이 한 번도 나가지 않았다는 뜻이다(`sent`는 명령을 내보내기 **전에** 저장된다). 이것은 문서에 있는 그 취소다 |
| `sent`, `accepted` | 처리. **작업과 그 작업의 키는 그대로 둔다.** 기록의 `kept_removal_id` | 명령이 보드에 있을 수 있다. 취소한 것으로 치지 않고, 끝난 것으로도 치지 않는다. 보드가 나중에 인증된 `accepted`·`released` 통지·영수증을 보내면 그 작업은 평소대로 진행하고, 영수증만이 그것을 `completed`로 만든다. Base 자체는 목록에서 빠지고 차단된다(그 키로 온 데이터 경로의 메시지는 거절). 작업은 `sent`/결과 미확인으로 계속 목록에 남는다: 보드의 증명이 오거나 그 보드가 다시 등록될 때까지 |
| `released` | `404 unknown_power_base` | 그 Base는 이미 목록에 없다. 작업은 영수증을 기다린다 |

`DELETE /api/v1/power-base-removals/<id>`는 여전히 `pending`인 삭제 요청의 **취소**이고, 이 기능과 무관하다.

### 제거 뒤에 오는 것

| 도착하는 것 | 허브 |
|---|---|
| 그 Base의 heartbeat, topology, sample, 모듈 삭제 표식 | `4.03`, 아무것도 기록하지 않음. 재시작 뒤에도 같다 |
| 버린 등록의 키로 온 poll, HELLO, LOCATE(두 포트), accepted, released 통지, 영수증 | `4.01`. 영수증의 HTTP 전달은 `409 receipt_rejected` |
| 같은 요청(같은 `request_id`) | 같은 기록, `200`. 아무것도 두 번 하지 않는다 |
| 같은 Base에 대한 다른 요청 | `404 unknown_power_base` |
| 버린 등록의 `record`를 든 요청(그 보드가 다시 등록된 뒤) | `409 stale_record` |
| 새 등록의 인증된 첫 poll | 차단 해제. 새 등록은 다른 등록과 같다 |

### 저장

`power_base_mgmt.json`의 schema는 **2 그대로**이고, 선택 목록 `forgets`가 늘었다. 그 목록에는 기록만 있다(누가,
어느 Base의 어느 `record`를, 언제, 어느 삭제 작업을 취소했거나 남겼는지). **효과는 기존 목록에 있다**: 차단은
`blocked`, 버린 자격증명은 `credentials`에서 빠진 것, 정리할 방 지정은 `cleanups`(그 항목의 `removal_id` 자리에
`forget_id`).

- 기록은 끝난 삭제 작업과 같은 시간(10분) 동안 목록에 있고, 설정 정리가 남아 있는 동안은 그보다 오래 있다.
  0.4.1부터는 **그 Base가 목록 밖에 있는 동안**(다시 등록되기 전까지) 그 Base의 마지막 기록이 남는다(14절 "자리를
  비웠던 기기").
- 한도: 기록 64개(`kMaxForgetRecords`). 지울 수 있는 기록이 없으면 `503 too_many_forgets`. 차단 자리(64개,
  `kMaxBlockedPowerBases`)가 없으면 `409 forget_capacity_full`. 어느 쪽이든 **기존 차단을 버려 자리를 만들지
  않고**, 아무것도 바뀌지 않는다. `pending` 삭제가 있던 Base는 그 작업이 잡아 둔 자리를 쓴다.
- 읽을 수 없는 파일(JSON 아님, 잘림, 모르는 `schema_version`, 형식이 틀린 `forgets` 항목)이면 관리 기능 전체가
  꺼지고(`503`), 그 파일은 덮어쓰지 않는다.
- 시각은 기록을 목록에서 내리는 데만 쓴다. 허브의 시계가 앞뒤로 바뀌어도 차단은 그대로이고, 삭제 작업의 상태는
  바뀌지 않는다.

**이전 실행 파일로 되돌릴 때 (Linux bridge 0.3.0 이하 + 0.4.0이 쓴 파일).** 2026-10-10에 0.3.0의 소스로 만든
읽기 프로그램에 0.4.0이 쓴 파일을 넣어 확인했다([`validation-status.md`](validation-status.md) "허브 목록에서
제거 (2026-10-10)"):

- 파일은 읽힌다(schema 2, 모르는 목록 `forgets`는 무시). `blocked`의 차단은 그대로 걸린다: 목록에서 제거한
  Base의 데이터는 이전 실행 파일에서도 거절된다.
- 이전 실행 파일이 그 파일을 **다시 쓰는 순간 `forgets`가 사라진다**(모르는 목록을 보존하지 않는다). 차단·
  자격증명·정리 목록은 남는다. 잃는 것은 기록뿐이다: 누가 언제 제거했는지, 같은 요청을 다시 보냈을 때의 `200`.
- **`sent`/`accepted` 작업을 남긴 채 제거한 Base는 이전 실행 파일에서 다시 목록에 보인다**: 그 작업의 키가
  active로 남아 있고, 이전 실행 파일은 "차단된 Base는 목록에 올리지 않는다"는 규칙을 모른다. 오프라인 Base로,
  그 삭제 작업과 함께 보인다. 데이터는 여전히 거절된다.
- 그러므로 이전 실행 파일로 되돌리는 것은 "데이터가 다시 받아들여진다"는 뜻의 위험은 없지만 **같은 상태로
  돌아가는 것은 아니다.** 되돌리기 전에 0.4.0을 설치하기 **전의** `power_base_mgmt.json` 백업을 복원할지(그러면
  그 뒤의 목록 제거·삭제·등록이 모두 사라진다), 0.4.0이 쓴 파일을 그대로 둘지(위의 차이)를 정해야 한다.
  자동으로 해 주는 것은 없다.

### HTTP API

경로와 응답은 [`pogohome-api-v1.md`](pogohome-api-v1.md) "Removing a Power Base from the hub's list". 요약:

- health: `"power_base_forget": 1`. 이 필드가 없는 허브에는 이 경로가 없다고 봐야 한다.
- snapshot: Power Base마다 `"record"`, 맨 뒤에 `"power_base_forgets": [...]`.
- `POST /api/v1/power-base-forgets` `{"power_base_uuid","record","request_id"}` → `201`(처리) / `200`(같은 요청).
  TLS와 페어링된 기기의 토큰이 필요하다(삭제 경로와 같은 문). EUI-64나 Thread 주소는 권한이 아니다.
- `GET /api/v1/power-base-forgets`, `GET /api/v1/power-base-forgets/<forget_id>`. `DELETE`는 없다(`405`).

### 앱

"Power Base 관리"의 둘째 동작 "허브 목록에서 제거"(앱 저장소 `PowerBaseManagementSheet`, `PowerBaseRemovalController`).

- 허브가 할 수 있다고 말할 때만(snapshot의 `power_base_forgets`), 허브가 오프라인으로 보는 Base에 `record`가 있을
  때만 보인다. 삭제가 진행 중(`sent`/`accepted`이고 결과 미확인이 아님)이면 보이지 않는다.
- 확인창은 "이 허브의 목록에서 Power Base와 센서를 제거합니다. Power Base에 저장된 연결 정보는 지워지지
  않습니다."이고, `pending` 삭제가 있으면 "대기 중인 삭제 요청은 취소됩니다."가 붙는다.
- 요청에는 확인창이 보여 준 `record`와 그 기기가 만든 `request_id`가 들어간다. 응답을 받지 못하면 "알 수 없음"으로
  두고, **같은 `request_id`로** 다시 보낸다. snapshot에 그 `request_id`의 기록이 오면 그것으로 끝난다.
- 결과는 "허브 목록에서 제거됨"으로 한 번 알리고, "삭제 완료"로 표시하는 곳은 없다. 다른 기기가 제거한 것은
  알리지 않는다(그 Base가 목록에서 사라질 뿐이다).

### 다른 허브로 옮긴 보드

보드를 버튼으로 초기화해 다른 허브에 등록하면(9절 "원격 삭제와 로컬 초기화"), 이전 허브에는 그 보드의 등록이
오프라인으로 남는다. 정리하려면 **앱을 이전 허브에 연결해** 거기서 "허브 목록에서 제거"를 한다. 그 요청은 이전
허브의 `record`를 가리키고 이전 허브만 바꾼다: 새 허브의 등록·키·모듈·방 지정은 hub_id가 다른 별개의 상태다.
연결할 수 없는 허브의 기록은 정리되지 않으며, 앱은 그것을 정리했다고 표시하지 않는다.

### 하지 않는 것, 남은 것

- 이전 허브가 꺼져 있거나 닿지 않으면 그 허브의 기록은 그대로다.
- 허브가 재시작된 뒤 한 번도 다시 들리지 않은 Base의 방 지정 정리: 0.4.0은 하지 못했다. **0.4.1부터 한다**
  (14절 "어느 모듈이 어느 Base의 것인가"). 0.4.1이 그 모듈을 적어 둔 적이 없으면(이전 버전이 쓴 파일, 적기 전에
  끝난 프로세스) 여전히 하지 않는다: 근거 없이 지우지 않는다.
- `sent`/`accepted`에서 멈춘 삭제 작업은 목록 제거 뒤에도 남는다(위 표). 그 작업을 끝내는 것은 영수증이나 재등록뿐이다.
  0.4.1부터 그 작업은 자기가 무엇인지 말한다(`list_removed`, 14절 "목록에서 빠진 Base의 삭제 작업").
- 보드는 자기 등록을 그대로 갖고 있다. 다시 연결되면 poll은 `4.01`, 데이터는 `4.03`을 받는다. 보드를 다시 쓰려면
  앱으로 등록한다(버튼 초기화가 필요한 보드는 9절).
- **실물로 실행하지 않았다.** 근거는 Mac의 host test다([`validation-status.md`](validation-status.md) "허브 목록에서
  제거 (2026-10-10)").

## 14. 목록 제거의 후속 수정 (2026-10-10, Linux bridge 0.4.1)

13절의 기능(0.4.0)을 독립 검토한 결과 두 결함이 실제 코드로 재현됐다. 이 절은 그 둘의 수정과, 13절이 "남은 것"으로
적어 둔 것의 정리다. 0.4.0은 어느 허브에도 설치된 적이 없다.

| | 0.4.0 | 0.4.1 |
|---|---|---|
| snapshot | Registry를 복사한 **뒤** 관리 기록을 따로따로 읽었다. 그 사이에 목록 제거나 release가 끝나면 한 snapshot에 "아직 목록에 있는 Base"와 "그 Base를 목록에서 뺀 기록"이 함께 들어갔다 | 관리 기록을 한 임계구역에서 읽고, 그보다 오래된 Registry 사본은 그 자리에서 다시 뜬다 |
| 방 지정 정리 | "지금 등록돼 있지 않은 모듈"을 고른 뒤, 잠금 밖에서 **UUID만으로** 방을 지웠다. 그 사이에 다른 Base에 붙어 새 방을 받은 모듈의 방도 지워졌다 | 정리는 "그 등록 아래에서 그 모듈이 있던 방"에 묶이고, 설정을 쓰는 순간에 다시 묻는다 |
| 재시작 뒤 다시 들리지 않은 Base | 어느 모듈이 그 Base의 것이었는지 몰라 정리하지 못했다 | Base별 모듈을 상태 파일에 적어 둔다 |
| `sent`/`accepted` 작업 + 목록 제거 | 작업이 그대로 남아 "결과 확인 불가"로만 보였다 | 작업이 `list_removed`를 달고 나온다: 목록은 끝났고 보드는 미확인 |
| 목록 제거의 기록 | 10분 뒤 목록에서 내려갔다. 그 사이 자리를 비운 기기는 빈 목록의 이유를 찾지 못했다 | 그 Base가 다시 등록될 때까지 남는다 |

### snapshot: Registry 사본과 관리 기록을 맞춘다

`GET /api/v1/snapshot`과 SSE는 Registry를 복사한 뒤 `Server::SnapshotJson()`을 부른다. 0.4.0의 그 함수는 관리 쪽을
네 번에 나눠 읽었다(관리 대상, 삭제 작업, `record`, 목록 제거 기록). 다른 요청의 목록 제거가 복사와 읽기 사이에
끝나면 **오래된 목록 + 새 기록**이 한 응답에 들어갔고, 그것은 새 등록이 아닌데도 앱에게는 "기록이 있는데 Base가
다시 목록에 있다 = 그 기록은 지난 일"로 읽혔다. 다음 snapshot의 빈 목록은 그래서 "설명되지 않은 빈 목록"이 되어
옛 센서가 화면에 남았다.

0.4.1 (`PowerBaseManagement::DescribeForSnapshot()`):

- 관리 쪽이 snapshot에 주는 것 전부를 **한 번의 잠금** 안에서 읽는다.
- Base와 그 모듈을 Registry에서 **빼는** 일 — release, 목록 제거 — 은 모두 관리의 잠금 안에서 일어난다. 그 일이
  끝날 때마다 Registry의 generation을 적어 둔다(`list_changed_generation_`, RAM). (새 등록 세대가 active가 되는
  것은 Registry에서 아무것도 빼지 않는다: 차단된 Base는 Registry에 항목이 없다.)
- Registry 사본은 자기가 뜬 순간의 generation을 갖고 있다(`ingest::RegistryView::generation`, Registry의 잠금 안에서
  복사와 함께 읽는다). **그 값이 위의 값보다 작으면 사본은 그 변화보다 먼저 뜬 것**이므로, 관리의 잠금을 쥔 채 새로
  뜬다. 그 뒤로 함수가 끝날 때까지 목록을 바꿀 수 있는 것은 없다.
- 시각이나 허용 시간은 쓰지 않는다. generation은 Registry가 자기 잠금 안에서 올리는 단조 증가 값이다.
- 이미 본 사본보다 오래된 사본도 다시 뜬다: snapshot은 뒤로 가지 않는다.
- 관리 쪽 변화가 없었으면 호출자의 사본을 그대로 쓴다. Registry가 스스로 배운 것(새 heartbeat 등)은 다음 snapshot의 몫이다.

삭제(release)에도 같은 순서의 틈이 있었고 같은 수정으로 닫혔다: `completed`/`released`인 작업과 "아직 목록에 있는
Base"가 한 snapshot에 함께 나오지 않는다.

앱도 따로 고쳤다(11절의 판정, 앱 저장소 `PowerBaseReleaseEvidence`): 0.4.0 같은 허브가 섞인 snapshot을 보내더라도,
기록이 가리키는 등록(`record`)과 목록의 등록이 **다를 때만** 그 기록을 지난 일로 친다.

### 자리를 비웠던 기기: 기록은 Base가 목록 밖에 있는 동안 남는다

앱은 허브의 목록이 비었을 때 화면의 센서를 바로 지우지 않는다(허브가 다시 시작하는 동안에도 목록은 빈다). "목록에서
빠졌다"는 **허브의 기록**이 있어야 지운다. 0.4.0은 그 기록을 10분 뒤에 목록에서 내렸다. 그래서 마지막 Power Base를
한 기기가 목록에서 제거하는 동안 다른 기기의 앱이 10분 넘게 백그라운드에 있었다면, 돌아온 그 앱은 빈 목록만 보고
옛 센서를 "복구 중"으로 계속 보여 줬다(앱을 다시 실행할 때까지).

0.4.1은 **그 Base를 목록에서 뺀 마지막 기록**을, 그 Base가 차단돼 있는 동안(= 다시 등록되기 전까지) 지우지도 목록에서
내리지도 않는다(`ForgetExplainsBlockLocked()`). Base가 다시 등록되면 그 기록은 지난 일이고, 보존 시간이 지났으면
다음 쓰기에서 사라진다. 차단된 Base마다 많아야 하나이므로 차단 한도(64)를 넘지 않는다. 시각은 여전히 "최근 기록을
얼마나 더 보여 줄까"에만 쓰인다.

삭제(`completed`)의 기록은 이번에 바꾸지 않았다: 끝난 삭제 작업은 이전처럼 10분 뒤 목록에서 내려간다(아래 "하지 않는
것, 남은 것").

### 방 지정 정리는 "그 등록의 그 방"에 묶인다

정리 항목(`cleanups`)은 원래 자기 등록을 갖고 있다(`eui64`, `generation`: 목록 제거에서는 `record`). 0.4.1은 여기에
**그 순간 각 모듈이 있던 방**을 더한다(`rooms`, `modules`와 같은 순서). 정리는 "이 모듈의 방"이 아니라 "이 등록
아래에서 이 모듈이 있던 그 방 지정"을 지운다.

설정을 쓰는 함수(`UnassignModuleRooms()`, `settings_store.cpp`)는 **시도할 때마다**, 자기가 읽은 설정에서 다음을
다시 정한다.

1. 방을 아는 대상은 그 모듈이 **아직 그 방에 있을 때만** 지운다. 그 뒤에 받은 방은 다른 지정이다.
2. 남은 대상마다 관리에게 묻는다(`RoomCleanupGuard`): "이 모듈은 아직 이 정리의 것인가?" 관리는 **그 순간의**
   Registry를 읽는다. 어느 Base 아래에든 등록돼 있는 모듈은 그 등록의 것이고, 모든 정리에서 영구히 빠진다.
3. 그 다음에 쓴다. 쓰기는 읽은 revision에 대한 조건부 쓰기다. 질문 **뒤에** 누가 설정을 저장했다면 쓰기는
   conflict가 되고, 다음 시도는 그 사람의 설정에서 1부터 다시 한다.

순서로 보면: 설정을 읽음(revision r) → 질문 → 쓰기(r일 때만). 다른 Base에 붙은 모듈에 사용자가 방을 주려면 먼저
그 모듈이 목록에 보여야 한다. 그것이 질문보다 먼저면 질문이 "등록돼 있다"고 답하고, 질문보다 나중이면 그 뒤의
방 저장이 쓰기를 conflict로 만든다. 대기 시간이나 재시도 횟수에 기대는 것은 없다(한 번에 네 번 시도하는 한도는
그대로 있지만, 넘으면 다음에 다시 할 뿐이다).

- 떠나는 순간 방이 없던 모듈은 정리 항목에 들어가지 않는다. 지울 것이 없고, 나중에 받는 방은 이 정리의 것이 아니다.
- 그 순간 설정을 읽을 수 없으면(설정 파일 손상) 방을 모르는 채로 적는다. 그 정리는 2의 질문만으로 보호된다.
  0.4.0이 적어 둔 정리(`rooms` 없음)도 같다.
- 이름, 방 목록, 집 이름, 다른 모듈은 건드리지 않는다(이전과 같다).
- 자동 삭제(release)의 정리도 같은 함수를 쓴다. 같은 보호를 받는다.
- 훅의 형식이 바뀌었다(`PowerBaseManagementConfig::unassign_rooms`: 대상과 질문을 함께 받는다. 새
  `read_rooms`). **UUID만으로 방을 지우는 훅은 더 없다.**

### 어느 모듈이 어느 Base의 것인가: `members`

Registry는 메모리에만 있다. 재시작 뒤 다시 보고하지 않는 Base에 대해 허브가 아는 것은 자격증명뿐이었다. 0.4.1은
**목록에 있는 Base마다 Registry가 마지막으로 그 아래에 갖고 있던 모듈**을 상태 파일에 적는다.

```json
"members": [ { "eui64": "<16 hex>", "modules": ["<16 hex>", …] } ]
```

- 모듈은 한 Base에만 있다: 마지막으로 그 모듈을 보고한 Base. 다른 Base가 보고하면 그리로 옮긴다(snapshot이 그
  모듈을 보여 주는 Base와 같은 규칙, `ResolveModuleOwners()`).
- Base가 그 모듈의 detach를 알려 노드가 Registry에서 지워지면 members에서도 뺀다. **이 프로세스가 본** 변화만이다.
- 언제 적나: snapshot을 만들 때마다(보여 주는 것은 먼저 적는다), 정리 worker의 주기(5초), release와 목록 제거의
  순간. 바뀐 것이 있을 때만 파일을 쓴다. 쓰기가 실패하면 메모리에는 남고, 다음 쓰기에 함께 간다(실패한 디스크를
  두드리지 않도록 재시도 간격을 `cleanup_retry_min`에서 `cleanup_retry_max`까지 두 배씩 늘린다).
- Base가 목록을 떠나면(release, 목록 제거) 그 Base의 members는 **같은 파일 쓰기에서** 정리 항목으로 옮겨지고
  members에서 사라진다. 이때 다른 Base가 지금 갖고 있는 모듈은 옮기지 않는다.
- 파일에서 읽은 members는 "이전 프로세스가 본 것"이다. 그것만을 근거로 한 정리는 **시작 유예**
  (`cleanup_startup_grace`, 120초: 파일에서 읽은 정리에 원래 있던 것)를 기다린다. 그 사이에 다른 Base가 그 모듈을
  보고하면 그 모듈은 정리에서 빠진다.
- 차단된 Base는 members가 없다(목록을 떠날 때 옮겨지고, 파일에 그런 항목이 있으면 읽을 때 버린다).
- 한도: Base 96개, 모듈 합계 256개, Base당 64개. 넘는 것은 적지 않는다(적지 않은 것은 재시작 뒤 정리되지
  않는다. 무엇을 버려 자리를 만들지 않는다).

**근거가 없으면 지우지 않는다.** members에 없는 모듈, `members` 목록이 없는 파일(0.4.0 이하가 쓴 것), 쓰기 전에
끝난 프로세스: 그 Base의 목록 제거는 그대로 되고(`affected_module_count`는 0), 방 지정은 남는다. 허브는 "아마 그
Base의 것이었을 모듈"을 추정하지 않는다.

상태 파일이 자기가 읽을 수 있는 크기(256 KiB)를 넘게 되는 쓰기는 하지 않는다(그 변경은 저장 실패로 끝난다).

### 목록에서 빠진 Base의 삭제 작업: `list_removed`

13절 "삭제 작업과 만날 때"는 그대로다: `sent`/`accepted` 작업과 그 키는 남고, 차단도 남는다. 0.4.1은 그 작업이
**두 가지 답을 따로** 말하게 한다.

- 목록: 끝났다. Base는 목록에 없고 데이터는 거절된다. → 작업에 `"list_removed": true`.
- 보드: 모른다. 작업의 `state`는 그대로 `sent`/`accepted`이고 `completed_at_unix_ms`는 `null`이다. 보드의 인증된
  영수증만이 `completed`로 만든다(영수증 검증에 쓸 키가 그래서 남아 있다).

`list_removed`는 저장하는 값이 아니다. "명령이 나갔을 수 있는 작업인데 그 Base가 차단돼 있다"에서 나오고(그런
상태를 만드는 것은 목록 제거뿐이다), 보드의 `released` 통지가 오면 작업이 스스로 말하므로 사라진다. 그 보드가
다시 등록되면 작업은 `superseded`로 끝나고 키는 버려진다: **옛 작업은 새 등록의 관리·삭제를 막지 않는다**
(`RequestRemoval`은 새 등록에 새 작업을 만든다).

### 저장 형식과 되돌리기

schema는 **2 그대로**다. 더해진 것은 선택 항목 둘이다: 최상위 목록 `members`, 정리 항목의 `rooms`. 0.4.0 이하가
쓴 파일은 그대로 읽는다(둘 다 없음 = 모름).

이전 실행 파일(0.4.0, 0.3.0)이 0.4.1의 파일을 읽으면:

- 읽힌다. 모르는 `members`와 `rooms`는 무시한다. 차단·자격증명·작업·정리 대상 모듈은 그대로다.
- 그 실행 파일이 파일을 **다시 쓰는 순간 `members`와 `rooms`가 사라진다.** 남은 정리는 그 버전의 방식(UUID만으로
  지움, 이 절이 고친 경쟁이 있는 방식)으로 실행된다.
- 다시 쓰지 않으면 `members`는 파일에 남지만 **그동안의 모듈 이동을 반영하지 않는다.** 그 뒤 0.4.1로 돌아오면
  그것은 "파일에서 읽은, 이 프로세스가 보지 못한" members이고, 시작 유예와 정리 때의 질문으로만 걸러진다. 유예
  안에 새 Base가 보고하지 않으면(그 Base도 꺼져 있으면) 옮겨 간 모듈의 **바뀌지 않은** 방 지정이 지워질 수 있다.
  0.4.0으로 되돌렸다가 다시 올릴 계획이면, 올리기 전에 Base들을 켜 두거나 `members`를 뺀 파일로 시작한다.
- 13절의 되돌리기 차이(`forgets`가 사라짐, `sent`/`accepted` 작업을 남긴 Base가 0.3.0에서 다시 보임)는 그대로다.

### HTTP API

- snapshot·SSE: 형식은 그대로, 내용이 한 시점의 것이 됐다.
- 삭제 작업: `"list_removed": true`가 붙을 수 있다(그럴 때만 나온다).
- health의 `power_base_forget`는 `1` 그대로다: 경로와 요청·응답 형식이 바뀌지 않았다.

### 앱 (앱 저장소 1.3.1)

- 제거 판정(`PowerBaseReleaseEvidence`): 위 "snapshot" 끝.
- 삭제 작업은 한 등록의 것이다: 목록의 Base와 `record`가 다른 작업은 그 Base의 상태도, 삭제 버튼을 막는 것도 아니다.
- `list_removed` 작업은 "삭제 미확인"으로 보인다: "허브 목록에서는 제거됐습니다. 그 전에 보낸 삭제 명령이 Power
  Base에서 실행됐는지는 확인되지 않았습니다. …" `삭제 완료`도 `삭제 중`도 아니다. 0.4.0처럼 표시가 없는 허브에서는
  목록 제거 기록의 `kept_removal_id`로 같은 것을 안다.
- 응답을 받지 못한 요청("알 수 없음")은 자기가 가리킨 등록이 목록을 떠나면 — 다른 기기의 요청으로, 삭제로, 또는
  다른 등록이 목록에 오르면 — 끝난다. 새 등록에 다시 나타나지 않고, 늦게 온 응답은 버린다. 허브가 아무 말 없이 목록만
  비운 것(재시작)으로는 끝나지 않는다. 거절은 읽을 때까지 남고, 그 Base가 목록에 없어지면 함께 사라진다.
- 확인창은 열릴 때 본 등록을 잡아 둔다: 열려 있는 동안 목록이 바뀌어도 그 등록으로 요청한다(허브가 `stale_record`로 거절).

### 하지 않는 것, 남은 것

- **끝난 삭제 작업(`completed`)의 기록은 여전히 10분 뒤 사라진다.** 마지막 Power Base를 한 기기가 **삭제**하고
  (`completed`) 다른 기기의 앱이 그동안 10분 넘게 백그라운드에 있었다면, 돌아온 앱은 빈 목록을 설명할 기록을 찾지
  못해 옛 센서를 "복구 중"으로 보여 준다(앱을 다시 실행하면 사라진다). 목록 제거에는 위의 수정이 들어갔고, 삭제
  작업의 보존 규칙은 이번 작업(목록 제거의 후속 수정)의 범위 밖이라 건드리지 않았다. 같은 방식(차단된 Base의
  마지막 `completed` 작업을 남김)으로 고칠 수 있다. **(0.4.2에서 고쳤다. 작업을 남기는 방식이 아니다: 15절.)**
- `list_removed` 작업은 보드가 영수증을 내거나 다시 등록될 때까지 남는다. 돌아오지 않는 보드의 작업은 계속 남고,
  진행 중인 삭제의 자리(32개) 하나를 쓴다. 증명 없이 끝난 것으로 바꾸는 방법은 두지 않았다.
- members에 적히기 전에 프로세스가 끝난 모듈 이동(가장 길어야 worker 주기 5초, snapshot을 읽는 기기가 있으면 그
  snapshot까지)은 다음 프로세스가 모른다. 시작 유예 안에 그 Base가 보고하면 바로잡힌다.
- 질문과 쓰기 사이에 다른 Base에 붙었고 **그 사이 아무도 설정을 저장하지 않은** 모듈의 옛 방 지정은 지워진다. 정리가
  그 직전에 끝난 것과 구별할 수 없는 순서이고, 새 방 지정이 지워지는 경우는 아니다. **(틀린 판단이었다. 구별할 수
  있고, 지워져서는 안 된다. 0.4.2에서 고쳤다: 15절.)**
- **실물로도 Linux에서도 실행하지 않았다.** 근거는 Mac의 host test와, 검토가 남긴 두 재현이다
  ([`validation-status.md`](validation-status.md) "목록 제거 후속 수정 (2026-10-10)").

## 15. 목록 제거의 후속 수정 2 (2026-10-10, Linux bridge 0.4.2)

14절(0.4.1)을 다시 독립 검토한 결과 남은 것 둘이다. 하나는 14절이 한계로 적어 둔 순서가 실제 코드로 재현된 것이고,
하나는 14절이 "범위 밖"으로 남긴 것이다. 0.4.1도 어느 허브에 설치된 적이 없다.

| | 0.4.1 | 0.4.2 |
|---|---|---|
| 방 지정 정리 | "아직 이 정리의 것인가"를 **묻고**, 그 뒤에 설정을 **썼다.** 그 사이에 모듈이 다른 Base 아래 목록에 오르고 아무도 설정을 저장하지 않으면 쓰기는 conflict가 아니었고, 그 모듈이 **그대로 갖고 있던** 방이 지워졌다 | 판단과 쓰기가 **한 구간**이다. 그 구간 동안 Registry는 보고를 받아들이지 않는다 |
| 관리가 보지 못한 사이의 일 | 모듈이 다른 Base에 붙었다가 다시 떼어진 것을, 관리가 Registry를 본 두 시점 사이에 일어났다면 알지 못했다 | Registry가 "모듈이 마지막으로 새로 목록에 오른 때"를 기억한다 |
| 일반 삭제를 통째로 놓친 기기 | 끝난 삭제 작업은 10분 동안만 snapshot에 있다. 그동안 백그라운드에 있던 기기는 빈 목록의 이유를 찾지 못해 옛 센서를 "복구 중"으로 계속 보여 줬다 | snapshot이 **지금 목록 밖에 있는 Base**를 말한다. 작업의 표시 기간과 무관하다 |

### 판단과 쓰기를 한 구간으로

0.4.1의 순서:

1. 설정을 읽는다.
2. 모듈마다 관리에게 묻는다: 아직 이 정리의 것인가? (관리가 Registry를 그때 본다.)
3. 설정을 쓴다(읽은 revision을 조건으로).

2와 3 사이에 Base B가 그 모듈을 보고하면 — 모듈을 B에 옮겨 꽂은 것이다 — 3은 그것을 모른다. 누군가 그 사이에 설정을
저장했다면 3이 conflict가 되어 처음부터 다시 하지만, 방을 **바꾸지 않고** 옮기기만 한 모듈에는 저장이 없다. 그 모듈의
방은 B 아래의 방 지정이 됐는데도 지워졌다. 한 번 더 묻는 것으로는 닫히지 않는다: 마지막 질문과 쓰기 사이는 언제나
남는다.

0.4.2에서 쓰기는 `RoomCleanupGuard::commit()`을 지난다(`room_cleanup.hpp`). 관리가 그 안에서 하는 일:

1. 관리의 잠금을 잡는다.
2. `ingest::Registry::hold()`: Registry의 잠금을 **잡은 채로** 그 사본을 받는다. 이 함수가 돌려줄 때까지 어떤 보고도
   적용되지 않는다 — 모듈이 어느 Base 아래 목록에 오르지도, 떨어지지도, Base가 차단되지도 않는다.
3. 그 사본으로 이번 쓰기가 지우려는 모듈들을 **다시 판단**한다(아래의 규칙 전부).
4. 아직 이 정리의 것인 모듈이 하나라도 있으면, 그것들만 지우는 설정 쓰기를 **그 자리에서** 한다. 하나도 없으면 쓰지
   않는다.
5. 두 잠금을 놓는다.

3에서 "어느 Base에도 없다"고 본 모듈은 4가 끝날 때까지 어느 Base에도 없다. 그 사이에 도착한 보고는 5 뒤에 적용되고,
그때 그 모듈은 방 없이 새 Base 아래에 오른다 — 정리가 먼저이고 이동이 나중인, 온전한 순서 둘 가운데 하나다. 반쯤
섞인 순서는 없다.

- **대기나 재시도 횟수로 고친 것이 아니다.** 시도 횟수의 상한(4)은 그대로이고 아무것도 그 상한에 기대지 않는다:
  모든 시도가 설정을 다시 읽고, 위의 구간을 지난다.
- **쓰기 전의 질문(2단계)은 없앴다.** 구간 밖에서 받은 답은 쓰기 때 이미 낡았을 수 있고, 구간이 같은 판단을
  하므로 남길 이유가 없다. 설정을 읽은 뒤 "그 방에 있는 대상"이 모두 후보이고, 구간이 그 가운데 아직 이 정리의 것인
  모듈을 가린다.
- **설정 충돌**: 구간 안의 쓰기도 읽은 revision을 조건으로 한다. 그사이 기기가 저장했으면 conflict이고, 다음 시도가
  그 설정에서 다시 시작해 다시 구간을 지난다. 새로 받은 방은 0.4.1처럼 "방 결속"(그 등록에서 있던 방과 다른 방은
  건드리지 않음)으로도 보호된다.
- **자동 삭제(release)와 목록 제거는 같은 함수**(`ProcessSettingsCleanups()`)이고 같은 구간을 지난다.

### 잠금의 순서

| 잠금 | 누가 그 안에서 무엇을 더 잡는가 |
|---|---|
| 정리 pass 하나(`cleanup_run_mutex_`) | 관리 |
| 관리(`PowerBaseManagement::mutex_`) | Registry, 설정 저장소(`read_rooms`) |
| Registry(`ingest::Registry::mutex_`) | 설정 저장소(위 4의 쓰기), 관리의 상태 파일 쓰기(목록 제거의 commit: 0.4.0부터) |
| 설정 저장소(`SettingsStore`) | 없다 |

순서는 언제나 관리 → Registry → 설정 저장소다. 거꾸로 가는 길은 없다: Registry는 관리나 설정을 부르지 않고(차단
목록은 관리가 Registry에 넣어 준다), 설정 저장소는 아무것도 부르지 않으며, HTTP의 설정 저장은 설정 저장소의 잠금만
잡는다. 구간 안의 코드는 Registry를 부르지 않는다(자기 자신을 기다리게 된다): 그 안에서 관리가 "정리가 끝났다"를
알리는 일은 잠금을 놓은 뒤로 미뤘다.

구간이 잡고 있는 동안 ingest 스레드(보고)와 snapshot을 만드는 요청은 기다린다. 그 시간은 설정 파일 한 번의 쓰기다.
목록 제거가 자기 상태 파일을 Registry의 잠금 아래에서 쓰는 것(0.4.0)과 같은 종류의 대기이고, 정리는 Base가 목록을
떠났을 때만 돈다.

### "정리가 적힌 뒤에 목록에 오른 적이 있는가"

0.4.1의 관리는 Registry를 **볼 때마다** 목록에 있는 모듈을 정리에서 뺐다(worker가 5초마다, snapshot을 만들 때마다).
두 번 보는 사이에 모듈이 다른 Base에 붙었다가 그 Base가 detach를 확인해 다시 사라지면, 관리는 그것을 본 적이 없다.
그 모듈은 잠깐이라도 다른 등록의 것이었으므로 방을 지우면 안 된다.

Registry가 이제 모듈마다 **마지막으로 새로 목록에 오른 때**를 기억한다(`RegistryView::listings`: 그 보고가 만든
`generation`). 노드가 생기는 한 곳 — 어떤 Base 아래의 첫 보고, 또는 detach 뒤의 첫 보고 — 에서만 적히고, 노드가
사라져도 남는다. 정리 항목은 자기가 적힌 때의 Registry generation을 갖고(RAM. 파일에서 읽은 정리는 읽은 때), 관리는
"그 뒤에 새로 목록에 오른 모듈"을 정리에서 영영 뺀다.

- 표는 256개(동시에 목록에 있을 수 있는 수의 네 배)로 제한한다. 넘치면 지금 노드가 없는 모듈의 가장 오래된 기록부터
  버리고, **어느 generation까지 버렸는지**를 말한다(`listings_dropped_through`). 기록이 없는 모듈은 "오른 적이
  없다"거나 "기록이 버려졌다"인데, 관리가 마지막으로 본 뒤에 생긴 기록이 벌써 버려졌을 때만 후자를 의심할 수 있다.
  그때는 **방을 지우지 않는다**(그 모듈을 정리에서 뺀다). 추정으로 지우는 일은 없다.
- 프로세스가 다시 시작되면 표는 비어 있다. 파일에서 읽은 정리는 14절의 시작 유예를 기다리고, 그동안 보고된 모듈은
  목록에 올라 있다.
- 14절의 규칙 — **지금** 어느 Base 아래 목록에 있는 모듈은 정리에서 빠진다 — 은 그대로 남아 있고, 이 기록과 겹치지
  않는 경우가 하나 있다: 관리가 그 정리를 넘겨받기(파일에서 읽기) **전에** 이미 목록에 올라 있던 모듈이다. 그 모듈의
  기록은 정리가 넘겨받은 때보다 앞서므로 "그 뒤에 오른 적"으로는 가려지지 않고, "지금 목록에 있다"로 가려진다. bridge는
  관리를 읽은 뒤에 수신을 시작하므로 운영에서는 생기지 않는 순서이지만, 그 순서에 기대지 않는다.

### 지금 목록 밖에 있는 Base: `power_base_departures`

두 가지를 나눈다.

- **작업의 이력을 보여 주는 기간.** 끝난 삭제 작업과 목록 제거 기록은 10분 동안 snapshot에 있다(목록 제거 기록은
  14절의 규칙으로 더 오래). 요청한 사람에게 결과를 보여 주기 위한 것이다. 바꾸지 않았다.
- **그 등록이 끝났다는 사실.** Base는 등록이 끝난 순간부터 새 등록이 active가 될 때까지 **차단**돼 있다(7절).
  차단은 이력이 아니라 허브의 현재 상태이고, 상태 파일에 이미 있다.

snapshot은 이제 차단된 Base를 그대로 말한다:

```json
"power_base_departures": [
  {"power_base_uuid": "6055f9fffe001234", "record": "…32 hex…", "reason": "removed", "since_unix_ms": 1790000100000}
]
```

| 필드 | 뜻 |
|---|---|
| `power_base_uuid` | 목록 밖에 있는 Base |
| `record` | 끝난 등록(목록에 있을 때의 `record`, 삭제 작업의 `generation`). 이전 허브가 적은 차단에는 없다 |
| `reason` | `removed`: 보드가 등록을 놓았다(삭제의 통지나 영수증). `forgotten`: 오프라인인 채 목록에서 뺐다(보드가 지웠다는 말이 아니다). 모르면 없다 |
| `since_unix_ms` | 차단된 때(허브의 시계. 판단에 쓰지 않는다) |

- **보존 시간이 없다.** Base가 다시 등록되지 않는 한 몇 시간 뒤에도, 재시작 뒤에도 있다. 다시 등록되어 차단이 풀리면
  그 순간 없어진다. 그래서 **오래된 근거가 남아 다시 등록된 Base를 가리킬 수 없다**: 목록에 있는 Base는 이 목록에
  없다(한 임계구역에서 만든 snapshot이다: 14절).
- **수와 크기**: 차단마다 하나, 차단은 64개가 상한이다(7절). 상태 파일에는 차단 항목마다 선택 키 둘(`reason`,
  `record`)이 늘었다: 100바이트 남짓.
- **저장**: 차단과 **같은 쓰기**에 들어간다(release의 저장, 목록 제거의 저장). 그 쓰기가 실패하면 차단도 없고 이
  항목도 없다. 따로 실패할 수 있는 쓰기가 없다.
- **이전 상태 파일**: `reason`·`record`가 없는 차단은 그대로 읽히고, snapshot에는 Base만 나온다(모르는 것을 지어내지
  않는다). 그것으로 충분하다: 차단돼 있다는 것이 "지금 등록이 없다"는 뜻이다. 그런 차단의 목록 제거 기록은 14절의
  규칙(그 Base의 마지막 기록)으로 남긴다.
- **목록 제거 기록을 남기는 규칙**이 차단의 이유를 따른다: 차단이 삭제(release)로 생겼거나 다른 등록의 목록 제거로
  생겼으면, 예전 등록의 목록 제거 기록은 남기지 않는다(0.4.1은 "차단된 Base의 마지막 기록"이면 남겼다: 목록 제거 →
  재등록 → 삭제 뒤에 첫 기록이 다시 붙들렸다).
- `sent`/`accepted` 작업을 남긴 목록 제거(14절 `list_removed`): Base는 `forgotten`으로 나오고 작업은 그대로다. 나중에
  영수증이 와서 작업이 `completed`가 돼도 차단의 이유는 처음 그대로다. **증명 없이 작업을 끝난 것으로 바꾸지 않고,
  키와 차단을 지우지 않는다.**

### 앱 (앱 저장소 1.3.2)

- `power_base_departures`가 말한 Base가 그 snapshot의 목록에 없으면, 그 Base와 그 센서는 **사라진 것**이다. 삭제
  작업이나 목록 제거 기록이 snapshot에 없어도 그렇다. 16분, 5시간 뒤에 처음 받은 snapshot으로 옛 센서가 내려간다.
- 이 근거는 **기억하지 않는다.** snapshot마다 새로 읽고, 그 Base를 다시 목록에 올린 snapshot은 그 말을 하지 않는다.
  작업·기록에 쓰는 "지난 일" 판정(14절)이 필요 없다.
- 목록에 있는 Base는 무엇이 함께 와도 목록에 있는 것이다.
- 허브가 재시작해 목록이 비었고 아무 Base도(또는 다른 Base만) 이 목록에 있으면: 이전처럼 센서를 유지하고 복구
  중으로 보인다.
- 이 필드가 없는 허브(0.4.1 이하)에는 이전 판정 그대로다. 그 조합에서는 위의 결함이 남는다.

### 저장 형식과 되돌리기

- 상태 파일은 schema 2 그대로다. 차단 항목의 선택 키 `reason`·`record`만 늘었다. 값이 이 허브가 쓸 수 없는 것이면
  (모르는 `reason`, 32자리 hex가 아닌 `record`) 파일이 손상된 것으로 보고 관리 기능을 시작하지 않는다.
- 0.4.2 → 0.4.1/0.4.0/0.3.0: 파일은 읽힌다(두 키를 모른다). 차단은 그대로이고, 그 실행 파일이 파일을 다시 쓰면 두
  키가 사라진다. 그 뒤 0.4.2로 돌아오면 그 차단은 "이유를 모르는 차단"이다: Base는 여전히 목록 밖으로 나온다.
- HTTP API: `schema_version` 1 그대로, health의 `power_base_forget`도 `1` 그대로. snapshot에 최상위 배열 하나가
  `power_base_forgets` 뒤에 늘었다. 그 필드를 모르는 앱(1.3.1 이하)은 무시한다.
- 관리 규약(CoAP), BLE 등록 규약, enrollment record, POGO: 그대로. 보드 펌웨어는 바꾸지 않았다.

### 하지 않는 것, 남은 것

- 앱이 **0.4.1 이하의 허브**에 연결돼 있으면 일반 삭제를 통째로 놓친 기기의 옛 센서는 이전처럼 남는다(허브가
  말해 주지 않는다).
- 요청한 기기가 완료를 보지 못하고 오래 뒤에 돌아오면 "삭제 완료" 알림은 뜨지 않는다. Base와 센서가 사라져 있을
  뿐이다.
- 구간이 잡혀 있는 동안의 대기(설정 파일 한 번의 쓰기)는 실제 저장 장치에서 재 보지 않았다.
- Registry의 기록 표가 넘치고 그 넘친 기록이 "관리가 마지막으로 본 뒤"의 것이면 방을 지우지 않는다(안전한 쪽).
  그 방 지정은 사용자가 고칠 때까지 남는다. 256개의 다른 모듈이 관리가 한 번도 보지 않는 사이(가장 길어야 worker
  주기)에 새로 목록에 올라야 하는 일이다.
- 14절의 나머지 한계는 그대로다: `list_removed` 작업은 영수증이나 재등록까지 남고, `members`에 적히기 전에 끝난
  프로세스의 모듈 이동은 다음 프로세스가 모른다.
- **실물로도 Linux에서도 실행하지 않았다.** 근거는 Mac의 host test와 검토가 남긴 재현이다
  ([`validation-status.md`](validation-status.md) "목록 제거 후속 수정 2 (2026-10-10)"). 경쟁 순서는 주입한 것이고,
  "16분 뒤"는 test가 시각을 옮긴 것이다.

## 16. 삭제 요청은 한 등록의 것이다, snapshot에는 순서가 있다 (2026-10-11, Linux bridge 0.5.0)

15절(0.4.2)을 독립 검토한 결과 둘이다. 0.4.2도 어느 허브에 설치된 적이 없다.

| | 0.4.2까지 | 0.5.0 |
|---|---|---|
| 삭제 요청 | Base의 EUI-64 **하나만** 보냈다. 허브는 그때 active인 등록에 삭제 작업을 만들었다. 요청의 응답을 놓친 기기가 같은 요청을 다시 보내면, 그사이 삭제가 끝나고 다른 기기가 그 Base를 다시 등록한 뒤라도 **새 등록**의 삭제 작업이 만들어지고 UNENROLL이 나갔다 | 요청이 **확인 당시의 등록**을 말한다. 허브는 그 등록에만, 한 번만 적용한다 |
| snapshot의 순서 | 없었다. `GET`의 응답과 event는 서로 다른 연결로 오고, 늦게 도착한 것이 나중 상태라는 보장이 없다. 오래된 `GET` 응답이 새 event 뒤에 적용되면 화면이 뒤로 갔다 | snapshot마다 "이 실행의 몇 번째인가"를 말한다. 앱은 더 오래된 것을 적용하지 않는다 |

### 왜 EUI-64만으로는 안 되는가

Base는 보드가 있는 한 같은 EUI-64다. **등록**은 끝나고 새로 시작한다: 삭제하면 끝나고, 누가 다시 등록하면 다른
등록이다(`record`가 다르다: 13절). "이 Base를 삭제해 달라"는 요청은 사용자가 화면에서 본 **그 등록**에 대한 것인데,
EUI-64만 실은 요청은 어느 등록에 대한 것인지 말하지 않는다. 그래서 다음 셋을 허브가 구별할 수 없었다.

- 같은 요청이 다시 온 것(응답 유실 뒤의 재시도, 네트워크가 두 번 전달한 것, 늦게 도착한 첫 전달).
- 확인창을 열어 둔 사이 목록이 바뀐 뒤에 눌린 확인.
- 사용자가 지금 목록을 보고 새로 한 확인.

앞의 둘은 새 등록에 닿으면 안 되고, 셋째는 실행돼야 한다. 앱에서 버튼을 막거나 문구를 바꾸는 것으로는 닫히지 않는다:
이미 떠난 요청은 앱이 되돌릴 수 없다.

### 요청이 말하는 것

`POST /api/v1/power-base-removals`의 본문은 정확히 네 멤버다.

```json
{"power_base_uuid": "6055f9fffe001234", "record": "…32 hex…", "removal_serial": 0, "request_id": "…32 hex…"}
```

| 멤버 | 뜻 | 어디서 오는가 |
|---|---|---|
| `power_base_uuid` | Base | snapshot |
| `record` | 사용자가 확인한 **등록** | snapshot의 그 Base 항목 `record`(13절) |
| `removal_serial` | 그 등록에 지금까지 삭제가 몇 번 요청됐는가 | snapshot의 그 Base 항목 `removal_serial`(이번에 추가) |
| `request_id` | **이 확인** 하나의 id. 기기가 고른다 | 확인마다 새로. 같은 요청을 다시 보낼 때는 같은 값 |

허브가 작업을 만드는 조건은 셋 모두다.

1. 그 Base가 지금 `record`의 등록으로 목록에 있다.
2. 그 등록의 삭제가 진행 중이지 않다.
3. `removal_serial`이 그 등록의 현재 값이다.

작업을 만들면서 **같은 저장 한 번에** 그 등록의 `removal_serial`을 1 올린다. 그래서:

- **요청은 자기가 말한 등록 밖으로 나가지 못한다.** Base가 삭제되고 다시 등록됐으면 1이 깨진다: `409 stale_record`,
  아무것도 만들지 않고 보드에 명령을 주지 않는다. 요청이 처음 도착한 것이든 다시 온 것이든 같다.
- **요청 하나는 작업을 많아야 하나 만든다.** 작업을 만든 요청이 다시 오면 3이 깨진다(자기가 serial을 올렸다).
  취소된 요청이 다시 와도 새 작업이 생기지 않는다.
- **이 보장은 기록에 기대지 않는다.** `removal_serial`은 등록(자격증명)과 함께 저장되고 그 등록이 있는 동안 있다.
  끝난 작업이 10분 뒤 목록에서 사라져도, 허브가 다시 시작해도, 과거 요청은 여전히 1이나 3에서 걸린다. "기록이
  없어졌으니 새 요청으로 본다"는 길이 없다.
- **사용자가 새로 한 확인은 실행된다.** 지금의 `record`와 `removal_serial`, 새 `request_id`를 싣기 때문이다.

### 답

| 상황 | 답 |
|---|---|
| 조건 셋이 모두 맞다 | `202`, 새 작업(`request_id`가 든다) |
| **같은 `request_id`**의 작업이 아직 허브에 있다(진행 중이거나, 끝나서 표시 기간 안) | `200`, 그 작업. 지금 상태 그대로: `cancelled`·`completed`·`superseded`일 수 있다. 새로 만드는 것은 없다 |
| 요청이 말한 등록의 삭제가 이미 진행 중이다(누가 요청했든. `released`로 목록을 떠난 뒤에도) | `200`, 그 작업 |
| 목록의 등록이 `record`와 다르다, 또는 `removal_serial`이 지났다 | `409 stale_record` |
| `request_id`가 다른 Base의 작업에 쓰였다 | `409 request_id_conflict` |
| Base가 목록에 없다 / 자격증명이 없다 | 이전과 같다: `404 unknown_power_base` / `409 power_base_management_unsupported` |
| **본문이 `power_base_uuid` 하나뿐이다** (이전 앱의 요청) | `422 removal_registration_required`. 어떤 Base든, 무엇이 목록에 있든. 아무것도 조회하지 않는다 |

마지막 줄이 호환 정책이다. 이전 형식의 요청은 등록을 말하지 않으므로 위의 어느 것과도 구별할 수 없고, 그래서 **어떤
경우에도 실행하지 않는다.** "지금 목록에 있는 등록에 적용한다"는 호환 경로를 남기면 그것이 곧 결함이다.

두 기기가 같은 등록을 동시에 확인하면 한쪽이 `202`, 다른 쪽이 같은 작업으로 `200`이다(조건 2). 그 작업이 취소된
**뒤에** 도착한 다른 기기의 예전 요청은 `stale_record`다: 그 기기가 확인한 뒤로 그 등록에 삭제가 한 번 요청되고
끝났다. 다시 확인하면 된다.

### 지원 판정: 앱은 무엇을 보고 요청하는가

| | 뜻 |
|---|---|
| health `power_base_removal_request` | `2`: 위의 요청을 받는다. 없거나 모르는 수: 그 허브에 삭제 요청을 보내면 안 된다 |
| snapshot의 Base 항목 `removal_serial` | 허브가 그 Base를 삭제할 수 있고, 요청이 실을 값이 이것이다. `management`가 `supported`일 때만 있다 |

앱(1.3.3)은 Base 항목에 `record`와 `removal_serial`이 **둘 다** 있을 때만 삭제를 제안한다. 조합별:

| 앱 | 허브 | 일반 삭제 |
|---|---|---|
| 1.3.3 | 0.5.0 | 위의 요청. 정상 |
| 1.3.3 | 0.4.2 이하 | **제안하지 않는다.** "지원하지 않음: … 허브를 업데이트해야 합니다"를 보여 준다. EUI-64만 실은 요청은 보내지 않는다. 이미 있는 작업의 표시·취소·삭제 확인(영수증)과 "허브 목록에서 제거"는 그대로 된다 |
| 1.3.2 이하 | 0.5.0 | 요청이 `422`로 거부된다. 그 앱은 "허브가 요청을 받아들이지 않았습니다. 아무것도 삭제되지 않았습니다."를 보여 준다. 작업 목록·취소·영수증·목록 제거는 그대로 된다. 앱을 업데이트해야 삭제할 수 있다 |
| 1.3.2 이하 | 0.4.2 이하 | 이전 그대로이고, **이 절의 결함이 그대로 있다** |

### 저장

- 상태 파일은 schema 2 그대로다. 선택 키 둘이 늘었다: 자격증명의 `removal_serial`(0이면 쓰지 않는다), 작업의
  `request_id`(이전 허브가 만든 작업에는 없다). 삭제를 요청한 적이 없는 허브의 파일은 0.4.2가 쓰는 것과 바이트까지
  같다.
- **용량**: `request_id`는 작업에만 붙고, 작업의 수는 이전의 상한 그대로다(진행 중 32, 끝난 것 32, `released`는 차단
  64개 안). `removal_serial`은 자격증명마다 정수 하나다(상한 96). 과거 요청의 목록 같은 것은 두지 않는다.
- **저장 실패**: 작업과 serial이 한 쓰기다. 실패하면 `503`이고 작업도 없고 serial도 그대로다. 메모리에만 있는
  작업으로 보드에 명령을 주는 일은 없다(7절).
- **재시작**: 둘 다 파일에 있다.
- **보존 기간이 끝난 뒤**: 끝난 작업이 지워지면 `request_id`도 함께 지워진다. 그 요청이 다시 오면 "같은 요청"으로
  알아보지는 못하지만, 위의 조건 1(다른 등록)이나 3(지난 serial)에서 거부된다. 답이 `200`(그 작업)에서
  `409 stale_record`로 바뀔 뿐이고, 어느 쪽도 아무것도 만들지 않는다.
- 값이 틀린 키(음수나 32비트를 넘는 `removal_serial`, 32자리 hex가 아닌 `request_id`)가 있는 파일은 손상된 것으로
  보고 관리 기능을 시작하지 않는다.
- 0.5.0 → 0.4.2 이하: 파일은 읽힌다(두 키를 모른다). 그 실행 파일이 파일을 다시 쓰면 두 키가 사라진다. 그 뒤
  0.5.0으로 돌아오면 serial은 0부터다: 그 사이에 만들어진 "serial 0의 요청"은 다시 유효해 보일 수 있다. 0.4.x는
  설치하지 않는다(15절).

### snapshot의 순서: `snapshot_stream`, `snapshot_sequence`

snapshot(`GET /api/v1/snapshot`의 응답과 `/api/v1/events`의 event)의 **맨 끝**에 두 필드가 늘었다.

```json
"snapshot_stream": "…32 hex…", "snapshot_sequence": 41
```

| 필드 | 뜻 |
|---|---|
| `snapshot_stream` | bridge가 시작할 때 만든 값. **실행 하나에 하나** |
| `snapshot_sequence` | 그 실행이 snapshot을 만든 순서: 1, 2, 3, … `GET`과 event가 한 줄로 센다 |

허브는 snapshot을 **한 번에 하나씩** 만든다(`Server::SnapshotBody()`의 구간). 그 구간 안에서 snapshot이 말하는 것을
모두 읽는다: 관리의 상태(관리의 잠금 아래), 설정의 revision(설정 저장소의 잠금 아래), 그리고 Registry의 사본 —
호출자가 구간 **밖에서** 미리 뜬 사본이 앞의 snapshot의 호출자가 가져온 사본보다 오래됐으면 그 자리에서 새로 뜬다.
관리가 켜져 있으면 앞의 snapshot은 관리가 그 순간 새로 뜬 사본으로 만들어졌을 수도 있는데, 그것은 관리가 스스로
기억한다(15절: 마지막으로 받아들인 사본보다 오래된 사본은 받지 않고 새로 뜬다). 그래서 한
stream의 두 snapshot 가운데 **번호가 큰 쪽은 작은 쪽보다 오래된 것을 말하지 않는다.** 어느 쪽이 먼저 도착하든,
`GET`이든 event든 같다. 설정만 바뀐 snapshot도 새 번호를 받는다(아무것도 바뀌지 않은 `GET`도 그렇다: 번호는 상태의
버전이 아니라 만든 순서다).

잠금의 순서는 이 구간 → 관리 → Registry → 설정 저장소이고, 이 구간을 잡는 것은 snapshot을 만드는 요청뿐이다
(15절의 순서 앞에 하나가 붙었다).

**두 stream 사이에는 번호가 아무것도 말하지 않는다.** 다른 stream은 bridge의 다른 실행이다. 허브는 "어느 실행이
나중인가"를 말하지 않는다: 그것을 저장해 두는 값(재시작 횟수 같은 것)은 파일이 지워지거나 되돌려지면 거꾸로 가고,
그러면 앱이 살아 있는 허브의 말을 모두 "오래된 것"으로 버리게 된다. 실행의 순서는 앱이 자기가 **요청을 시작한
순서**로 안다(아래).

시각은 쓰지 않는다. `generated_at_unix_ms`는 허브의 시계이고 뒤로 갈 수 있다.

### 앱 (앱 저장소 1.3.3)

**삭제 요청**

- 삭제 확인창이 열릴 때 그 Base의 `record`와 `removal_serial`을 잡아 둔다. 확인 버튼은 **그 값**으로 요청한다:
  창이 열려 있는 동안 목록이 바뀌어도 요청이 말하는 등록은 바뀌지 않는다.
- 확인 한 번이 `request_id` 하나다. 응답을 받지 못한 요청은 "요청이 전달됐는지 알 수 없습니다. 다시 요청해도
  안전합니다."와 **"다시 요청"** 버튼으로 남고, 그 버튼은 **같은 요청**(같은 등록·serial·id)을 다시 보낸다.
  그래서 그 문장이 참이다: 허브는 그 요청에 자기 작업으로 답하거나, 그 작업 하나를 지금 만들거나, 거부한다.
- 응답 없는 요청은 snapshot이 그 등록에 대해 말하는 것으로 정리된다: 자기 `request_id`의 작업이 보이면 그것이
  응답이다. 그 등록의 삭제가 진행 중이거나 끝났으면, 또는 Base가 **다른 등록**으로 목록에 있으면 요청은 조용히
  끝난다. 그 뒤에 도착한 응답은 누구의 것도 아니다: 새 등록의 작업·버튼·센서 표시를 바꾸지 않는다.
- `409 stale_record`는 "그사이 이 Power Base의 등록이나 삭제 요청 상태가 바뀌어 삭제를 요청하지 않았습니다. 목록을
  새로 확인한 뒤 다시 시도하세요."다.

**snapshot의 순서**

- 한 stream 안: 이미 적용한 가장 큰 `snapshot_sequence`보다 크지 않은 snapshot은 **통째로 적용하지 않는다**
  (Base 목록, 작업, 설정 신호, 삭제 근거 판정 어디에도 넣지 않는다).
- stream이 바뀔 때(허브가 다시 시작했다): 앱은 요청마다 — `GET` 하나, event 연결 하나 — **시작한 순서**의 번호를
  붙여 둔다(시각이 아니다). 지금 따르는 stream을 처음 본 **뒤에 시작한** 요청의 응답은 그 실행이나 그 뒤 실행의
  것일 수밖에 없다(앞선 실행은 그 전에 끝났다). 그래서 다른 stream의 snapshot은 그 요청이 뒤에 시작됐으면 새
  stream으로 받아들이고, 앞에 시작됐으면 적용하지 않는다. 실제로는 더 새로운 snapshot을 한 번 버릴 수 있지만,
  다음 요청은 언제나 뒤에 시작한 요청이므로 앱이 허브를 영영 못 따라가는 일은 없다.
- 다른 허브로 바꾸면 이 판단은 처음부터다.
- 두 필드가 없는 허브(0.4.2 이하): 이전처럼 도착한 순서대로 적용한다. 판단할 근거가 없고, 시각으로 추정하지
  않는다. 그 조합에서는 순서 역전의 표시 문제가 남는다.
- 15절의 "목록 밖의 Base" 판정은 바꾸지 않았다. 오래 자리를 비운 기기가 처음 받는 snapshot은 그 stream의 새
  번호이므로 그대로 적용되고, 옛 센서가 내려간다.

### HTTP API와 버전

- `schema_version` 1 그대로. **삭제 요청의 본문이 바뀌었다**(멤버 하나 → 넷, 이전 형식은 거부): 이것이
  `linux_bridge`가 0.4.2에서 **0.5.0**이 된 이유다(0.y.z의 MINOR: 이전 클라이언트의 요청 하나가 더는 받아들여지지
  않는다).
- 추가된 것: health의 `power_base_removal_request`(2), snapshot의 Base 항목 `removal_serial`, 작업의 `request_id`,
  snapshot 끝의 `snapshot_stream`·`snapshot_sequence`, 오류 `stale_record`(삭제 경로에도)·`request_id_conflict`
  (삭제 경로에도)·`removal_registration_required`·`invalid_record`·`invalid_removal_serial`·`invalid_request_id`.
- 그대로인 것: 작업 조회·목록·취소·영수증, 목록 제거, 등록, 설정, 관리 규약(CoAP), BLE 등록 규약, enrollment
  record, POGO. 보드 펌웨어는 바꾸지 않았다.

### 하지 않는 것, 남은 것

- **이전 앱(1.3.2 이하)과 이전 허브의 조합에는 결함이 그대로 있다.** 허브만 올려도(0.5.0) 그 앱의 삭제 요청은
  거부되므로 결함은 없어지고, 대신 그 앱에서는 삭제할 수 없다.
- 앱 1.3.3은 0.4.2 이하의 허브에 삭제를 **요청하지 않는다.** 지금 운영 중인 허브(0.3.0)에 이 앱만 올리면 허브를
  올릴 때까지 앱에서 Power Base를 삭제할 수 없다.
- 자격증명이 새로 **발급만 되고 아직 active가 아닌** 동안(등록을 시작했다가 끝내지 않은 Base)에는 목록의 `record`가
  그 새 자격증명이다. 그때의 삭제 요청은 이전과 같이 active인 등록의 작업을 만든다(목록이 요청 뒤로 바뀌지 않았다는
  것이 조건 1의 뜻이다). 그 작업의 `generation`은 목록의 `record`와 다르다: 이전부터 있던 모양이고 이번에 바꾸지
  않았다.
- 순서 방어는 **표시**의 문제를 고친다. 삭제 명령이나 보드의 상태와는 무관하다(이전에도 잘못된 명령이 나간 것은
  아니었다).
- stream이 바뀌는 순간에 더 새로운 snapshot 하나를 버릴 수 있다(위). 다음 요청(주기 5초)으로 따라잡는다.
- **앱은 실제 연결에서 event의 snapshot을 적용하지 못한다**(이 작업에서 찾았고 고치지 않은, 앱의 이전부터 있던 결함:
  앱이 쓰는 시스템의 줄 읽기가 event를 끝내는 빈 줄을 넘겨주지 않는다. 앱 저장소 `CHANGELOG.md` "검증 한계"). 앱은
  연결할 때의 `GET`과 5초 간격의 `GET`으로 허브를 따라간다. 허브가 event에 싣는 순서 필드는 그대로 옳고, 앱의 순서
  판단은 무엇이 가져온 snapshot이든 같으므로 그 결함이 고쳐지면 event에도 그대로 적용된다. 실제 연결에서 지금
  뒤바뀔 수 있는 것은 두 `GET` 응답의 순서다.
- 응답을 놓친 요청을 앱은 메모리에만 둔다. 앱을 다시 실행하면 "다시 요청"은 없고, 사용자가 목록을 보고 새로
  확인한다(새 요청이다). 예전 요청의 복사본이 그 뒤에 허브에 닿아도 허브가 거부한다.
- **실물로도 Linux에서도 실행하지 않았다.** 근거는 Mac의 host test(실제 관리 구현과 서명된 관리 메시지, 실제
  TLS·HTTP 경로)와 검토가 남긴 재현, 앱의 시뮬레이터 test다([`validation-status.md`](validation-status.md) "삭제 요청의
  등록 결속과 snapshot 순서 (2026-10-11)"). 순서는 주입한 것이고 기다린 시간은 test의 시계다.
