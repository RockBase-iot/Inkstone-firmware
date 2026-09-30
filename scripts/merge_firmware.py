"""
merge_firmware.py — PlatformIO extra_script (post-build hook)

Merges bootloader + partitions + boot_app0 + firmware into a single 0x0 image:
    inkstone-<env>.bin  (written to the project root)

Adapted from RockBase-iot/NM-EPD-420 scripts/merge_firmware.py with
per-environment output naming.
"""

import os
import subprocess
import sys

Import("env")   # noqa: F821 — SCons environment injected by PlatformIO


def _find_boot_app0(env):
    framework_dir = env.subst("$PROJECT_PACKAGES_DIR")
    for base in (framework_dir, os.path.expanduser("~/.platformio/packages")):
        if not os.path.isdir(base):
            continue
        for entry in os.listdir(base):
            if "arduinoespressif32" in entry.lower():
                candidate = os.path.join(
                    base, entry, "tools", "partitions", "boot_app0.bin")
                if os.path.isfile(candidate):
                    return candidate
    return None


def _factory_offset(env):
    """Read the factory app offset from the project's partition CSV.

    The custom table (partitions_inkstone.csv) places factory at 0x20000
    (NVS was enlarged to 64KB), while the ESP32 default is 0x10000. Merging
    the app at the wrong offset makes the image unbootable, so derive it
    from the CSV instead of hard-coding.
    """
    csv_name = env.GetProjectOption("board_build.partitions", "")
    csv_path = os.path.join(env.subst("$PROJECT_DIR"), csv_name)
    if csv_name and os.path.isfile(csv_path):
        with open(csv_path, "r", encoding="ascii") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                fields = [c.strip() for c in line.split(",")]
                if len(fields) >= 5 and fields[1] == "app" \
                        and fields[2] in ("factory", "ota_0"):
                    return int(fields[3], 0)
    return 0x10000


def _bootloader_offset(env):
    """Bootloader flash offset per chip. ESP32-C5/P4 ROMs load the bootloader
    from 0x2000 (NOT 0x0): pioarduino builder/frameworks/espidf.py maps it as
        0x1000 if mcu in (esp32, esp32s2)
        0x2000 if mcu in (esp32c5, esp32p4)
        0x0    otherwise (esp32s3, esp32c3, ...)
    Matching CONFIG_BOOTLOADER_OFFSET_IN_FLASH in the framework's sdkconfig.h
    (framework-arduinoespressif32-libs/<mcu>/.../include/sdkconfig.h).
    A wrong offset bricks the board with ROM "invalid header" boot loops."""
    mcu = env.BoardConfig().get("build.mcu", "")
    if mcu in ("esp32", "esp32s2"):
        return 0x1000
    if mcu in ("esp32c5", "esp32p4"):
        return 0x2000
    return 0x0


def _merge_firmware(source, target, env):   # noqa: ANN001
    build_dir   = env.subst("$BUILD_DIR")
    project_dir = env.subst("$PROJECT_DIR")
    pioenv      = env.subst("$PIOENV")
    output      = os.path.join(project_dir, "inkstone-%s.bin" % pioenv)

    bootloader = os.path.join(build_dir, "bootloader.bin")
    partitions = os.path.join(build_dir, "partitions.bin")
    firmware   = os.path.join(build_dir, "firmware.bin")
    boot_app0  = _find_boot_app0(env)

    for f in (bootloader, partitions, firmware):
        if not os.path.isfile(f):
            print("[merge] WARNING: %s not found, skipping merge." % f)
            return
    if not boot_app0 or not os.path.isfile(boot_app0):
        print("[merge] WARNING: boot_app0.bin not found, skipping merge.")
        return

    esptool = os.path.join(
        env.subst("$PROJECT_PACKAGES_DIR"), "tool-esptoolpy", "esptool.py")
    esptool_cmd = ([sys.executable, esptool] if os.path.isfile(esptool)
                   else [sys.executable, "-m", "esptool"])

    app_offset = _factory_offset(env)
    bootloader_offset = _bootloader_offset(env)

    # Chip / flash parameters come from the board definition instead of being
    # hard-coded: nm-epd-420-4c is esp32s3/dio/16MB, esp32-c5-spectra-e6 is
    # esp32c5/qio/8MB.
    board = env.BoardConfig()
    chip = board.get("build.mcu", "esp32s3")
    flash_mode = env.GetProjectOption(
        "board_build.flash_mode", board.get("build.flash_mode", "dio"))
    # Mirror pioarduino's _get_board_flash_mode(): qio/qout are normalized to
    # dio in the image header (same runtime behaviour, broader compatibility) —
    # keeps this merged image identical to what `pio run -t upload` flashes.
    if flash_mode in ("qio", "qout"):
        flash_mode = "dio"
    flash_size = board.get("upload.flash_size", "16MB")

    args = esptool_cmd + [
        "--chip", chip,
        "merge_bin",
        "-o", output,
        "--flash_mode", flash_mode,
        "--flash_freq", "80m",
        "--flash_size", flash_size,
        hex(bootloader_offset), bootloader,
        "0x8000", partitions,
        "0xe000", boot_app0,
        hex(app_offset), firmware,
    ]

    print("=" * 60)
    print("[merge] Creating inkstone-%s.bin (bootloader + partitions + app)" % pioenv)
    print("=" * 60)

    result = subprocess.run(args, cwd=project_dir)
    if result.returncode == 0:
        size_mb = os.path.getsize(output) / (1024 * 1024)
        print("[merge] OK -> %s  (%.1f MB)" % (os.path.basename(output), size_mb))
        print("[merge] Flash from address 0x0 to programme the whole device.")
    else:
        print("[merge] ERROR: merge_bin exited with code %d" % result.returncode)


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", _merge_firmware)
