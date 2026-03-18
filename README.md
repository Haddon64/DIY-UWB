# DIY-UWB: Bidirectional UWB Serial Bridge with Ranging

A PlatformIO/Zephyr firmware for two **Qorvo DWM3001CDK** development kits that creates a bidirectional serial bridge over UWB (Ultra-Wideband) radio, with continuous distance measurement.

## What It Does

```
App A  --COBS/UART-->  Unit A (Initiator)  <==UWB==>  Unit B (Responder)  --COBS/UART-->  App B
App A  <--COBS/UART--  Unit A (Initiator)  <==UWB==>  Unit B (Responder)  <--COBS/UART--  App B
```

- **Continuous ranging at ~50 Hz** between the two units (SS-TWR)
- **Bidirectional data relay**: COBS-encoded packets received on either unit's UART are transmitted over UWB and output on the peer's UART
- **Range included in every output**: each COBS output frame includes the current distance measurement

## Hardware

- 2x [Qorvo DWM3001CDK](https://www.qorvo.com/products/p/DWM3001CDK) (nRF52833 + DW3110 UWB)
- USB cables connected to **J9** (micro-USB) on each board
- [SEGGER J-Link Software Pack](https://www.segger.com/downloads/jlink/) installed on the host PC

### Pin Mapping (DWM3001CDK)

| Function | Pin | Notes |
|----------|-----|-------|
| SPI3 SCK | P0.03 | DW3110 clock |
| SPI3 MOSI | P0.08 | DW3110 data out |
| SPI3 MISO | P0.29 | DW3110 data in |
| SPI CS | P1.06 | Manual GPIO control |
| DW IRQ | P1.02 | DW3110 interrupt |
| DW RST | P0.25 | DW3110 reset |
| DW WAKEUP | P1.19 | DW3110 wakeup |
| UART TX | P0.19 | To J-Link VCOM (via solder bridge J14) |
| UART RX | P0.15 | From J-Link VCOM (via solder bridge J15) |
| LED D9 | P0.04 | TX indicator |
| LED D10 | P0.05 | RX indicator |
| LED D11 | P0.22 | Ranging success |
| LED D12 | P0.14 | Error |

## Building & Flashing

### Prerequisites

- [PlatformIO](https://platformio.org/) with VS Code extension
- [SEGGER J-Link Software v9.26+](https://www.segger.com/downloads/jlink/) (the "Software and Documentation Pack" for Windows 64-bit)

The required Qorvo DW3xxx driver files are included in `lib/dwt_uwb_driver/` under Qorvo's license (see `lib/dwt_uwb_driver/LICENSES/`). No separate SDK download is needed.

### Build

```bash
# Build both firmware images
pio run

# Build only initiator or responder
pio run -e dwm3001cdk_initiator
pio run -e dwm3001cdk_responder
```

### Flash

PlatformIO's built-in upload uses an older J-Link DLL that may fail. Use the system-installed J-Link Commander directly:

```bash
# Flash initiator (replace SN with your J-Link serial number)
"C:\Program Files\SEGGER\JLink_V926\JLink.exe" -CommandFile flash_initiator.jlink

# Flash responder
"C:\Program Files\SEGGER\JLink_V926\JLink.exe" -CommandFile flash_responder.jlink
```

Example `flash_initiator.jlink`:
```
USB <your-jlink-serial>
device NRF52833_XXAA
si SWD
speed 4000
connect
loadfile .pio\build\dwm3001cdk_initiator\firmware.hex
r
g
q
```

### LED Behavior

On boot, all 4 LEDs flash briefly, then each init step blinks a different LED:
- **D9** = `dwt_probe` OK
- **D10** = IDLE_RC OK
- **D11** = `dwt_initialise` OK
- **D12** = `dwt_configure` OK

During operation:
- **D9** blinks = transmitting UWB frame
- **D10** lights = receiving UWB frame
- **D11** blinks = successful ranging exchange
- **D12** blinks = error/timeout

## COBS Serial Protocol

**Baud rate**: 115200, 8N1

All communication uses [COBS](https://en.wikipedia.org/wiki/Consistent_Overhead_Byte_Stuffing) framing with `0x00` as the frame delimiter.

### Input (External App -> Unit)

Send raw application data as a COBS-encoded frame:

```
COBS-encode([your payload bytes]) + 0x00
```

Maximum payload: **64 bytes**. The unit will embed this in the next outgoing UWB frame.

### Output (Unit -> External App)

Each COBS frame decodes to:

```
[type: 1 byte] [range_cm: 4 bytes LE int32] [plen: 1 byte] [payload: 0-64 bytes]
```

| Field | Size | Description |
|-------|------|-------------|
| `type` | 1 byte | `0x01` = from initiator, `0x02` = from responder, `0xFF` = boot |
| `range_cm` | 4 bytes | Signed 32-bit little-endian, distance in centimeters. `-1` if unavailable |
| `plen` | 1 byte | Payload length (0-64) |
| `payload` | 0-64 bytes | Application data received from the peer unit |

### Example

1. App A sends COBS frame to initiator: `"Hello"` (5 bytes)
2. Initiator embeds `"Hello"` in the next UWB poll frame
3. Responder receives the poll, extracts `"Hello"`, outputs COBS frame:
   - `type=0x02, range_cm=-1, plen=5, payload="Hello"`
4. App B decodes the COBS frame and gets `"Hello"` + range info

### Notes

- The initiator computes range via SS-TWR; the responder reports `range_cm = -1`
- Ranging runs continuously at ~50 Hz regardless of payload
- Output frames are only emitted when the peer sends data (plen > 0)
- A pending payload is retransmitted every ranging cycle until a new one replaces it
- The UART RX queue holds up to 4 COBS frames; older frames are dropped if the queue fills

## Python Example

```bash
pip install pyserial cobs
```

```python
import serial
import struct
from cobs import cobs

INITIATOR_PORT = "COM5"  # Adjust to your system
RESPONDER_PORT = "COM7"

# Send data from App A -> App B
ser_init = serial.Serial(INITIATOR_PORT, 115200, timeout=1)
payload = b"Hello from A"
ser_init.write(cobs.encode(payload) + b'\x00')

# Read data on App B side
ser_resp = serial.Serial(RESPONDER_PORT, 115200, timeout=5)
buf = bytearray()
while True:
    byte = ser_resp.read(1)
    if not byte:
        break
    if byte[0] == 0x00:
        if buf:
            decoded = cobs.decode(bytes(buf))
            msg_type = decoded[0]
            range_cm = struct.unpack_from('<i', decoded, 1)[0]
            plen = decoded[5]
            payload = decoded[6:6+plen]
            print(f"Type: {msg_type:#x}, Range: {range_cm}cm, Payload: {payload}")
            buf.clear()
            break
    else:
        buf.append(byte[0])
```

## Project Structure

```
DIY-UWB/
  src/main.c              # All firmware (initiator + responder, compile-time switch)
  zephyr/
    prj.conf              # Zephyr Kconfig
    nrf52833_dk.overlay   # DTS overlay for DWM3001CDK pin mapping
    CMakeLists.txt        # Adds Qorvo driver sources
  lib/dwt_uwb_driver/     # Qorvo DW3xxx driver (included, see LICENSES/)
  extra_script.py         # PlatformIO build script for driver includes
  platformio.ini          # Two environments: initiator + responder
  test_cobs.py            # Python test script for bidirectional COBS
```

## Antenna Delay Calibration

The default antenna delay (`TX_ANT_DLY = RX_ANT_DLY = 16893`) was calibrated at ~3m distance. For better accuracy:

1. Place the two units at a known distance (e.g., 5.00m)
2. Read the reported range
3. Adjust the delay values in `src/main.c`:
   - If reported range is **too high**: increase the delay
   - If reported range is **too low**: decrease the delay
   - Each tick ~ 4.7mm of range change

## License

Qorvo DW3xxx driver files under `SDK/` are subject to Qorvo's license terms. Application code in `src/` is provided as-is.
