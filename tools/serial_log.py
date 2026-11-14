#!/usr/bin/env python3
"""Capture ESP32 serial output to a timestamped log file.

Usage:
    /usr/bin/python3 tools/serial_log.py [port] [baud]

Defaults: port=/dev/cu.usbserial-0001 baud=115200
Writes to serial_log.txt (appends). Ctrl-C to stop.
"""
import sys
import time
import datetime

try:
    import serial
except ImportError:
    print("pyserial not installed. Run: /usr/bin/python3 -m pip install pyserial")
    sys.exit(1)

port = sys.argv[1] if len(sys.argv) > 1 else "/dev/cu.usbserial-0001"
baud = int(sys.argv[2]) if len(sys.argv) > 2 else 115200
logfile = "serial_log.txt"

print(f"Opening {port} @ {baud} ... (Ctrl-C to stop)")
print(f"Logging to {logfile}")

with serial.Serial(port, baud, timeout=1) as ser, open(logfile, "a") as f:
    f.write(f"\n===== session start {datetime.datetime.now().isoformat()} =====\n")
    f.flush()
    while True:
        try:
            line = ser.readline()
            if not line:
                continue
            text = line.decode("utf-8", errors="replace").rstrip("\r\n")
            stamp = datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]
            out = f"[{stamp}] {text}"
            print(out)
            f.write(out + "\n")
            f.flush()
        except KeyboardInterrupt:
            print("\nStopped.")
            break
        except Exception as e:
            print(f"Error: {e}")
            time.sleep(0.5)
