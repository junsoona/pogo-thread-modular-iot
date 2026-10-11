# 변경 이력

이 저장소의 네 구성요소(Power Base `power_base`, 센서 모듈 `module`, OTBR RCP `rcp`, Linux bridge
`linux_bridge`)의 변경을 적는다. 버전 규칙과 도구는 [`docs/versioning.md`](docs/versioning.md)에 있다.

- 버전은 구성요소마다 따로 가진다. 각 항목 앞의 `[…]`가 어느 구성요소의 변경인지 나타낸다.
- `Unreleased`에는 아직 release로 만들지 않은 변경을 모은다. release 빌드를 만들 때
  `## [<구성요소> <버전>] - <날짜>`로 옮긴다.
- 항목은 **추가 / 수정 / 호환성 / 검증 한계**로 나눈다. "검증 한계"는 빼지 않는다: 무엇을 실물로
  확인하지 않았는지가 변경 이력의 일부다.
- 설정 코드, PSKd, Dataset, Network Key, 관리 키, 토큰, 보드·허브 식별값, 운영 주소는 여기에 적지
  않는다.

## [Unreleased]

### 2026-10-11 실물 시험 및 Git 보존

두 실제 iPhone에서 앱 1.3.4 (17)와 허브 0.5.0 조합의 핵심 시험을 완료했습니다. [결과와 제외 범위](docs/release-records/2026-10-11-two-iphones-power-base.md)를 별도로 기록합니다. 아래 미설치·실물 미검증 설명은 각 구현 작업 당시의 상태입니다. 공개 문서만 정리하며 제품 버전·시험한 코드·기존 산출물은 유지합니다.

현재 버전: `power_base` 0.3.0, `module` 0.1.1, `rcp` 0.1.0, `linux_bridge` **0.5.0**. 2026-10-11에 `0.4.2`의 독립
검토에서 나온 둘 — 과거의 삭제 요청이 다시 등록된 Base의 새 등록을 삭제하는 결함, 오래된 snapshot 응답이 새 상태를
되돌리는 표시 문제 — 을 고치면서(아래 "수정"의 첫 항목) `linux_bridge`만 `0.4.2`에서 `0.5.0`으로 올렸다. **MINOR인
이유**: 이번에는 선택 항목의 추가만이 아니다. `POST /api/v1/power-base-removals`의 본문이 바뀌었고, EUI-64만 실은
이전 형식의 요청은 `422`로 거부된다 — 이전 클라이언트(앱 1.3.2 이하)의 요청 하나가 더는 받아들여지지 않는다.
`docs/versioning.md`의 규칙으로는 "이전 상대와 더는 동작하지 않는 변경"이고, `0.y.z` 동안 그런 변경은 MINOR로 올리고
"호환성"에 적는다(아래에 적었다). 새 사용자 기능은 없다. 보드 펌웨어의 소스는 바뀌지 않았다(`power_base`·`module`·
`rcp`는 `versions.json`이 빌드 입력이라 빌드 식별자만 달라진다). 이 버전으로 만든 것은 Mac의 host 빌드뿐이고,
Linux에서 빌드하거나 설치한 적이 없으며 실물 검증된 버전이 아니다. `0.4.2`의 산출물은 그대로 `0.4.2`다.

그 전: `linux_bridge` **0.4.2**. 2026-10-10의 **두 번째 후속
수정**(아래 "수정"의 둘째 항목)으로 `linux_bridge`만 `0.4.1`에서 `0.4.2`로 올렸다. `0.4.1`을 다시 독립 검토해 남은 결함
둘 — 방 지정 정리의 판단과 쓰기 사이, 일반 삭제를 통째로 놓친 기기의 옛 센서 — 을 고친 것이다. PATCH로 둔 이유는
`0.4.1` 때와 같다: `0.4.1`도 어느 허브에 설치된 적이 없는 후보이고, 이 변경은 그 후보의 결함 수정이며 새 사용자
기능이 없다. 더해진 형식 — snapshot의 `power_base_departures`, 상태 파일 차단 항목의 `reason`·`record` — 은 없으면
`0.4.1`과 같은 동작이고 이전 앱과 이전 허브가 모르는 채 동작한다. `docs/versioning.md`의 표를 글자대로 읽으면 새 API
필드는 MINOR 쪽이라는 점도 같고, 같은 판단을 했다(다르게 정하면 `versions.json`의 한 줄이다). 보드 펌웨어의 소스는
바뀌지 않았다. 이 버전으로 만든 것은 Mac의 host 빌드뿐이고, Linux에서 빌드하거나 설치한 적이 없으며 실물 검증된
버전이 아니다.

그 전: `linux_bridge` **0.4.1**. 2026-10-10의 **후속 수정**
(아래 "수정"의 셋째 항목)으로 `linux_bridge`만 `0.4.0`에서 `0.4.1`로 올렸다. `0.4.0`의 "허브 목록에서 제거"를 독립
검토한 결과 실제 코드로 재현된 두 결함을 고치고, 그 기능이 남겨 둔 것을 정리한 것이다. 버전을 PATCH로 둔 이유:
`0.4.0`은 어느 허브에도 설치된 적이 없는 개발 후보이고, 이 변경은 그 기능의 **결함 수정**이며 새 사용자 기능이
없다. 더해진 형식 — 삭제 작업의 `list_removed`, 상태 파일의 `members`와 정리 항목의 `rooms` — 은 모두 선택
항목이고, 없으면 `0.4.0`과 같은 동작이며, 이전 앱과 이전 허브가 모르는 채로 계속 동작한다(두 버전의 실제 코드로
확인: "호환성"). `docs/versioning.md`의 표를 글자 그대로 읽으면 "새 API 필드 추가"는 MINOR 쪽이다. 이 선택은
그 점을 알고 한 판단이고, 다르게 정하면 `versions.json`의 한 줄이다. `0.3.0` 때의 후속 수정(아래)과 달리 버전을
**올린** 이유는 이번에는 다른 구성요소가 보는 형식에 추가가 있기 때문이다. 보드 펌웨어의 소스는 바뀌지 않았다.
이 버전으로 만든 것은 Mac의 host 빌드뿐이고, Linux에서 빌드하거나 설치한 적이 없으며 실물 검증된 버전이 아니다.

그 전: `linux_bridge` 0.4.0. 2026-10-10에 돌아오지 않는
Power Base를 허브의 목록에서 지우는 기능(아래 "추가"의 첫 항목)을 넣으면서 `linux_bridge`만 `0.4.0`으로 올렸다
(이전 상대와 계속 동작하는 기능 추가: MINOR). 보드 펌웨어의 소스는 바뀌지 않았다. `versions.json`이 모든
구성요소의 빌드 입력이므로 `power_base`·`module`·`rcp`의 빌드 식별자는 달라지지만 버전과 동작은 그대로다. 이
버전으로 만든 것은 Mac의 host 빌드뿐이고, Linux에서 빌드하거나 설치한 적이 없으며 실물 검증된 버전이 아니다.

그 전: `power_base` 0.3.0, `module` 0.1.1, `rcp` 0.1.0, `linux_bridge` 0.3.0. 2026-10-09에 같은 허브의
주소·포트 변경 복구(아래 "추가"의 첫 항목)를 넣으면서 그 기능을 가진 `power_base`와 `linux_bridge`를 `0.3.0`으로
(이전 상대와 계속 동작하는 기능 추가: MINOR), 연결된 센서 체인의 재등록 복구 결함(아래 "수정"의 첫 항목)을
고치면서 `module`을 `0.1.1`로(다른 구성요소가 보는 형식이 그대로인 수정: PATCH) 올렸다. `rcp`는 소스가 바뀌지
않아 `0.1.0` 그대로다. 이 버전들로 만든 산출물도 모두 개발 빌드이며 실물 검증된 버전이 아니다.

같은 날의 **후속 수정 두 건**(아래 "수정"의 첫 두 항목: 주소 복구의 탐색 누락, 센서 체인의 이전 JOIN_RESULT)은
버전을 올리지 않았다. `0.3.0`과 `0.1.1`은 아직 어느 보드에도 허브에도 설치된 적이 없는 개발 후보이고, 두 수정은
그 후보가 실물 시험에 나가기 전에 코드 검토에서 나온 결함을 고친 것이며, 다른 구성요소가 보는 형식(POGO 메시지,
관리 메시지, BLE, HTTP API, record, 상태 파일, capability)은 하나도 바뀌지 않았다. 앞의 후보와 새 후보는 build id와
소스 지문으로 구별한다([`docs/versioning.md`](docs/versioning.md) "언제 올리나", "빌드 종류"). 앞의 후보(build id가
`…dirty.0a4e8b76307a`, `…dirty.7ae0df18db6e`, `…dirty.dfeda036433e`인 것)는 실물 시험에 쓰지 않는다.

그 전: `power_base` 0.2.0, `module` 0.1.0, `rcp` 0.1.0, `linux_bridge` 0.2.0. `0.1.0`은 버전 체계를
시작한 **개발 기준선**이었고, 2026-10-08에 Power Base 삭제의 자동 확인(아래 "추가"의 첫 항목)을 넣으면서
그 기능을 가진 두 구성요소만 `0.2.0`으로 올렸다(이전 상대와 계속 동작하는 기능 추가: MINOR). 센서 모듈과
RCP는 기능이 바뀌지 않아 `0.1.0` 그대로다. 이 버전들로 만든 산출물은 모두 개발 빌드이며, 배포 후보도 실물
검증된 버전도 아니다.

기준 커밋은 `590e2a11d2cb93f1cbc2b2b0eb094d50ea289a50`(`feature/unified-module-role-manager`)이고,
아래 내용은 2026-10-09 현재 **커밋되지 않은 작업 트리**에 있다.

### 추가

