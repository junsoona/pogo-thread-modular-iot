#!/usr/bin/env python3
"""
Security 2 interoperability and end-to-end admission check WITHOUT a board
and WITHOUT BLE.

Client side: ESP-IDF's official Python provisioning client code, unmodified
  ($IDF_PATH/examples/protocols/esp_local_ctrl/scripts: security/security2.py,
  security/srp6a.py and the generated protocomm protobufs) -- the SRP-6a
  client, the session messages and the AES-GCM nonce handling are theirs.
Server side: tools/host_ble_sec2 -- ESP-IDF's real protocomm.c, security2.c,
  esp_srp.c built for the host, plus this repo's real pb_ble_service.c,
  pb_ble_admission.c, power_base_ble_auth.c and power_base_enrollment.c.
Credential: a real cfg image made by tools/gen_power_base_ble_config.py; the
  record the server gets is read back out of that image with ESP-IDF's NVS
  parser.

What it shows: a client that knows the board's setup code and EUI-64 can
establish a Security 2 session with the stored salt/verifier and then use
pb-status / pb-start / pb-cancel; every other client gets nothing started.

What it does NOT show: anything about a real BLE link (advertising, GATT
long writes/reads, MTU, coexistence with Thread) or the iOS client. The
"connection" here is protocomm_open_session()/protocomm_close_session(),
which is what protocomm_nimble.c calls on connect/disconnect.

Requires:
  source ~/.espressif/v6.1/esp-idf/export.sh
  the harness built (see tools/host_ble_sec2/CMakeLists.txt); its path is
      $PB_BLE_HARNESS or ~/esp-builds/pogo_host_ble_sec2/pb_ble_sec2_harness.elf
  the `protobuf` Python package (ESP-IDF's environment does not ship it), e.g.
      python -m pip install --target "$HOME/pb-pydeps" protobuf
      PYTHONPATH="$HOME/pb-pydeps" python tools/test_power_base_ble_sec2_interop.py
Exit code 2 = skipped because one of these is missing.
"""
import ipaddress
import json
import os
import struct
import subprocess
import sys
import tempfile

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS_DIR = os.path.join(ROOT_DIR, "tools")
GEN = os.path.join(TOOLS_DIR, "gen_power_base_ble_config.py")
HARNESS = os.environ.get("PB_BLE_HARNESS",
                         os.path.expanduser("~/esp-builds/pogo_host_ble_sec2/pb_ble_sec2_harness.elf"))

# Made-up values (documentation ranges); no real board, hub or network.
EUI = "6055f9fffe001234"
OTHER_EUI = "6055f9fffe005678"
BRIDGE = "fd00:db8:1:2::1234"
PORT = 5684
PSKD = "J01NME7K"
PSKD2 = "ANY0THER1"

# pb_ble_proto.h
CODE = {0x00: "OK", 0x01: "ACCEPTED", 0x02: "DUPLICATE", 0x10: "UNSUPPORTED_VERSION", 0x11: "MALFORMED",
        0x12: "WRONG_TARGET", 0x13: "CHALLENGE_INVALID", 0x14: "CHALLENGE_EXPIRED", 0x15: "INVALID_PSKD",
        0x16: "BUSY", 0x17: "REQUEST_CONFLICT", 0x18: "NOT_WAITING", 0x19: "NO_SUCH_ATTEMPT", 0x1A: "CLOSED",
        0x1B: "UNAVAILABLE", 0x1C: "INTERNAL"}
STATE_WAITING, STATE_JOINING, STATE_ATTACHING, STATE_ENROLLED = 0, 1, 2, 3
PHASE_QUEUED, PHASE_RUNNING, PHASE_FAILED, PHASE_ENROLLED, PHASE_CANCELLED = 1, 2, 3, 4, 5
CLOSE_OPEN, CLOSE_ENROLLED, CLOSE_AUTH_LOCKED, CLOSE_WINDOW_EXPIRED = 0, 1, 2, 3
AUTH_OK, AUTH_ABSENT, AUTH_CORRUPT, AUTH_UNSUPPORTED, AUTH_FOREIGN = 0, 1, 2, 3, 4
ESP_OK = 0

failures = 0


