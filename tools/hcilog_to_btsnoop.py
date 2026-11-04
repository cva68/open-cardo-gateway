"""Convert the firmware's `hcilog` dump into a btsnoop file for Wireshark.

    python tools/hcilog_to_btsnoop.py serial.log capture.btsnoop

Reads the last "--- HCI log start ---" ... "--- HCI log end ---" block in the log
(host timestamps from serial_bridge.py are ignored). Each HCI line looks like
`NN C:<8-byte little-endian µs timestamp> <packet bytes>`. SCO packets (S:) carry no
direction in the dump, so they are skipped.

Stands in for ESP-IDF's tools/bt/bt_hci_to_btsnoop, which needs extra packages.
"""

import re
import struct
import sys

# H4 packet type and btsnoop direction (0 sent by host, 1 received) for each tag
TAGS = {
    "C": (0x01, 0),  # command
    "E": (0x04, 1),  # event
    "H": (0x02, 0),  # ACL host to controller
    "D": (0x02, 1),  # ACL controller to host
}
LINE = re.compile(r"^(?:[\d:.]+ )?[0-9a-f]{2} ([A-Z]):((?:[0-9a-f]{2} ?)+)$")
# btsnoop timestamps count µs from 0000-01-01; this shifts boot time to the Unix epoch
BTSNOOP_EPOCH_DELTA_US = 0x00DCDDB30F2F8000


def last_block(lines: list[str]) -> list[str]:
    """Return the lines of the last HCI log dump in the serial log."""
    starts = [i for i, l in enumerate(lines) if "--- HCI log start ---" in l]
    if not starts:
        sys.exit("No HCI log block found; run `hcilog` on the board first")
    block = lines[starts[-1] + 1:]
    end = next((i for i, l in enumerate(block) if "--- HCI log end ---" in l), len(block))
    return block[:end]


def main() -> None:
    """Write the last dump's HCI packets to a btsnoop file."""
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    with open(sys.argv[1], errors="replace") as f:
        lines = [l.rstrip() for l in f]

    packets = 0
    with open(sys.argv[2], "wb") as out:
        out.write(b"btsnoop\x00" + struct.pack(">II", 1, 1002))  # version 1, H4 datalink
        for line in last_block(lines):
            m = LINE.match(line)
            if not m or m.group(1) not in TAGS:
                continue
            h4_type, direction = TAGS[m.group(1)]
            raw = bytes.fromhex(m.group(2))
            if len(raw) <= 8:
                continue
            timestamp_us = int.from_bytes(raw[:8], "little")
            packet = bytes([h4_type]) + raw[8:]
            flags = direction | (2 if h4_type in (0x01, 0x04) else 0)
            out.write(struct.pack(">IIIIQ", len(packet), len(packet), flags, 0,
                                  timestamp_us + BTSNOOP_EPOCH_DELTA_US))
            out.write(packet)
            packets += 1
    print(f"Wrote {packets} packets to {sys.argv[2]}")


if __name__ == "__main__":
    main()
