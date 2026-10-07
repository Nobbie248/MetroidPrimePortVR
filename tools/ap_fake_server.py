#!/usr/bin/env python3
"""Run a dependency-free Archipelago test server for Metroid Prime Port.

Write an ``archipelago.json`` with ``server`` set to
``ws://127.0.0.1:38281``, ``slot`` set to ``Player1``, and ``items`` mapping
the item IDs passed with ``--item`` to retail item names (for example
``{"1234":{"item":"EnergyTanks"},"5678":{"item":"Missiles"}}``).
Then launch the port with ``MP_AP_CONFIG=/path/to/archipelago.json``; use
``MP_USER_PATH`` to choose the default config/state directory when
``MP_AP_CONFIG`` is unset (with an explicit config, state is stored beside it),
or ``MP_AP_SEND_ALL=1`` to send every configured location check after
connecting.

With ``--tls --cert FILE --key FILE`` the server speaks ``wss://`` instead;
point ``tls_ca`` in ``archipelago.json`` at the CA that signed the certificate.
"""

import argparse
import base64
import hashlib
import json
import socket
import ssl
import struct
import sys
import time
import zlib


GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

# Per-connection deflate state for clients that negotiated permessage-deflate.
COMPRESSORS = {}


def recv_exact(sock, size):
    chunks = bytearray()
    while len(chunks) < size:
        data = sock.recv(size - len(chunks))
        if not data:
            return None
        chunks.extend(data)
    return bytes(chunks)


def send_frame(sock, opcode, payload):
    payload = payload if isinstance(payload, bytes) else payload.encode("utf-8")
    first = 0x80 | opcode
    # permessage-deflate (RFC 7692): data messages go out compressed with RSV1
    # set, sharing one window across the connection, as MultiServer's do.
    compressor = COMPRESSORS.get(sock)
    if compressor is not None and opcode in (1, 2):
        payload = compressor.compress(payload) + compressor.flush(zlib.Z_SYNC_FLUSH)
        payload = payload[:-4]
        first |= 0x40
    header = bytearray([first])
    length = len(payload)
    if length < 126:
        header.append(length)
    elif length <= 0xFFFF:
        header.append(126)
        header.extend(struct.pack("!H", length))
    else:
        header.append(127)
        header.extend(struct.pack("!Q", length))
    sock.sendall(header + payload)


def send_json(sock, packet):
    send_frame(sock, 1, json.dumps(packet, separators=(",", ":")))


def read_frame(sock):
    header = recv_exact(sock, 2)
    if header is None:
        return None
    first, second = header
    opcode = first & 0x0F
    length = second & 0x7F
    if length == 126:
        extended = recv_exact(sock, 2)
        if extended is None:
            return None
        length = struct.unpack("!H", extended)[0]
    elif length == 127:
        extended = recv_exact(sock, 8)
        if extended is None:
            return None
        length = struct.unpack("!Q", extended)[0]
    masked = bool(second & 0x80)
    mask = recv_exact(sock, 4) if masked else b""
    if masked and mask is None:
        return None
    payload = recv_exact(sock, length)
    if payload is None:
        return None
    if masked:
        payload = bytes(value ^ mask[index & 3] for index, value in enumerate(payload))
    return opcode, payload


def websocket_upgrade(sock, allow_deflate=True):
    request = bytearray()
    while b"\r\n\r\n" not in request:
        data = sock.recv(4096)
        if not data:
            return False
        request.extend(data)
        if len(request) > 16384:
            return False
    headers = {}
    for line in bytes(request).split(b"\r\n")[1:]:
        if not line:
            break
        if b":" in line:
            key, value = line.split(b":", 1)
            headers[key.strip().lower()] = value.strip()
    key = headers.get(b"sec-websocket-key")
    if key is None:
        return False
    accept = base64.b64encode(hashlib.sha1(key + GUID.encode("ascii")).digest()).decode("ascii")
    offered = [part.split(b";")[0].strip().lower()
               for part in headers.get(b"sec-websocket-extensions", b"").split(b",")]
    deflate = allow_deflate and b"permessage-deflate" in offered
    response = (
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        + ("Sec-WebSocket-Extensions: permessage-deflate\r\n" if deflate else "")
        + f"Sec-WebSocket-Accept: {accept}\r\n\r\n"
    )
    sock.sendall(response.encode("ascii"))
    if deflate:
        COMPRESSORS[sock] = zlib.compressobj(6, zlib.DEFLATED, -15)
    else:
        # MultiServer's notice for a client that doesn't offer compression.
        print("[server] client does not support compressed websocket connections", flush=True)
    return True


