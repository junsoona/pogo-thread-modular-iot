# 과거 산출물 목록 (명시 버전 미관리)

분류: **기존 버전 표기 / 명시 버전 미관리.** `docs/versioning.md`의 버전 체계(2026-10-06)가 연결되기
전에 만든 펌웨어 앱 이미지다. 이 문서는 그 이미지들을 **있는 그대로** 식별할 뿐이다.

- 소급해서 버전을 붙이지 않았고, 다시 빌드하거나 포장하지 않았고, 파일을 고치지 않았다.
- "빌드 당시 문자열"은 이미지의 ESP-IDF app description에 들어 있던 값이다. 당시에는 `git describe`
  결과가 들어갔고 32바이트 필드라서 31자에서 잘렸다(`…-dirt`는 `-dirty`가 잘린 것). 이 문자열은
  제품 버전이 아니며, `-dirty`가 붙은 이미지는 어떤 미커밋 변경이 들어갔는지 이 문자열만으로는 알
  수 없다.
- 목록은 `tools/pogo_version.py legacy-inventory --dir ~/esp-builds`로 만들었다(2026-10-06, 읽기
  전용). 각 빌드 디렉터리 바로 아래의 `*.bin`만 본다. 이름이 설정 이미지·Dataset·백업으로 보이는
  파일(`cfg`, `config`, `nvs`, `backup`, `dataset` 등)은 열지 않는다. 그 밖의 파일은 앞부분의 app
  description만 확인하고, 앱 이미지일 때만 해시를 계산해 싣는다.
- 이 목록은 **빌드 디렉터리에 남아 있는 파일**의 목록이다. 어느 보드에 무엇이 들어 있는지는 말하지
  않는다. 이름에 `_flashed`가 붙은 파일은 기록 당시 따로 복사해 둔 사본이며, 같은 디렉터리의 빌드
  결과와 SHA-256이 같은지는 표에서 확인할 수 있다.
- `reenroll_20261006_*`는 BOOT 재등록 작업의 검증 빌드로, 같은 날 버전 체계를 연결하기 **전에**
  만들어졌다. 같은 소스 계열의 버전 체계 빌드는 `pogover_20261006_*`이며 각자 `artifact/manifest.json`을
  가진다(이 목록의 대상이 아니다).

기준 디렉터리: `~/esp-builds/`. 이미지 32개.

