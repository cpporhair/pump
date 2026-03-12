#!/usr/bin/env python3
"""Simple test client for the PUMP inference server.

Protocol (over TCP with 4-byte length-prefix framing):
  Request:  [total_len:4] [prompt UTF-8]
  Response: [total_len:4] [flags:1] [payload]
    flags: 0=token, 1=eos, 2=error
"""

import codecs
import socket
import struct
import sys


def send_prompt(sock: socket.socket, prompt: str):
    payload = prompt.encode("utf-8")
    total_len = len(payload) + 4  # includes the 4-byte length field itself
    sock.sendall(struct.pack("<I", total_len) + payload)


def recv_frame(sock: socket.socket) -> bytes:
    """Receive one length-prefixed frame."""
    hdr = b""
    while len(hdr) < 4:
        chunk = sock.recv(4 - len(hdr))
        if not chunk:
            raise ConnectionError("connection closed")
        hdr += chunk
    total_len = struct.unpack("<I", hdr)[0]
    payload_len = total_len - 4

    payload = b""
    while len(payload) < payload_len:
        chunk = sock.recv(payload_len - len(payload))
        if not chunk:
            raise ConnectionError("connection closed")
        payload += chunk
    return payload


def recv_tokens(sock: socket.socket):
    """Receive streaming tokens until EOS.

    Uses an incremental UTF-8 decoder to correctly handle multi-byte
    characters split across BPE tokens (e.g. a 3-byte CJK char where
    bytes land in different tokens).
    """
    decoder = codecs.getincrementaldecoder("utf-8")("replace")
    while True:
        payload = recv_frame(sock)
        flags = payload[0]
        if flags == 0:  # TOKEN
            text = decoder.decode(payload[1:], final=False)
            if text:
                print(text, end="", flush=True)
        elif flags == 1:  # EOS
            # Flush any remaining bytes in decoder
            tail = decoder.decode(b"", final=True)
            if tail:
                print(tail, end="", flush=True)
            print()
            break
        elif flags == 2:  # ERROR
            print(f"\n[error] {payload[1:].decode('utf-8', errors='replace')}")
            break


def main():
    host = "127.0.0.1"
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8080

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect((host, port))
    print(f"Connected to {host}:{port}")

    try:
        while True:
            prompt = input("> ")
            if not prompt:
                continue
            send_prompt(sock, prompt)
            recv_tokens(sock)
    except (EOFError, KeyboardInterrupt):
        print()
    finally:
        sock.close()


if __name__ == "__main__":
    main()