- **[linux_bridge 0.4.0] 허브 목록에서 제거 (2026-10-10).** 돌아오지 않는 Power Base(고장, 분실, 버튼으로 초기화해
  다른 허브에 등록)를 **이 허브의 목록에서** 지운다. 삭제가 아니다: 보드에 저장된 연결 정보는 지워지지 않고,
  아무것도 그렇다고 말하지 않으며, 어떤 삭제 작업도 이것으로 `completed`가 되지 않는다. 규약과 설계:
  `docs/power-base-management.md` 13절, HTTP: `docs/pogohome-api-v1.md` "Removing a Power Base from the hub's list".
  - **허브가 하는 일**(`PowerBaseManagement::ForgetPowerBase()`): 그 등록의 자격증명을 버리고, Base를 차단하고
    (삭제의 `released`와 같은 차단: 새 등록 세대가 active가 될 때만 풀린다), Base와 그 아래 모듈을 Registry에서
    지우고, 떠난 모듈의 방 지정을 기존 설정 정리에 넘긴다. 자격증명 없이 등록된 이전 방식의 Base도 된다.
  - **오프라인 확인은 처리하는 순간에 허브가 한다.** 판정·저장·반영이 Registry의 한 단계다
    (`ingest::Registry::block_and_remove_power_base_if_offline()`): 들리는 Base면 파일을 쓰지 않고 `409`, 파일을
    쓰지 못하면 아무것도 차단하거나 지우지 않는다.
  - **요청은 등록을 가리킨다.** snapshot의 Power Base마다 `record`(비밀 아님)가 있고, 요청의 `record`가 지금의
    것과 다르면 `409 stale_record`: 확인창을 연 뒤 다시 등록됐거나 등록이 시작된 보드의 새 등록은 지워지지 않는다.
    같은 `request_id`의 요청은 같은 기록을 받는다(두 번 하지 않는다).
  - **삭제 작업과 만날 때**: `pending`은 같은 파일 쓰기에서 `cancelled`가 된다(명령이 나간 적이 없다).
    `sent`·`accepted`는 **작업과 그 키를 그대로 두고** Base만 목록에서 뺀다 — 취소로도 완료로도 치지 않으며, 보드의
    인증된 영수증만이 그 작업을 `completed`로 만든다. `released`의 Base는 이미 목록에 없다.
  - **HTTP**: health `power_base_forget: 1`, snapshot의 `record`와 `power_base_forgets`,
    `POST`/`GET /api/v1/power-base-forgets`(TLS + 페어링된 기기의 토큰. `DELETE` 없음).
  - **저장**: `power_base_mgmt.json` schema 2 그대로, 선택 목록 `forgets`. 효과(차단, 버린 자격증명, 정리 목록)는
    기존 목록에 있다.
  - 검증 도구: `linux/matter_bridge/app/api/tests/test_power_base_management.cpp`의 `ListRemoval*` 다섯 장면,
    `test_removal_api.cpp`의 `ForgetApiTests`, `linux/matter_bridge/ingest/tests/test_power_base_block.cpp`의
    마지막 장면.
- **[power_base 0.3.0][linux_bridge 0.3.0] 같은 허브의 주소·포트 변경 복구 (2026-10-09).** 등록된 Power Base가,
  같은 허브(같은 hub_id·등록 세대·관리 키, 같은 Thread 네트워크)의 Thread 수신 주소나 ingest port가 바뀌었을
  때 허브를 스스로 다시 찾아 목적지를 고치고 통신을 잇는다. 앱은 관여하지 않는다(꺼져 있어도 된다). 이전에는
  등록 때 받은 목적지를 고치는 것이 없어 재등록이 유일한 복구였다. 규약과 설계:
  `docs/power-base-management.md` 12절.
  - **[power_base] 찾기(`components/power_base_mgmt`의 "locate").** 관리 poll이 인증된 답 없이 6번 연속
    끝나면(약 1분 30초) 관리 task가 poll 사이에 round를 돈다. 힌트는 보드 자신의 Thread Network Data와 router
    table에서 읽은 border router·server·leader·router의 locator 주소다(아무것도 보내지 않고 읽는다.
    `power_base/main/enrollment_locate_hints.c`). 힌트는 인증된 것이 아니다. 각 후보에 `LOCATE`를 보내고, 그 등록의
    키·보드·hub_id·세대·방금 보낸 nonce가 모두 맞는 `LOCATE_ACK`가 말하는 목적지를, **그 목적지에서 직접 한 번
    더 확인받은 뒤에만** 쓴다. 목적지는 record의 Mesh-Local Prefix 안의 EID여야 한다.
  - **[power_base] 저장과 전환.** flash의 record를 다시 읽어 여전히 같은 등록(`ENROLLED`)일 때만, 목적지만
    바꿔 blob 하나로 쓰고 다시 읽는다. 그 뒤에 관리 poll과 exporter가 새 목적지를 쓴다. 쓰기 실패나 전원
    차단 뒤에는 이전 record 전체 또는 새 record 전체가 남는다. `UNENROLLING`·`RELEASED`·미등록·로컬 초기화
    중에는 찾지도 쓰지도 않고, 확인된 답이 삭제된 record를 되살리지 못한다.
  - **[power_base] exporter의 목적지 전환**(`linux_bridge_exporter_set_destination()`). 새 목적지로 heartbeat,
    전체 topology, 모듈별 최신 sample, 전달되지 않은 삭제 표식을 주기를 기다리지 않고 다시 보낸다. 전송 중에
    목적지가 바뀐 요청의 답은 전달로 치지 않는다.
  - **[power_base][linux_bridge] 관리 메시지 `LOCATE`/`LOCATE_ACK`**(type `0x06`/`0x86`)와 capability bit 두 개
    (`PB_MGMT_BOARD_CAP_ENDPOINT_LOCATE`, `PB_MGMT_HUB_CAP_ENDPOINT_LOCATE`). 규약 번호는 1 그대로다.
  - **[linux_bridge] `LOCATE`의 답.** `provisional`·`active` 자격증명의 인증된 `LOCATE`에, `wpan0`의 주소
    목록에서 그때 읽은 Mesh-Local EID와 실제 ingest port로 답한다(권한이나 OTBR helper 호출 없음). 상태 파일과
    자격증명의 상태는 바뀌지 않는다.
  - **[linux_bridge] locate 포트.** `--port`가 5684가 아니면 5684에 socket을 하나 더 열어 `LOCATE`에만 답한다
    (그 밖에는 답하지 않고 Registry에 닿지 않는다). port만 바뀐 허브를 보드가 물을 수 있는 자리다.
    `docs/matter-bridge.md`.
  - 찾기의 한도: round당 unicast 요청 12개 이하, round 간격은 poll 6·12·24·48번 뒤, 그 뒤로는 90번마다(약 1~1.5분에서 15~22분까지), multicast 없음, 찾는 동안
    flash 쓰기 없음. record v1·v2의 보드는 찾지 않는다(답을 인증할 키가 없다).
  - 검증 도구: `tools/test_power_base_locate.c`, `tools/test_linux_bridge_exporter_destination.c`(host),
    `linux/matter_bridge/app/api/tests/test_power_base_management.cpp`의 `EndpointLocate`(실제 허브 코드와 실제
    보드 코드), `linux/matter_bridge/ingest/tests/test_loopback.cpp`(실제 UDP socket의 locate 포트),
    `test_interface_address.cpp`(EID 고르기), `tools/test_power_base_locate_openthread.sh`(ESP-IDF의 OpenThread
    core를 host에서 실행하는 재현: `tools/ot_host_repro/pb_locate_nexus.cpp`. 펌웨어의 힌트 수집 파일
    `power_base/main/enrollment_locate_hints.c`를 그대로 link한다).
- **[power_base 0.2.0][linux_bridge 0.2.0] Power Base 삭제의 자동 확인 (2026-10-08).** 삭제를 끝낸 보드가
  영수증(`UNENROLL_DONE`/`ERASED`)을 스스로 허브에 보낸다. 이전에는 영수증이 BLE로만 나와, 앱이 보드를
  찾아 설정 코드로 인증하고 읽어 전달해야 허브의 삭제 작업이 `completed`가 됐다. 앱(1.2.0 (7))은 삭제를 한
  번 확인받은 뒤 허브의 상태만 보여 준다. 규약: `docs/power-base-management.md`(머리말, 2·4·6·7절).
  - **`completed`의 조건은 그대로다.** 보드가 저장된 가입 정보를 지우고, 다시 읽어 확인하고, 최종 record를
    저장한 뒤에 만든 영수증을 허브가 그 등록의 키로 검증했을 때만이다. 통지, 시간 경과, 보드의 capability
    알림은 어느 것도 완료를 만들지 않는다. 영수증의 바이트도 그대로다(BLE로 읽는 것과 같다).
  - **[power_base] 영수증 전달 세션.** 최종 record를 저장한 뒤, OpenThread의 영속 저장소 두 곳(settings와
    PSA 키 저장소)을 RAM으로 돌리고, 새 extended address로, 삭제 직전에 RAM에 읽어 둔 Dataset으로 Thread에
    한 번 더 붙어 영수증을 보낸다(최대 5회). flash에는 아무것도 쓰이지 않고, 끝나면 두 저장소를 다시 읽은
    뒤 재시작한다. 새 component `components/power_base_ot_store`(저장소 스위치 `pb_volatile_store`, 저장소
    read-back `pb_stored_network`)와 `power_base`의 link 옵션(`-Wl,--wrap`, 아홉 함수). 서비스·exporter·POGO는
    켜지 않는다.
  - **[power_base][linux_bridge] 관리 메시지 `HELLO`/`HELLO_ACK`**(type `0x05`/`0x85`, capability bitmask).
    보드가 "영수증을 스스로 보낸다"(`PB_MGMT_BOARD_CAP_THREAD_RECEIPT`)를 인증된 메시지로 알린다. 허브는
    자격증명과 함께 저장한다.
  - **[linux_bridge] `erase_confirmation`.** 끝나지 않은 삭제 작업과, 삭제할 수 있는 `power_bases[]` 항목의
    선택 필드(`automatic`/`manual`). 보드가 capability를 알렸는지와, `released` 뒤 4분이 지났는지로 정한다.
    표시용이며 상태를 바꾸지 않는다. `docs/pogohome-api-v1.md`.
  - **[linux_bridge] 같은 영수증에는 같은 확인.** 완료하면서 키를 지운 뒤에도, 관리 resource로 다시 온 같은
    영수증에는 그때의 `RESULT_ACK`를 다시 준다(완료된 작업에 확인의 MAC을 남긴다). 아무것도 다시 쓰지 않는다.
  - 검증 도구: `tools/test_power_base_ot_store.c`(host), `tools/check_power_base_ot_store.py`(link된 이미지와
    ESP-IDF 소스 대조), `tools/test_power_base_release_session_openthread.sh`(ESP-IDF의 OpenThread core와 PSA
    키 저장소를 host에서 실행하는 재현. `tools/ot_host_repro/`).
- **[도구] 설정 코드 QR 생성 도구 (2026-10-06).** 보드가 이미 가진 설정 코드로 그 보드의 QR
  (`POGO-PB:1:<eui64>:<setup_code>`, 형식 버전 1, 53바이트 ASCII)을 PNG로 만드는
  `tools/gen_power_base_setup_qr.swift`(macOS, Core Image·ImageIO·Vision, 설치할 라이브러리 없음)와
  그 test `tools/test_gen_power_base_setup_qr.py`, 독립 판독기 `tools/power_base_setup_qr_check.swift`,
  앱의 파서와 함께 쓰는 벡터 `tools/power_base_setup_qr_vectors.json`. 앱(1.1.0 (3))이 이 QR에서 설정
  코드를 읽어 입력 칸에 넣는다. 형식·사용법·비밀 취급: `docs/power-base.md` "QR 설정 코드".
  - 새 코드를 만들지 않고 cfg 이미지와 보드를 건드리지 않는다. 설정 코드는 mode 0600 파일에서만
    읽고(인자·환경 변수·출력에 없다), QR은 mode 0600으로 본인만 접근하는 디렉터리에 쓰며, Git 작업
    트리 안과 기존 파일 위에는 쓰지 않는다.
  - **이 변경은 네 구성요소의 어느 것도 바꾸지 않는다.** 펌웨어와 Linux bridge의 소스·빌드 입력·
    런타임 동작은 그대로이고 버전은 모두 0.1.0 그대로다.
