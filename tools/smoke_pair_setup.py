#!/usr/bin/env python3
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Exercise the standalone Pi's optional pair-setup path with a synthetic client.

Requires Python cryptography on the test machine. The accessory identity and
private key must be synthetic fixtures. The client exercises a local endpoint.
"""

import argparse
import ctypes
import hashlib
import json
import os
import plistlib
import select
import socket
import stat
import subprocess
import tempfile
import time
import threading

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ed25519, x25519
from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305
from cryptography.hazmat.primitives.kdf.hkdf import HKDF


TEST_SECRET = b"123-45-678"


def wait_publication(path, after_sequence=0, not_before_ns=0):
    """Read a fresh real publisher heartbeat, never manufacture readiness."""
    deadline = time.monotonic() + 1
    while time.monotonic() < deadline:
        try:
            with open(path, "rb") as source:
                body = source.read(4097)
            assert len(body) <= 4096
            state = json.loads(body)
            if (int(state["sequence"]) > after_sequence and
                    int(state["updatedMonotonicNs"]) >= not_before_ns):
                return state
        except FileNotFoundError:
            pass
        time.sleep(0.01)
    raise RuntimeError("Synthetic receiver did not publish a fresh status heartbeat")


def wait_ready(process):
    """Bound startup while allowing the receiver's content-free diagnostics."""
    deadline = time.monotonic() + 3
    output = bytearray()
    while time.monotonic() < deadline:
        remaining = deadline - time.monotonic()
        if remaining <= 0 or not select.select([process.stdout], [], [], remaining)[0]:
            break
        chunk = os.read(process.stdout.fileno(), 4096)
        if not chunk:
            break
        output.extend(chunk)
        if len(output) > 8192:
            break
        if b"control listener ready on port 7001\n" in output:
            return
    raise RuntimeError("Synthetic control probe did not become ready within startup bounds")


def digest(data):
    return hashlib.sha512(data).digest()


def derive(key, salt, info):
    return HKDF(algorithm=hashes.SHA512(), length=32,
                salt=salt.encode(), info=info.encode()).derive(key)


def encode_tlv(fields):
    result = bytearray()
    for tag, value in fields:
        if not value:
            result.extend((tag, 0))
        for start in range(0, len(value), 255):
            chunk = value[start:start + 255]
            result.extend((tag, len(chunk)))
            result.extend(chunk)
    return bytes(result)


def decode_tlv(data):
    result = {}
    cursor = 0
    while cursor < len(data):
        assert cursor + 2 <= len(data)
        tag, size = data[cursor:cursor + 2]
        cursor += 2
        assert cursor + size <= len(data)
        result[tag] = result.get(tag, b"") + data[cursor:cursor + size]
        cursor += size
    return result


def request(connection, reader, sequence, body, path="/pair-setup",
            expected_status=b"200 OK"):
    header = (f"POST {path} RTSP/1.0\r\nCSeq: {sequence}\r\n"
              f"X-Apple-HKP: 0\r\nContent-Type: application/pairing+tlv8\r\n"
              f"Content-Length: {len(body)}\r\n\r\n").encode()
    connection.sendall(header + body)
    status = reader.readline()
    headers = {}
    while line := reader.readline().strip():
        name, value = line.decode().split(":", 1)
        headers[name.lower()] = value.strip()
    assert expected_status in status, status
    assert headers.get("cseq") == str(sequence)
    return decode_tlv(reader.read(int(headers["content-length"])))


