"""Non-interactive access to the ESP32 console.

Logs everything the board prints to a file (each line prefixed with host time) and
forwards lines written to a named pipe to the board's console.

    python tools/serial_bridge.py --log serial.log --cmd cmd.fifo [--reset]
    echo status > cmd.fifo

Run with PlatformIO's Python (~/.platformio/penv/bin/python), which has pyserial.
Only one program can hold the port, so close `pio device monitor` first. On macOS,
opening the port resets the board even with DTR/RTS held low, including when the
bridge reopens it after a USB drop.
"""

import argparse
import os
import sys
import threading
import time
from datetime import datetime

import serial


def open_port(name: str, baud: int) -> serial.Serial:
    """Open the port, retrying until it appears, without toggling DTR/RTS (which would reset the board)."""
    while True:
        port = serial.Serial()
        port.port, port.baudrate, port.timeout = name, baud, 0.1
        port.dtr = port.rts = False
        try:
            port.open()
            return port
        except serial.SerialException:
            time.sleep(1)


def log_line(log, text: str) -> None:
    """Append a line to the log, prefixed with host time."""
    stamp = datetime.now().strftime("%H:%M:%S.%f")[:-3]
    log.write(f"{stamp} {text}\n")


def forward_commands(ports: list, fifo_path: str) -> None:
    """Send each line written to the named pipe to the board's console, forever."""
    while True:
        # open() blocks until a writer appears; reopen after each writer closes
        with open(fifo_path, "r") as fifo:
            for line in fifo:
                try:
                    ports[0].write(line.rstrip("\r\n").encode() + b"\r")
                except serial.SerialException:
                    pass  # port is being reopened; the command is dropped


def main() -> None:
    """Open the port, start forwarding commands and log the board's output until interrupted."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="/dev/cu.usbserial-0001")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--log", required=True)
    parser.add_argument("--cmd", required=True, help="named pipe to read commands from")
    parser.add_argument("--reset", action="store_true", help="reset the board on start")
    args = parser.parse_args()

    if not os.path.exists(args.cmd):
        os.mkfifo(args.cmd)

    # Held in a list so the command thread sees the port after a reopen
    ports = [open_port(args.port, args.baud)]
    if args.reset:
        # Pulse EN via RTS; DTR stays released so GPIO0 is high and it boots normally
        ports[0].rts = True
        time.sleep(0.1)
        ports[0].rts = False

    threading.Thread(target=forward_commands, args=(ports, args.cmd), daemon=True).start()

    pending = b""
    with open(args.log, "a", buffering=1) as log:
        while True:
            try:
                pending += ports[0].read(4096)
            except serial.SerialException as e:
                # Seen when USB drops briefly (replug, or another program probing the port)
                log_line(log, f"[bridge] serial port lost ({e}), reopening")
                ports[0].close()
                ports[0] = open_port(args.port, args.baud)
                log_line(log, "[bridge] serial port reopened")
                continue
            *lines, pending = pending.split(b"\n")
            for line in lines:
                log_line(log, line.decode(errors="replace").rstrip())


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(0)