- **[전체] 버전 관리와 빌드 식별 (2026-10-06).** 버전의 단일 원본 `versions.json`, 소스 지문과
  빌드 식별자, 빌드 manifest와 그 검증, 설치·실물 시험 기록 양식(`tools/pogo_version.py`,
  `tools/cmake/pogo_version.cmake`, `components/pogo_build_info`, `docs/versioning.md`,
  `docs/release-records/`).
  - [power_base][module] app description의 `version`이 제품 버전(`0.1.0`)이 되고, 부팅 로그에
    `POGO <구성요소> <버전> build <식별자> (<종류>, <구성>)` 한 줄이 나온다.
  - [rcp] app description과 이미지 안의 빌드 정보만 들어간다(`rcp/main/rcp_build_info.c`). 운영
    RCP는 로그를 내지 않으며, RCP는 여전히 공유 컴포넌트를 쓰지 않는다.
  - [linux_bridge] `pogo_matter_bridge --version`, `pogohome-hubctl --version`, health의 선택 필드
    `build`.
- **[power_base] BOOT 버튼 로컬 재등록 (2026-10-06).** 정상 부팅한 Base에서 BOOT를 5초 누르고 떼면
  120초 동안만 재등록용 BLE 창이 열리고, 설정 코드로 인증한 앱이 사용자의 확인을 받은 뒤에만
  보드가 자신의 등록 기록과 Thread 저장소를 지우고 검증한다. BLE 규약에 상태 조회 `0x08`, 초기화
  명령 `0x09`, capability `local_reset_v1`이 추가됐다. 버튼 구성
  (`power_base/sdkconfig.reenroll_button_gpio9`)에서만 켜진다. 설계: `docs/power-base.md`
  "BOOT 버튼 로컬 재등록".
- **[power_base][linux_bridge] Power Base 관리: 일반 등록과 삭제 (2026-10-05).** 허브가 등록 세대마다
  발급하는 관리 자격증명과 그 BLE 전달(`0x06`, capability `mgmt_v1`), 인증된 등록 해제(CoAP
  `pogo/v1/mgmt`), 허브의 삭제 작업·차단·Registry/Matter/공유 설정 정리, 삭제 영수증
  (capability `release_receipt_v1`). 영속 등록 레코드 v3. 규약: `docs/power-base-management.md`.

### 수정

- **[linux_bridge 0.5.0] 삭제 요청은 한 등록의 것이다, snapshot에는 순서가 있다 (2026-10-11).** `0.4.2`의 독립 검토에서
  나온 둘. 설계: `docs/power-base-management.md` 16절.
  - **과거의 삭제 요청이 새 등록을 삭제했다.** 삭제 요청은 Base의 EUI-64 하나만 실었고, 허브는 그때 active인 등록에
    작업을 만들었다. 응답을 놓친 기기가 같은 요청을 다시 보내면 — 그사이 삭제가 끝나고 다른 기기가 그 Base를 다시
    등록한 뒤라도 — **새 등록**의 삭제 작업이 만들어지고 그 보드에 UNENROLL이 나갔다(검토가 실제 관리 구현과 서명된
    관리 메시지로 재현했다). 이제 요청은 **확인 당시의 등록**(`record`), 그 등록에 삭제가 몇 번 요청됐는지
    (`removal_serial`), 그 확인의 id(`request_id`)를 싣고, 허브는 셋이 지금도 맞을 때만 작업을 만들며 만들면서 같은
    쓰기로 `removal_serial`을 올린다. 요청 하나는 자기가 말한 등록에만, 많아야 한 번 적용된다: 다시 등록된 Base에는
    `409 stale_record`, 같은 요청이 다시 오면 그 요청이 만든 작업(`200`) — 취소됐든 완료됐든 — 이고, 그 작업이 목록에서
    사라진 뒤(10분)나 허브가 다시 시작한 뒤에는 `409 stale_record`다. 이 보장은 과거 요청의 기록이 아니라 **등록과
    함께 저장되는 serial**에 선다: 기록이 사라졌다고 과거 요청이 현재 등록의 새 요청이 되지 않는다. 사용자가 지금
    목록을 보고 새로 한 확인(현재의 `record`·`removal_serial`, 새 `request_id`)은 그대로 실행된다.
  - **EUI-64만 실은 이전 형식의 요청은 어떤 Base에도 실행하지 않는다**(`422 removal_registration_required`, 아무것도
    조회하지 않는다). 등록을 말하지 않는 요청은 위의 어느 경우와도 구별할 수 없고, "지금 목록에 있는 등록에
    적용한다"는 호환 경로가 곧 결함이다. 작업 조회·목록·취소·영수증과 목록 제거는 바뀌지 않았다.
  - **지원 판정**: health의 `power_base_removal_request`(값 2), snapshot의 Base 항목 `removal_serial`(삭제할 수 있는
    Base에만). 작업에는 `request_id`가 붙는다(응답을 놓친 기기가 자기 요청의 작업을 알아본다).
  - **오래된 snapshot이 새 상태를 되돌렸다.** `GET`의 응답과 event는 다른 연결로 온다. 새 등록을 보여 준 event 뒤에,
    그 전에 만들어진 `GET` 응답(이전 등록이 "목록 밖"이라고 말하는 것)이 적용되면 앱이 새 등록의 Base를 사라진 것으로
    판정했다(다음 snapshot까지. `0.4.2`가 한계로 적어 둔 것을 검토가 재현했다). snapshot의 맨 끝에
    `snapshot_stream`(bridge의 실행 하나에 하나)과 `snapshot_sequence`(그 실행이 snapshot을 만든 순서)가 붙는다. 허브는
    snapshot을 한 번에 하나씩 만들고 그 구간 안에서 관리·설정·Registry를 읽으므로, 한 stream에서 번호가 큰 snapshot은
    작은 것보다 오래된 것을 말하지 않는다 — `GET`이든 event든, 어느 것이 먼저 도착하든. 시각은 쓰지 않는다. 앱 쪽
    판단은 앱 저장소 `1.3.3`.
  - 검증: `docs/validation-status.md` "삭제 요청의 등록 결속과 snapshot 순서 (2026-10-11)".
- **[linux_bridge 0.4.2] 목록 제거의 후속 수정 2 (2026-10-10).** `0.4.1`의 독립 검토에서 남은 둘. 설계:
  `docs/power-base-management.md` 15절.
  - **방 지정 정리가 "확인한 뒤, 쓰기 전"에 다른 Base로 옮겨 간 모듈의 방을 지웠다.** `0.4.1`은 "아직 이 정리의
    것인가"를 묻고 그 뒤에 설정을 썼다. 그 사이에 모듈이 다른 Base 아래 목록에 오르고 아무도 설정을 저장하지 않으면
    쓰기는 conflict가 아니었고, 그 모듈이 **그대로 갖고 있던** 방이 지워졌다(검토가 실제 코드로 재현했다. `0.4.1`의
    문서가 "구별할 수 없는 순서"라고 적은 것은 틀렸다). 지금은 판단과 쓰기가 **한 구간**이다: 관리가 자기 잠금과
    Registry의 잠금을 잡은 채 Registry를 복사해 다시 판단하고, 그 잠금을 놓기 전에 설정을 쓴다
    (`ingest::Registry::hold()`, `RoomCleanupGuard::commit()`). 그 구간 동안 어떤 보고도 적용되지 않으므로, 쓰기가
    시작될 때 정리의 것인 모듈은 쓰기가 끝날 때까지 어느 Base에도 없다. 한 번 더 묻거나 기다리는 것으로 고친 것이
    아니다. 자동 삭제(release)의 정리도 같은 함수다. 잠금의 순서는 언제나 관리 → Registry → 설정 저장소이고 거꾸로
    가는 길이 없다.
  - **관리가 보지 못한 사이에 다른 Base에 붙었다가 떼어진 모듈.** Registry가 모듈마다 마지막으로 새로 목록에 오른
    때를 기억한다(`RegistryView::listings`, 256개 상한). 정리가 적힌 뒤에 한 번이라도 목록에 오른 모듈은 그 정리의
    것이 아니다. 표가 넘쳐 알 수 없게 된 경우에는 방을 지우지 않는다.
  - **일반 삭제를 통째로 놓친 기기가 옛 센서를 "복구 중"으로 계속 보여 줬다.** 끝난 삭제 작업은 10분 동안만
    snapshot에 있다. 그동안 백그라운드에 있던 기기는 돌아와서 빈 목록의 이유를 찾지 못했다. 보존 시간을 늘리지
    않았다: snapshot이 **지금 목록 밖에 있는 Base**를 말한다(`power_base_departures`: 차단된 Base마다 하나, 끝난
    등록의 `record`와 `reason`). 차단은 등록이 끝난 순간부터 새 등록이 active가 될 때까지 있는 허브의 현재
    상태이므로, 그 근거는 몇 시간 뒤에도 재시작 뒤에도 있고, 다시 등록되는 순간 없어진다 — 오래된 근거가 다시
    등록된 Base를 가리킬 수 없다. 작업을 보여 주는 기간(10분)과 `sent`/`accepted` 작업·키·차단은 그대로다.
  - 목록 제거 기록을 남기는 규칙이 차단의 이유를 따른다: 차단이 삭제로 생겼거나 다른 등록의 목록 제거로 생겼으면
    예전 등록의 기록은 붙들지 않는다(`0.4.1`은 목록 제거 → 재등록 → 삭제 뒤에 첫 기록을 다시 붙들었다).