| 디렉터리 | 파일 | 크기 (B) | SHA-256 | 프로젝트 (app description) | 빌드 당시 문자열 | ESP-IDF |
|---|---|---:|---|---|---|---|
| `pbmgmt2_20261005_bench` | `power_base.bin` | 1009520 | `e9b3d32fed1f1ca048731beec8d1accdd8b92228e90445eed4072062b9adeacc` | power_base | `mvp-pairing-v1-41-g590e2a1-dirt` | v6.1 |
| `pbmgmt2_20261005_ble` | `power_base.bin` | 1415264 | `c2b6bc2822241505687a432724d6225ef67feadbfca7735dc48d822f6614e547` | power_base | `mvp-pairing-v1-41-g590e2a1-dirt` | v6.1 |
| `pbmgmt2_20261005_default` | `power_base.bin` | 873056 | `ac829c80c70c592cb49e1e3c7fceaac6cb15677b826b592cdf610464e52a0921` | power_base | `mvp-pairing-v1-41-g590e2a1-dirt` | v6.1 |
| `pbmgmt2_20261005_module` | `pogo_module.bin` | 885408 | `a4827347ae624cbb1d60f981d87a456655c27269986ab94ac59857cac3a88b2d` | pogo_module | `mvp-pairing-v1-41-g590e2a1-dirt` | v6.1 |
| `pbmgmt_20261005_bench` | `power_base.bin` | 1008352 | `64e1a75e8f21d8022c616f607177439a3fbc1b3bcae480c02e931d10282c90fb` | power_base | `mvp-pairing-v1-41-g590e2a1-dirt` | v6.1 |
| `pbmgmt_20261005_ble` | `power_base.bin` | 1413200 | `16f0f359d86f99495c365194e8742996e767b5123d0148a0a76da61eb3650b95` | power_base | `mvp-pairing-v1-41-g590e2a1-dirt` | v6.1 |
| `pbmgmt_20261005_default` | `power_base.bin` | 871888 | `4f1cae20cab84ad4859b99fc28f97418ca8758eab33af9c0f461019f212bf832` | power_base | `mvp-pairing-v1-41-g590e2a1-dirt` | v6.1 |
| `pbmgmt_20261005_module` | `pogo_module.bin` | 885408 | `17d5015e4413f81838a3f218a508a316b436af46ba1fa145bfcfcc45329072d8` | pogo_module | `mvp-pairing-v1-41-g590e2a1-dirt` | v6.1 |
| `pogo_lower` | `pogo_lower.bin` | 701904 | `3ea9073bfcad7ce4189a3d45b8e055efc307477b7be6bed5341805b309a64633` | pogo_lower | `mvp-pairing-v1-15-g9ff080b-dirt` | v6.1 |
| `pogo_module` | `pogo_module.bin` | 884736 | `94128123807990869941ea4cdfc9542af48a3ad28f979d0256e3d56a20ee4cd5` | pogo_module | `mvp-pairing-v1-32-ga89c30d` | v6.1 |
| `pogo_module` | `pogo_module_flashed.bin` | 884736 | `94128123807990869941ea4cdfc9542af48a3ad28f979d0256e3d56a20ee4cd5` | pogo_module | `mvp-pairing-v1-32-ga89c30d` | v6.1 |
| `pogo_module_removal_verify` | `pogo_module.bin` | 884736 | `ad098ba180ce899b3664c1fd76c983b03996fe8eede123f8f1786ca02ae3bded` | pogo_module | `mvp-pairing-v1-36-g46621a1-dirt` | v6.1 |
| `pogo_module_verify` | `pogo_module.bin` | 882000 | `8cd8aac80eadb9475f9a7433dbc203eb9d9a67b7bf90ddaf557b1a9aff22754c` | pogo_module | `mvp-pairing-v1-9-g7cac318-dirty` | v6.1 |
| `pogo_power_base` | `power_base.bin` | 841472 | `92530d0c65ec8164287a943d64db8401ebfa40286cdfcc28e260fc4fdd884f64` | power_base | `mvp-pairing-v1-32-ga89c30d` | v6.1 |
| `pogo_power_base` | `power_base_flashed.bin` | 841472 | `92530d0c65ec8164287a943d64db8401ebfa40286cdfcc28e260fc4fdd884f64` | power_base | `mvp-pairing-v1-32-ga89c30d` | v6.1 |
| `pogo_power_base_bench_joiner` | `power_base.bin` | 989504 | `0963038bff55124a2e65ea42661d19bb9d77a8f7223975fa88d7f39334be52a4` | power_base | `mvp-pairing-v1-39-g431b280-dirt` | v6.1 |
| `pogo_power_base_ble_enrollment` | `power_base.bin` | 1392592 | `a95b6c7977b6cb2937b46eb562652fd3e2218788593cbb8c34a0337a32291028` | power_base | `mvp-pairing-v1-39-g431b280-dirt` | v6.1 |
| `pogo_power_base_removal_verify` | `power_base.bin` | 846960 | `8384c56ac169b8511a39d3a77872c6c1f3cd15b3e24e55a98a5f05ddfcdd1a3e` | power_base | `mvp-pairing-v1-37-gcc018ea` | v6.1 |
| `pogo_power_base_removal_verify` | `power_base_flashed.bin` | 846960 | `8384c56ac169b8511a39d3a77872c6c1f3cd15b3e24e55a98a5f05ddfcdd1a3e` | power_base | `mvp-pairing-v1-37-gcc018ea` | v6.1 |
| `pogo_rcp` | `pogo_rcp.bin` | 253936 | `5236e60490d7ef943fefc003057270086cab3630355d26423856a3c708a70b25` | pogo_rcp | `mvp-pairing-v1-24-gf0334de-dirt` | v6.1 |
| `pogo_rcp` | `pogo_rcp_flashed.bin` | 253936 | `5236e60490d7ef943fefc003057270086cab3630355d26423856a3c708a70b25` | pogo_rcp | `mvp-pairing-v1-24-gf0334de-dirt` | v6.1 |
| `pogo_rcp_diag` | `pogo_rcp.bin` | 259248 | `148a6d541c777d8963c7e486fea61538cd7be229924d764a0600fc2710b4a199` | pogo_rcp | `mvp-pairing-v1-24-gf0334de-dirt` | v6.1 |
| `pogo_rcp_diag` | `pogo_rcp_flashed.bin` | 259248 | `148a6d541c777d8963c7e486fea61538cd7be229924d764a0600fc2710b4a199` | pogo_rcp | `mvp-pairing-v1-24-gf0334de-dirt` | v6.1 |
| `pogo_upper` | `pogo_upper.bin` | 793616 | `3b4ea261e815a608fb0b889adb31dfe5aa2c7f870c8fabe58c4e5748048ebcc7` | pogo_upper | `mvp-pairing-v1-15-g9ff080b-dirt` | v6.1 |
| `pogo_verify_ble_bench` | `power_base.bin` | 989504 | `8be17d660db4f7833f00c8658fb74f199771e01e422bad291d4be08e57097faa` | power_base | `mvp-pairing-v1-39-g431b280-dirt` | v6.1 |
| `pogo_verify_ble_default` | `power_base.bin` | 853104 | `3c10df402077df27fb9888965f85b07b004848f7c477eb75f924520c89ec124b` | power_base | `mvp-pairing-v1-39-g431b280-dirt` | v6.1 |
| `pogo_verify_ble_module` | `pogo_module.bin` | 884736 | `cbf3f2c3cd4f95eb2a792250525720595bd53ace8052402440ae269909b9cc33` | pogo_module | `mvp-pairing-v1-39-g431b280-dirt` | v6.1 |
| `reenroll_20261006_bench` | `power_base.bin` | 1017024 | `cb050fbf3d8ef5b09c3c749fea8e57b8bcc13ed040d903536f9165940b377b32` | power_base | `mvp-pairing-v1-41-g590e2a1-dirt` | v6.1 |
| `reenroll_20261006_ble` | `power_base.bin` | 1426608 | `1a131a92f9b7d409be1e404f46971066c3093618f04f082a9b942d4238508a4b` | power_base | `mvp-pairing-v1-41-g590e2a1-dirt` | v6.1 |
| `reenroll_20261006_ble_gpio9` | `power_base.bin` | 1430864 | `67971b77072c69f8514dc4caec7887fbcf406676b1748029826a8f82f84530de` | power_base | `mvp-pairing-v1-41-g590e2a1-dirt` | v6.1 |
| `reenroll_20261006_default` | `power_base.bin` | 880848 | `13f5a6ab0953c8f2be51734739a033cb2f9a89bb5f3e1294b293cc7c394f5f10` | power_base | `mvp-pairing-v1-41-g590e2a1-dirt` | v6.1 |
| `reenroll_20261006_module` | `pogo_module.bin` | 885408 | `4bd0d2f013759c73065c990c895b9a0c0007266a0033febebc1a657bb63c3fa3` | pogo_module | `mvp-pairing-v1-41-g590e2a1-dirt` | v6.1 |
