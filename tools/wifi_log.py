#!/usr/bin/env python3
"""Watch the ESP32 PATH clock log over WiFi (no USB needed).

Usage:
    ~/.platformio/penv/bin/python tools/wifi_log.py [ip] [port]

Defaults: ip=192.168.1.60 port=23
Ctrl-C to stop.
"""
import socket
import sys
import time

ip = sys.argv[1] if len(sys.argv) > 1 else "192.168.1.60"
port = int(sys.argv[2]) if len(sys.argv) > 2 else 23

print(f"Connecting to {ip}:{port} ... (Ctrl-C to stop)")
while True:
    try:
        s = socket.create_connection((ip, port), timeout=5)
        s.settimeout(30)
        print(f"Connected to {ip}:{port}\n")
        while True:
            try:
                data = s.recv(4096)
                if not data:
                    print("[connection closed, reconnecting...]")
                    break
                sys.stdout.write(data.decode("utf-8", errors="replace"))
                sys.stdout.flush()
            except socket.timeout:
                pass  # idle is fine; keep waiting
    except KeyboardInterrupt:
        print("\nStopped.")
        break
    except Exception as e:
        print(f"[{e}] retrying in 3s...")
        time.sleep(3)