def check(cond, msg):
    global failures
    print(("ok:   " if cond else "FAIL: ") + msg)
    if not cond:
        failures += 1


def skip(msg):
    print("SKIP: " + msg)
    sys.exit(2)


class Harness:
    """One process = one boot of the board."""

    def __init__(self, log_path):
        to_r, to_w = os.pipe()
        from_r, from_w = os.pipe()
        self.log = open(log_path, "ab")
        env = dict(os.environ, PB_HARNESS_IN_FD=str(to_r), PB_HARNESS_OUT_FD=str(from_w))
        self.proc = subprocess.Popen([HARNESS], pass_fds=(to_r, from_w), env=env, stdin=subprocess.DEVNULL,
                                     stdout=self.log, stderr=self.log)
        os.close(to_r)
        os.close(from_w)
        self.w = os.fdopen(to_w, "wb", buffering=0)
        self.r = os.fdopen(from_r, "rb", buffering=0)

    def _read(self, n):
        data = b""
        while len(data) < n:
            chunk = self.r.read(n - len(data))
            if not chunk:
                raise RuntimeError("harness exited unexpectedly (see its log)")
            data += chunk
        return data

    def cmd(self, c, payload=b""):
        self.w.write(c.encode() + struct.pack(">H", len(payload)) + payload)
        status, length = struct.unpack(">BH", self._read(3))
        return status, self._read(length)

    def init(self, eui_hex, record, joiner_ok=True):
        return self.cmd("I", bytes.fromhex(eui_hex) + bytes([1 if joiner_ok else 0]) + record)[0]

    def connect(self, session):
        return self.cmd("O", struct.pack(">I", session))[0] == 0

    def disconnect(self, session):
        return self.cmd("X", struct.pack(">I", session))[0] == 0

    def request(self, session, endpoint, data):
        """-> (esp_err, response bytes): what one characteristic write + read does."""
        name = endpoint.encode()
        status, resp = self.cmd("R", struct.pack(">IB", session, len(name)) + name + data)
        if status != 0:
            raise RuntimeError("harness refused the request frame")
        return struct.unpack(">I", resp[:4])[0], resp[4:]

    def advance(self, ms):
        self.cmd("T", struct.pack(">I", ms))

    def poll(self):
        return self.cmd("P")[1][0]

    def joiner_done(self, outcome):
        self.cmd("J", bytes([outcome]))

    def tick(self, attached, complete):
        self.cmd("A", bytes([1 if attached else 0, 1 if complete else 0]))

    def stats(self):
        s = self.cmd("S")[1]
        state, last, attempt, starts, stops, thread, services, marker = struct.unpack(">BBIIIBBB", s[:17])
        return {"state": state, "last": last, "attempt": attempt, "joiner_starts": starts, "joiner_stops": stops,
                "thread": thread, "services": services, "marker": marker,
                "pskd": s[17:].split(b"\0")[0].decode()}

    def stop_service(self):
        self.cmd("D")

    def quit(self):
        try:
            self.cmd("Q")
        except Exception:
            pass
        self.proc.wait(timeout=10)
        self.w.close()
        self.r.close()
        self.log.close()


