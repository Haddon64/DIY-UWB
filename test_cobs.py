"""
DIY-UWB COBS serial bridge test.

Tests bidirectional COBS communication between two DWM3001CDK units.

Usage:
    python test_cobs.py

Requires: pip install pyserial cobs
"""

import serial
import struct
import time
import threading
from cobs import cobs

INITIATOR_PORT = "COM5"
RESPONDER_PORT = "COM7"
BAUD = 115200


def cobs_encode_frame(data: bytes) -> bytes:
    """COBS-encode data and append 0x00 delimiter."""
    return cobs.encode(data) + b'\x00'


def cobs_decode_frame(frame: bytes) -> bytes:
    """COBS-decode a frame (without the 0x00 delimiter)."""
    return cobs.decode(frame)


def parse_output_frame(raw: bytes) -> dict:
    """Parse a decoded COBS output frame:
    [type:1][range_cm:4 LE i32][plen:1][payload:0-64]
    """
    if len(raw) < 6:
        return None
    msg_type = raw[0]
    range_cm = struct.unpack_from('<i', raw, 1)[0]
    plen = raw[5]
    payload = raw[6:6 + plen] if plen > 0 else b''
    return {
        'type': 'initiator' if msg_type == 0x01 else 'responder',
        'range_cm': range_cm,
        'range_m': range_cm / 100.0 if range_cm >= 0 else None,
        'payload': payload,
        'payload_str': payload.decode('utf-8', errors='replace'),
    }


def read_cobs_frames(ser: serial.Serial, timeout: float = 5.0) -> list:
    """Read COBS frames from serial port until timeout."""
    frames = []
    buf = bytearray()
    end_time = time.time() + timeout

    while time.time() < end_time:
        data = ser.read(ser.in_waiting or 1)
        if not data:
            continue
        for byte in data:
            if byte == 0x00:
                if len(buf) > 0:
                    try:
                        decoded = cobs_decode_frame(bytes(buf))
                        parsed = parse_output_frame(decoded)
                        if parsed:
                            frames.append(parsed)
                    except Exception as e:
                        print(f"  COBS decode error: {e} (buf={buf.hex()})")
                    buf.clear()
            else:
                buf.append(byte)

    return frames


def test_initiator_to_responder():
    """Send COBS data to initiator, verify it arrives at responder."""
    print("=" * 60)
    print("TEST 1: Initiator → Responder")
    print("  Sending 'Hello from App A' to initiator (COM5)")
    print("  Expecting it to appear on responder (COM7)")
    print("=" * 60)

    init_ser = serial.Serial(INITIATOR_PORT, BAUD, timeout=0.1)
    resp_ser = serial.Serial(RESPONDER_PORT, BAUD, timeout=0.1)

    # Flush any stale data
    time.sleep(0.5)
    init_ser.reset_input_buffer()
    resp_ser.reset_input_buffer()

    # Send COBS-encoded payload to initiator
    payload = b'Hello from App A'
    frame = cobs_encode_frame(payload)
    print(f"  TX → initiator: {payload} ({frame.hex()})")
    init_ser.write(frame)

    # Read COBS output from responder
    print("  Waiting for responder output...")
    frames = read_cobs_frames(resp_ser, timeout=5.0)

    if frames:
        for f in frames:
            print(f"  RX ← responder: type={f['type']} range={f['range_m']}m "
                  f"payload='{f['payload_str']}' ({f['payload'].hex()})")
        # Check if our payload made it through
        payloads = [f['payload'] for f in frames]
        if payload in payloads:
            print("  ✓ PASS: Payload received correctly!")
        else:
            print(f"  ✗ FAIL: Expected payload not found. Got: {payloads}")
    else:
        print("  ✗ FAIL: No COBS frames received from responder")

    init_ser.close()
    resp_ser.close()
    return len(frames) > 0