- **[linux_bridge 0.4.1] 목록 제거의 후속 수정 (2026-10-10).** `0.4.0`의 독립 검토가 실제 구현으로 재현한 두
  결함과, `0.4.0`이 "남은 것"으로 적어 둔 것. 설계: `docs/power-base-management.md` 14절.
  - **snapshot에 "제거 전 목록"과 "제거 뒤 기록"이 섞였다.** `GET /api/v1/snapshot`과 SSE는 Registry를 복사한
    뒤에 관리 기록을 (네 번에 나눠) 읽었다. 그 사이에 다른 요청의 목록 제거나 보드의 `released`가 끝나면 한
    응답에 아직 목록에 있는 Base와 그 Base를 뺀 기록이 함께 들어갔고, 앱은 그 기록을 "지난 일"로 읽어 다음의
    빈 목록에서 옛 센서를 화면에 남겼다. 지금은 관리 쪽을 한 임계구역에서 읽고
    (`PowerBaseManagement::DescribeForSnapshot()`), 관리가 목록을 마지막으로 바꾼 것보다 **먼저 뜬** Registry
    사본은 그 자리에서 다시 뜬다. 판단은 Registry의 generation이다(`ingest::RegistryView::generation`). 시각이나
    허용 시간은 쓰지 않는다.
  - **늦게 실행된 방 지정 정리가 다른 Base로 옮긴 모듈의 새 방까지 지웠다.** 정리는 "지금 등록돼 있지 않은
    모듈"을 고른 뒤 잠금 밖에서 UUID만으로 방을 지웠다. 지금은 (1) 정리 항목이 **그 순간 각 모듈이 있던 방**을
    갖고 있어 그 방 지정만 지우고, (2) 설정을 쓰는 함수가 **읽은 뒤·쓰기 전에, 시도할 때마다** 관리에게 "아직 이
    정리의 것인가"를 묻고(그 순간의 Registry), (3) 그 뒤의 설정 저장은 조건부 쓰기를 conflict로 만들어 처음부터
    다시 하게 한다. 대기 시간을 늘리거나 재시도를 제한해서 고친 것이 아니다. 자동 삭제(release)의 정리도 같은
    함수이고 같은 보호를 받는다. 이름·방 목록·집 이름·다른 모듈은 이전처럼 건드리지 않는다.
  - **재시작 뒤 다시 보고하지 않은 Base의 방 지정을 정리하지 못했다.** 목록에 있는 Base마다 Registry가 마지막으로
    그 아래 갖고 있던 모듈을 상태 파일에 적는다(`members`). 그런 Base를 삭제하거나 목록에서 빼면 그 모듈이 정리
    대상이 된다. 다른 Base가 그 뒤 보고한 모듈은 그 Base의 것이고, 파일에서 읽은 것만을 근거로 한 정리는 시작
    유예를 기다린다. **적어 둔 적이 없는 것은 정리하지 않는다**(이전 버전의 파일, 적기 전에 끝난 프로세스).
  - **`sent`/`accepted` 작업을 남긴 목록 제거를 작업이 스스로 말한다.** 그런 작업에 `"list_removed": true`가
    붙는다: 목록은 끝났고(Base는 목록에 없다) 보드는 미확인(작업의 state는 그대로, `completed`가 아니다). 키와
    차단은 그대로 남는다. 보드가 다시 등록되면 작업은 `superseded`로 끝나고 새 등록의 삭제를 막지 않는다
    (이것은 `0.4.0`에서도 그랬고, test로 고정했다).
  - **목록 제거의 기록이 10분 뒤 사라져, 그 사이 자리를 비운 기기가 빈 목록의 이유를 찾지 못했다.** 마지막 Power
    Base를 한 기기가 목록에서 제거하는 동안 다른 기기의 앱이 10분 넘게 백그라운드에 있었다면 그 앱은 옛 센서를 "복구
    중"으로 계속 보여 줬다. 지금은 Base를 목록에서 뺀 마지막 기록을 그 Base가 다시 등록될 때까지 남긴다(차단된 Base마다
    많아야 하나). 삭제(`completed`) 기록의 10분 보존은 바꾸지 않았다("검증 한계").
  - 훅의 형식이 바뀌었다: `PowerBaseManagementConfig::unassign_rooms`는 대상(모듈과 방)과 질문을 함께 받고,
    새 `read_rooms`가 떠나는 순간의 방을 읽는다. UUID만으로 방을 지우는 훅은 더 없다.
  - 상태 파일이 자기가 읽을 수 있는 크기를 넘게 되는 쓰기는 하지 않는다(그 변경은 저장 실패로 끝난다).

- **[power_base 0.3.0, 후속 수정] 허브가 후보 목록의 뒤쪽에 있으면 몇 round를 돌아도 찾지 못하던 것 (2026-10-09.
  코드 검토의 host 재현으로 발견. 실물에서 본 증상이 아니다).** 주소·포트 변경 복구의 round는 매번 같은 후보
  목록을 처음부터 물었고 한 round의 요청은 12개였다. 저장된 포트가 5684가 아니면 힌트마다 후보가 둘이라 6번째
  힌트의 둘째 포트부터는 영영 묻지 않았다. 힌트 수집도 앞의 8개에서 멈췄다(`enrollment_locate_hints.c`): router가
  9개 이상인 네트워크에서 뒤쪽 router에 있는 허브는 후보에 들지도 못했다. `docs/power-base-management.md` 12절
  "round 사이의 진행".
  - **수정.** ① 힌트는 전부 받는다(상한 128. Thread 네트워크가 낼 수 있는 것보다 많다). ② round는 지난 round가
    마지막으로 물은 후보의 **다음부터** 잇고, 끝에 닿으면 처음으로 돈다. 그 후보는 주소와 port로 기억하므로
    목록이 늘거나 줄어도 처음으로 되돌아가지 않는다. ③ round의 마지막 요청에 허브가 답해 직접 확인할 요청이 남지
    않으면, 답이 말한 목적지를 다음 round의 첫 요청으로 묻는다(새 nonce, 다른 후보와 같은 규칙). 확인했지만
    저장에 실패한 목적지도 같다.
  - **그대로인 것.** round당 요청 12개, round 간격(무응답 poll 6·12·24·48번 뒤, 그 뒤 90번마다), multicast 없음,
    찾는 동안 flash 쓰기 없음, 인증·확인·저장의 규칙 전부. 한 round에 들어가는 목록(가정의 네트워크)은 이전과
    같은 순서로 매 round 전부 묻는다. round 사이에 RAM에 남는 것은 주소 두 개이고 어느 것도 신뢰되지 않는다.
  - **도달 상한.** 후보 C개가 그대로이면 연이은 ⌈C/10⌉ round 안에 모두 한 번 이상 묻는다. 시간으로는 12절의 표.
  - 검증 도구: `tools/test_power_base_locate.c`(검토의 재현 한 줄을 그 시나리오에 넣었고
    `scenario_progress_from_round_to_round`를 더했다), `tools/test_power_base_locate_openthread.sh`(실제 OpenThread
    core에서 router 13개: 펌웨어의 수집이 13개를 모두 내고, 마지막 router의 허브를 둘째 round에 찾는다).
- **[module 0.1.1][power_base 0.3.0, 후속 수정] 실패한 재시도 뒤에는 이전 session의 JOIN_RESULT가 새 session을
  성공시킬 수 있던 것 (2026-10-09. 코드 검토의 host 재현으로 발견. 실물에서 본 증상이 아니다).**
  `parent_service`는 "이전 session이 처리한 JOIN_RESULT의 seq"를 session의 node 기록에서 가져왔다. 결과 없이 끝난
  session(시간 초과, 세대 교체, 앞 단계 실패) 다음의 재시도는 그 기록이 비어 있어, 같은 이전 결과가 다시 오면
  그것으로 `JOIN OK`가 됐다. `docs/state-machine.md` "UPSTREAM 세대 변경과 DOWNSTREAM 재설정".
  - **수정.** 꽂혀 있는 child에게서 본 가장 새 JOIN_RESULT의 seq를 **handshake의 수명**으로 기억한다. session의
    시작·실패·시간 초과·세대 교체는 그것을 건드리지 않고, ATTACHED/DETACHED/ERROR/PEER_READY만 버린다. join
    대기뿐 아니라 session의 앞 단계와 session 밖에서 온 JOIN_RESULT도 기록한다.
  - **형식은 바꾸지 않았다.** JOIN_RESULT에는 session을 가리키는 것이 없다. 지금의 frame으로 보장되는 범위와 그
    근거(child는 최신 결과만 같은 seq로 재전송하고 새 IDENTIFY_REQ에서 그만둔다. 실제 `child_service`로 확인),
    보장되지 않는 범위(그 규칙을 따르지 않는 상대가 한 번도 보인 적 없는 이전 결과를 join 대기 중에 처음 보내는
    경우)는 `docs/state-machine.md`에 적었다.
  - **[power_base]에 미치는 것.** `components/parent_service`는 Power Base 이미지에도 들어간다. Power Base가 첫
    module을 다시 설정하는 재시도에도 같은 규칙이 적용된다.
  - 최초 연결, 같은 세대에서의 무동작, role 변경, 재시도 간격, conditioning, 처리한 결과의 재ACK는 그대로다.
  - 검증 도구: `tools/test_module_upstream_regeneration.c` 국면 10~16.

- **[module 0.1.1] Base를 삭제하고 다시 등록하면 계속 꽂혀 있던 아래 module이 돌아오지 않던 것 (2026-10-09.
  사용자가 실물에서 보고: Power Base → SGP41 → PMS5003에서 SGP만 표시되고 PMS는 뽑았다 꽂아야 나타났다.
  설치돼 있던 것은 10-03 이미지다).** 가운데 module(SGP)의 UPSTREAM은 새로 provisioning되어 새 collector와
  Dataset을 받았지만, 그 `parent_service`는 아래 module(PMS)에 대해 이전의 `JOINED`에 머물렀다. 아래 module은
  물리적으로 그대로라 DOWNSTREAM port에 event가 없었기 때문이다. PMS는 없어진 collector로 계속 보냈다.
  `docs/state-machine.md` "UPSTREAM 세대 변경과 DOWNSTREAM 재설정".
  - **수정.** `child_service`가 UPSTREAM provisioning 세대 번호를 가진다(session이 JOINED가 될 때만 오른다).
    `parent_service`는 DOWNSTREAM session을 그 번호에 묶어, 번호가 달라지면 진행 중인 session을 버리고
    꽂혀 있는 child를 처음부터 다시 provisioning한다. 번호를 비교하므로 짧은 전이를 놓치지 않고, 같은 번호
    에서는 아무것도 하지 않는다(role 변경, ROUTER 승격, 무선 detach는 번호를 바꾸지 않는다). 3단 이상에서는
    각 단계가 차례로 아래를 다시 설정한다.
  - **함께 고친 것.** ① 이전 session의 JOIN_RESULT가 다시 와도 새 session을 성공시키지 못한다(ACK만 다시
    보낸다). ② `topology_reporter`가 같은 번호로 `ATTACHED` 등록을 다시 한다(전송 재시도 중에 지나간 재설정을
    놓치던 경우). ③ 실패한 DOWNSTREAM session의 재시도 간격이 1초 고정에서 1→16초로 늘어나는 대기가 됐고,
    그 대기 중에도 task가 port event를 처리한다.
  - **[power_base]에 미치는 것.** `components/parent_service`는 Power Base 이미지에도 들어간다. Power Base는
    세대 번호를 주지 않으므로 재설정 경로는 돌지 않고, ③의 재시도 간격만 달라진다.
  - POGO 메시지의 형식·순서, child가 받는 쪽의 동작, 최초 연결, 단일 센서, PMS terminal-only 구성, conditioning,
    중복 write의 캐시 응답, JOIN_RESULT 재ACK는 그대로다.
  - 검증 도구: `tools/test_module_upstream_regeneration.c`. 실제 `pogo_port`·`role_manager`·`module_wiring`·
    `child_service`·`parent_service`·`topology_reporter`를 돌리는 module 하나(9개 국면)와, 그 module 세 개를
    프로세스 세 개로 이은 체인. 수정 전 소스에서는 실물과 같은 모양으로 실패한다.