class Client:
    """ESP-IDF's official Security 2 client over the harness 'connection'."""

    def __init__(self, harness, session, username, password, security_module):
        self.h = harness
        self.session = session
        self.sec = security_module.Security2(1, username, password, False)  # sec_patch_ver=1, as proto-ver says
        self.established = False
        self.sizes = []  # (request bytes, response bytes) of each handshake message

    def handshake(self):
        """Runs the official client state machine. -> (established, esp_err of the last message)."""
        response = None
        err = ESP_OK
        try:
            while True:
                request = self.sec.security_session(response)
                if request is None:
                    self.established = True
                    return True, err
                err, raw = self.h.request(self.session, "prov-session", request.encode("latin-1"))
                self.sizes.append((len(request.encode("latin-1")), len(raw)))
                if err != ESP_OK:
                    return False, err
                response = raw.decode("latin-1")
        except RuntimeError as e:  # the official client refused the device's answer
            return False, str(e)

    def call(self, endpoint, plaintext):
        """-> (esp_err, decrypted response or None)"""
        err, raw = self.h.request(self.session, endpoint, self.sec.encrypt_data(plaintext))
        if err != ESP_OK:
            return err, None
        return err, self.sec.decrypt_data(raw)

    def status(self, issue=False, request_id=None):
        err, resp = self.call("pb-status", bytes([1, 0x01, 1 if issue else 0, 0]) + (request_id or bytes(16)))
        if err != ESP_OK:
            return err, None
        check(len(resp) == 40 and resp[0] == 1 and resp[1] == 0x81, "    (pb-status response is 40 bytes, v1, type 0x81)")
        return err, {"code": resp[2], "eui64": resp[4:12].hex(), "state": resp[12], "last": resp[13],
                     "flags": resp[14], "phase": resp[15], "result": resp[16],
                     "ttl": struct.unpack(">H", resp[18:20])[0], "attempt": struct.unpack(">I", resp[20:24])[0],
                     "challenge": resp[24:40]}

    def start(self, target_hex, request_id, challenge, pskd):
        body = (bytes([1, 0x02, len(pskd), 0]) + bytes.fromhex(target_hex) + request_id + challenge +
                pskd.encode())
        err, resp = self.call("pb-start", body)
        return (err, None) if err != ESP_OK else (err, resp[2])

    def cancel(self, target_hex, request_id):
        err, resp = self.call("pb-cancel", bytes([1, 0x03, 0, 0]) + bytes.fromhex(target_hex) + request_id)
        return (err, None) if err != ESP_OK else (err, resp[2])


def extract_record(image_path, pack):
    """pb_ble_auth:cred out of a cfg image, with ESP-IDF's NVS parser."""
    tool_dir = os.path.join(os.environ["IDF_PATH"], "components", "nvs_flash", "nvs_partition_tool")
    if tool_dir not in sys.path:
        sys.path.insert(0, tool_dir)
    import nvs_parser
    with open(image_path, "rb") as f:
        partition = nvs_parser.NVS_Partition("cfg", bytearray(f.read()))
    ns_id = None
    for page in partition.pages:
        for entry in page.entries:
            if entry.key == pack.PB_BLE_AUTH_NAMESPACE and entry.metadata["namespace"] == 0 and entry.data:
                ns_id = entry.data.get("value")
            elif (ns_id is not None and entry.metadata["namespace"] == ns_id and entry.key == pack.PB_BLE_AUTH_KEY
                  and entry.metadata["type"] == "blob_data"):
                raw = bytearray()
                for child in entry.children:
                    raw += child.raw
                return bytes(raw[:entry.data.get("size")])
    raise RuntimeError("pb_ble_auth:cred not found in the image")


