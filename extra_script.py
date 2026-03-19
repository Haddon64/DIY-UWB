Import("env")
import os
import sys
import glob
import shutil
import subprocess
import tempfile

# ── Qorvo DW3xxx driver sources ────────────────────────────────────────────

SDK_DRIVER = os.path.join(
    env.subst("$PROJECT_DIR"),
    "lib", "dwt_uwb_driver"
)

env.Append(CPPPATH=[
    SDK_DRIVER,
    os.path.join(SDK_DRIVER, "dw3000"),
    os.path.join(SDK_DRIVER, "lib", "qmath", "include"),
])

env.BuildSources(
    os.path.join("$BUILD_DIR", "dwt_uwb_driver"),
    SDK_DRIVER,
    src_filter=[
        "-<*>",
        "+<deca_interface.c>",
        "+<deca_compat.c>",
        "+<deca_rsl.c>",
        "+<dw3000/dw3000_device.c>",
        "+<lib/qmath/src/qmath.c>",
    ],
)

# ── J-Link: auto-patch + serial-number-targeted upload ─────────────────────
#
# PlatformIO bundles tool-jlink but the DLL may be broken.
# We auto-detect the system-installed SEGGER J-Link and patch if newer.
#
# For multi-board setups, set 'jlink_serial' in platformio.ini per-env
# to target a specific J-Link probe by serial number.
#
# Users must install SEGGER J-Link Software Pack (free download):
#   https://www.segger.com/downloads/jlink/

def find_system_jlink_dir():
    """Find the newest system-installed SEGGER J-Link directory."""
    if sys.platform != "win32":
        return None
    segger_base = os.path.join(
        os.environ.get("ProgramFiles", r"C:\Program Files"), "SEGGER"
    )
    if not os.path.isdir(segger_base):
        return None
    dirs = sorted(glob.glob(os.path.join(segger_base, "JLink_V*")), reverse=True)
    return dirs[0] if dirs else None


def patch_jlink_if_needed():
    """Copy newer system J-Link DLLs into PlatformIO's tool-jlink package."""
    if sys.platform != "win32":
        return

    try:
        pio_jlink_dir = env.PioPlatform().get_package_dir("tool-jlink") or ""
    except Exception:
        return
    if not os.path.isdir(pio_jlink_dir):
        return

    sys_jlink = find_system_jlink_dir()
    if not sys_jlink:
        return

    sys_dll = os.path.join(sys_jlink, "JLinkARM.dll")
    pio_dll = os.path.join(pio_jlink_dir, "JLinkARM.dll")

    if not os.path.isfile(sys_dll):
        return
    if os.path.isfile(pio_dll) and os.path.getmtime(sys_dll) <= os.path.getmtime(pio_dll):
        return

    print("[DIY-UWB] Patching PlatformIO J-Link with: " + sys_jlink)
    for f in os.listdir(sys_jlink):
        if f.endswith(".dll") or f == "JLink.exe":
            try:
                shutil.copy2(os.path.join(sys_jlink, f), os.path.join(pio_jlink_dir, f))
            except (PermissionError, OSError):
                pass


def upload_with_serial(source, target, env):
    """
    Upload firmware using J-Link Commander with serial number targeting.

    Set 'jlink_serial' in platformio.ini to target a specific probe:
        [env:my_board]
        jlink_serial = 760220786
    """
    # Find J-Link Commander
    sys_jlink = find_system_jlink_dir()
    if sys_jlink:
        jlink_exe = os.path.join(sys_jlink, "JLink.exe")
    else:
        try:
            pio_jlink_dir = env.PioPlatform().get_package_dir("tool-jlink") or ""
            jlink_exe = os.path.join(pio_jlink_dir, "JLink.exe")
        except Exception:
            jlink_exe = "JLink.exe"

    if not os.path.isfile(jlink_exe):
        print("\n" + "=" * 60)
        print("ERROR: SEGGER J-Link Software not found!")
        print("")
        print("Install from: https://www.segger.com/downloads/jlink/")
        print("Download: 'J-Link Software and Documentation Pack'")
        print("=" * 60 + "\n")
        env.Exit(1)

    hex_path = os.path.abspath(str(source[0]))
    serial = env.GetProjectOption("jlink_serial", "")

    # Build J-Link command script
    script = tempfile.NamedTemporaryFile(mode="w", suffix=".jlink", delete=False)
    if serial:
        script.write(f"USB {serial}\n")
    script.write("device NRF52833_XXAA\n")
    script.write("si SWD\n")
    script.write("speed 4000\n")
    script.write("connect\n")
    script.write(f"loadfile {hex_path}\n")
    script.write("r\n")
    script.write("g\n")
    script.write("q\n")
    script.close()

    board_name = env.subst("$PIOENV")
    if serial:
        print(f"[DIY-UWB] Uploading {board_name} to J-Link S/N {serial}")
    else:
        print(f"[DIY-UWB] Uploading {board_name} (first available J-Link)")

    result = subprocess.run(
        [jlink_exe, "-CommandFile", script.name, "-AutoConnect", "1"],
        timeout=30,
    )
    os.unlink(script.name)

    if result.returncode != 0:
        env.Exit(1)


# Auto-patch on every build
try:
    patch_jlink_if_needed()
except Exception:
    pass

# Override the upload action with our serial-number-aware version
env.Replace(UPLOADCMD=upload_with_serial)
