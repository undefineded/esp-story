#!/usr/bin/env python3
"""Bounded serial capture for board sessions.

Usage:
    python tools/serial_capture.py <port> <seconds> [baud] [outfile] [--reset]

Reads the port for the given number of seconds, echoes everything to stdout and
optionally appends it to outfile. Unlike `idf.py monitor` it exits on its own, so
it works from a script or an agent session.

--reset pulses DTR/RTS first, which on USB-Serial-JTAG boards restarts the chip,
so the capture starts at the ROM boot messages (timestamps from 0).

Notes:
  - Do not pipe the output into Tee-Object with the same path as `outfile`; the
    file will be locked and the final write raises PermissionError.
  - The port is exclusive: close `idf.py monitor` before capturing.
"""
import sys
import time

import serial

if len(sys.argv) < 3:
    sys.exit(__doc__)

port = sys.argv[1]
seconds = float(sys.argv[2])
rest = sys.argv[3:]
do_reset = "--reset" in rest
rest = [item for item in rest if item != "--reset"]
baud = int(rest[0]) if len(rest) > 0 else 115200
outfile = rest[1] if len(rest) > 1 else None

ser = serial.Serial(port, baud, timeout=0.2)
if do_reset:
    ser.dtr = True
    ser.rts = True
    time.sleep(0.15)
    ser.dtr = False
    ser.rts = False
    time.sleep(0.15)
ser.reset_input_buffer()

deadline = time.time() + seconds
chunks = []
try:
    while time.time() < deadline:
        data = ser.read(8192)
        if data:
            text = data.decode("utf-8", "replace")
            chunks.append(text)
            sys.stdout.write(text)
            sys.stdout.flush()
finally:
    ser.close()
    if outfile:
        with open(outfile, "a", encoding="utf-8") as handle:
            handle.write("".join(chunks))