def main():
    idf_path = os.environ.get("IDF_PATH")
    if not idf_path:
        skip("IDF_PATH not set -- source the ESP-IDF v6.1 export.sh first")
    if not os.path.isfile(HARNESS):
        skip(f"host harness not built: {HARNESS} (see tools/host_ble_sec2/CMakeLists.txt)")
    scripts = os.path.join(idf_path, "examples", "protocols", "esp_local_ctrl", "scripts")
    sys.path.insert(0, scripts)
    sys.path.insert(0, TOOLS_DIR)
    try:
        import proto  # noqa: F401 -- ESP-IDF's protocomm protobufs
        import security
    except ImportError as e:
        skip(f"cannot load ESP-IDF's official client ({e}); the `protobuf` package is needed -- see this file's "
             f"docstring")
    import pack_power_base_config as pack

    with tempfile.TemporaryDirectory() as tmp:
        os.chmod(tmp, 0o700)
        log_path = os.path.join(tmp, "harness.log")
        code_file = os.path.join(tmp, "board.setup-code")
        gen = subprocess.run([sys.executable, GEN, "--eui64", EUI, "--bridge-host", BRIDGE, "--bridge-port", str(PORT),
                              "--setup-code-file", code_file, "--out-dir", os.path.join(tmp, "img")],
                             capture_output=True, text=True)
        if gen.returncode != 0:
            print(gen.stdout + gen.stderr)
            print("FAIL: the prepare tool did not produce an image")
            return 1
        image = os.path.join(tmp, "img", f"power_base_ble_{EUI}_config.bin")
        with open(code_file, "r", encoding="ascii") as f:
            setup_code = f.read().strip()
        record = extract_record(image, pack)
        check(len(record) == 420 and record[:4] == b"PBA1" and record[8:16].hex() == EUI,
              "record read back from the real cfg image: 420 bytes, for this EUI-64")
        pack.verify_pb_bridge_config(image, expect_enabled=True,
                                     expected_host_addr=ipaddress.IPv6Address(BRIDGE).packed, expected_port=PORT)

        def boot(rec=record, eui=EUI, joiner_ok=True):
            h = Harness(log_path)
            return h, h.init(eui, rec, joiner_ok)

        def client(h, session, password=setup_code, username=EUI):
            return Client(h, session, username, password, security)

        # ------------------------------------------------------------------
        print("\n-- credential problems: the service never starts --")
        h, st = boot(eui=OTHER_EUI)
        check(st == AUTH_FOREIGN, "this image on a board with another EUI-64: refused (foreign)")
        check(not h.connect(1), "no service: nothing to connect to")
        h.quit()
        bad = bytearray(record)
        bad[100] ^= 0x01
        h, st = boot(rec=bytes(bad))
        check(st == AUTH_CORRUPT, "one flipped verifier bit: refused (corrupt)")
        h.quit()
        h, st = boot(rec=record[:200])
        check(st == AUTH_CORRUPT, "truncated record: refused (corrupt)")
        h.quit()
        h, st = boot(rec=b"")
        check(st == AUTH_CORRUPT, "empty record: refused")
        h.quit()

        # ------------------------------------------------------------------
        print("\n-- before authentication --")
        h, st = boot()
        check(st == AUTH_OK, "valid record for this board: service started")
        check(h.connect(1), "connected")
        err, ver = h.request(1, "proto-ver", b"ESP")
        info = json.loads(ver.decode())
        check(err == ESP_OK and info["prov"]["sec_ver"] == 2 and info["prov"]["sec_patch_ver"] == 1,
              "proto-ver (readable without a session) announces Security 2, patch version 1")
        check("no_sec" not in info["prov"].get("cap", []) and EUI not in ver.decode().lower(),
              "proto-ver offers no unsecured mode and reveals no EUI-64")
        rid = bytes(range(1, 17))
        plain_start = bytes([1, 0x02, len(PSKD), 0]) + bytes.fromhex(EUI) + rid + bytes(16) + PSKD.encode()
        err, resp = h.request(1, "pb-start", plain_start)
        check(err != ESP_OK and resp == b"", "plaintext pb-start before any handshake: refused by protocomm")
        err, resp = h.request(1, "pb-status", bytes([1, 0x01, 1, 0]) + bytes(16))
        check(err != ESP_OK and resp == b"", "plaintext pb-status before any handshake: refused, no identity")
        err, resp = h.request(1, "no-such-endpoint", b"x")
        check(err != ESP_OK, "unknown endpoint: refused")
        h.poll()
        check(h.stats()["joiner_starts"] == 0, "nothing reached the Joiner")

        print("\n-- security downgrade attempts --")
        s0 = proto.session_pb2.SessionData()
        s0.sec_ver = proto.session_pb2.SecScheme0
        s0.sec0.sc.SetInParent()
        err, _ = h.request(1, "prov-session", s0.SerializeToString())
        check(err != ESP_OK, "a Security 0 (no security) session request: refused")
        s1 = proto.session_pb2.SessionData()
        s1.sec_ver = proto.session_pb2.SecScheme1
        s1.sec1.sc0.client_pubkey = bytes(32)
        err, _ = h.request(1, "prov-session", s1.SerializeToString())
        check(err != ESP_OK, "a Security 1 session request: refused")
        err, resp = h.request(1, "pb-status", bytes([1, 0x01, 0, 0]) + bytes(16))
        check(err != ESP_OK, "still no access to pb-status afterwards")
        h.quit()

        # ------------------------------------------------------------------
        print("\n-- wrong credentials --")
        h, _ = boot()
        h.connect(1)
        wrong_code = ("A" if setup_code[0] != "A" else "B") + setup_code[1:]
        c = client(h, 1, password=wrong_code)
        ok, err = c.handshake()
        check(not ok and err != ESP_OK, "wrong setup code (one character off): handshake refused by the device")
        err, _ = h.request(1, "pb-status", os.urandom(20 + 16))
        check(err != ESP_OK, "a request that client sends anyway (it has no session key) is refused")
        c = client(h, 1, username=OTHER_EUI)
        ok, err = c.handshake()
        check(not ok and err != ESP_OK, "right setup code with another board's EUI-64 as username: refused")
        c = client(h, 1, password=setup_code.lower())
        ok, err = c.handshake()
        check(not ok, "setup code in the wrong case: refused")
        check(h.poll() == CLOSE_OPEN and h.stats()["joiner_starts"] == 0, "3 failures: still open, nothing started")
        h.quit()

        # ------------------------------------------------------------------
        print("\n-- correct setup code: session, identity, start --")
        h, _ = boot()
        h.connect(1)
        c = client(h, 1)
        ok, err = c.handshake()
        check(ok, "ESP-IDF's official client and the device completed SRP-6a and verified each other's proof")
        print(f"      handshake sizes (request, response) in bytes: {c.sizes}")
        check(len(c.sizes) == 2 and all(0 < n <= 512 for pair in c.sizes for n in pair),
              "two handshake messages, each value within protocomm_nimble's 512-byte attribute limit")
        check(max(c.sizes[0]) > 244, "the first exchange is larger than one ATT PDU at any iOS MTU: it needs "
              "GATT long write / long read (NOT exercised here)")
        err, st = c.status(issue=True)
        check(err == ESP_OK and st["code"] == 0 and st["eui64"] == EUI,
              "pb-status inside the session: the board's real EUI-64, to compare with the expected one")
        check(st["state"] == STATE_WAITING and (st["flags"] & 0x04) and st["ttl"] == 60 and
              st["challenge"] != bytes(16), "waiting, with a fresh challenge (60 s)")
        ch = st["challenge"]

        err, code = c.start(OTHER_EUI, rid, ch, PSKD)
        check(err == ESP_OK and CODE[code] == "WRONG_TARGET", "start addressed to another EUI-64: refused")
        err, code = c.start(EUI, rid, ch, PSKD)
        check(CODE[code] == "CHALLENGE_INVALID", "the challenge that request carried is spent")
        _, st = c.status(issue=True)
        err, code = c.start(EUI, rid, st["challenge"], "j01nme7k")
        check(CODE[code] == "INVALID_PSKD", "malformed PSKd: refused")
        _, st = c.status(issue=True)
        err, resp = c.call("pb-start", bytes([1, 0x02, 33, 0]) + bytes.fromhex(EUI) + rid + st["challenge"] +
                           b"A" * 33)
        check(err == ESP_OK and CODE[resp[2]] == "MALFORMED", "oversized start (33-character PSKd): refused")
        err, resp = c.call("pb-start", (bytes([1, 0x02, len(PSKD), 0]) + bytes.fromhex(EUI) + rid +
                                        st["challenge"] + PSKD.encode())[:-1])
        check(CODE[resp[2]] == "MALFORMED", "truncated start: refused")
        err, resp = c.call("pb-start", bytes([2, 0x02, len(PSKD), 0]) + bytes.fromhex(EUI) + rid + st["challenge"] +
                           PSKD.encode())
        check(resp[0] == 1 and CODE[resp[2]] == "UNSUPPORTED_VERSION", "start with protocol version 2: refused")
        h.poll()
        check(h.stats()["joiner_starts"] == 0, "none of the refused requests started a Joiner")

        _, st = c.status(issue=True)
        err, code = c.start(EUI, rid, st["challenge"], PSKD)
        check(CODE[code] == "ACCEPTED", "well-formed start with the session's challenge: accepted")
        print(f"      pb-start as sent: {44 + len(PSKD) + 16} bytes (44 + PSKd + 16-byte GCM tag; at most 92); "
              f"pb-status response: {40 + 16} bytes")
        check(h.stats()["joiner_starts"] == 0, "accepted is not started: the request is only queued")
        err, code = c.start(EUI, rid, st["challenge"], PSKD)
        check(CODE[code] == "DUPLICATE", "the same request again: duplicate")
        check(h.poll() == CLOSE_OPEN, "worker pass")
        s = h.stats()
        check(s["joiner_starts"] == 1 and s["pskd"] == PSKD and s["state"] == STATE_JOINING,
              "the worker started exactly one Joiner, with the PSKd that travelled encrypted")
        _, st = c.status(request_id=rid)
        check(st["state"] == STATE_JOINING and (st["flags"] & 0x02) and st["phase"] == PHASE_RUNNING,
              "status: joining; this request is running")

        print("\n-- disconnect, reconnect --")
        _, st = c.status(issue=True, request_id=rid)
        old_challenge = st["challenge"]
        check(old_challenge != bytes(16), "(a challenge issued in the first session, left unused)")
        check(h.disconnect(1), "disconnected")
        h.advance(5000)
        check(h.poll() == CLOSE_OPEN and h.stats()["state"] == STATE_JOINING and h.stats()["joiner_stops"] == 0,
              "the accepted attempt keeps running without the BLE connection")
        check(h.connect(1), "reconnected (same connection handle)")
        err, _ = h.request(1, "pb-status", c.sec.encrypt_data(bytes([1, 0x01, 0, 0]) + rid))
        check(err != ESP_OK, "the previous session's keys are useless on the new connection")
        c2 = client(h, 1)
        ok, _ = c2.handshake()
        check(ok, "a fresh handshake establishes a new session")
        other = bytes(range(50, 66))
        err, code = c2.start(EUI, other, old_challenge, PSKD2)
        check(CODE[code] == "CHALLENGE_INVALID", "the previous session's challenge is refused in the new session")
        _, st2 = c2.status(issue=True, request_id=rid)
        check((st2["flags"] & 0x02) and st2["phase"] == PHASE_RUNNING,
              "the request id still names the running attempt after reconnecting")
        err, code = c2.start(EUI, other, st2["challenge"], PSKD2)
        check(CODE[code] == "BUSY", "another request while that attempt runs: busy")
        err, code = c2.cancel(EUI, other)
        check(CODE[code] == "NO_SUCH_ATTEMPT", "cancel with a request id that did not start it: refused")
        h.poll()
        s = h.stats()
        check(s["joiner_starts"] == 1 and s["joiner_stops"] == 0 and s["pskd"] == PSKD, "the attempt is untouched")

        print("\n-- tampered and replayed ciphertext --")
        good = c2.sec.encrypt_data(bytes([1, 0x01, 0, 0]) + rid)
        tampered = bytes([good[0] ^ 1]) + good[1:]
        err, _ = h.request(1, "pb-status", tampered)
        check(err != ESP_OK, "one flipped ciphertext bit: refused (AES-GCM tag)")
        err, raw = h.request(1, "pb-status", good)
        check(err == ESP_OK, "(the untouched ciphertext is accepted once)")
        c2.sec.decrypt_data(raw)
        err, _ = h.request(1, "pb-status", good)
        check(err != ESP_OK, "replaying that same ciphertext: refused (nonce counter moved on)")

        print("\n-- explicit cancel by the client that started it --")
        err, code = c2.cancel(EUI, rid)
        check(err == ESP_OK and CODE[code] == "OK", "cancel with the attempt's own request id: taken")
        check(h.stats()["joiner_stops"] == 0, "the BLE callback itself did not stop the Joiner")
        h.poll()
        s = h.stats()
        check(s["joiner_stops"] == 1 and s["state"] == STATE_WAITING, "the worker cancelled it through the controller")
        _, st2 = c2.status(request_id=rid)
        check(st2["phase"] == PHASE_CANCELLED, "status: cancelled")

        print("\n-- retry, Joiner success, attach, enrolled --")
        rid2 = bytes(range(100, 116))
        _, st2 = c2.status(issue=True)
        err, code = c2.start(EUI, rid2, st2["challenge"], PSKD2)
        check(CODE[code] == "ACCEPTED", "a new request (new id, new challenge, new PSKd) is accepted")
        h.poll()
        check(h.stats()["joiner_starts"] == 2 and h.stats()["pskd"] == PSKD2, "second Joiner attempt started")
        h.joiner_done(0)  # PB_ENROLL_JOINER_OK
        check(h.poll() == CLOSE_OPEN, "after Joiner success the worker keeps BLE open")
        _, st2 = c2.status(request_id=rid2)
        s = h.stats()
        check(st2["state"] == STATE_ATTACHING and st2["phase"] == PHASE_RUNNING and s["services"] == 0 and
              s["marker"] != 1, "Joiner success is only 'attaching': no marker, services not allowed")
        h.tick(attached=True, complete=True)
        check(h.poll() == CLOSE_ENROLLED, "attached with a complete Dataset and the marker stored: close BLE (enrolled)")
        s = h.stats()
        check(s["state"] == STATE_ENROLLED and s["services"] == 1 and s["marker"] == 1 and s["thread"] == 1,
              "enrolled: marker valid, Thread up, services allowed")
        err, st2 = c2.status(request_id=rid2)
        check(err == ESP_OK and CODE[st2["code"]] == "CLOSED" and st2["phase"] == PHASE_ENROLLED,
              "a status that still got through says: closed, enrolled")
        err, code = c2.cancel(EUI, rid2)
        check(CODE[code] == "CLOSED", "cancel after enrollment: refused")
        h.disconnect(1)
        h.stop_service()
        h.poll()
        s = h.stats()
        check(s["state"] == STATE_ENROLLED and s["marker"] == 1 and s["thread"] == 1 and s["joiner_starts"] == 2,
              "stopping the BLE service changed neither the enrollment nor Thread, and started nothing")
        h.quit()

        # ------------------------------------------------------------------
        print("\n-- authentication attempt limit --")
        h, _ = boot()
        h.connect(1)
        for i in range(5):
            ok, _ = client(h, 1, password=wrong_code).handshake()
            check(not ok, f"wrong setup code, attempt {i + 1}: refused")
        check(h.poll() == CLOSE_AUTH_LOCKED, "after 5 refused handshakes the worker is told to shut BLE down")
        ok, _ = client(h, 1).handshake()
        check(not ok, "even the right setup code is refused for the rest of this boot")
        check(h.stats()["joiner_starts"] == 0, "nothing started")
        h.quit()

        print("\n-- advertising window --")
        h, _ = boot()
        h.connect(1)
        h.advance(15 * 60 * 1000 - 1)
        check(h.poll() == CLOSE_OPEN, "1 ms before 15 minutes: open")
        h.advance(1)
        check(h.poll() == CLOSE_WINDOW_EXPIRED, "at 15 minutes with nothing in progress: close")
        ok, _ = client(h, 1).handshake()
        check(not ok, "no new session after the window")
        h.quit()

        print("\n-- board whose Joiner EUI-64 does not match --")
        h, _ = boot(joiner_ok=False)
        h.connect(1)
        c = client(h, 1)
        check(c.handshake()[0], "(the admission code is exercised directly here; the board itself does not open BLE)")
        _, st = c.status(issue=True)
        err, code = c.start(EUI, rid, st["challenge"], PSKD)
        h.poll()
        check((st["flags"] & 0x08) and CODE[code] == "UNAVAILABLE" and h.stats()["joiner_starts"] == 0,
              "status says unavailable; start refused; nothing started")
        h.quit()

        # ------------------------------------------------------------------
        print("\n-- nothing secret in the device log (ESP-IDF + this repo, INFO level as in the firmware build) --")
        with open(log_path, "rb") as f:
            log = f.read()
        keep = os.environ.get("PB_BLE_KEEP_LOG")
        if keep:
            with open(keep, "wb") as f:
                f.write(log)
        check(len(log) > 200 and b"security2" in log, "the device side produced log output (incl. ESP-IDF's)")
        for label, secret in (("setup code", setup_code.encode()), ("PSKd", PSKD.encode()), ("PSKd", PSKD2.encode()),
                              ("salt", record[16:32].hex().encode()), ("verifier", record[32:64].hex().encode())):
            check(secret not in log and secret.lower() not in log.lower(), f"no {label} in the device log")
        check(b"Session Key" not in log and b"Client proof" not in log,
              "ESP-IDF's debug dumps of the session key/proofs are not compiled in")
        for label, secret in (("setup code", setup_code), ("verifier", record[32:64].hex())):
            check(secret not in gen.stdout + gen.stderr, f"no {label} in the prepare tool's output")

    print(f"\n{'PASS' if failures == 0 else 'FAIL'}: {failures} failure(s)")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