def test_responder_to_initiator():
    """Send COBS data to responder, verify it arrives at initiator."""
    print()
    print("=" * 60)
    print("TEST 2: Responder → Initiator")
    print("  Sending 'Hello from App B' to responder (COM7)")
    print("  Expecting it to appear on initiator (COM5)")
    print("=" * 60)

    init_ser = serial.Serial(INITIATOR_PORT, BAUD, timeout=0.1)
    resp_ser = serial.Serial(RESPONDER_PORT, BAUD, timeout=0.1)

    time.sleep(0.5)
    init_ser.reset_input_buffer()
    resp_ser.reset_input_buffer()

    # Send COBS-encoded payload to responder
    payload = b'Hello from App B'
    frame = cobs_encode_frame(payload)
    print(f"  TX → responder: {payload} ({frame.hex()})")
    resp_ser.write(frame)

    # Read COBS output from initiator
    print("  Waiting for initiator output...")
    frames = read_cobs_frames(init_ser, timeout=5.0)

    if frames:
        for f in frames:
            print(f"  RX ← initiator: type={f['type']} range={f['range_m']}m "
                  f"payload='{f['payload_str']}' ({f['payload'].hex()})")
        payloads = [f['payload'] for f in frames]
        if payload in payloads:
            print("  ✓ PASS: Payload received correctly!")
        else:
            print(f"  ✗ FAIL: Expected payload not found. Got: {payloads}")
    else:
        print("  ✗ FAIL: No COBS frames received from initiator")

    init_ser.close()
    resp_ser.close()
    return len(frames) > 0


def test_bidirectional():
    """Send data both directions simultaneously."""
    print()
    print("=" * 60)
    print("TEST 3: Bidirectional")
    print("  Sending data both directions at once")
    print("=" * 60)

    init_ser = serial.Serial(INITIATOR_PORT, BAUD, timeout=0.1)
    resp_ser = serial.Serial(RESPONDER_PORT, BAUD, timeout=0.1)

    time.sleep(0.5)
    init_ser.reset_input_buffer()
    resp_ser.reset_input_buffer()

    payload_a = b'Data-A->B'
    payload_b = b'Data-B->A'

    # Send both
    init_ser.write(cobs_encode_frame(payload_a))
    resp_ser.write(cobs_encode_frame(payload_b))
    print(f"  TX → initiator: {payload_a}")
    print(f"  TX → responder: {payload_b}")

    # Read both outputs
    print("  Waiting for outputs...")

    init_frames = []
    resp_frames = []

    def read_init():
        nonlocal init_frames
        init_frames = read_cobs_frames(init_ser, timeout=5.0)

    def read_resp():
        nonlocal resp_frames
        resp_frames = read_cobs_frames(resp_ser, timeout=5.0)

    t1 = threading.Thread(target=read_init)
    t2 = threading.Thread(target=read_resp)
    t1.start()
    t2.start()
    t1.join()
    t2.join()

    print(f"\n  Initiator received {len(init_frames)} frames:")
    for f in init_frames:
        print(f"    type={f['type']} range={f['range_m']}m payload='{f['payload_str']}'")

    print(f"  Responder received {len(resp_frames)} frames:")
    for f in resp_frames:
        print(f"    type={f['type']} range={f['range_m']}m payload='{f['payload_str']}'")

    init_payloads = [f['payload'] for f in init_frames]
    resp_payloads = [f['payload'] for f in resp_frames]

    pass_a = payload_a in resp_payloads
    pass_b = payload_b in init_payloads
    print(f"\n  A→B: {'✓ PASS' if pass_a else '✗ FAIL'}")
    print(f"  B→A: {'✓ PASS' if pass_b else '✗ FAIL'}")

    init_ser.close()
    resp_ser.close()
    return pass_a and pass_b


if __name__ == '__main__':
    print("DIY-UWB COBS Serial Bridge Test")
    print(f"Initiator: {INITIATOR_PORT}  Responder: {RESPONDER_PORT}")
    print()

    # Install cobs if needed
    try:
        from cobs import cobs
    except ImportError:
        import subprocess, sys
        subprocess.check_call([sys.executable, '-m', 'pip', 'install', 'cobs'])
        from cobs import cobs

    results = []
    results.append(("Init→Resp", test_initiator_to_responder()))
    results.append(("Resp→Init", test_responder_to_initiator()))
    results.append(("Bidirectional", test_bidirectional()))

    print()
    print("=" * 60)
    print("RESULTS:")
    for name, passed in results:
        print(f"  {name}: {'✓ PASS' if passed else '✗ FAIL'}")
    print("=" * 60)