- **[power_base 0.2.0] 자동 확인의 두 실패 경로 (2026-10-08, 같은 날의 첫 구현을 검토하다 재현. 버전은
  0.2.0 그대로다: 0.2.0은 배포된 적이 없는 개발 버전이고, 이 수정은 그 기능의 일부다. 빌드 식별자가 다르다).**
  영수증을 보내는 마지막 Thread 세션이 저장소에 네트워크를 다시 쓴 경우에 대한 것이다. 세션은 그러지 못하게
  돼 있지만(저장소 스위치), 그렇게 되지 않았을 때를 잡는 쪽에 구멍이 둘 있었다.
  1. **영수증이 저장소 확인보다 먼저 나갔다.** 저장소는 세션을 내린 뒤에야 다시 읽었으므로, 세션이 붙으면서
     Dataset을 다시 저장했고 그 뒤의 재삭제마저 실패하면 보드는 "지워지지 않음"(`RELEASED_NOT_ERASED`)인데
     허브는 이미 `completed`였다. 이제 세션이 붙은 뒤와 **보낼 때마다 그 직전에** flash를 다시 읽고, 비어
     있을 때만 보낸다. 그 경우 허브는 `released`에 머문다.
  2. **최종 record만 있으면 영수증을 내줬다.** 그 세션 중에 전원이 끊기면 다음 부팅에는 최종 record와 저장된
     Dataset이 함께 있었고, `pb_mgmt_receipt_load()`(BLE 영수증 경로가 그대로 부르는 함수)는 record만 보고
     `ERASED` 영수증을 돌려줬다. 이제 그 함수는 저장소 확인을 인자로 받아 요청마다 부르고(없으면 거부),
     네트워크가 있거나 읽지 못하면 영수증을 내주지 않는다. 부팅할 때마다 최종 record를 저장소와 대조해,
     네트워크가 있으면 지우고 read-back한 뒤 재시작한다(`pb_mgmt_client_check_final_release()`).
  - 함께 고친 것: **스위치를 켠 뒤의 재삭제가 flash에 닿지 않았다.** 세션 뒤의 재삭제를 OpenThread의 삭제로
    했는데, 그 호출의 wipe와 키 제거도 스위치가 RAM에서 답한다(실제 PSA 키 저장소로 재현: destroy는 성공,
    저장소의 키는 그대로). host test의 가짜 삭제는 항상 flash를 지웠으므로 드러나지 않았다. 스위치가 켜져
    있으면 두 저장소 자체를 지운다(`pb_stored_network_erase()`).
  - 삭제하지 않은 부팅은 "비어 있는가"를 물을 수 없다(OpenThread가 부팅마다 `0x20001`에 자기 키를 만든다).
    그런 부팅과 BLE 요청은 "네트워크가 저장돼 있는가"를 묻는다(`pb_stored_network_holds_no_network()`):
    settings 항목 없음, `0x20002`~`0x20007` 없음, 두 저장소를 읽을 수 있음. `0x20001`의 키가 부팅 때 만든
    것인지 네트워크의 것인지는 저장소로 구분할 수 없다. **이 항목의 `0x20001` 처리는 바로 아래 수정으로
    바뀌었다.**
  3. **Network Key 자리의 키를 출처와 무관하게 받아들이던 것 (2026-10-08, 위 수정의 검토에서 재현).** 위의
     "네트워크가 저장돼 있는가"는 `0x20001`에 키가 있어도 통과시켰다. OpenThread는 시작할 때 그 자리가 비어
     있을 때만 자기 키를 만들고 **있는 키는 건드리지 않으므로**, 삭제가 Network Key 하나만 남기고 실패한 뒤의
     다음 부팅은 settings도 다른 키도 없어 통과했고, 그 네트워크의 키가 남은 채 영수증을 내줬다. 이제
     - 최종 record를 가진 보드는 부팅마다 **OpenThread를 시작하기 전에** 두 저장소를 지우고 다시 읽어 비어
       있음을 확인한다(`pb_mgmt_released_prestart()`, `app_main.c`에서 `thread_manager_init()` 앞). 남아 있던
       키가 누구의 것이든 여기서 없어진다.
     - 그 뒤 `0x20001`에 대한 쓰기와 제거를 저장소 인터페이스에서 세고, 한 번의 쓰기는 내용을 RAM에 둔다.
       `0x20001`의 키는 "이 부팅이 비어 있음을 읽은 뒤 정확히 한 번 쓰였고, 지워진 적 없고, flash의 항목이 그
       쓰기와 바이트까지 같을 때"만 OpenThread의 것으로 본다. 자리로 추정하지 않는다.
     - 지우지 못했거나 읽지 못한 부팅은 영수증을 내주지 않고, OpenThread의 삭제로 한 번 더 지워 본다.
       지워진 부팅부터 영수증이 다시 나온다: 수동 "삭제 확인"은 기존 키가 실제로 없어진 뒤에만 된다.
     - 대가: 해제된 보드는 부팅마다 flash를 두 번 쓴다(이전 부팅의 키 제거, 새 키 저장).
     - 버전은 그대로 0.2.0이다(같은 이유: 배포된 적 없는 버전의 같은 기능). 프로토콜·BLE 메시지·record 형식·
       API·허브·앱은 바뀌지 않았다.
  - 프로토콜, BLE 메시지, record 형식, HTTP API, 허브 코드는 바뀌지 않았다. 허브·앱은 다시 빌드할 필요가
    없다(허브는 test만 늘었다). 정상 경로의 동작도 같다: 달라진 것은 영수증 전송 전의 읽기 몇 번뿐이다.
- **[power_base 0.2.0] 삭제 검증이 Network Key가 있는 저장소를 읽지 않던 것 (2026-10-08, 설계 조사에서
  발견).** "저장된 네트워크가 지워졌는가"를 NVS의 `openthread` namespace만 읽어 판정하고 있었다. ESP-IDF v6.1은
  OpenThread를 key reference로 빌드하므로 Network Key와 PSKc는 그 namespace가 아니라 PSA 키 저장소(NVS의
  `psa_its`)에 영속 키로 있고, 삭제는 그 키들을 결과 확인 없이 지운다. settings는 비었는데 키가 남은 보드를
  "지워졌다"고 하고 영수증을 만들 수 있었다. 이제 OpenThread의 영속 PSA 키 일곱 개가 저장소에 없는 것까지
  직접 읽어 확인한다(`components/power_base_ot_store/pb_stored_network.c`). 원격 삭제와 BOOT 버튼 로컬
  초기화가 같은 확인을 쓴다. 이 결함이 실물에서 영수증을 잘못 만든 적이 있는지는 알 수 없다(정상 경로에서는
  키가 지워진다. 확인하지 않았을 뿐이다).
- **[power_base 0.2.0] 부팅 경로의 통지가 나가지 못하던 것 (2026-10-08, 설계 조사에서 발견).** 끝나지 않은
  해제를 마치는 부팅은 서비스를 시작하지 않는데, 요청을 보내는 CoAP socket은 서비스가 시작될 때만 열렸다.
  그 부팅의 `UNENROLL_DONE`/`RELEASED` 재전송은 OpenThread가 거부해 한 번도 나가지 않았다(삭제 자체는 통지와
  무관하게 진행돼 끝났다). 통지와 영수증을 보내기 전에 socket을 연다.
- **[power_base][linux_bridge] 삭제 완료의 정의 (2026-10-05 후속).** 허브의 `completed`는 보드가
  저장된 네트워크를 지우고 다시 읽어 확인한 뒤 만든 영수증을 확인했을 때만 된다. 그 전의 통지는
  "등록 사용 중지"로만 다룬다. 차단 목록이 찼을 때 오래된 차단을 버리던 동작, 방 배정 정리가
  실패·중단되면 다시 하지 않던 동작도 고쳤다.

- **[전체] 빌드 식별이 빌드 도구의 변경을 놓치던 것 (2026-10-06, 검토에서 재현).** 소스 지문이
  빌드가 실제로 쓰는 `tools/cmake/pogo_version.cmake`와 `tools/pogo_version.py`를 빼고 있었다. 공용
  CMake 파일에 컴파일 정의를 더해도 지문·빌드 식별자가 그대로였고, clean으로 판정되어 release까지
  허용됐다. 빌드 입력을 구성요소별로 명시하고(`COMPONENT_SOURCES`), 지문·dirty 판정·빌드 전후 비교가
  한 정의(`is_input`)를 쓰게 했다. Git이 무시하지만 빌드가 읽는 파일도 dirty로 본다.
  **이 변경으로 같은 소스의 빌드 식별자가 달라진다**(아래 "호환성").
- **[전체] manifest 검증이 바이너리의 구성을 보지 않던 것 (2026-10-06, 검토에서 재현).** 일반 BLE
  이미지의 manifest를 BOOT 버튼 구성(`ble+reenroll_button_gpio9`)으로 고쳐 쓰고 그 구성을
  `--expect-variant`로 요구하면 검증이 통과했다. 이제 `primary` 바이너리를 매번 다시 읽어 variant,
  config id, dirty, 대상 칩(이미지 헤더)까지 manifest와 대조하고, `--expect-*`는 그 위에 추가로
  본다. 필수 필드가 없거나 형식이 다른 manifest는 거부한다. 파일에서 읽을 수 없는 값은 "검증하지
  않음"으로 출력한다. `manifest-create`는 variant·config id·target의 기본값을 없애고 바이너리의 값을
  쓴다.
- **[전체] 산출물의 무결성과 현재 트리와의 일치를 구분.** `manifest-verify --match-current-tree`가
  둘째 질문을 따로 답한다.

### 호환성