def packet_command(packet):
    return packet.get("cmd", "?") if isinstance(packet, dict) else "?"


def handle_client(sock, address, args, item_ids, bounce_sources=()):
    try:
        handle_session(sock, address, args, item_ids, bounce_sources)
    finally:
        COMPRESSORS.pop(sock, None)


def handle_session(sock, address, args, item_ids, bounce_sources):
    with sock:
        sock.settimeout(None)
        if not websocket_upgrade(sock, not args.no_deflate):
            print(f"[server] rejected invalid WebSocket upgrade from {address}", flush=True)
            return
        if sock in COMPRESSORS:
            print("[server] permessage-deflate on", flush=True)
        send_json(sock, {
            "cmd": "RoomInfo",
            "version": {"major": 0, "minor": 6, "build": 8},
            "password": False,
            "games": ["Metroid Prime"],
            "seed_name": "Metroid Prime Port fake seed",
        })

        while True:
            frame = read_frame(sock)
            if frame is None:
                return
            opcode, payload = frame
            if opcode == 8:
                return
            if opcode == 9:
                send_frame(sock, 10, payload)
                continue
            if opcode != 1:
                continue
            try:
                packet_data = json.loads(payload.decode("utf-8"))
            except (UnicodeDecodeError, json.JSONDecodeError) as error:
                print(f"[server] invalid client JSON: {error}", flush=True)
                continue
            if not isinstance(packet_data, list):
                # MultiServer iterates the message as a list of commands and
                # drops the client on anything else.
                print("[server] client sent a bare JSON object, not a list; closing", flush=True)
                return
            packets = packet_data
            for packet in packets:
                print(f"[server] {packet_command(packet)} {json.dumps(packet, separators=(',', ':'))}",
                      flush=True)
                # Sync asks for the whole inventory again, as a real server does.
                if packet_command(packet) == "Sync":
                    send_json(sock, {
                        "cmd": "ReceivedItems",
                        "index": 0,
                        "items": [[item_id, 0, 1, 0] for item_id in item_ids],
                    })
                    continue
                # Say: chat is echoed to everyone, as a real server does; a
                # command gets a two-line CommandResult, like !help's reply.
                if packet_command(packet) == "Say":
                    text = str(packet.get("text", ""))
                    if text.startswith("!"):
                        send_json(sock, {"cmd": "PrintJSON", "type": "CommandResult",
                                         "data": [{"text": f"Fake server got {text}\n"
                                                           "and has no commands."}]})
                    else:
                        send_json(sock, {"cmd": "PrintJSON", "type": "Chat", "team": 0,
                                         "slot": 1, "message": text,
                                         "data": [{"text": f"{args.slot}: {text}"}]})
                    continue
                # LocationScouts: answer from --scouts; locations it doesn't
                # name are left out, as if the server didn't know them.
                if packet_command(packet) == "LocationScouts":
                    send_json(sock, {"cmd": "LocationInfo", "locations": [
                        {"item": item, "location": location, "player": player, "flags": flags}
                        for location, (item, player, flags) in args.scouts.items()
                        if location in packet.get("locations", [])
                    ]})
                    continue
                # Get: data storage holds only this slot's hints (--hints).
                if packet_command(packet) == "Get":
                    keys = packet.get("keys", [])
                    send_json(sock, {"cmd": "Retrieved", "keys": {
                        key: (args.hints if key == "_read_hints_0_1" else None) for key in keys
                    }})
                    continue
                if packet_command(packet) != "Connect":
                    continue
                name = packet.get("name", "") if isinstance(packet, dict) else ""
                if name != args.slot:
                    send_json(sock, {"cmd": "ConnectionRefused", "errors": [
                        f"Unknown slot {name!r}; expected {args.slot!r}"
                    ]})
                    continue
                send_json(sock, {
                    "cmd": "Connected",
                    "team": 0,
                    "slot": 1,
                    # Slot 2 is another game's player, for --scouts and --hints.
                    "players": [{"team": 0, "slot": 1, "alias": args.slot, "name": args.slot},
                                {"team": 0, "slot": 2, "alias": "Bob", "name": "Bob"}],
                    "checked_locations": [],
                    "missing_locations": [],
                    "slot_data": args.slot_data,
                })
                send_json(sock, {
                    "cmd": "ReceivedItems",
                    "index": 0,
                    "items": [[item_id, 0, 1, 0] for item_id in item_ids],
                })
                # DeathLink: a bounce from another player, so the client's
                # reaction to one can be exercised without a second session.
                # It is sent after the items, which is when a real server would
                # deliver it too, and in the shape a real server relays one: a
                # Bounced tagged DeathLink whose source is the player's name.
                for bounce_source in bounce_sources:
                    send_json(sock, {
                        "cmd": "Bounced",
                        "tags": ["DeathLink"],
                        "data": {"time": time.time(), "source": bounce_source,
                                 "cause": f"{bounce_source} died"},
                    })


