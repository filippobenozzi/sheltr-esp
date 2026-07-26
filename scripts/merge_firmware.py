"""Genera il binario unico (bootloader + partizioni + app) per il flash da browser.

esp-web-tools puo' scrivere piu' file a offset diversi, ma un solo binario a
offset 0x0 rende la pagina di flash molto piu' semplice e affidabile.
Il file viene creato in `.pio/build/<env>/sheltr-esp-merged.bin`.
"""

from pathlib import Path

Import("env")  # noqa: F821  (fornito da PlatformIO)


def merge_bin(source, target, env):  # noqa: ANN001, ARG001
    build_dir = Path(env.subst("$BUILD_DIR"))
    firmware = build_dir / "firmware.bin"
    bootloader = build_dir / "bootloader.bin"
    partitions = build_dir / "partitions.bin"
    boot_app0 = Path(
        env.subst("$PROJECT_PACKAGES_DIR")
    ) / "framework-arduinoespressif32" / "tools" / "partitions" / "boot_app0.bin"
    merged = build_dir / "sheltr-esp-merged.bin"

    missing = [str(path) for path in (firmware, bootloader, partitions) if not path.exists()]
    if missing:
        print(f"[merge] file mancanti, merge saltato: {missing}")
        return

    flash_size = env.BoardConfig().get("upload.flash_size", "16MB")
    flash_mode = env.BoardConfig().get("build.flash_mode", "dio")
    flash_freq = env.BoardConfig().get("build.f_flash", "80000000L").replace("000000L", "m")

    cmd = [
        "--chip",
        env.BoardConfig().get("build.mcu", "esp32s3"),
        "merge_bin",
        "-o",
        str(merged),
        "--flash_mode",
        flash_mode,
        "--flash_freq",
        flash_freq,
        "--flash_size",
        flash_size,
        "0x0",
        str(bootloader),
        "0x8000",
        str(partitions),
        "0xe000",
        str(boot_app0),
        "0x10000",
        str(firmware),
    ]
    if not boot_app0.exists():
        # boot_app0 non disponibile: si puo' comunque unire il resto.
        del cmd[cmd.index("0xe000") : cmd.index("0xe000") + 2]

    env.Execute(" ".join(['"$PYTHONEXE" "$OBJCOPY"'] + cmd))
    if merged.exists():
        print(f"[merge] binario unico: {merged} ({merged.stat().st_size} byte)")


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", merge_bin)  # noqa: F821
