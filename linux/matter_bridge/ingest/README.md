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
| 4.04 Not Found | topology가 없는 UUID의 SAMPLE |
| 4.05 Method Not Allowed | Confirmable이지만 POST가 아님 |
| 4.12 Precondition Failed | offline, bitmap 불일치, 오래된 generation/sequence |
| 5.03 Service Unavailable | bounded registry가 가득 참 |