def parse_item_ids(text):
    if not text:
        return []
    try:
        return [int(part, 10) for part in text.split(":")]
    except ValueError as error:
        raise argparse.ArgumentTypeError("item IDs must be colon-separated decimal integers") from error


def parse_scouts(text):
    scouts = {}
    try:
        for entry in filter(None, text.split(",")):
            location, target = entry.split("=")
            target, _, flags = target.partition(":")
            item, _, player = target.partition("@")
            scouts[int(location, 10)] = (int(item, 10), int(player or "1", 10), int(flags or "0", 10))
    except ValueError as error:
        raise argparse.ArgumentTypeError("scouts must look like LOC=ITEM[@PLAYER][:FLAGS],...") from error
    return scouts


def parse_hints(text):
    hints = []
    try:
        for entry in filter(None, text.split(",")):
            item, target = entry.split("=")
            location, _, player = target.partition("@")
            hints.append({"receiving_player": 1, "finding_player": int(player or "1", 10),
                          "location": int(location, 10), "item": int(item, 10), "found": False,
                          "entrance": "", "item_flags": 1})
    except ValueError as error:
        raise argparse.ArgumentTypeError("hints must look like ITEM=LOC[@PLAYER],...") from error
    return hints


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1", help="interface to listen on (default: %(default)s)")
    parser.add_argument("--port", type=int, default=38281, help="TCP port (default: %(default)s)")
    parser.add_argument("--slot", default="Player1", help="accepted slot name (default: %(default)s)")
    parser.add_argument("--bounce", metavar="NAME", action="append", default=[],
                        help="send a DeathLink bounce from this player name after the items "
                             "(repeat for several)")
    parser.add_argument("--item", type=parse_item_ids, default=parse_item_ids("1234:5678"),
                        metavar="ID[:ID...]", help="item IDs sent in ReceivedItems (default: 1234:5678)")
    parser.add_argument("--slot-data", type=json.loads, default={}, metavar="JSON",
                        help="slot_data object sent in Connected (default: {})")
    parser.add_argument("--scouts", type=parse_scouts, default={}, metavar="LOC=ITEM[@PLAYER][:FLAGS],...",
                        help="LocationInfo answers to LocationScouts; PLAYER defaults to 1 (this "
                             "slot), any other player is another game; FLAGS (default 0) are "
                             "the item's classification bits (1 progression, 2 useful, 4 trap)")
    parser.add_argument("--hints", type=parse_hints, default=[], metavar="ITEM=LOC[@PLAYER],...",
                        help="this slot's hints, returned for its _read_hints key; PLAYER (default "
                             "1) is who finds the item, 2 is Bob")
    parser.add_argument("--no-deflate", action="store_true",
                        help="refuse permessage-deflate and send uncompressed messages")
    parser.add_argument("--tls", action="store_true", help="serve wss:// (requires --cert and --key)")
    parser.add_argument("--cert", metavar="FILE", help="PEM server certificate chain for --tls")
    parser.add_argument("--key", metavar="FILE", help="PEM private key for --tls")
    args = parser.parse_args()

    context = None
    if args.tls:
        if not args.cert or not args.key:
            parser.error("--tls requires --cert and --key")
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(args.cert, args.key)
    elif args.cert or args.key:
        parser.error("--cert and --key only apply with --tls")

    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server:
        server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server.bind((args.host, args.port))
        server.listen()
        scheme = "wss" if context is not None else "ws"
        print(f"[server] listening on {scheme}://{args.host}:{args.port}/ (slot {args.slot})", flush=True)
        try:
            while True:
                client, address = server.accept()
                if context is not None:
                    try:
                        client.settimeout(10)
                        client = context.wrap_socket(client, server_side=True)
                    except (ssl.SSLError, OSError) as error:
                        # A client that rejects our certificate lands here.
                        print(f"[server] TLS handshake with {address} failed: {error}", flush=True)
                        client.close()
                        continue
                handle_client(client, address, args, args.item, args.bounce)
        except KeyboardInterrupt:
            print("\n[server] shutting down", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