- **[linux_bridge 0.5.0] 이전 앱의 삭제 요청은 받아들여지지 않는다.** 이번 변경의 유일한 비호환이고, 의도한 것이다.
  - **요청**: `POST /api/v1/power-base-removals`의 본문은 `power_base_uuid`·`record`·`removal_serial`·`request_id`
    넷이다. `power_base_uuid` 하나뿐인 본문(앱 1.3.2 이하)은 `422 removal_registration_required`다. 그 앱은 "허브가
    요청을 받아들이지 않았습니다. 아무것도 삭제되지 않았습니다."를 보여 준다(그 앱의 기존 문구이고, 사실이다).
  - **조합**: 앱 1.3.3 + 허브 0.5.0은 정상. 앱 1.3.3 + 허브 0.4.2 이하: 앱이 삭제를 **제안하지 않고** 허브를
    업데이트하라고 말한다(EUI-64만 실은 요청을 보내지 않는다). 앱 1.3.2 이하 + 허브 0.5.0: 요청이 거부되고, 앱을
    업데이트해야 삭제할 수 있다. 앱 1.3.2 이하 + 허브 0.4.2 이하: 이전 그대로이고 위의 결함이 남는다. 어느 조합에서도
    작업 목록·취소·삭제 확인(영수증)·"허브 목록에서 제거"·등록·설정·센서 표시는 그대로다.
  - **추가만 된 것**: `schema_version` 1, `power_base_management` 1, `power_base_forget` 1 그대로. health에
    `power_base_removal_request`, snapshot의 Base 항목에 `removal_serial`, 작업에 `request_id`, snapshot 끝에
    `snapshot_stream`·`snapshot_sequence`. 모르는 앱은 무시한다. snapshot의 두 순서 필드가 없는 허브에는 앱이 이전처럼
    도착 순서대로 따른다(그 조합에서는 위의 표시 문제가 남는다).
  - **허브 상태 파일**: schema 2 그대로. 자격증명에 선택 키 `removal_serial`(0이면 쓰지 않는다), 작업에 선택 키
    `request_id`. 삭제를 요청한 적이 없는 허브의 파일은 `0.4.2`가 쓰는 것과 바이트까지 같다. `0.5.0`은 두 키가 없는
    파일(`0.4.2` 이하)을 그대로 읽는다(serial 0, 요청 id 없음). **이전 버전으로 되돌리면** 파일은 읽히고(두 키를
    모른다) 그 실행 파일이 다시 쓸 때 두 키가 사라진다. 그 뒤 `0.5.0`으로 돌아오면 serial은 0부터다. 네 버전의 실제
    코드로 확인했다(`docs/validation-status.md`).
  - 관리 규약(CoAP)·BLE 등록 규약·enrollment record·POGO: 그대로. 보드 펌웨어는 바꾸지 않았다
    (`tools/pogo_version.py compat`의 출력이 작업 전과 같다).
- **[linux_bridge 0.4.2] 선택 항목만 더했다.**
  - HTTP API: `schema_version` 1, health의 `power_base_forget` `1` 그대로. snapshot(GET·SSE)에 최상위 배열
    `power_base_departures`가 `power_base_forgets` 뒤에 붙는다. 앱 1.3.1 이하는 무시한다(그 조합에서는 위의 세 번째
    결함이 남는다).
  - 허브 상태 파일: schema 2 그대로. 차단 항목에 선택 키 `reason`·`record`. `0.4.2`는 그 키가 없는 파일(`0.4.1`
    이하)을 그대로 읽고, 그런 차단은 snapshot에 Base만 나온다(모르는 것을 지어내지 않는다). **이전 버전으로
    되돌리면** 파일은 읽히고 차단은 그대로이며, 그 실행 파일이 파일을 다시 쓸 때 두 키가 사라진다.
  - `UnassignModuleRooms()`와 훅의 형식은 `0.4.1`과 소스 호환이다(검토의 재현이 고치지 않고 빌드된다).
    `RoomCleanupGuard`는 함수 하나에서, 쓰기가 지나는 구간(`commit()`)을 가진 타입이 됐다. 관리는 쓰기 전에 따로
    묻지 않는다.
  - 관리 규약(CoAP), BLE 등록 규약, enrollment record, POGO 메시지: 그대로(`tools/pogo_version.py compat`의 출력이
    변경 전과 같다). 보드 펌웨어는 바꾸지 않았다.
- **[linux_bridge 0.4.1] 선택 항목만 더했다.**
  - HTTP API: `schema_version` 1 그대로, health의 `power_base_forget`도 `1` 그대로(경로와 요청·응답 형식은
    바뀌지 않았다). 삭제 작업에 `"list_removed": true`가 **그럴 때만** 붙는다. snapshot과 SSE는 형식이 같고
    내용이 한 시점의 것이 됐다. 앱 1.3.0 이하는 모르는 필드를 무시한다.
  - 허브 상태 파일: schema 2 그대로. 선택 목록 `members`와, 정리 항목의 선택 필드 `rooms`가 늘었다. `0.4.1`은
    `0.4.0` 이하의 파일을 그대로 읽는다(없음 = 모름: 그런 Base·정리는 이전처럼 다뤄지고, 모르는 것을 추정해
    지우지 않는다). **`0.4.0`/`0.3.0`으로 되돌리면** 파일은 읽히고(둘 다 무시) 차단·자격증명·작업은 그대로지만,
    그 실행 파일이 파일을 다시 쓸 때 두 항목이 사라지고 남은 정리는 그 버전의 방식으로 실행된다. 다시 쓰지
    않은 채 `0.4.1`로 돌아오면 `members`가 그동안의 모듈 이동을 반영하지 않는다(시작 유예와 정리 때의 질문으로만
    걸러진다). 두 버전의 실제 코드로 확인했다: `docs/power-base-management.md` 14절 "저장 형식과 되돌리기".
  - 관리 규약(CoAP), BLE 등록 규약, enrollment record, POGO 메시지: 그대로(`tools/pogo_version.py compat`의 출력이
    변경 전과 같다). 보드 펌웨어는 바꾸지 않았고 다시 기록할 필요가 없다.

- **[linux_bridge 0.4.0] 추가만 했다.**
  - HTTP API: `schema_version` 1 그대로. health의 `power_base_forget`, snapshot의 `record`·`power_base_forgets`,
    경로 `/api/v1/power-base-forgets`가 늘었다. 앱 1.2.x는 모르는 필드를 무시하고 그 경로를 쓰지 않는다. 앱 1.3.0은
    그 필드가 없는 허브(0.3.0 이하)에 이 기능을 보여 주지도 묻지도 않는다.
  - 관리 규약(CoAP), BLE 등록 규약, enrollment record, POGO 메시지: 그대로(`tools/pogo_version.py compat`의 출력이
    변경 전과 같다). 보드 펌웨어는 바꾸지 않았고 다시 기록할 필요가 없다.
  - 허브 상태 파일: schema 2 그대로이고 0.4.0은 이전 파일을 그대로 읽는다. **0.3.0 이하로 되돌리면**: 0.4.0이
    쓴 파일은 읽히고 차단은 유지되지만, 그 실행 파일이 파일을 다시 쓸 때 `forgets` 기록이 사라지고, `sent`·
    `accepted` 작업을 남긴 채 제거한 Base는 목록에 다시 보인다(데이터는 여전히 거절). 두 버전의 실제 코드로
    확인했다. 같은 상태로 돌아가는 되돌리기가 아니다: `docs/power-base-management.md` 13절 "저장".
- **2026-10-09의 후속 수정 두 건은 다른 구성요소가 보는 것을 바꾸지 않는다.**
  - POGO 프레임·메시지, 관리 규약 번호와 메시지(`LOCATE`/`LOCATE_ACK` 포함)의 바이트와 golden MAC, capability
    bit, BLE 등록 규약, HTTP API, record v1·v2·v3, 허브 상태 파일 schema: 모두 그대로다
    (`tools/pogo_version.py compat`의 출력이 수정 전과 같다).
  - 버전도 그대로다: `power_base` 0.3.0, `module` 0.1.1, `linux_bridge` 0.3.0, `rcp` 0.1.0. 이유는 이 파일 머리의
    "후속 수정 두 건" 단락.
  - **`linux_bridge`의 소스는 바뀌지 않았다.** bridge의 지문이 달라진 것은 `components/`가 그 빌드 입력이기도
    해서다(바뀐 `pb_mgmt_client.c`·`parent_service.c`는 bridge 실행 파일에 들어가지 않는다. bridge의 host test만
    `pb_mgmt_client.c`를 link한다). `rcp`의 지문은 그대로다.
  - 신·구 조합은 앞 항목들과 같다. module 0.1.1(후속)과 0.1.1(앞 후보)·0.1.0은 어느 순서로 이어도 된다.
  - 저장소 안에서만 달라진 약속: `pb_mgmt_ops_t.locate_hints`는 "앞의 몇 개"가 아니라 "가진 것 전부를 같은
    순서로"를 뜻하고 `PB_MGMT_LOCATE_MAX_HINTS`는 8에서 128이 됐다. 구현은 `power_base/main` 하나다.
  - **설치.** 이전과 같다: Power Base와 module 모두 app 파티션만, cfg·NVS·Dataset·등록 record 그대로.

- **주소·포트 변경 복구(Power Base 0.3.0, Linux bridge 0.3.0)는 추가다. 이전 상대와 계속 동작한다.**
  - **바뀌지 않은 것.** 관리 규약 번호 `1`과 기존 여덟 메시지의 바이트·뜻(golden MAC 그대로), BLE 등록 규약과
    그 메시지·capability, 영속 등록 record v1·v2·v3의 형식과 부팅 규칙, 재등록 journal, HTTP API(경로·필드·
    오류 코드·`schema_version`), 허브 상태 파일의 schema `2`, POGO 프레임과 메시지.
  - **더한 것.** 관리 메시지 type `0x06`·`0x86`, capability bit 두 개, 허브의 locate 포트(`--port`가 5684가
    아닐 때만 생기는 두 번째 socket). `tools/pogo_version.py compat`의 `mgmt_messages`·`mgmt_capabilities`가
    그만큼 늘었다(도구는 바꾸지 않았다: 소스에서 읽는다. 그 목록을 적어 둔 `tools/test_pogo_version.py`의 기대값 한 곳만
    새 목록으로 고쳤다).
  - **신·구 조합.** 보드 0.3.0 + 허브 0.2.0 이하: 평소 동작은 같다. 허브가 옮겨 가면 보드의 `LOCATE`를 이전
    허브가 모르는 type으로 거부하고(`4.00`), 보드는 아무것도 바꾸지 않는다(복구는 이전처럼 재등록). 보드
    0.2.0 이하 + 허브 0.3.0: 보드가 묻지 않는다. 이전과 같다. 앱은 어느 조합에도 관여하지 않으므로 앱
    1.2.1 (10)·(11)과 그 이전 모두 그대로 쓴다.
  - **구형 record.** record v1·v2(관리 키 없음)의 보드는 0.3.0 펌웨어에서도 찾지 않고, record는 읽기만 한다.
    그런 보드의 허브가 옮겨 가면 재등록이 필요하다. v3 record는 형식이 같고 목적지의 값만 바뀌므로 0.2.0
    펌웨어로 되돌려도 읽힌다.
  - **설치.** Power Base는 app 파티션만 갱신한다(cfg·NVS·Dataset·등록 record를 건드리지 않는다). 전체 erase가
    필요 없다. 허브는 bridge 실행 파일만 바꾸고 `<state-dir>`는 그대로 둔다. helper·sudoers·systemd 단위는
    바꿀 필요가 없다(새 권한 없음).
