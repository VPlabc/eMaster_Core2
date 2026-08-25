#!/usr/bin/env python3
"""Small C3/InBio TCP panel simulator for gateway integration tests.

The simulator intentionally implements the wire format used by
src/zk_controller/C3Codec.cpp.  It is dependency-free and is suitable for
running on the Ubuntu test machine:

    python3 scripts/fake_c3_panel.py --port 4370

The default panel accepts both CONNECT_SESSION and CONNECT_SESSION_LESS.
RTLOG replies are empty until a card is entered at the console:

    card 123456 1001

The event is returned once on the next RTLOG poll and then consumed. Control
commands are acknowledged without operating real hardware.
"""

import argparse
import logging
import queue
import socket
import struct
import sys
import threading
import time
from typing import Optional, Tuple


START = 0xAA
END = 0x55
VERSION = 0x01
CONNECT_LESS = 0x01
DISCONNECT = 0x02
GET_PARAM = 0x04
CONTROL = 0x05
TABLE_CFG = 0x06
GET_DATA = 0x08
RTLOG_BINARY = 0x0B
CONNECT = 0x76
RTLOG_KEYVALUE = 0x79
OK = 0xC8
ERROR = 0xC9


def crc16(data: bytes) -> int:
    crc = 0
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = ((crc >> 1) ^ 0xA001) if crc & 1 else crc >> 1
    return crc


def frame(command: int, payload: bytes = b"", session: Optional[Tuple[int, int]] = None) -> bytes:
    session_bytes = b"" if session is None else struct.pack("<HH", session[0], session[1] & 0xFFFF)
    body = struct.pack("<BBH", VERSION, command, len(session_bytes) + len(payload))
    body += session_bytes + payload
    return bytes((START,)) + body + struct.pack("<H", crc16(body)) + bytes((END,))


def read_frame(buffer: bytes) -> Tuple[Optional[Tuple[int, bytes, bytes]], bytes]:
    """Return (command, body-after-header, complete-frame), or wait for data."""
    if len(buffer) < 5:
        return None, buffer
    if buffer[0] != START:
        start = buffer.find(bytes((START,)))
        buffer = b"" if start < 0 else buffer[start:]
        if len(buffer) < 5:
            return None, buffer
    length = struct.unpack_from("<H", buffer, 3)[0]
    size = 5 + length + 3
    if len(buffer) < size:
        return None, buffer
    raw = buffer[:size]
    if raw[-1] != END or crc16(raw[1:5 + length]) != struct.unpack_from("<H", raw, 5 + length)[0]:
        logging.warning("discarding malformed frame: %s", raw.hex(" "))
        return None, buffer[1:]
    return (raw[2], raw[5:5 + length], raw), buffer[size:]


def c3_time(timestamp: Optional[float] = None) -> int:
    """Encode a timestamp using the panel's packed 2000-based C3 format."""
    value = time.localtime(timestamp)
    days = (value.tm_year - 2000) * 12 * 31 + (value.tm_mon - 1) * 31 + (value.tm_mday - 1)
    return (((days * 24 + value.tm_hour) * 60 + value.tm_min) * 60 + value.tm_sec)


