#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""PC 端接收上位机：监听 socket，按行打印板端上报的告警事件 JSON。
用法: python3 receiver.py [port]   (默认 9000)
"""
import socket
import json
import sys


def main(host="0.0.0.0", port=9000):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((host, port))
    srv.listen(1)
    print(f"[receiver] listening on {host}:{port} ...")
    while True:
        conn, addr = srv.accept()
        print(f"[receiver] connected: {addr}")
        buf = b""
        try:
            while True:
                data = conn.recv(4096)
                if not data:
                    break
                buf += data
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    line = line.strip()
                    if not line:
                        continue
                    try:
                        ev = json.loads(line)
                        print("[event]", json.dumps(ev, ensure_ascii=False))
                    except Exception as e:
                        print("[bad-line]", line, e)
        except Exception as e:
            print("[conn-error]", e)
        finally:
            conn.close()
            print("[receiver] closed:", addr)


if __name__ == "__main__":
    p = int(sys.argv[1]) if len(sys.argv) > 1 else 9000
    main(port=p)