- **`module` 0.1.1은 형식을 바꾸지 않는다.** 0.1.0 module과 0.1.1 module은 어느 순서로 이어도 provisioning된다.
  체인이 재등록 뒤 스스로 돌아오려면 **가운데 있는 module**(아래에 무엇인가 꽂힌 module)이 0.1.1이어야 한다:
  Power Base → SGP41 → PMS5003이라면 SGP41이다. 맨 끝 module(PMS5003)은 0.1.0이어도 다시 설정된다(받는 쪽
  동작은 원래 있었다). Power Base 0.2.0·0.3.0 어느 쪽과도 동작한다. module도 app 파티션만 갱신하고 cfg(센서·
  배선 설정)는 그대로 둔다.
- **자동 삭제 확인(Power Base 0.2.0, Linux bridge 0.2.0)은 추가다. 이전 상대와 계속 동작한다.**
  - **바뀌지 않은 것.** 관리 프로토콜 번호 `1`과 기존 여섯 메시지의 바이트·뜻(golden MAC 그대로), BLE 등록
    프로토콜 `1`과 그 메시지·capability(삭제 영수증 `0x07`, `release_receipt_v1` 포함), 영속 등록 레코드
    v1·v2·v3의 형식과 부팅 규칙(새 flag나 필드 없음), 재등록 journal, HTTP API의 경로·요청·기존 필드와
    `schema_version`, 허브 상태 파일의 schema `2`.
  - **더한 것.** 관리 메시지 type `0x05`·`0x85`와 capability bit 두 개; 삭제 작업과 `power_bases[]`의 선택
    필드 `erase_confirmation`; 허브 상태 파일의 선택 필드 두 개(자격증명의 `board_capabilities`, 완료된 작업의
    `receipt_ack_mac`). `tools/pogo_version.py compat`에 `mgmt_messages`·`mgmt_capabilities`가 생겼다(manifest의
    `compatibility`에도 들어간다. 이전 manifest는 그대로 유효하다).
  - **신·구 조합.** 보드 0.2.0 + 허브 0.1.0: 이전 허브는 `HELLO`를 모르는 type으로 거부하고(보드는 그 부팅에서
    다시 보내지 않는다), 관리 resource로 온 영수증은 **이전 허브도 검증해 완료한다**. 같은 영수증이 다시 오면
    키가 없어 거부한다. 보드 0.1.0 + 허브 0.2.0: 보드가 알리지 않으므로 작업은 `manual`이고 이전처럼 BLE
    영수증으로 완료한다. 이전 앱은 새 필드를 읽지 않는다. 표와 근거: `docs/power-base-management.md` 2절.
  - **마이그레이션은 없다.** 0.1.0으로 등록된 보드(레코드 v3)는 0.2.0 펌웨어에서 그대로 부팅·복원되고, 다음
    삭제부터 영수증을 스스로 보낸다. 다시 등록할 필요가 없다. 허브는 이전 상태 파일을 그대로 읽는다. 0.2.0이
    쓴 상태 파일을 0.1.0 허브가 읽으면 두 선택 필드를 무시한다(다음 저장에서 사라진다).
  - **달라지는 동작.** ① 삭제 검증이 더 엄격하다(위 "수정"의 첫 항목): OpenThread의 PSA 키가 남아 있으면
    영수증을 만들지 않고 다음 부팅에 다시 지운다. 원격 삭제와 로컬 초기화 모두. ② 삭제를 끝낸 보드가
    재시작하기 전에 영수증 전달 세션(최대 약 90초)을 거친다. 그동안 BLE 등록은 열리지 않는다. ③ 버전
    문자열로 기능을 켜거나 끄는 곳은 여전히 없다.
  - `module`과 `rcp`는 기능이 바뀌지 않았고 `0.1.0` 그대로다. 다만 `versions.json`과 빌드 도구
    (`tools/pogo_version.py`)가 바뀌었으므로 **네 구성요소 모두 소스 지문과 빌드 식별자가 달라진다**(`module`은
    `components/` 전체가 입력이기도 하다). `power_base`만 새 component와 link 옵션을 쓴다.
- **설정 코드 QR은 펌웨어·허브와 무관하다.** QR은 사람이 옮겨 적던 26자리 설정 코드를 앱의 입력
  칸에 넣는 방법일 뿐이다. 보드의 인증(Security 2, 설정 코드 5회 잠금), BLE 메시지와 capability,
  HTTP API, 영속 레코드는 바뀌지 않았고 `tools/pogo_version.py compat`의 값도 그대로다. QR 형식 버전
  `1`은 그 어느 것과도 별개의 숫자다. 생성 도구·test·벡터는 빌드 입력이 아니어서 네 구성요소의 소스
  지문과 빌드 식별자가 이 추가로 달라지지 않는다. QR의 EUI-64는 인증 증거가 아니며, 인증 뒤 보드가
  말하는 EUI-64를 앱이 다시 확인하는 절차는 그대로 필요하다.
- 버전 체계를 넣으면서 HTTP API, BLE 메시지, 영속 레코드 형식, Matter 동작은 **바꾸지 않았다.**
  health의 `build`는 선택 필드 하나를 추가한 것이고 `schema_version`은 `1` 그대로다. 관리·삭제
  기능이 꺼진 bridge의 health는 이전과 같은 모양이다.
- app description의 `version` 문자열이 `git describe` 결과에서 `0.1.0`으로 바뀐다. 이 문자열을
  읽어 동작을 정하는 코드는 이 저장소와 앱에 없다.
- BLE 등록 프로토콜 번호는 `1` 그대로다. 새 메시지와 capability는 추가이며, 앱은 capability로
  지원 여부를 판단한다. 버전 문자열을 비교해 기능이나 보안을 판단하는 곳은 없다.
- 레코드 v1/v2로 등록된 기존 보드는 계속 부팅·복원된다. 다만 원격 삭제는 v3로 다시 등록한 보드만
  지원한다.
- **빌드 식별자의 계산 범위가 바뀌었다(제품 버전은 그대로 `0.1.0`).** 같은 소스라도 2026-10-06의 첫
  구현으로 만든 산출물(`~/esp-builds/pogover_20261006_*`)과 지금 만드는 산출물은 지문과 빌드 식별자가
  다르다. 이전 산출물과 그 manifest는 고치지 않았고, 새 도구로도 **자체 무결성 검증은 통과**한다.
  현재 트리와의 일치(`--match-current-tree`)는 통과하지 않는다. 이전 규칙으로 만든 `SOURCE_INFO.json`은
  더 이상 유효한 기준선이 아니다(commit `unknown`으로 처리). manifest 스키마 번호는 `1` 그대로이며
  새 manifest에는 `binary_says.image`가 추가로 들어간다.
- `manifest-create`의 명령행이 바뀌었다: `--primary`는 필수이고, `--variant`의 기본값 `default`와
  `--target`의 구성요소별 기본값이 없어졌다(바이너리에서 읽는다). 이 도구는 아직 배포된 적이 없다.
- `tools/pogo_version.py compat`이 소스에서 읽은 현재 값: HTTP API `pogohome-api-v1`, BLE 등록
  프로토콜 `1`, BLE 메시지 `0x01`–`0x04`·`0x06`–`0x09`, capability `bridge_endpoint_v1`·
  `local_reset_v1`·`mgmt_v1`·`release_receipt_v1`, 등록 레코드 버전 `1`·`2`·`3`, 관리 프로토콜 `1`,
  관리 메시지 `0x01`–`0x03`·`0x05`·`0x81`–`0x83`·`0x85`, 관리 capability `board_cap_thread_receipt`·
  `hub_cap_thread_receipt`, 재등록 journal 형식 `1`.

### 검증 한계

- **삭제 요청의 등록 결속과 snapshot 순서(`linux_bridge` 0.5.0)는 실물로도 Linux에서도 확인하지 않았다.** 근거는 Mac의
  host test — 실제 관리 구현과 서명된 관리 메시지로 "새 등록에 UNENROLL이 나가지 않는다"까지, 실제 TLS·HTTP 경로로
  같은 본문의 재전송까지 — 와 검토가 남긴 재현이다. 검토의 삭제 재현 원본은 EUI-64만 싣는 호출을 쓰므로 새 계약에서
  컴파일되지 않는다: 원본을 그대로 두고, 같은 순서를 새 요청으로 보내는 adapter로 확인했다(그 사실과 차이를 적었다).
  두 기기는 두 호출자이고, "10분 뒤"는 test의 시계이며, 응답 유실은 응답을 버린 것이다. **남긴 것**: 이전 앱과 이전
  허브의 조합에는 결함이 그대로 있다. 등록을 시작만 한 Base(자격증명이 발급되고 아직 active가 아님)의 삭제 요청은
  이전처럼 active인 등록의 작업을 만든다. stream이 바뀌는 순간(허브 재시작)에 앱이 더 새로운 snapshot 하나를 버릴 수
  있다(다음 요청으로 따라잡는다). 기록: `docs/validation-status.md` "삭제 요청의 등록 결속과 snapshot 순서
  (2026-10-11)".
- **두 번째 후속 수정(`linux_bridge` 0.4.2)도 실물로도 Linux에서도 확인하지 않았다.** 근거는 Mac의 host test와
  검토가 남긴 재현(원본 그대로 빌드되고, 수정 전 실패 → 수정 후 통과)이다. 순서는 주입한 것이고, "16분 뒤"는 test가
  관리의 시계를 옮긴 것이며, 판단과 쓰기 사이에 보고가 끼어들 수 없다는 것은 두 스레드로 확인했다(부하가 크면 그
  검사는 결함을 놓칠 수는 있어도 거짓으로 실패하지는 않는다). 구간이 잡혀 있는 동안의 대기 시간은 실제 저장 장치에서
  재지 않았다. 기록: `docs/validation-status.md` "목록 제거 후속 수정 2 (2026-10-10)". **남긴 것**: `0.4.1` 이하의
  허브에 연결된 앱에는 일반 삭제를 놓친 기기의 옛 센서가 남는다. `list_removed` 작업과 `members`의 한계는 `0.4.1`
  그대로다.