class FakePanel:
    def __init__(self, password: str, rtlog_mode: str):
        self.password = password
        self.rtlog_mode = rtlog_mode
        self.session_id = 0x1234
        self.cards = queue.Queue()

    def add_card(self, card_no: int, pin: int, door: int = 1) -> None:
        self.cards.put((card_no, pin, door))

    def reply(self, command: int, body: bytes) -> Tuple[bytes, bool]:
        # CONNECT_SESSION includes the pre-session block and password.
        if command == CONNECT:
            password = body[4:].decode("ascii", "replace") if len(body) >= 4 else ""
            if password != self.password:
                return frame(ERROR), True
            return frame(OK, struct.pack("<H", self.session_id)), False

        if command == CONNECT_LESS:
            if body.decode("ascii", "replace") != self.password:
                return frame(ERROR), True
            return frame(OK), False

        # All post-connect session frames carry session id and request number.
        session = None
        payload = body
        if len(body) >= 4:
            session = struct.unpack_from("<HH", body)[0], struct.unpack_from("<HH", body)[1]
            payload = body[4:]

        if command == DISCONNECT:
            return frame(OK), True
        if command == CONTROL:
            logging.info("CONTROL payload=%s", payload.hex(" "))
            return frame(OK, session=session), False
        if command == GET_PARAM:
            requested = payload.decode("ascii", "replace")
            values = {
                "~SerialNumber": "FAKE-C3-0001",
                "~ZKFPVersion": "FakeC3/1.0",
                "Lock": "2",
                "Device": "Fake C3 Panel",
            }
            response = ",".join(f"{key}={values.get(key, '0')}" for key in requested.split(",") if key)
            return frame(OK, response.encode(), session), False
        if command == TABLE_CFG:
            response = b"user=0,name=s1,cardno=L2,pin=i3\n"
            return frame(OK, response, session), False
        if command == GET_DATA:
            if len(payload) < 2:
                return frame(ERROR, session=session), False
            table, count = payload[0], payload[1]
            indexes = payload[2:2 + count]
            response = bytes((table, count)) + indexes
            values = {2: struct.pack("<I", 123456), 3: struct.pack("<I", 1001)}
            for index in indexes:
                value = values.get(index, b"0")
                response += bytes((len(value),)) + value
            return frame(OK, response, session), False
        if command == RTLOG_BINARY:
            if self.rtlog_mode == "keyvalue":
                # A non-16-byte successful payload is the protocol's signal to
                # switch the client to RTLOG_KEYVALUE mode.
                return frame(OK, b"keyvalue", session), False
            try:
                card_no, pin, door = self.cards.get_nowait()
            except queue.Empty:
                return frame(OK, b"", session), False
            record = struct.pack("<IIBBBBBI", card_no, pin, 1, door, 3, 0, 0, c3_time())
            return frame(OK, record, session), False
        if command == RTLOG_KEYVALUE:
            try:
                card_no, pin, door = self.cards.get_nowait()
            except queue.Empty:
                return frame(OK, b"", session), False
            response = (f"time=2026-01-01 12:00:00,pin={pin},cardno={card_no},"
                        f"door={door},eventtype=3,inoutstate=0,verified=1").encode()
            return frame(OK, response, session), False

        logging.warning("unknown command 0x%02x", command)
        return frame(ERROR, session=session), False


def serve(args: argparse.Namespace) -> None:
    panel = FakePanel(args.password, args.rtlog_mode)

    def console_input() -> None:
        logging.info("enter 'card <card_number> [pin] [door]' to inject a card event")
        for line in sys.stdin:
            fields = line.strip().split()
            if not fields:
                continue
            if fields[0].lower() in ("card", "c") and len(fields) >= 2:
                try:
                    card_no = int(fields[1])
                    pin = int(fields[2]) if len(fields) >= 3 else card_no
                    door = int(fields[3]) if len(fields) >= 4 else 1
                    panel.add_card(card_no, pin, door)
                    logging.info("queued card=%d pin=%d door=%d", card_no, pin, door)
                except ValueError:
                    logging.error("usage: card <card_number> [pin] [door]")
            elif fields[0].lower() in ("help", "?"):
                logging.info("usage: card <card_number> [pin] [door]")

    threading.Thread(target=console_input, name="card-input", daemon=True).start()
    with socket.create_server((args.host, args.port), reuse_port=False) as server:
        logging.info("fake C3 panel listening on %s:%d", args.host, args.port)
        while True:
            client, address = server.accept()
            logging.info("client connected: %s:%d", *address[:2])
            client.settimeout(args.timeout)
            buffer = b""
            try:
                while True:
                    chunk = client.recv(4096)
                    if not chunk:
                        break
                    buffer += chunk
                    close = False
                    while True:
                        parsed, buffer = read_frame(buffer)
                        if parsed is None:
                            break
                        command, body, raw = parsed
                        logging.info("request command=0x%02x bytes=%d", command, len(raw))
                        response, close = panel.reply(command, body)
                        client.sendall(response)
                        if close:
                            break
                    if close:
                        break
            except (ConnectionError, socket.timeout) as error:
                logging.info("client ended: %s", error)
            finally:
                client.close()
                logging.info("client disconnected")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=4370)
    parser.add_argument("--password", default="")
    parser.add_argument("--rtlog-mode", choices=("binary", "keyvalue"), default="binary")
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()
    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO,
                        format="%(asctime)s %(levelname)s %(message)s")
    try:
        serve(args)
    except KeyboardInterrupt:
        logging.info("stopped")


if __name__ == "__main__":
    main()