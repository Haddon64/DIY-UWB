# DIY-UWB: Bidirectional UWB Serial Bridge with Ranging

A PlatformIO/Zephyr firmware for **Qorvo DWM3001CDK** development kits that creates a bidirectional serial bridge over UWB (Ultra-Wideband) radio, with continuous distance measurement.

## What It Does

```
App A  --COBS/UART-->  Unit A (Initiator)  <==UWB==>  Unit B (Responder)  --COBS/UART-->  App B
App A  <--COBS/UART--  Unit A (Initiator)  <==UWB==>  Unit B (Responder)  <--COBS/UART--  App B
```

- **Continuous ranging at ~50 Hz** between units (SS-TWR)
- **Bidirectional data relay**: COBS-encoded packets received on either unit's UART are transmitted over UWB and output on the peer's UART
- **Range included in every output**: each COBS output frame includes the current distance measurement
- **MessagePack encoding**: compact binary serialization for app protocol frames (fits in standard 127-byte IEEE 802.15.4 packets)

## Quick Start

### 1. Install Prerequisites

**PlatformIO** (build system):
- Install [VS Code](https://code.visualstudio.com/)
- Install the [PlatformIO IDE extension](https://platformio.org/install/ide?install=vscode) from the VS Code marketplace
- Or install the CLI: `pip install platformio`

**SEGGER J-Link Software** (required for flashing — the DWM3001CDK uses an onboard J-Link debugger):
- Download from https://www.segger.com/downloads/jlink/
- Install the **"J-Link Software and Documentation Pack"** for your platform:
  - Windows: 64-bit `.exe` installer
  - Linux: `.deb` or `.tgz` package
  - macOS: `.pkg` installer
- The build script auto-detects the installation and patches PlatformIO's bundled J-Link if needed

**Python packages** (for testing — optional):
```bash
pip install pyserial cobs
```

### 2. Clone and Build

```bash
git clone https://github.com/MytraAI/DIY-UWB-AI-SWAG.git
cd DIY-UWB-AI-SWAG

# Build both firmware images
pio run
```

The Qorvo DW3xxx driver files are included in `lib/dwt_uwb_driver/` — no separate SDK download is needed.

### 3. Find Your Board Serial Numbers

Each DWM3001CDK has a unique J-Link serial number. With your boards plugged into USB (J9 connector):

```bash
pio device list
```

You'll see output like:
```
COM5
----
Hardware ID: USB VID:PID=1366:0105 SER=000760220786
Description: USB Serial Device (COM5)

COM8
----
Hardware ID: USB VID:PID=1366:0105 SER=000760220781
Description: USB Serial Device (COM8)
```

### 4. Configure Board Serial Numbers

Edit `platformio.ini` and set the `jlink_serial` for each environment to match your boards:

```ini
[env:dwm3001cdk_initiator]
jlink_serial = 760220786    ; <-- Your initiator board's serial
monitor_port = COM5          ; <-- Its COM port

[env:dwm3001cdk_responder]
jlink_serial = 760220781    ; <-- Your responder board's serial
monitor_port = COM8          ; <-- Its COM port
```

### 5. Flash

```bash
# Flash initiator (targets specific board by serial number)
pio run -e dwm3001cdk_initiator -t upload

# Flash responder
pio run -e dwm3001cdk_responder -t upload

# Or build + flash both (works with both boards connected simultaneously)
pio run -t upload
```

### 6. Verify

On boot, all 4 LEDs (D9-D12) flash briefly. Then each init step blinks a different LED:
- **D9** = UWB probe OK
- **D10** = IDLE_RC OK
- **D11** = initialise OK
- **D12** = configure OK

During operation:
- **D9** blinks = transmitting UWB frame
- **D10** lights = receiving UWB frame
- **D11** blinks = successful ranging exchange

## Hardware

- 2+ [Qorvo DWM3001CDK](https://www.qorvo.com/products/p/DWM3001CDK) (nRF52833 + DW3110 UWB)
- USB cables connected to **J9** (micro-USB, J-Link port) on each board
- Powered USB hub recommended when using multiple boards

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
| UART TX | P0.19 | To J-Link VCOM via solder bridge J14 |
| UART RX | P0.15 | From J-Link VCOM via solder bridge J15 |
| LED D9 | P0.04 | TX indicator |
| LED D10 | P0.05 | RX indicator |
| LED D11 | P0.22 | Ranging success |
| LED D12 | P0.14 | Error |

## COBS Serial Protocol

**Baud rate**: 115200, 8N1

All communication uses [COBS](https://en.wikipedia.org/wiki/Consistent_Overhead_Byte_Stuffing) framing with `0x00` as the frame delimiter.

### Input (External App -> Unit)

Send raw application data as a COBS-encoded frame:

```
COBS-encode([your payload bytes]) + 0x00
```

Maximum payload: **100 bytes**. The unit embeds this in the next outgoing UWB frame.

### Output (Unit -> External App)

Each COBS frame decodes to:

```
[type: 1 byte] [range_cm: 4 bytes LE int32] [plen: 1 byte] [payload: 0-100 bytes]
```

| Field | Size | Description |
|-------|------|-------------|
| `type` | 1 byte | `0x01` = from initiator, `0x02` = from responder, `0xFF` = boot |
| `range_cm` | 4 bytes | Signed 32-bit little-endian, distance in centimeters. `-1` if unavailable |
| `plen` | 1 byte | Payload length |
| `payload` | 0-100 bytes | Application data received from the peer unit |

### Notes

- The initiator computes range via SS-TWR; the responder reports `range_cm = -1`
- Ranging runs continuously at ~50 Hz regardless of payload
- Output frames are only emitted when the peer sends data (plen > 0)
- Pending payloads are retransmitted until a new one replaces them or delivery is confirmed
- The UART RX queue holds up to 2 COBS frames

## Web UI (quorvo-uwb)

The companion web app [quorvo-uwb](https://github.com/MytraAI/quorvo-uwb) provides a browser-based interface for the UWB bridge. It uses the Web Serial API (Chrome/Edge) to connect directly to the boards.

The app's protocol frames are [MessagePack](https://msgpack.org/)-encoded, wrapped in `[0xAA][LEN][MsgPack][CRC8]` framing, then COBS-encoded for transport over UWB. The firmware is transparent — it relays the bytes without parsing them.

See the `hardware-integration` branch of quorvo-uwb for the COBS transport layer.

## Python Example

```bash
pip install pyserial cobs
```

```python
import serial
import struct
from cobs import cobs

INITIATOR_PORT = "COM5"  # Adjust to your system
RESPONDER_PORT = "COM8"

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
  extra_script.py         # Build script: driver includes + J-Link auto-patch + upload
  platformio.ini          # Board environments with J-Link serial targeting
  test_cobs.py            # Python test script for bidirectional COBS
```

## Adding More Boards

To add more boards (e.g., for TDMA testing), add new environments to `platformio.ini`:

```ini
[env:dwm3001cdk_bot_1]
platform = nordicnrf52
board = nrf52833_dk
framework = zephyr
build_flags = -DUWB_INITIATOR -DDTC_OVERLAY_FILE=nrf52833_dk.overlay
board_build.zephyr.cmake_extra_args = -DDTC_OVERLAY_FILE=nrf52833_dk.overlay
extra_scripts = pre:extra_script.py
upload_protocol = custom
jlink_serial = XXXXXXXXX    ; Your board's J-Link serial
monitor_port = COMX
monitor_speed = 115200
```

Find board serials with `pio device list`. All boards can be connected and flashed simultaneously — each upload targets a specific J-Link by serial number.

## Antenna Delay Calibration

The default antenna delay (`TX_ANT_DLY = RX_ANT_DLY = 16390`) is the Qorvo-recommended value for Channel 5 on DWM3001CDK.

To calibrate for your specific boards:

1. Place the two units at a known, measured distance (e.g., 3.00m)
2. Read the reported range from the COBS output or web UI
3. Adjust the delay values in `src/main.c`:
   - If reported range is **too high**: increase the delay
   - If reported range is **too low**: decrease the delay
   - Each tick = ~4.7mm of range change
4. Rebuild and reflash: `pio run -t upload`

Per the Qorvo Quick Start Guide: *"For Engineering Samples (E1.0): do not use the Channel 5 Antenna Delay in OTP — use default value 16390."*

## Troubleshooting

### Upload fails with "Failed to open DLL"
The build script auto-patches PlatformIO's bundled J-Link with your system install. If upload still fails:
1. Verify J-Link Software is installed: `"C:\Program Files\SEGGER\JLink_V926\JLink.exe" --version`
2. Try a clean build: `pio run -t clean && pio run -t upload`

### No serial output / UART not working
- UART goes through the J-Link VCOM (solder bridges J14/J15 on CDK — closed by default)
- Connect via **J9** (J-Link USB), not J20 (nRF USB)
- The firmware uses binary COBS protocol, not text — use the Python test script or web UI to decode

### LEDs don't flash on boot
- Check USB connection on J9
- Try pressing the reset button (SW1)
- Verify the board is powered (D20 LED should be on)

### Ranging shows wrong distance
- See [Antenna Delay Calibration](#antenna-delay-calibration)
- Ensure both boards have the same firmware version
- Use a powered USB hub if boards are browning out

## License

Qorvo DW3xxx driver files under `lib/dwt_uwb_driver/` are subject to Qorvo's license terms (see `lib/dwt_uwb_driver/LICENSES/`). Application code in `src/` is provided as-is.