- **목록 제거의 후속 수정(`linux_bridge` 0.4.1)은 실물로도 Linux에서도 확인하지 않았다.** 근거는 Mac의 host
  test — 실제 `PowerBaseManagement`·`ingest::Registry`·`SettingsStore`·HTTP 서버(GET과 SSE의 실제 경로에 "Registry
  복사 직후"의 순서를 끼워 넣음) — 와, 검토가 남긴 두 재현(허브 쪽은 훅 형식 변경 때문에 두 줄짜리 adapter로)이다.
  경쟁 순서는 결정적으로 주입한 것이고 실제 두 기기·실제 네트워크 타이밍으로 만든 것이 아니다. Linux의 GN/CHIP
  빌드, 운영 허브, 실제 보드·Thread, 실제 재시작과 전원 차단은 실행하지 않았다. 기록:
  `docs/validation-status.md` "목록 제거 후속 수정 (2026-10-10)". **남긴 것**: 끝난 삭제 작업(`completed`)의 기록은
  여전히 10분 뒤 사라진다. 마지막 Power Base를 한 기기가 삭제하는 동안 10분 넘게 백그라운드에 있던 다른 기기의 앱은
  돌아와서 옛 센서를 "복구 중"으로 보여 준다(앱을 다시 실행하면 사라진다). 이번에 손대지 않았다.

- **허브 목록에서 제거(`linux_bridge` 0.4.0)는 실물로 확인하지 않았다.** 근거는 Mac의 host test다: 실제
  `PowerBaseManagement`·`ingest::Registry`·HTTP 서버(TLS loopback)와, 보드 쪽은 실제 펌웨어 client 코드. Linux의
  GN/CHIP 빌드, 운영 허브, 실제 보드·Thread, 실제 Matter controller에서의 endpoint 제거는 실행하지 않았다.
  시각과 저장 실패는 주입한 것이고, 실제 NTP 변경이나 전원 차단을 시험한 것이 아니다. 허브가 재시작된 뒤 한 번도
  다시 들리지 않은 Base를 제거하면 방 지정 정리를 하지 못한다(허브가 그 Base의 모듈을 모른다). 기록:
  `docs/validation-status.md` "허브 목록에서 제거 (2026-10-10)".
- **2026-10-09의 후속 수정 두 건도 실물로 확인하지 않았다.** 두 결함은 코드 검토가 host에서 재현한 것이고 실물
  실패가 아니다. 수정 전 실패와 수정 후 통과는 검토의 재현 소스를 고치지 않고 현재 소스에 연결해 확인했다
  (`docs/validation-status.md` "후속 수정: 탐색 누락과 이전 JOIN_RESULT (2026-10-09)").
  - 주소 복구의 "router가 많은 네트워크"는 실제 OpenThread core의 시뮬레이션(router 13개)까지다. 실제 무선에서
    router가 그만큼 있는 네트워크, 그 안에서의 round 소요 시간은 보지 않았다. 128개 상한은 Thread의 한도에서
    계산한 것이고 그런 네트워크를 만든 것이 아니다.
  - 센서 체인의 이전 JOIN_RESULT는 대본 보드가 만든 지연·반복이다. 실제 UART에서 그런 순서가 생기는지는
    보지 않았다. `child_service`가 그렇게 보내지 않는다는 것은 실제 코드(host)로 확인했다.
  - 실행하지 못한 것은 아래 항목과 같다: 실제 보드, 실제 OTBR·`wpan0`, Linux GN/CHIP 빌드, 운영 허브.

- **2026-10-09의 두 변경(주소·포트 변경 복구, 센서 체인 재등록 복구)은 실물로 확인하지 않았다.** 확인한 것은
  Mac의 host test와 펌웨어·Linux host 빌드다(`docs/validation-status.md` "주소 복구와 센서 체인 재등록 복구
  (2026-10-09)").
  - 주소 복구는 ESP-IDF가 펌웨어에 넣는 OpenThread core를 Mac에서 실행한 재현까지 했다(힌트 수집, locator로
    보낸 요청의 왕복, OTBR 교체 뒤의 이전). **그 재현의 허브는 Linux가 아니다.** 실행하지 못한 것: 실제 보드
    (ESP-IDF port, radio), 실제 OTBR의 `wpan0`에서 EID를 고르는 것, locator 주소로 온 datagram이 Linux의 bridge
    socket에 닿고 답이 보드로 돌아가는 것, Linux용 `pogo_matter_bridge`(GN/CHIP) 빌드, 운영 허브. OTBR이 주소를
    인터페이스에 올리는 방식은 OpenThread 소스로만 확인했다.
  - 센서 체인에서 실행하지 못한 것: 실제 OpenThread의 join(host test의 Thread는 곧바로 성공하는 대역이다),
    실제 PMS5003·SGP41, 실제 Power Base 펌웨어와의 조합(Base 쪽은 같은 packer로 만든 대본이다).
  - 수정 전 결함의 원인은 실제 코드의 재현(수정 전 실패 → 수정 후 통과)으로 확인했다. 그것이 사용자가 본
    증상의 유일한 원인이라는 것은 실물에서 새 이미지로 확인해야 한다.
- (2026-10-09 추가. 바로 아래 항목은 2026-10-08에 적은 그대로 둔다. 그 뒤 사용자가 그때 설치돼 있던 조합
  (Power Base 0.2.0·Linux bridge 0.2.0)으로 자동 삭제의 실물 시험을 했고 결과를 알려 왔다. 그 근거와 한계는
  `docs/validation-status.md` "주소 복구와 센서 체인 재등록 복구 (2026-10-09)"의 "이전 기록의 판정"에 있다. 그
  시험은 0.3.0·0.1.1의 시험이 아니다.)
- **자동 삭제 확인(2026-10-08)은 실물로 확인하지 않았다. 실물에서 자동 삭제가 끝까지 되는지는 아직 모른다.**
  확인한 것은 다음이다(`docs/validation-status.md` "Power Base 자동 삭제 확인 검증 (2026-10-08)").
  - host test: 보드의 관리 코드와 허브의 관리 코드를 실제 코드로 서로 붙여, 정상 완료·응답 유실·거부·위조·
    중복·다른 세대·재등록 뒤의 재도착·각 경계의 전원 차단을 실행했다. 저장소 스위치와 read-back도 host
    test다.
  - **ESP-IDF v6.1이 펌웨어에 넣는 바로 그 OpenThread core와 PSA 키 저장소(TF-PSA-Crypto)를 Mac에서
    실행한 재현**: 지운 뒤 다시 붙으면 Dataset이 다시 저장되고 재시작 뒤 그 네트워크에 붙는다는 것, 같은
    extended address로는 붙지 못한다는 것, 스위치를 켜면 붙어서 CoAP를 주고받는 동안 저장소가 비어 있다는
    것. OpenThread 재현은 key reference가 없는 구성이고(Nexus 시뮬레이션), PSA 재현은 OpenThread 없이 같은
    PSA 호출을 직접 한 것이다. **둘을 합친 실제 구성(ESP-IDF의 settings port, NVS, radio)은 실행하지 않았다.**
  - link된 Power Base 이미지 네 구성에서, 아홉 저장소 함수의 호출이 모두 스위치를 지난다는 것(disassembly).
  - 하지 않은 것: 보드에 기록, 실제 Thread 네트워크에서의 재attach(부모가 받아 주는지, 걸리는 시간, 다중 hop),
    실제 flash에서 세션 전후 NVS가 비어 있는지, **새 PSA 키 read-back이 실제 보드에서 통과하는지**(통과하지
    못하면 삭제가 `삭제 대기`에 머물고 영수증이 만들어지지 않는다), 실제 전원 차단, 운영 Linux 허브(GN/CHIP
    빌드 포함), 실제 iPhone.
  - 같은 날의 수정(위 "수정"의 첫 항목)도 host에서만 확인했다. 특히 **해제된 보드가 재시작한 뒤 "네트워크
    없음"으로 읽히는지**는 실물로 보지 못했다: 그렇게 읽히지 않으면 해제된 보드가 BLE로 영수증을 내주지
    않아 수동 "삭제 확인"이 "영수증 없음"으로 끝난다(지운 것으로 잘못 보고하지는 않는다). 그렇게 읽히려면
    실물에서 (1) OpenThread 시작 전의 직접 삭제(`nvs_erase_all`, `psa_its_remove`)가 되고, (2) OpenThread가
    시작하며 `0x20001`에 정확히 한 번 쓰고, (3) 그 항목을 다시 읽은 것이 쓴 것과 같아야 한다. 근거는 ESP-IDF의
    OpenThread·PSA 소스와, 그 PSA 키 저장소 core를 돌린 재현이다. ESP-IDF의 `esp_psa_its.c`·NVS, 시작 전
    시점의 그 호출들, BLE 요청이 NimBLE host task에서 저장소를 읽는 것의 stack 여유는 실행하지 않았다.
- **설정 코드 QR은 실물로 확인하지 않았다.** 생성 도구는 Mac의 host test로만 확인했다(공개된 가짜
  EUI-64·가짜 코드로 만든 PNG를 Vision과 Core Image 검출기로 다시 읽기, 파일 권한과 거부 경로).
  실제 보드의 설정 코드 파일로 QR을 만들지 않았고, 인쇄하거나 화면에 띄운 QR을 실제 카메라로 읽지
  않았으며, 그 코드로 실제 보드를 등록·재등록·삭제 확인하지 않았다. 다른 사용자 소유의 파일을
  거부하는 경로는 root 없이 시험할 수 없어 실행하지 않았다. `docs/validation-status.md` "QR 설정 코드
  입력 검증 (2026-10-06)".
- 위의 추가·수정 가운데 **실물로 확인한 것은 없다.** host test, host harness 상호운용 시험, iOS
  시뮬레이터·Mac Catalyst test, 빌드로만 확인했다(`docs/validation-status.md`의 2026-10-05,
  2026-10-06 절).
- 실행하지 않은 것: 보드 기록과 부팅 로그, 실제 BLE 광고·연결, 물리 버튼, 실제 전원 차단, 새 Thread
  네트워크로의 이동, 운영 Linux 허브의 등록·삭제와 health 응답, Linux 장비의 GN/CHIP 빌드와
  `pogo_matter_bridge --version`.
- BOOT 버튼의 GPIO9 배선은 확인하지 않은 하드웨어 가정이다.
- 이 체계의 설치·실물 시험 기록은 아직 한 건도 없다.
- 빌드 식별 보완(위 "수정"의 두 항목)은 도구 host test, 실제 RCP 증분 빌드, 펌웨어 7개 구성과 Linux
  API host 빌드의 재빌드, 이전·새 산출물의 manifest 검증으로 확인했다. GN/CHIP 빌드의 stamp 단계와
  실물은 여전히 실행하지 않았다. manifest가 **검증하지 않는** 값(`build.configuration`, ESP-IDF 외의
  toolchain, `compatibility`, `source.origin` 등)은 manifest가 적은 그대로다.

## 이 체계 이전

2026-10-06 이전의 커밋과 산출물에는 구성요소 버전이 없다. 소급해서 붙이지 않는다.

- 커밋 이력은 Git에 있다. 태그 `mvp-pairing-v1`은 이 규칙 이전의 것이며 그대로 둔다.
- 그때 만든 이미지의 식별값(실제 SHA-256과 빌드 당시 문자열)은
  [`docs/release-records/legacy-artifacts.md`](docs/release-records/legacy-artifacts.md)에 있다.
  분류는 "기존 버전 표기 / 명시 버전 미관리"다.
- 그 시기의 실물 검증 기록은 `docs/validation-status.md`에 날짜별로 있다.