class EncryptedClient:
    """One outstanding RTSP request, with independent record counters."""
    def __init__(self, connection, reader, read_key, write_key):
        self.connection, self.reader = connection, reader
        self.read_key, self.write_key = read_key, write_key
        self.sent_nonce = self.read_nonce = 0

    def request(self, method, path, sequence, body=b"", expected_status=b"200 OK"):
        plain = (f"{method} {path} RTSP/1.0\r\nCSeq: {sequence}\r\n"
                 f"Content-Length: {len(body)}\r\n\r\n").encode() + body
        for offset in range(0, len(plain), 1024):
            part = plain[offset:offset + 1024]
            size = len(part).to_bytes(2, "little")
            iv = b"\x00" * 4 + self.sent_nonce.to_bytes(8, "little")
            wire = size + ChaCha20Poly1305(self.write_key).encrypt(iv, part, size)
            self.connection.sendall(wire[:7])
            self.connection.sendall(wire[7:])
            self.sent_nonce += 1
        response = bytearray()
        while True:
            size = self.reader.read(2)
            assert len(size) == 2
            length = int.from_bytes(size, "little")
            assert 0 < length <= 1024
            ciphertext = self.reader.read(length + 16)
            assert len(ciphertext) == length + 16
            iv = b"\x00" * 4 + self.read_nonce.to_bytes(8, "little")
            response.extend(ChaCha20Poly1305(self.read_key).decrypt(iv, ciphertext, size))
            self.read_nonce += 1
            assert len(response) <= 65536
            if b"\r\n\r\n" not in response:
                continue
            headers, payload = bytes(response).split(b"\r\n\r\n", 1)
            fields = dict(line.split(b":", 1) for line in headers.split(b"\r\n")[1:])
            expected_length = int(fields[b"Content-Length"])
            if len(payload) < expected_length:
                continue
            assert len(payload) == expected_length
            assert expected_status in headers.split(b"\r\n")[0], headers[:100]
            assert fields[b"CSeq"].strip() == str(sequence).encode()
            return bytes(payload)

    def setup(self, sequence, value):
        return plistlib.loads(self.request("SETUP", "/stream", sequence,
            plistlib.dumps(value, fmt=plistlib.FMT_BINARY)))


def group_prime():
    crypto = ctypes.CDLL("libcrypto.so.3")
    crypto.BN_get_rfc3526_prime_3072.argtypes = [ctypes.c_void_p]
    crypto.BN_get_rfc3526_prime_3072.restype = ctypes.c_void_p
    crypto.BN_bn2hex.argtypes = [ctypes.c_void_p]
    crypto.BN_bn2hex.restype = ctypes.c_char_p
    return int(crypto.BN_bn2hex(crypto.BN_get_rfc3526_prime_3072(None)), 16)


