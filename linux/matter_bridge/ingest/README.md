# Power Base → Linux ingest server

`ingest_server`는 Power Base의 `linux_bridge_exporter`가 보내는 SAMPLE,
TOPOLOGY_SNAPSHOT, HEARTBEAT CoAP 메시지를 받아 UUID 기반 `ingest::Registry`에 저장하는
독립 smoke-test 서버다. Matter 기능은 [`docs/matter-bridge.md`](../../../docs/matter-bridge.md)의
`pogo_matter_bridge`가 담당한다.

## 빌드와 테스트

```bash
cd linux/matter_bridge/ingest
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

test 구성:

- `ingest_unit_tests`: registry와 session 순수 로직
- `ingest_registry_snapshot_test`: snapshot/generation/wait
- `ingest_loopback_test`: 실제 IPv6 UDP socket과 interface 검사
- `ingest_ack_transport_static_test`: ACK source와 `sendmsg()` 회귀 검사
- `ingest_power_base_block_test`: 삭제된 Power Base의 차단(`4.03`), Base와 그 아래 노드의 제거,
  `pogo/v1/mgmt` 요청의 handler 전달과 payload가 실린 ACK

## 실행

```bash
./build/ingest_server --interface wpan0 --port 5684
```

`--interface`는 필수다. server는 `recvmsg()`의 `IPV6_RECVPKTINFO`로 각 datagram이
실제로 해당 interface에 도착했는지 확인한다.

## 보안 범위

`bridge_export_proto`의 CRC는 전송 오류 검출용이며 인증 수단이 아니다. 이 server는 별도
암호 인증이나 Power Base allowlist를 구현하지 않았다.

따라서 UDP port는 Thread 네트워크의 보안 경계 안에서만 접근 가능해야 한다. 인터넷이나
신뢰하지 않는 LAN에 공개하지 않는다. interface 검사는 같은 Linux host의 다른 경로에서
들어오는 패킷을 차단하는 방어 수단이며 Thread 보안을 대신하지 않는다.

Power Base UUID allowlist는 고정된 장치 목록을 운영할 필요가 생기면 추가한다. 자동 TOFU는
사용하지 않는다.

## 요청 정책

- Confirmable POST만 registry 상태를 변경한다.
- Non-confirmable, ACK, Reset datagram은 응답 없이 버린다.
- 잘린 payload/ancillary data는 처리하지 않는다.
- ACK는 요청이 도착한 local IPv6 address와 interface를 source로 사용한다.

## 응답 코드

| 코드 | 의미 |
|---|---|
| 2.04 Changed | 새 상태 적용 |
| 2.03 Valid | 정확한 중복 |
| 4.00 Bad Request | 잘못된 envelope/payload 또는 resource 불일치 |
| 4.02 Bad Option | 알 수 없는 critical CoAP option |
| 2.05 Content | `pogo/v1/mgmt`의 인증된 답(payload 포함). 아래 "관리 resource" |
| 4.01 Unauthorized | `pogo/v1/mgmt`: MAC·등록 세대·hub 불일치 |
| 4.03 Forbidden | 등록을 해제해 차단된 Power Base의 SAMPLE·TOPOLOGY·HEARTBEAT·removal. Registry는 바뀌지 않는다 |
| 4.04 Not Found | topology가 없는 UUID의 SAMPLE. 관리 handler가 없는 server의 `pogo/v1/mgmt` |
| 4.05 Method Not Allowed | Confirmable이지만 POST가 아님 |
| 4.12 Precondition Failed | offline, bitmap 불일치, 오래된 generation/sequence |
| 5.03 Service Unavailable | bounded registry가 가득 참 |

## 관리 resource (`pogo/v1/mgmt`)

`ServerConfig::management`에 handler를 주면 `pogo/v1/mgmt`의 Confirmable POST payload를 그대로 넘기고,
handler가 준 응답 코드와 payload를 ACK에 실어 보낸다. 이 메시지는 `bridge_export_proto`가 아니며 ingest는
내용을 해석하지 않고 Registry에도 적용하지 않는다. 형식과 인증은
[`docs/power-base-management.md`](../../../docs/power-base-management.md)에 있고 구현은
`app/api`의 `PowerBaseManagement`다. 독립 `ingest_server`는 handler 없이 실행되므로 `4.04`로 답한다.

`Registry::set_power_base_blocked()`로 차단한 Base의 데이터 메시지는 `4.03`이고, 차단은 관리 handler
쪽에서만 걸고 푼다. 차단은 자리를 만들려고 버려지지 않으며, 그 보드의 새 인증된 등록 세대만 푼다.
