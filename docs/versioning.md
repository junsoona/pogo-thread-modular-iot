# 버전 관리와 빌드 식별

이 문서는 "지금 이 보드·허브·앱에 들어 있는 것이 어떤 소스에서 나온 무엇인가"를 답할 수 있게 하는
규칙과 도구를 설명한다. 대상은 이 저장소의 네 구성요소(Power Base, 센서 모듈, OTBR RCP, Linux
bridge)이고, 앱 저장소(`pogo-sensor-ios`)는 같은 규칙을 자기 문서(`docs/versioning.md`)에 둔다.

체계는 2026-10-06에 도입했다. 그 전에 만든 산출물에는 소급 적용하지 않는다
([과거 산출물](#과거-산출물)).

## 서로 다른 다섯 가지

한 문자열에 섞지 않는다.

| 이름 | 예 | 누가 정하나 | 뜻 |
|---|---|---|---|
| 제품 버전 | `0.1.0` | 사람이 `versions.json`을 고친다 | 구성요소가 무엇을 하는지에 대한 약속 |
| 빌드 식별자 | `590e2a11d2cb+dirty.bb2b25c9dc29` | 도구가 계산한다 | 정확히 어떤 소스로 만들었는가 |
| 소스 식별 정보 | commit 전체 SHA, clean/dirty, 소스 지문 | 도구가 계산한다 | 빌드 식별자의 근거 |
| 빌드 종류 | `development` / `release` | 빌드할 때 명시한다 | 배포 후보로 만들었는가 |
| 빌드 구성 | `ble`, `bench`, `diag`, Xcode의 Debug/Release | 빌드 설정 | 어떤 옵션으로 컴파일했는가 |

- **빌드 구성은 빌드 종류가 아니다.** Xcode의 Release 구성이나 ESP-IDF의 최적화 설정으로 만든
  산출물도, 커밋하지 않은 소스에서 나왔거나 release 모드로 만들지 않았으면 `development`다.
- **제품 버전은 자동으로 오르지 않는다.** 컴파일할 때 버전 파일이나 프로젝트 파일을 고쳐 쓰는
  단계는 없다. 버전은 사람이 한 파일을 고쳐서만 바뀐다.
- **버전 문자열로 기능이나 보안을 판단하지 않는다.** [호환성](#호환성-정책) 참고.

## 버전의 단일 원본

```
versions.json            이 저장소의 네 구성요소
```

```json
{
  "schema": 1,
  "components": {
    "power_base":   { "version": "0.3.0" },
    "module":       { "version": "0.1.1" },
    "rcp":          { "version": "0.1.0" },
    "linux_bridge": { "version": "0.3.0" }
  }
}
```

- 네 구성요소는 **각자** 버전을 가진다. 한쪽만 올릴 수 있다.
- 형식은 `MAJOR.MINOR.PATCH`, 세 개의 정수뿐이다. `dev`, `-rc1`, Git SHA, 앞자리 0은
  `tools/pogo_version.py check`가 거부한다. 개발 중이라는 사실과 소스 위치는 버전이 아니라 빌드
  종류와 빌드 식별자가 말한다.
- `0.1.0`은 **이 체계를 시작한 개발 기준선**이다. 그 전의 산출물이 0.1.0이었다는 뜻도, 0.1.0이
  실물에서 검증됐다는 뜻도 아니다. 2026-10-06 현재 이 버전으로 만든 산출물은 모두 개발 빌드다.
- 2026-10-08에 Power Base 삭제의 자동 확인([`power-base-management.md`](power-base-management.md))을 넣으면서
  그 기능을 가진 `power_base`와 `linux_bridge`만 `0.2.0`으로 올렸다(이전 상대와 계속 동작하는 기능 추가:
  MINOR). `module`과 `rcp`는 기능이 바뀌지 않아 `0.1.0` 그대로다. 다만 `versions.json`은 네 구성요소 모두의
  빌드 입력이므로 그 둘의 소스 지문과 빌드 식별자도 달라졌다: 버전이 같다고 같은 빌드가 아니고, 빌드
  식별자가 달라졌다고 기능이 바뀐 것도 아니다. `0.2.0`도 개발 빌드만 있는 버전이다. 0.1.0으로 만든 이전
  산출물과 그 기록에 0.2.0을 붙이지 않는다.
- 같은 날 그 기능의 두 실패 경로를 고쳤고([`validation-status.md`](validation-status.md)의 "두 실패 경로
  수정"), **버전은 올리지 않았다.** `0.2.0`은 배포된 적도 보드에 기록된 적도 없는 버전이고, 고친 것은 그
  기능의 일부이며, 다른 구성요소가 보는 동작과 형식은 그대로다. 같은 날의 두 번째 수정(Network Key 자리의
  키의 출처)도 같은 이유로 `0.2.0` 안에서 했다. 그래서 `0.2.0`이라고 말하는 산출물이 세 묶음 있다:
  `pogoauto_20261008_*`와 `pogoautofix_20261008_*`(둘 다 쓰지 않는다. 뒤의 수정이 들어 있지 않다), 그리고
  `pogoautokey_20261008_*`. 가르는 것은 빌드 식별자다. 버전만 보고 산출물을 고르지 않는다(아래 "빌드
  manifest"의 "검증").
- 2026-10-11에 `linux_bridge`를 **0.5.0**으로 올렸다: 아래 0.4.2를 독립 검토해 나온 둘의 수정
  ([`power-base-management.md`](power-base-management.md) 16절: 삭제 요청을 확인 당시의 등록에 묶고, snapshot에
  순서를 싣는다). 이번에는 **MINOR**다. 앞의 두 번(0.4.1, 0.4.2)은 선택 항목의 추가뿐이라 PATCH로 뒀지만, 이번에는
  이전 클라이언트의 요청 하나가 **더는 받아들여지지 않는다**: `POST /api/v1/power-base-removals`의 본문이 멤버 하나에서
  넷으로 바뀌었고, EUI-64만 실은 이전 형식은 `422`로 거부된다. 아래 표("언제 올리나")의 "이전 상대와 더는 동작하지
  않는 변경"이고, `0.y.z` 동안 그런 변경은 MINOR로 올리고 CHANGELOG의 "호환성"에 적는다는 규칙 그대로다. 컴파일했다는
  이유로 올린 것이 아니고, 0.4.2의 산출물에 0.5.0을 붙이지 않는다. `power_base`·`module`·`rcp`는 소스가 바뀌지 않아
  그대로다(빌드 식별자만 달라진다). 0.5.0도 개발 빌드만 있다.
- 2026-10-10에 `linux_bridge`를 **0.4.2**로 올렸다: 아래 0.4.1을 다시 독립 검토해 남은 결함 둘의 수정
  ([`power-base-management.md`](power-base-management.md) 15절: 방 지정 정리의 판단과 쓰기를 한 구간으로, 일반 삭제를
  통째로 놓친 기기를 위한 `power_base_departures`). 0.4.1과 같은 이유로 PATCH다: 0.4.1도 설치된 적이 없는 후보이고,
  이 변경은 그 후보의 결함 수정이며 새 사용자 기능이 없다. 더해진 형식(snapshot의 최상위 배열 하나, 상태 파일의 차단
  항목에 선택 키 둘)은 없으면 0.4.1과 같은 동작이고 이전 앱·이전 허브가 모르는 채 동작한다. 표("언제 올리나")의
  글자대로는 새 API 필드가 MINOR 쪽이라는 점은 0.4.1 때와 같고, 같은 판단을 다시 했다. `power_base`·`module`·`rcp`는
  그대로다(빌드 식별자만 달라진다). 0.4.2도 개발 빌드만 있다.
- 2026-10-10에 `linux_bridge`를 **0.4.1**로 올렸다: 아래 0.4.0의 독립 검토에서 재현된 두 결함의 수정과 남은 구현의
  정리([`power-base-management.md`](power-base-management.md) 14절). 새 사용자 기능이 없고, 더해진 형식(삭제 작업의
  `list_removed`, 상태 파일의 `members`와 정리 항목의 `rooms`)은 모두 선택 항목이며 없으면 0.4.0과 같은 동작이다.
  그래서 PATCH로 뒀다. 아래 표("언제 올리나")를 글자 그대로 읽으면 새 API 필드의 추가는 MINOR 쪽이라는 점을 알고
  한 판단이다: 0.4.0은 설치된 적이 없는 후보이고, 이 변경은 그 후보의 결함 수정이다. `power_base`·`module`·`rcp`는
  소스가 바뀌지 않아 그대로이고, `versions.json`이 빌드 입력이라 빌드 식별자만 달라진다. 0.4.1도 개발 빌드만 있다.
- 2026-10-10에 `linux_bridge`를 **0.4.0**으로 올렸다: 돌아오지 않는 Power Base를 허브의 목록에서 지우는 기능
  ([`power-base-management.md`](power-base-management.md) 13절). HTTP API에 경로와 필드를 **추가**했고(이전 앱은
  모르는 필드를 무시한다), 허브 상태 파일은 schema 2에 선택 목록 하나를 더했다. 이전 상대와 계속 동작하는 기능
  추가이므로 MINOR다. `power_base` 0.3.0, `module` 0.1.1, `rcp` 0.1.0은 소스가 바뀌지 않아 그대로다 — 다만
  `versions.json`이 모든 구성요소의 빌드 입력이라 세 구성요소의 **소스 지문과 빌드 식별자는 달라진다**(아래
  "소스 지문과 빌드 입력"). 2026-10-09에 만든 펌웨어 산출물은 자기 manifest로는 그대로 유효하고,
  `--match-current-tree`만 더는 통과하지 않는다. 0.4.0도 개발 빌드만 있는 버전이고, Linux에서 빌드하거나 설치한
  적이 없다.
- 2026-10-09에 두 가지를 넣으면서 세 구성요소를 올렸다.
  - `power_base` **0.3.0**, `linux_bridge` **0.3.0**: 같은 허브의 Thread 수신 주소·포트가 바뀌었을 때 Power
    Base가 허브를 다시 찾는 기능([`power-base-management.md`](power-base-management.md) 12절). 이전 상대와 계속
    동작하는 기능 추가이므로 MINOR다(관리 메시지 type 두 개와 capability bit 두 개를 **추가**했고, 관리 규약
    번호·BLE 규약·HTTP API·record 버전은 그대로다).
  - `module` **0.1.1**: Base를 삭제하고 다시 등록했을 때 계속 꽂혀 있던 아래 module이 새 정보를 받지 못하던
    결함의 수정([`state-machine.md`](state-machine.md) "UPSTREAM 세대 변경과 DOWNSTREAM 재설정"). POGO 메시지의
    형식과 순서는 그대로이고 다른 구성요소가 보는 형식이 달라지지 않으므로 PATCH다. 고친 코드
    (`components/parent_service`)는 Power Base 이미지에도 들어가지만 Power Base는 그 경로를 쓰지 않는다
    (세대 번호를 주지 않는다). Power Base에서 달라지는 것은 실패한 downstream session의 재시도 간격뿐이다.
  - `rcp`는 `0.1.0` 그대로다. 소스가 바뀌지 않았다. `versions.json`이 빌드 입력이라 지문은 달라졌다: 기능
    변경이 아니다. 새 RCP 이미지는 만들지 않았다.
  - `0.3.0`과 `0.1.1`도 개발 빌드만 있는 버전이다. 0.2.0·0.1.0으로 만든 산출물(`pogoautokey_20261008_*`)과 그
    기록에 새 버전을 붙이지 않는다.
  - 같은 날 코드 검토가 host 재현으로 찾은 결함 두 건(주소 복구가 후보 목록의 뒤쪽을 묻지 못하던 것, 실패한
    재시도 뒤에 이전 JOIN_RESULT가 새 session을 성공시키던 것)을 고쳤고 **버전은 올리지 않았다.** `0.3.0`과
    `0.1.1`은 배포된 적도 보드에 기록된 적도 없는 버전이고, 고친 것은 그 두 변경의 일부이며, 다른 구성요소가
    보는 동작과 형식은 그대로다(`tools/pogo_version.py compat`의 출력이 같다). 그래서 `0.3.0`·`0.1.1`이라고
    말하는 산출물이 두 묶음 있다: `pogolocate_20261009_*`(쓰지 않는다. 후속 수정이 들어 있지 않다)와
    `pogolocfix_20261009_*`. 가르는 것은 빌드 식별자다. `linux_bridge`는 소스가 바뀌지 않았지만 `components/`가
    그 빌드 입력이라 지문이 달라졌다: 기능 변경이 아니다. `rcp`의 지문은 그대로다.
- 이 값은 `tools/cmake/pogo_version.cmake`가 ESP-IDF의 `PROJECT_VER`로 넘기고,
  `components/pogo_build_info`(Power Base·센서 모듈), `rcp/main/rcp_build_info.c`, Linux bridge의
  `build_info.cpp`가 같은 도구로 읽는다. 다른 곳에
  버전을 다시 적지 않는다.

### 언제 올리나

| 올리는 자리 | 기준 |
|---|---|
| PATCH | 다른 구성요소가 보는 동작이 달라지지 않는 수정 |
| MINOR | 기능 추가. 이전 상대와 계속 동작한다(새 BLE capability, 새 API 필드, 새 레코드 버전을 **추가**) |
| MAJOR | 이전 상대와 더는 동작하지 않는 변경(필드 삭제·의미 변경, 이전 영속 레코드를 못 읽음) |

`0.y.z` 동안에는 호환되지 않는 변경도 MINOR로 올리되 CHANGELOG의 "호환성"에 반드시 적는다.
`1.0.0`은 실물 검증을 마친 첫 정식 배포에 붙인다.

버전을 올리는 것만으로는 API, BLE 메시지, 영속 레코드 형식, Matter 동작의 의미가 바뀌지 않는다.
그런 변경은 각자의 스키마 번호와 capability로 표시하고(`tools/pogo_version.py compat`), 버전은 그
결과를 사람이 읽기 좋게 요약할 뿐이다.

## 빌드 식별 정보

### 소스 지문과 빌드 입력

커밋만으로는 부족하다. 같은 HEAD 위에서 커밋하지 않은 내용이 다르면 다른 빌드이기 때문이다.
그래서 구성요소의 **빌드 입력 전체**의 SHA-256을 쓴다. 빌드 입력은 컴파일되는 소스만이 아니라,
그 빌드가 실행하는 **저장소 안의 빌드 도구**까지다. 도구가 바뀌면 같은 소스에서도 다른 바이너리나
다른 식별 정보가 나올 수 있기 때문이다.

```
SHA-256( "pogo-source-fingerprint-v1" NUL  구성요소 이름 NUL
         각 파일을 상대 경로의 바이트 순서로:
           일반 파일   "F" 경로 NUL 크기(십진수) NUL 내용
           심볼릭 링크 "L" 경로 NUL 링크 대상 NUL )
```

| 구성요소 | 소스 | 빌드 도구 (파일 단위로 명시) |
|---|---|---|
| `power_base` | `power_base/`, `components/`, `versions.json` | `tools/cmake/pogo_version.cmake`, `tools/pogo_version.py` |
| `module` | `module/`, `components/`, `versions.json` | 위와 같음 |
| `rcp` | `rcp/`, `versions.json` | 위와 같음 |
| `linux_bridge` | `linux/matter_bridge/`, `components/`, `versions.json` | `tools/build_matter_bridge.sh`, `tools/pogo_version.py` |

- `tools/cmake/pogo_version.cmake`는 세 ESP-IDF 프로젝트의 `CMakeLists.txt`가 include한다(버전 전달,
  stamp 단계. 컴파일 옵션을 더할 수도 있는 자리다). `tools/pogo_version.py`는 모든 빌드가 컴파일해
  넣는 식별 헤더를 쓰고 variant의 sdkconfig 조합을 정한다. `tools/build_matter_bridge.sh`는 bridge의
  GN 빌드를 구성한다.
- **`tools/` 전체가 입력인 것은 아니다.** host test, 설정 이미지 생성기, host stub은 아무것도 빌드하지
  않으므로 넣지 않는다. 입력은 위 표처럼 구성요소별로 이름을 적는다(`COMPONENT_SOURCES`). 실제 빌드
  파일이 가리키는 `../tools/…`와 `components/<이름>`이 이 목록에 들어 있는지는
  `tools/test_pogo_version.py`가 실제 저장소에서 검사한다. 새 빌드 도구를 빌드에 연결하면 그 목록에도
  넣어야 하고, 넣지 않으면 그 검사가 실패한다.
- bridge가 컴파일하는 컴포넌트는 여섯 개지만 `components/` 전체를 입력으로 둔다(넓은 쪽으로 틀린다).
- Git이 추적하지 않는 새 파일도 들어간다.
- 들어가지 않는 것: 빌드 출력(`build/`, `out/`, `managed_components/`, `generated/`), 캐시
  (`__pycache__/`, `*.pyc`), 프로젝트의 로컬 `sdkconfig`, `*.bin`/`*.elf`/`*.map`/`*.log`, 편집기·OS
  파일, 도구가 만드는 헤더·`manifest.json`·`SOURCE_INFO.json`. 입력 스크립트 자체를 넣는다고 해서
  재귀가 생기지 않는다: 스크립트가 **만드는 것**이 빠져 있기 때문이다.
- 절대 경로, 시간, 사용자 이름은 들어가지 않는다. 같은 입력은 어느 디렉터리에서 계산해도(공백이
  있는 경로 포함) 같은 값이다.
- 문서(`docs/`)는 넣지 않는다.
- RCP는 다른 펌웨어 프로젝트와 컴포넌트를 공유하지 않는다(`tools/test_rcp_config.py`). 그래서
  `components/pogo_build_info`를 쓰지 않고 같은 몇 줄을 `rcp/main/rcp_build_info.c`에 따로 갖는다.
  헤더를 만드는 CMake 함수(`tools/cmake/pogo_version.cmake`의 `pogo_build_info_attach`)와 도구만
  공유한다.

2026-10-06의 첫 구현은 빌드 도구를 지문에서 빼고 있었다. 그래서 공용 CMake 파일에 컴파일 정의를
더해도 지문·빌드 식별자가 그대로였고, clean으로 판정되어 release까지 허용됐다(검토에서 재현). 같은 날
위와 같이 고쳤다. 그 전에 만든 산출물의 식별자는 그 당시 규칙으로 계산된 값이며, 같은 소스라도 지금
계산하면 다른 값이 나온다([빌드 manifest](#빌드-manifest)의 "두 가지 질문").

### commit, dirty, 빌드 식별자

- **commit**: `git rev-parse HEAD`의 40자리 전체. 줄인 SHA는 기록하지 않는다.
- **dirty**: 빌드 입력이 HEAD의 내용과 정확히 같지 않으면 `true`. **지문과 같은 파일 집합**으로
  판정한다(`is_input`). Git이 알려 주는 경로 가운데 빌드 입력인 것이 하나라도 수정·stage·삭제·
  이름 변경·미추적이면 dirty다.
  - Git이 **무시하지만 빌드는 읽는** 파일(예: `.gitignore`에 걸리는 헤더)도 dirty다. 그 파일은
    지문에 들어가므로, 지문이 커밋의 것이 아닌 트리를 clean이라 부르지 않는다.
  - 반대로 Git이 추적하지만 빌드 입력이 아닌 파일(로그 등)은 바뀌어도 지문도 dirty도 그대로다.
- **빌드 식별자**: `<commit 앞 12자리>[+dirty].<지문 앞 12자리>`. 최대 31자라서 32바이트 필드에
  들어간다. 전체 값은 manifest와 바이너리 안의 `POGO_BUILD_INFO{…}` 문자열에 있다.

Git 상태는 `GIT_OPTIONAL_LOCKS=0`으로 읽는다. 도구는 index를 포함해 저장소의 어떤 파일도 쓰지
않는다.

### Git이 없는 소스 묶음

`.git` 없이 옮긴 소스에서는 commit을 알 수 없다. 이때 도구는 **SHA를 지어내지 않는다.**

- 묶음의 맨 위에 `SOURCE_INFO.json`이 있고, 거기 적힌 지문이 지금 빌드 입력의 지문과 **같을 때만**
  그 commit과 dirty를 유지한다(`origin: source-info-verified`).
- 파일이 없거나, 읽을 수 없거나, 소스나 **빌드 도구**가 한 글자라도 달라졌으면 commit과 dirty는
  `unknown`이고 빌드 식별자는 `unknown.<지문 12자리>`다. 지문은 여전히 정확하다. 이전 규칙으로 만든
  `SOURCE_INFO.json`은 지문이 맞지 않으므로 유효한 기준선으로 인정되지 않는다.
- `SOURCE_INFO.json`은 Git 작업 트리에서 `tools/pogo_version.py source-info --out <트리 밖 경로>`로
  만든다.

### 빌드 종류

- `release`: `--release`(또는 `POGO_RELEASE=1`)를 **명시**했고, 빌드 입력이 clean이며 commit을
  아는 경우에만. 조건이 안 맞으면 빌드가 시작되기 전에 실패한다. 빌드 도구만 커밋하지 않은
  상태여도 거부된다.
- `development`: 그 밖의 모든 빌드. dirty 트리는 여기서만 허용된다.

### 빌드 중에 입력이 바뀌면

`build-firmware`는 빌드 전과 후에 지문·commit·dirty를 다시 계산한다(같은 입력 집합). 소스든 빌드
도구든 하나라도 다르면 그 출력은 어느 한 트리의 빌드가 아니므로 **manifest를 쓰지 않고 실패한다.**
배포 후보나 시험 대상으로 쓰지 않는다.

### 증분 빌드

빌드 식별 헤더(`pogo_build_info_gen.h`)는 매 빌드마다 다시 계산하되, 내용이 달라졌을 때만 파일을
고쳐 쓴다. 소스나 빌드 도구가 바뀌면 다음 증분 빌드에서 그 헤더를 포함하는 파일 하나
(`pogo_build_info.c`, RCP는 `rcp_build_info.c`)가 다시 컴파일되어 새 값이 들어가고, 바뀌지 않았으면
아무것도 다시 빌드하지 않는다. 헤더는 빌드 디렉터리 안에만 있다(소스 트리에 생기지 않는다). GN으로
빌드하는 Linux bridge만 예외로, `tools/build_matter_bridge.sh`가 Git이 무시하는
`linux/matter_bridge/app/api/generated/`에 쓴다.

## 어디에서 확인하나

| 구성요소 | 확인 방법 |
|---|---|
| Power Base, 센서 모듈 | 부팅 로그 한 줄: `POGO power_base 0.2.0 build 590e2a11d2cb+dirty.<지문 12자리> (development, ble)` |
| 모든 펌웨어 이미지 | `tools/pogo_version.py appdesc <app.bin>`: ESP-IDF app description의 `version`·`project_name`과 이미지 안의 `POGO_BUILD_INFO{…}` |
| RCP | 위 `appdesc`만. 운영 RCP는 USB가 Spinel 링크라서 **로그를 내지 않는다**(`docs/otbr-rcp.md`). 실행 중인 RCP의 버전은 이 체계로 읽지 않는다 |
| Linux bridge | `pogo_matter_bridge --version`, `pogohome-hubctl --version`, 그리고 `GET /api/v1/health`의 선택 필드 `build` (`docs/pogohome-api-v1.md`) |
| `pogohome-otbr-helper` | 버전 출력 없음. root로 실행되는 이 도구는 바꾸지 않았다. manifest의 파일 SHA-256으로만 식별한다 |
| 앱 | 설정 > 앱 정보의 버전·빌드 번호, "빌드 상세 정보"의 소스 식별 정보 (앱 저장소 문서) |

- app description의 `version` 필드는 32바이트다. 제품 버전(`0.1.0`)만 넣는다. 예전에는
  `git describe` 결과가 들어가 31자에서 잘렸다(`mvp-pairing-v1-41-g590e2a1-dirt`).
- 앱은 허브가 `build`를 주지 않아도, 형식이 달라도 그대로 동작한다. **보드의 버전을 앱으로 보내는
  프로토콜은 없고 만들지 않았다.** 앱은 보드 버전을 추정해서 보여 주지 않는다.

## 도구

모든 명령은 어디에서 실행해도 되고(경로에 공백 가능), 소스 트리와 Git index를 바꾸지 않는다.

```bash
tools/pogo_version.py versions                 # versions.json의 네 버전
tools/pogo_version.py check                    # 형식 검사
tools/pogo_version.py identity --component power_base [--json]
tools/pogo_version.py fingerprint --component power_base
tools/pogo_version.py files --component rcp    # 지문에 들어가는 파일(빌드 입력) 목록
tools/pogo_version.py compat                   # API·BLE·레코드 스키마 식별값 (소스에서 읽음)
tools/pogo_version.py appdesc <app.bin>        # 이미지가 스스로 말하는 버전, 빌드 정보, 대상 칩
tools/pogo_version.py source-info --out <트리 밖>/SOURCE_INFO.json
```

### 펌웨어 빌드

```bash
source ~/.espressif/v6.1/esp-idf/export.sh
tools/pogo_version.py build-firmware --component power_base --variant ble \
    --out ~/esp-builds/<새 디렉터리>            # 개발 빌드
tools/pogo_version.py build-firmware --component power_base --variant ble \
    --out ~/esp-builds/<새 디렉터리> --release  # clean 트리에서만
```

| 구성요소 | variant | sdkconfig 조합 |
|---|---|---|
| `power_base` | `default` | `sdkconfig.defaults` |
| | `bench` | + `sdkconfig.bench_joiner` |
| | `ble` | + `sdkconfig.ble_enrollment` |
| | `ble_reenroll_gpio9` | + `sdkconfig.ble_enrollment` + `sdkconfig.reenroll_button_gpio9` |
| `module` | `default` | `sdkconfig.defaults` |
| `rcp` | `default`, `diag` | `sdkconfig.defaults` (+ `sdkconfig.diag`) |

- `--out`은 **없거나 비어 있는** 저장소 밖의 디렉터리여야 한다. 이미 무엇이든 들어 있으면
  거부한다(이전 시험 이미지·백업·release 디렉터리를 덮어쓰지 않는다). 프로젝트의 `sdkconfig`도
  쓰지 않고 `<out>/build/sdkconfig`를 따로 만든다.
- 결과: `<out>/build/`(빌드 트리), `<out>/build.log`, `<out>/artifact/`(앱 이미지, bootloader,
  partition table, `flash_args`, `manifest.json`).
- 빌드 뒤 도구가 확인하는 것: 빌드 입력(소스와 빌드 도구)이 빌드 중에 바뀌지 않았는가, 만들어진
  sdkconfig가 요청한 variant인가, 이미지의 app description과 `POGO_BUILD_INFO`(variant·config id
  포함)가 `versions.json`·현재 트리·요청과 같은가, 이미지 헤더가 그 구성요소의 칩인가. 하나라도
  어긋나면 manifest가 생기지 않는다. 쓴 뒤에는 바로 `manifest-verify --match-current-tree`를 통과해야
  한다.
- 이 명령은 보드에 아무것도 쓰지 않는다.

`idf.py`를 직접 실행해도 같은 버전과 빌드 정보가 들어간다(`PROJECT_VER`와 stamp는 CMake에
연결되어 있다). 다만 manifest와 빌드 전후 비교는 `build-firmware`만 한다.

### Linux bridge

```bash
tools/build_matter_bridge.sh                   # 개발 빌드 (GN, Linux 장비에서)
POGO_RELEASE=1 tools/build_matter_bridge.sh    # clean 트리에서만
pogo_matter_bridge --version
```

Mac에서는 bridge 본체(GN/CHIP)를 빌드할 수 없다. API 라이브러리와 `pogohome-hubctl`은 CMake로 host
빌드하며(`linux/matter_bridge/app/api`), 같은 stamp와 `--version`을 쓴다.

## 빌드 manifest

`artifact/manifest.json`은 한 디렉터리의 파일들이 **어떤 빌드인지** 기계가 확인할 수 있게 적은
것이다. 아래 필드는 모두 필수다(없거나 형식이 다르면 검증이 그 자리에서 실패한다).

| 필드 | 내용 |
|---|---|
| `manifest_schema`, `type`, `created_utc` | `1`, `pogo-build-manifest`, 작성 시각 |
| `component`, `version`, `build_id`, `build_kind` | 구성요소와 제품 버전, 빌드 식별자, `development`/`release` |
| `source` | `commit`(전체 SHA 또는 `unknown`), `dirty`, `fingerprint`, `origin`, `inputs_unchanged_during_build` |
| `build` | `configuration`, `variant`, `target`, `config_id`, `toolchain` |
| `primary`, `files[]` | 대조 대상인 바이너리 하나와, 실제 파일마다 상대 경로·크기·SHA-256 |
| `binary_says` | manifest를 쓸 때 바이너리에서 읽은 값의 기록(검증의 근거로 쓰지 않는다) |
| `compatibility` | HTTP API 이름, BLE 등록 프로토콜 번호·메시지·capability, 영속 레코드 버전, 관리 프로토콜 번호와 그 메시지·capability(2026-10-08부터. 번호를 올리지 않고 메시지와 capability를 더할 수 있으므로 번호만으로는 부족하다) |
| `status` | `deployed: false`, `hardware_verified: false` (항상) |

서로 다른 네 가지가 서로 다른 필드에 있다: **제품 버전**(`version`), **소스 식별**(`source`,
`build_id`), **빌드 구성**(`build.variant`·`config_id`·`target`), **프로토콜·저장 형식 버전**
(`compatibility`). 하나로 다른 것을 추정하지 않는다.

- `build.variant`와 `build.config_id`는 **바이너리가 스스로 말하는 값**이다. `manifest-create`에
  호출자가 넘긴 값은 바이너리와 같아야 하고, 넘기지 않으면 바이너리에서 가져온다. 기본값은 없다.
- `config_id`는 생성된 sdkconfig 전체의 해시다. 이 프로젝트는 설정 코드·PSKd·Dataset·키를 Kconfig에
  넣지 않으므로 sdkconfig에는 비밀이 없다. 설정 값 자체는 manifest에 옮겨 적지 않는다.
- `build.target`은 바이너리의 헤더에서 읽는다. 펌웨어는 이미지 헤더의 chip id(`esp32h2`)이고 구성요소의
  대상 칩과 같아야 한다. bridge의 실행 파일은 `linux-<arch>`(ELF) 또는 `macos-<arch>`(Mac host 빌드의
  Mach-O)다.
- `compatibility`는 manifest를 쓸 때 저장소의 헤더에서 읽은 값이다. 그 프로토콜을 구현하지 않는
  구성요소(RCP)의 manifest에도 같은 값이 들어가며, RCP의 지문은 그 헤더를 포함하지 않는다.
- **비밀 설정 이미지(`*_cfg.bin`, Dataset, 백업)는 manifest에 넣지 않는다.** 그런 이름의 파일은
  `manifest-create`가 거부하고, 해시도 공개 기록에 자동으로 들어가지 않는다.

### 검증

```bash
tools/pogo_version.py manifest-verify --dir <artifact> \
    [--expect-component power_base] [--expect-target esp32h2] [--expect-variant ble] \
    [--expect-version 0.1.0] [--release] [--match-current-tree]
```

세 단계이고, 뒤 단계가 앞 단계를 대신하지 않는다.

1. **형식.** 필수 필드와 그 형식.
2. **실제 파일과 실제 바이너리.** 목록의 파일마다 존재·크기·SHA-256. 그리고 `primary` 바이너리를
   **다시 읽어서** manifest의 대응 필드와 대조한다.

   | 바이너리에서 읽는 것 | 대조하는 manifest 필드 |
   |---|---|
   | `POGO_BUILD_INFO`의 component, version, build, kind | `component`, `version`, `build_id`, `build_kind` |
   | commit, fingerprint, dirty | `source.commit`, `source.fingerprint`, `source.dirty` |
   | **variant, config** | **`build.variant`, `build.config_id`** |
   | ESP-IDF app description의 version, project name, IDF 버전 | `version`, 구성요소, `build.toolchain.esp-idf` |
   | 이미지 헤더의 chip id / 실행 파일 헤더의 형식과 아키텍처 | `build.target` |

   manifest의 `binary_says`는 근거로 쓰지 않는다. 다만 거기 적힌 내용이 실제 바이너리와 다르면 그것도
   실패다.
3. **호출자의 기대.** `--expect-*`, `--release`는 1·2를 통과한 값에 **추가로** 대조한다. 예를 들어
   일반 BLE 이미지의 manifest를 BOOT 버튼 구성으로 고쳐 쓰고 `--expect-variant`로 그 구성을 요구해도,
   2단계에서 "바이너리는 variant=ble라고 한다"로 실패한다.

다음 경우에 실패한다(종료 코드 1, manifest가 없거나 JSON이 아니면 2).

- 필수 필드가 없거나 형식이 다르다
- 목록의 파일이 없다, 크기나 SHA-256이 다르다, 경로가 중복되거나 디렉터리 밖을 가리킨다
- manifest의 버전·구성요소·빌드 식별자·종류·commit·지문·dirty·**variant·config id**가 바이너리 자신이
  말하는 값과 다르다
- `build.target`이 바이너리의 헤더와 맞지 않는다
- `binary_says`가 실제 바이너리와 다르다
- 빌드 식별자가 commit·dirty·지문에서 계산되는 값이 아니다
- `release`인데 dirty이거나 commit을 모르거나 빌드 중 입력이 바뀌었다
- 기대한 구성요소·target·variant·버전·release와 다르다
- manifest가 배포됐다거나 실물 검증됐다고 주장한다

**검증했다고 말하지 않는 것.** 파일에서 읽을 수 없는 값은 manifest가 적은 그대로이고, 통과 메시지의
"not verified" 줄에 이름이 나온다: `build.configuration`, `build.toolchain`(ESP-IDF 버전 외),
`compatibility`, `created_utc`, `source.origin`, `source.inputs_unchanged_during_build`. bridge의 ELF
헤더는 아키텍처를 말할 뿐 운영체제를 증명하지 않는다(그것도 메시지에 적힌다).

### 두 가지 질문: 산출물의 무결성과 현재 트리와의 일치

- **자체 무결성**(기본): 이 디렉터리의 파일들이 manifest가 말하는 그 빌드인가. 소스 트리가 필요 없다.
- **현재 트리와의 일치**(`--match-current-tree`): 그 빌드가 **지금 이 트리**의 빌드인가. manifest의
  commit·dirty·지문을 현재 트리의 값과 비교한다. 결과는 따로 출력되고("current tree: …"), 무결성은
  통과했는데 트리만 다르면 종료 코드 1과 함께 그 사실을 구분해 말한다.

트리가 바뀌면(소스든 빌드 도구든) 이전 산출물은 첫 질문에는 계속 통과하고 둘째 질문에는 통과하지
않는다. 정상이다. 이전 산출물과 manifest는 고치거나 새 식별자로 바꿔 쓰지 않는다. 새로 필요하면 새
디렉터리에 다시 빌드한다. `build-firmware`는 방금 만든 산출물에 두 질문을 모두 묻는다.

**manifest는 파일을 설명할 뿐이다.** 만들었다고 해서 설치·배포·실물 검증이 된 것이 아니다. 그
사실은 아래의 별도 기록에 남긴다.

## 설치·실물 시험 기록

어떤 조합을 어디에 넣어 무엇을 확인했는지는 `docs/release-records/`의 기록으로 남긴다. 양식과
규칙은 [`docs/release-records/README.md`](release-records/README.md)에 있다.

- 공개 기록: 대상 **별칭**, 구성요소별 버전·빌드 식별자·manifest와 주 산출물의 SHA-256, 일시,
  단계별 결과와 근거 로그, **검증하지 않은 범위**, 되돌릴 산출물.
- 비공개 기록: 별칭이 실제로 가리키는 것(보드 EUI-64, 직렬 포트, 호스트·주소·계정)과 사용한 설정
  이미지의 위치. 저장소 밖에 두거나 Git이 무시하는 `docs/release-records/private/`에 둔다.
- 설정 코드, PSKd, Dataset, Network Key, 관리 키, 토큰은 **어느 쪽에도 값으로 적지 않는다.**

## 과거 산출물

2026-10-06 이전에 만든 이미지는 이 체계의 버전을 갖지 않는다. 분류는
**"기존 버전 표기 / 명시 버전 미관리"** 하나다.

- 소급해서 `0.1.0` 같은 버전을 붙이지 않고, 다시 포장하지 않으며, 파일을 고치지 않는다.
- 식별은 실제 SHA-256과, 이미지에 들어 있던 당시의 문자열(`git describe` 결과, 31자에서 잘림)로
  한다. 목록: [`docs/release-records/legacy-artifacts.md`](release-records/legacy-artifacts.md)
  (`tools/pogo_version.py legacy-inventory`로 만든다. 앱 이미지만 싣고, 이름이 설정 이미지·백업으로
  보이는 파일은 열지 않는다).
- 지금 보드나 허브에 들어 있는 것이 그중 무엇인지는 이 저장소에서 알 수 없다. 설치 기록이 없으면
  "알 수 없음"이다.

## Git, 태그, 변경 이력

### 커밋

도구와 빌드는 커밋하지 않는다. 검토를 마친 뒤 사람이 한다.

1. `CHANGELOG.md`의 `Unreleased`에 변경을 적는다(추가 / 수정 / 호환성 / 검증 한계).
2. 버전을 올릴 변경이면 `versions.json`의 해당 구성요소만 고친다.
3. `python3 tools/test_pogo_version.py`와 관련 host test, 대상 빌드를 실행한다.
4. 검토 뒤 커밋한다. 기능 단위로 나누고, 버전 변경은 그 기능의 커밋에 함께 넣는다.

### 배포 후보

1. 작업 트리가 clean인지 확인한다(`identity`의 `dirty False`).
2. `CHANGELOG.md`의 `Unreleased`를 `## [구성요소 버전] - 날짜`로 옮긴다. 커밋한다.
3. `build-firmware … --release`(Linux bridge는 `POGO_RELEASE=1`)로 새 디렉터리에 빌드한다.
4. `manifest-verify --release --match-current-tree`를 통과한 `artifact/`가 배포 후보다. **후보일
   뿐이다.**
5. 설치·시험하고 `docs/release-records/`에 기록한다.
6. 그 뒤에 태그를 붙인다.

### 태그 이름

| 구성요소 | 태그 |
|---|---|
| Power Base | `power-base-v0.1.0` |
| 센서 모듈 | `module-v0.1.0` |
| OTBR RCP | `rcp-v0.1.0` |
| Linux bridge | `linux-bridge-v0.1.0` |
| 앱 (앱 저장소) | `app-v1.0.0-b2` (제품 버전과 빌드 번호) |

- 태그는 그 버전의 release 빌드를 만든 **바로 그 커밋**에 붙인다(manifest의 `source.commit`).
- annotated 태그를 쓰고, 메시지에 manifest의 SHA-256과 설치 기록 파일 이름을 적는다.
- 한 번 붙인 태그는 옮기지 않는다. 고칠 것이 있으면 PATCH를 올린다.
- 기존 태그 `mvp-pairing-v1`은 이 규칙 이전의 것이며 그대로 둔다.

### 두 저장소에 걸친 배포

앱과 펌웨어·허브는 다른 저장소다. 함께 내보낸 조합은 **한 설치 기록**에 구성요소별로
적는다(저장소, 태그 또는 commit, 버전, 빌드 식별자, manifest SHA-256). 기록은 이 저장소의
`docs/release-records/`에 두고, 앱 저장소의 CHANGELOG는 그 파일 이름을 가리킨다. 두 저장소의
버전을 같은 숫자로 맞추지 않는다.

## 호환성 정책

- 서로 무엇을 지원하는지는 **capability와 스키마 번호로 협상한다.** BLE 등록의 capability
  목록(`mgmt_v1`, `local_reset_v1` 등)과 프로토콜 번호, 관리 프로토콜의 capability(`HELLO`의 bitmask)와
  모르는 메시지에 대한 상대의 실제 응답, HTTP API의 `schema_version`과 선택 필드, 영속 레코드의 버전
  바이트가 그것이다.
- **버전 문자열을 비교해서 기능을 켜거나 보안 판단을 하지 않는다.** 앱도, 허브도, 보드도 그렇게
  하지 않는다. `build` 필드와 `--version`은 사람이 식별하려고 보는 값이다.
- 버전 체계를 넣으면서 API, BLE 메시지, 영속 레코드, Matter 동작은 바꾸지 않았다. health의
  `build`는 선택 필드 하나를 **추가**한 것이고 `schema_version`은 그대로다.
- 2026-10-10의 변경(Linux bridge 0.4.0, 앱 1.3.0)으로 아래 표에서 달라지는 줄은 셋이다. 나머지는 그대로다
  (`tools/pogo_version.py compat`의 출력이 변경 전과 같다).

  | 번호 | 값 | 2026-10-10의 변경 |
  |---|---|---|
  | 제품 버전 | `linux_bridge` 0.4.0, 앱 1.3.0 (`power_base` 0.3.0, `module` 0.1.1, `rcp` 0.1.0 그대로) | 위 "버전의 단일 원본" |
  | HTTP API | `pogohome-api-v1`, `schema_version` 1 그대로 | **추가만**: health의 `power_base_forget`, snapshot의 Power Base별 `record`와 `power_base_forgets`, 경로 `/api/v1/power-base-forgets`. 이 셋이 없는 허브에 앱은 그 기능을 묻지 않는다 |
  | 허브 상태 파일(`power_base_mgmt.json`) schema | 2 그대로 | 선택 목록 `forgets` 추가. 이전 실행 파일은 그 목록을 무시하고, 파일을 다시 쓸 때 버린다. 되돌릴 때의 차이는 [`power-base-management.md`](power-base-management.md) 13절 "저장" |

  관리 규약(CoAP `pogo/v1/mgmt`)·BLE 등록 규약·enrollment record·재등록 journal·POGO 메시지는 바뀌지 않았다:
  보드 펌웨어는 이 기능에 관여하지 않는다.
- 같은 날의 후속 수정(Linux bridge 0.4.1, 앱 1.3.1)으로 달라지는 줄은 같은 셋이고, 모두 **선택 항목의 추가**다
  (`tools/pogo_version.py compat`의 출력은 그대로다).

  | 번호 | 값 | 0.4.1 / 1.3.1의 변경 |
  |---|---|---|
  | 제품 버전 | `linux_bridge` 0.4.1, 앱 1.3.1 (나머지 그대로) | 위 "버전의 단일 원본" |
  | HTTP API | `pogohome-api-v1`, `schema_version` 1, `power_base_forget` 1 그대로 | 삭제 작업의 `list_removed`(참일 때만). snapshot·SSE는 형식이 같고 한 시점의 내용이 됐다 |
  | 허브 상태 파일(`power_base_mgmt.json`) schema | 2 그대로 | 선택 목록 `members`, 정리 항목의 선택 필드 `rooms`. 이전 실행 파일은 둘을 무시하고 다시 쓸 때 버린다. 되돌릴 때의 차이는 [`power-base-management.md`](power-base-management.md) 14절 "저장 형식과 되돌리기" |
- 두 번째 후속 수정(Linux bridge 0.4.2, 앱 1.3.2)도 **선택 항목의 추가**뿐이다(`compat`의 출력은 그대로다).

  | 번호 | 값 | 0.4.2 / 1.3.2의 변경 |
  |---|---|---|
  | 제품 버전 | `linux_bridge` 0.4.2, 앱 1.3.2 (나머지 그대로) | 위 "버전의 단일 원본" |
  | HTTP API | `pogohome-api-v1`, `schema_version` 1, `power_base_forget` 1 그대로 | snapshot의 최상위 배열 `power_base_departures`(지금 목록 밖에 있는 Base). 모르는 앱은 무시한다 |
  | 허브 상태 파일(`power_base_mgmt.json`) schema | 2 그대로 | 차단 항목의 선택 키 `reason`·`record`. 이전 실행 파일은 둘을 무시하고 다시 쓸 때 버린다(차단은 남는다). [`power-base-management.md`](power-base-management.md) 15절 "저장 형식과 되돌리기" |
- 2026-10-11의 수정(Linux bridge 0.5.0, 앱 1.3.3)은 **요청 하나의 계약을 바꾼다**(`compat`의 출력은 그대로다:
  그 도구가 읽는 번호들은 바뀌지 않았다).

  | 번호 | 값 | 0.5.0 / 1.3.3의 변경 |
  |---|---|---|
  | 제품 버전 | `linux_bridge` 0.5.0, 앱 1.3.3 (나머지 그대로) | 위 "버전의 단일 원본" |
  | HTTP API | `pogohome-api-v1`, `schema_version` 1, `power_base_management` 1, `power_base_forget` 1 그대로 | **바뀐 것**: `POST /api/v1/power-base-removals`의 본문(`power_base_uuid`, `record`, `removal_serial`, `request_id`). 이전 형식은 `422 removal_registration_required`. **추가**: health의 `power_base_removal_request`(값 2: 이 요청 형식의 번호), snapshot의 Base 항목 `removal_serial`, 작업의 `request_id`, snapshot 끝의 `snapshot_stream`·`snapshot_sequence` |
  | 삭제 요청 형식(`power_base_removal_request`) | **2** (새 번호. 없는 허브는 1로 본다: EUI-64만) | 앱 1.3.3은 2인 허브에만 삭제를 요청한다. 앱 1.3.2 이하의 요청은 2인 허브가 거부한다 |
  | 허브 상태 파일(`power_base_mgmt.json`) schema | 2 그대로 | 자격증명의 선택 키 `removal_serial`(0이면 없음), 작업의 선택 키 `request_id`. 이전 실행 파일은 둘을 무시하고 다시 쓸 때 버린다. [`power-base-management.md`](power-base-management.md) 16절 "저장" |

  관리 규약(CoAP `pogo/v1/mgmt`)·BLE 등록 규약·enrollment record·재등록 journal·POGO 메시지는 바뀌지 않았다:
  보드 펌웨어는 이 수정에 관여하지 않는다.
- 제품 버전과 따로 관리하는 번호들, 2026-10-09 현재(`tools/pogo_version.py compat`이 소스에서 읽는다):

  | 번호 | 값 | 2026-10-09의 변경 |
  |---|---|---|
  | 제품 버전 | `power_base` 0.3.0, `linux_bridge` 0.3.0, `module` 0.1.1, `rcp` 0.1.0, 앱 1.2.1 | 위 "버전의 단일 원본" |
  | 관리 규약(`pogo/v1/mgmt`) 번호 | 1 | 그대로. 메시지 type `0x06`·`0x86`과 capability bit 두 개를 **추가**. 기존 여덟 메시지의 바이트·뜻·golden MAC은 같다 |
  | BLE 등록 규약 번호, message, capability 목록 | 1, 여덟 message, 네 capability | 변경 없음 |
  | HTTP API | `pogohome-api-v1`, `schema_version` 그대로 | 변경 없음(필드·경로·오류 코드 추가 없음) |
  | enrollment record 버전 | 1, 2, 3 | 변경 없음. v3의 목적지 필드의 **값**이 바뀔 수 있게 됐을 뿐이고 형식은 같다 |
  | 허브 상태 파일(`power_base_mgmt.json`) schema | 2 | 변경 없음. `board_capabilities`에 새 bit가 저장될 수 있다(이전 허브도 값 그대로 읽는다) |
  | 재등록 journal 형식 | 1 | 변경 없음 |
  | POGO 프레임·메시지 | 그대로 | 변경 없음(`module` 0.1.1은 보내는 **시점**만 바뀐다) |

## 검증 한계

`docs/validation-status.md`의 "버전 관리 체계 검증 (2026-10-06)"에 실제 실행 결과가 있다. 요약:

- Mac에서 한 것: 도구 host test, 펌웨어 7개 구성의 빌드와 manifest 검증, Linux API의 CMake host
  빌드와 test, 앱 빌드와 test.
- 하지 않은 것: Linux 장비의 GN/CHIP 빌드와 `pogo_matter_bridge --version`, 운영 허브의 health
  응답, 보드에 기록한 뒤의 부팅 로그, 설치 기록 양식의 실제 사용.