def controller_proof(challenge):
    prime = group_prime()
    pad = lambda number: number.to_bytes(384, "big")
    private_a = 0x12345678
    public_a = pow(5, private_a, prime)
    public_b = int.from_bytes(challenge[3], "big")
    x = int.from_bytes(digest(challenge[2] +
                              digest(b"Pair-Setup:" + TEST_SECRET)), "big")
    multiplier = int.from_bytes(digest(pad(prime) + pad(5)), "big")
    scrambling = int.from_bytes(digest(pad(public_a) + pad(public_b)), "big")
    secret = pow((public_b - multiplier * pow(5, x, prime)) % prime,
                 private_a + scrambling * x, prime)
    session_key = digest(pad(secret))
    group_xor = bytes(left ^ right for left, right in
                      zip(digest(pad(prime)), digest(b"\x05")))
    proof = digest(group_xor + digest(b"Pair-Setup") + challenge[2] +
                   pad(public_a) + pad(public_b) + session_key)
    expected_reply = digest(pad(public_a) + proof + session_key)
    return pad(public_a), proof, expected_reply, session_key


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary")
    parser.add_argument("identity_file")
    parser.add_argument("private_key_pem")
    options = parser.parse_args()
    identity = dict(line.split("=", 1) for line in
                    open(options.identity_file, encoding="ascii").read().splitlines())
    with tempfile.TemporaryDirectory(prefix="headunit-hap-smoke-") as directory:
        secret_file = os.path.join(directory, "secret")
        pair_store = os.path.join(directory, "paired")
        descriptor = os.open(secret_file, os.O_WRONLY | os.O_CREAT | os.O_EXCL,
                             0o600)
        os.write(descriptor, TEST_SECRET)
        os.close(descriptor)
        presentation_file = os.path.join(directory, "presentation.json")
        environment = dict(os.environ)
        environment.update({
            "HEADUNIT_PRESENTATION_STATUS_FILE": presentation_file,
            "HEADUNIT_PRESENTATION_STATUS_NONCE": os.urandom(16).hex(),
            # An explicit empty fixture attaches two typed vehicle services,
            # without claiming installed car capabilities or an Ultra renderer.
            "HEADUNIT_VEHICLE_STATE_PROFILE": "empty",
            "HEADUNIT_CONNECTION_DIAGNOSTICS": "1",
        })
        process = subprocess.Popen(
            [options.binary, "7001", "12", directory, options.identity_file,
             secret_file, options.private_key_pem, pair_store],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            env=environment)
        try:
            wait_ready(process)
            receiver_output = []
            receiver_bytes = 0
            receiver_truncated = False
            def drain_receiver_output():
                nonlocal receiver_bytes, receiver_truncated
                while chunk := process.stdout.read(4096):
                    receiver_bytes += len(chunk)
                    if receiver_bytes <= 131072:
                        receiver_output.append(chunk)
                    else:
                        receiver_truncated = True
            output_thread = threading.Thread(target=drain_receiver_output, daemon=True)
            output_thread.start()
            with socket.create_connection(("127.0.0.1", 7001), timeout=3) as conn:
                conn.settimeout(3)
                reader = conn.makefile("rb")
                m2 = request(conn, reader, 1, encode_tlv(
                    [(6, b"\x01"), (0, b"\x00")]))
                assert m2[6] == b"\x02" and len(m2[2]) == 16
                public_a, proof, expected_reply, session_key = controller_proof(m2)
                m4 = request(conn, reader, 2, encode_tlv(
                    [(6, b"\x03"), (3, public_a), (4, proof)]))
                assert m4[6] == b"\x04" and m4[4] == expected_reply

                phone_key = ed25519.Ed25519PrivateKey.generate()
                phone_public = phone_key.public_key().public_bytes(
                    serialization.Encoding.Raw,
                    serialization.PublicFormat.Raw)
                phone_id = b"synthetic-phone-id"
                phone_x = derive(session_key, "Pair-Setup-Controller-Sign-Salt",
                                 "Pair-Setup-Controller-Sign-Info")
                phone_signature = phone_key.sign(phone_x + phone_id + phone_public)
                inner_m5 = encode_tlv([(1, phone_id), (3, phone_public),
                                       (0x0a, phone_signature)])
                encryption_key = derive(session_key, "Pair-Setup-Encrypt-Salt",
                                        "Pair-Setup-Encrypt-Info")
                encrypted_m5 = ChaCha20Poly1305(encryption_key).encrypt(
                    b"\x00" * 4 + b"PS-Msg05", inner_m5, None)
                m6 = request(conn, reader, 3, encode_tlv(
                    [(6, b"\x05"), (5, encrypted_m5)]))
                assert m6[6] == b"\x06"
                inner_m6 = decode_tlv(ChaCha20Poly1305(encryption_key).decrypt(
                    b"\x00" * 4 + b"PS-Msg06", m6[5], None))
                assert inner_m6[1] == identity["pi"].encode()
                assert inner_m6[3] == bytes.fromhex(identity["pk"])
                accessory_x = derive(session_key, "Pair-Setup-Accessory-Sign-Salt",
                                     "Pair-Setup-Accessory-Sign-Info")
                ed25519.Ed25519PublicKey.from_public_bytes(inner_m6[3]).verify(
                    inner_m6[0x0a], accessory_x + inner_m6[1] + inner_m6[3])

                ephemeral = x25519.X25519PrivateKey.generate()
                phone_x_pub = ephemeral.public_key().public_bytes(
                    serialization.Encoding.Raw,
                    serialization.PublicFormat.Raw)
                pv2 = request(conn, reader, 4, encode_tlv(
                    [(6, b"\x01"), (3, phone_x_pub)]), path="/pair-verify")
                assert pv2[6] == b"\x02" and len(pv2[3]) == 32
                shared = ephemeral.exchange(
                    x25519.X25519PublicKey.from_public_bytes(pv2[3]))
                pv_key = derive(shared, "Pair-Verify-Encrypt-Salt",
                                "Pair-Verify-Encrypt-Info")
                pv2_inner = decode_tlv(ChaCha20Poly1305(pv_key).decrypt(
                    b"\x00" * 4 + b"PV-Msg02", pv2[5], None))
                assert pv2_inner[1] == identity["pi"].encode()
                ed25519.Ed25519PublicKey.from_public_bytes(inner_m6[3]).verify(
                    pv2_inner[0x0a], pv2[3] + pv2_inner[1] + phone_x_pub)
                pv_signature = phone_key.sign(phone_x_pub + phone_id + pv2[3])
                pv3_inner = encode_tlv([(1, phone_id), (0x0a, pv_signature)])
                pv3_encrypted = ChaCha20Poly1305(pv_key).encrypt(
                    b"\x00" * 4 + b"PV-Msg03", pv3_inner, None)
                pv4 = request(conn, reader, 5, encode_tlv(
                    [(6, b"\x03"), (5, pv3_encrypted)]),
                    path="/pair-verify")
                assert pv4[6] == b"\x04"

                phone_control = derive(shared, "Control-Salt",
                                       "Control-Write-Encryption-Key")
                receiver_control = derive(shared, "Control-Salt",
                                          "Control-Read-Encryption-Key")
                control = EncryptedClient(conn, reader, receiver_control, phone_control)
                info = plistlib.loads(control.request("GET", "/info", 6))
                assert isinstance(info, dict) and len(info["displays"]) == 2
                session = control.setup(7, {"timingPort": 12345,
                    "features": ["vehicleStateProtocol"]})
                assert set(session["enabledFeatures"]) == {"altScreen", "viewAreas", "vehicleStateProtocol"}
                standard_info = plistlib.loads(control.request("GET", "/info", 107))
                assert standard_info["vehicleStateProtocolInfo"] == info["vehicleStateProtocolInfo"]
                assert "displayPanels" not in standard_info and "uiSyncInfo" not in standard_info
                assert 0 < session["eventPort"] < 65536
                assert 0 < session["timingPort"] < 65536
                # Both authenticated sockets reject unsupported/malformed commands.
                # Valid modes are observations; focus needs a current audio worker.
                for sequence, value, status in (
                        (101, {"type": "modesChanged", "params": {"resources": list(range(40))}}, b"400 Bad Request"),
                        (102, {"type": "duckAudio"}, b"501 Not Implemented"),
                        (103, {"type": "unduckAudio", "params": {"volume": -12}}, b"501 Not Implemented"),
                        (104, {"type": "duckAudio", "params": {"durationMs": "500"}}, b"400 Bad Request")):
                    control.request("POST", "/command", sequence,
                                    plistlib.dumps(value, fmt=plistlib.FMT_BINARY), expected_status=status)
                control.request("POST", "/command", 105, b"not a plist", expected_status=b"400 Bad Request")
                assert len(plistlib.loads(control.request("GET", "/info", 106))["displays"]) == 2
                control.request("POST", "/command", 108,
                                plistlib.dumps({"type": "futureCommand"}, fmt=plistlib.FMT_BINARY),
                                expected_status=b"501 Not Implemented")
                control.request("OPTIONS", "*", 109)
                control.request("OPTIONS", "*", 110, b"invalid", expected_status=b"400 Bad Request")
                event = socket.create_connection(("127.0.0.1",
                                                  session["eventPort"]), timeout=3)
                event.settimeout(3)
                event_reader = event.makefile("rb")
                event_body = plistlib.dumps({"type": "modesChanged", "params": {"resources": []}},
                                             fmt=plistlib.FMT_BINARY)
                event_plain = (b"POST /command RTSP/1.0\r\nCSeq: 1\r\n" +
                               f"Content-Length: {len(event_body)}\r\n\r\n".encode() +
                               event_body)
                event_size = len(event_plain).to_bytes(2, "little")
                event_write = derive(shared, "Events-Salt",
                                     "Events-Read-Encryption-Key")
                event_read = derive(shared, "Events-Salt",
                                    "Events-Write-Encryption-Key")
                event.sendall(event_size + ChaCha20Poly1305(event_write).encrypt(
                    b"\x00" * 12, event_plain, event_size))
                response_size = event_reader.read(2)
                assert len(response_size) == 2
                response_cipher = event_reader.read(
                    int.from_bytes(response_size, "little") + 16)
                event_response = ChaCha20Poly1305(event_read).decrypt(
                    b"\x00" * 12, response_cipher, response_size)
                assert b"200 OK" in event_response and b"CSeq: 1" in event_response
                for event_nonce, (value, status) in enumerate((
                        ({"type": "duckAudio"}, b"501 Not Implemented"),
                        ({"type": "duckAudio", "params": {"durationMs": "500"}}, b"400 Bad Request"),
                        ({"type": "modesChanged", "params": {"resources": []}}, b"200 OK")), start=1):
                    body = plistlib.dumps(value, fmt=plistlib.FMT_BINARY)
                    plain = (f"POST /command RTSP/1.0\r\nCSeq: {event_nonce+1}\r\n"
                             f"Content-Length: {len(body)}\r\n\r\n").encode() + body
                    size = len(plain).to_bytes(2, "little")
                    iv = b"\x00" * 4 + event_nonce.to_bytes(8, "little")
                    event.sendall(size + ChaCha20Poly1305(event_write).encrypt(iv, plain, size))
                    size = event_reader.read(2)
                    assert len(size) == 2
                    sealed = event_reader.read(int.from_bytes(size, "little") + 16)
                    reply = ChaCha20Poly1305(event_read).decrypt(iv, sealed, size)
                    assert status in reply and f"CSeq: {event_nonce+1}".encode() in reply
                # Unknown event commands and routes share the main error policy.
                for event_nonce, method, path, body, status in (
                        (4, "POST", "/command", plistlib.dumps({"type": "futureCommand"},
                            fmt=plistlib.FMT_BINARY), b"501 Not Implemented"),
                        (5, "POST", "/unknown", b"", b"501 Not Implemented"),
                        (6, "OPTIONS", "*", b"", b"200 OK"),
                        (7, "OPTIONS", "*", b"invalid", b"400 Bad Request")):
                    plain = (f"{method} {path} RTSP/1.0\r\nCSeq: {event_nonce+1}\r\n"
                             f"Content-Length: {len(body)}\r\n\r\n").encode() + body
                    size = len(plain).to_bytes(2, "little")
                    iv = b"\x00" * 4 + event_nonce.to_bytes(8, "little")
                    event.sendall(size + ChaCha20Poly1305(event_write).encrypt(iv, plain, size))
                    size = event_reader.read(2)
                    assert len(size) == 2
                    sealed = event_reader.read(int.from_bytes(size, "little") + 16)
                    reply = ChaCha20Poly1305(event_read).decrypt(iv, sealed, size)
                    assert status in reply and f"CSeq: {event_nonce+1}".encode() in reply
                for nonce, kind in enumerate((130, 111, 110), start=2):
                    stream = {"type": kind}
                    if kind == 130:
                        stream["seed"] = 31
                    else:
                        stream["streamConnectionID"] = kind + 1
                    response = control.setup(nonce + 6, {"streams": [stream]})
                    reply_stream = response["streams"][0]
                    assert reply_stream["type"] == kind
                    if kind != 130:
                        assert reply_stream["streamConnectionID"] == kind + 1
                    with socket.create_connection(("127.0.0.1",
                                                   reply_stream["dataPort"]), timeout=3) as media:
                        if kind == 130:
                            body = b"\xff\x5a\x01\x02"
                            package = bytearray(32 + len(body))
                            package[:4] = len(package).to_bytes(4, "big")
                            package[16:20] = b"comm"
                            package[32:] = body
                            key = derive(shared, "DataStream-Salt31",
                                         "DataStream-Output-Encryption-Key")
                            size = len(package).to_bytes(2, "little")
                            sealed = ChaCha20Poly1305(key).encrypt(
                                b"\x00" * 12, package, size)
                            media.sendall(size + sealed)
                        else:
                            codec = b"\x01\x02\x03\x04"
                            config_header = bytearray(128)
                            config_header[:4] = len(codec).to_bytes(4, "little")
                            config_header[4] = 1
                            media.sendall(config_header + codec)
                            key = derive(shared, f"DataStream-Salt{kind + 1}",
                                         "DataStream-Output-Encryption-Key")
                            nalu = b"\x26\x01" + bytes(100000)
                            frame = len(nalu).to_bytes(4, "big") + nalu
                            header = bytearray(128)
                            header[:4] = (len(frame) + 16).to_bytes(4, "little")
                            encrypted = ChaCha20Poly1305(key).encrypt(
                                b"\x00" * 12, frame, header)
                            media.sendall(header + encrypted)
                control.request("RECORD", "/stream", 11)
                control.request("RECORD", "/stream", 107)
                control.request("RECORD", "/stream", 111, b"invalid", expected_status=b"400 Bad Request")
                # The private capture stops at 64 KiB, but the authenticated
                # control session must continue accepting later requests.
                for sequence, body in ((12, bytes(60000)),
                                       (13, bytes(5000)),
                                       (14, b"alive")):
                    control.request("POST", "/unsupported", sequence, body,
                                    expected_status=b"501 Not Implemented")
                # Exercise late diversity after private early samples are full.
                # Main/event share command coverage but retain separate results.
                event_client = EncryptedClient(event, event_reader, event_read, event_write)
                event_client.sent_nonce = event_client.read_nonce = 8
                for index in range(49):
                    unknown = plistlib.dumps({"type": f"lateCommand{index}",
                        "params": {"privateValue": "synthetic-not-for-reporting"}},
                        fmt=plistlib.FMT_BINARY)
                    control.request("POST", "/command", 200 + index, unknown,
                                    expected_status=b"501 Not Implemented")
                    event_client.request("POST", "/command", 300 + index, unknown,
                                         expected_status=b"501 Not Implemented")
                    path = f"/late/{index}"
                    control.request("POST", path, 400 + index,
                                    expected_status=b"501 Not Implemented")
                    event_client.request("POST", path, 500 + index,
                                         expected_status=b"501 Not Implemented")
                # Give the receiver's nonblocking media sockets time to drain
                # both large frames before this synthetic control peer exits.
                time.sleep(0.5)
                invalid_selection = plistlib.dumps({"streams": [
                    {"type": 130, "streamID": 999}]}, fmt=plistlib.FMT_BINARY)
                control.request("TEARDOWN", "/stream", 15, invalid_selection,
                                expected_status=b"400 Bad Request")
                vehicle_stream = control.setup(17, {"streams": [{
                    "type": 130, "seed": 32,
                    "clientTypeUUID": "3E2F3C61-AAD0-42CB-A8AA-BF22186DA62E"}]})["streams"][0]
                before = wait_publication(presentation_file)
                assert before["state"] == "starting" and before["negotiatedMode"] == "standard"
                selection = plistlib.dumps({"streams": [{"type": 130,
                    "streamID": vehicle_stream["streamID"]}]}, fmt=plistlib.FMT_BINARY)
                dispatched_ns = time.monotonic_ns()
                control.request("TEARDOWN", "/stream", 18, selection)
                after = wait_publication(presentation_file, int(before["sequence"]), dispatched_ns)
                assert after["connectionGeneration"] == before["connectionGeneration"]
                assert after["state"] == "starting" and after["negotiatedMode"] == "standard", after
                assert not after["returnReady"]  # Synthetic packets are no presentation proof.
                assert len(plistlib.loads(control.request("GET", "/info", 19))["displays"]) == 2
                # A rejected or selective close must leave control and its
                # presentation owner usable. Full teardown
                # replies before closing the control and all media sockets.
                dispatched_ns = time.monotonic_ns()
                control.request("TEARDOWN", "/stream", 16)
                assert reader.read(1) == b""
                retired = wait_publication(presentation_file, int(after["sequence"]), dispatched_ns)
                assert retired["state"] == "disconnected" and not retired["returnReady"]
                event_reader.close()
                event.close()
            # A saved identifier alone must not grant a new connection access.
            with socket.create_connection(("127.0.0.1", 7001), timeout=3) as conn:
                conn.settimeout(3)
                reader = conn.makefile("rb")
                attacker = x25519.X25519PrivateKey.generate()
                attacker_public = attacker.public_key().public_bytes(
                    serialization.Encoding.Raw,
                    serialization.PublicFormat.Raw)
                pv2 = request(conn, reader, 7, encode_tlv(
                    [(6, b"\x01"), (3, attacker_public)]), path="/pair-verify")
                shared = attacker.exchange(
                    x25519.X25519PublicKey.from_public_bytes(pv2[3]))
                pv_key = derive(shared, "Pair-Verify-Encrypt-Salt",
                                "Pair-Verify-Encrypt-Info")
                bad_signature = bytes(64)
                bad_proof = encode_tlv([(1, phone_id), (0x0a, bad_signature)])
                encrypted_bad_proof = ChaCha20Poly1305(pv_key).encrypt(
                    b"\x00" * 4 + b"PV-Msg03", bad_proof, None)
                rejected = request(conn, reader, 8, encode_tlv(
                    [(6, b"\x03"), (5, encrypted_bad_proof)]),
                    path="/pair-verify", expected_status=b"400 Bad Request")
                assert not rejected
            with socket.create_connection(("127.0.0.1", 7001), timeout=3) as conn:
                conn.settimeout(3)
                reader = conn.makefile("rb")
                m2 = request(conn, reader, 9, encode_tlv(
                    [(6, b"\x01"), (0, b"\x00"), (0x13, b"\x10")]))
                public_a, proof, expected_reply, session_key = controller_proof(m2)
                m4 = request(conn, reader, 10, encode_tlv(
                    [(6, b"\x03"), (3, public_a), (4, proof)]))
                assert m4[6] == b"\x04" and m4[4] == expected_reply
                phone_control = derive(session_key, "Control-Salt",
                                       "Control-Write-Encryption-Key")
                receiver_control = derive(session_key, "Control-Salt",
                                          "Control-Read-Encryption-Key")
                control = EncryptedClient(conn, reader, receiver_control, phone_control)
                info = plistlib.loads(control.request("GET", "/info", 11))
                assert isinstance(info, dict) and len(info["displays"]) == 2
                reader.close()  # Release makefile's descriptor before peer EOF.
            malformed_requests = (
                b"GET /info RTSP/1.0\r\nContent-Length: invalid\r\n\r\n",
                b"SETUP /stream RTSP/1.0\r\nContent-Length: 0\r\nContent-Length: 1\r\n\r\nx",
                b"GET\n/info RTSP/1.0\r\n\r\n",
                b"GET /info RTSP/1.0\r\nTransfer-Encoding: chunked\r\n\r\n",
            )
            for malformed_request in malformed_requests:
                with socket.create_connection(("127.0.0.1", 7001), timeout=3) as malformed:
                    malformed.settimeout(3)
                    malformed.sendall(malformed_request)
                    assert malformed.recv(1) == b""
            saved = os.listdir(pair_store)
            assert len(saved) == 1 and saved[0].endswith(".pair")
            stored_path = os.path.join(pair_store, saved[0])
            assert stat.S_IMODE(os.stat(stored_path).st_mode) == 0o600
            stored = open(stored_path, "rb").read()
            assert stored == b"HPR1" + bytes([len(phone_id)]) + phone_id + phone_public
            process.wait(timeout=15)
            output_thread.join(timeout=1)
            assert not output_thread.is_alive() and not receiver_truncated
            output = "".join(receiver_output)
            summaries = [line for line in output.splitlines() if line.startswith("connection=1 ")]
            assert len(summaries) == 1
            assert "video_configs=2 video_frames=2 iap_messages=1" in summaries[0], summaries[0]
            assert "event_requests=106" in summaries[0] and "iapNoticeDrops=0" in summaries[0]
            for scope in ("main-route", "event-route", "command"):
                rows = [line for line in output.splitlines()
                    if f"protocol coverage connection=1 scope={scope} " in line]
                assert any("unknownEvictions=18 diagnosticErrors=0" in line for line in rows), rows
                assert any("category=unknown" in line for line in rows)
            assert "synthetic-not-for-reporting" not in output and "/late/" not in output
            control_retirement = [line for line in output.splitlines()
                if "scope=control generation=1 stage=retired " in line]
            assert len(control_retirement) == 1 and " result=4 " in control_retirement[0]
            peer_retirement = [line for line in output.splitlines()
                if "scope=control generation=3 stage=retired " in line]
            assert len(peer_retirement) == 1 and " result=1 " in peer_retirement[0]
            for generation in range(4, 4 + len(malformed_requests)):
                invalid_retirement = [line for line in output.splitlines()
                    if f"scope=control generation={generation} stage=retired " in line]
                assert len(invalid_retirement) == 1 and " result=3 " in invalid_retirement[0]
            assert not any("scope=control generation=5 stage=" in line and
                           "setup-begin " in line for line in output.splitlines())
            feature_offer = [line for line in output.splitlines()
                if "scope=control generation=1 stage=setup-features " in line]
            assert len(feature_offer) == 1 and " result=4 " in feature_offer[0]
            print("pairing, authenticated control/event focus rejection and survival, SETUP, RECORD, selective presentation survival, full teardown retirement, and bad-signature rejection passed")
        finally:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=3)


if __name__ == "__main__":
    main()
