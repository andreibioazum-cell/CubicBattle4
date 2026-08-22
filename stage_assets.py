#!/usr/bin/env python3
"""Copy a game's assets into the directory that aapt packages into the APK."""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path


def stage_assets(source: Path, destination: Path) -> list[Path]:
    """Replace *destination* with a clean copy of *source*."""
    source = source.resolve()
    destination = destination.resolve()

    if not source.is_dir():
        raise ValueError(f"asset directory not found: {source}")
    if (source == destination
        or source in destination.parents
        or destination in source.parents):
        raise ValueError("the asset and staging directories must not overlap")

    shutil.rmtree(destination, ignore_errors=True)
    destination.mkdir(parents=True)

    staged: list[Path] = []
    for path in sorted(source.rglob("*")):
        relative_path = path.relative_to(source)
        target = destination / relative_path

        if path.is_symlink():
            raise ValueError(f"asset symlinks are not supported: {relative_path}")
        if path.is_dir():
            target.mkdir(parents=True, exist_ok=True)
        elif path.name != ".gitkeep":
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, target)
            staged.append(relative_path)

    return staged


def stage_sounds(source: Path, destination: Path) -> list[Path]:
    """Copy WAVs from *source* (game/sounds) into *destination* (assets/sounds).

    The game opens sounds by name "sounds/<file>" (sound.c), so the folder is
    staged next to the regular assets inside the APK. An absent folder is
    fine: the game just stays silent.
    """
    source = source.resolve()
    destination = destination.resolve()
    if not source.is_dir():
        return []

    staged: list[Path] = []
    for path in sorted(source.rglob("*")):
        relative_path = path.relative_to(source)
        target = destination / relative_path

        if path.is_symlink():
            raise ValueError(f"sound symlinks are not supported: {relative_path}")
        if path.is_dir():
            target.mkdir(parents=True, exist_ok=True)
        elif path.name != ".gitkeep":
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, target)
            staged.append(Path("sounds") / relative_path)

    return staged


def build_android_activity(source: Path, apk_root: Path) -> Path | None:
    """Compile the optional Java NativeActivity bridge into ``classes.dex``.

    The repository's Android workflow already calls this staging script after
    installing the SDK, so keeping the Java/Dex step here also makes manual APK
    staging reproducible without a Gradle project. On development hosts without
    an Android SDK, asset-only staging remains available.
    """
    sources = sorted(source.rglob("*.java")) if source.is_dir() else []
    if not sources:
        return None

    sdk_value = os.environ.get("ANDROID_HOME") or os.environ.get("ANDROID_SDK_ROOT")
    if not sdk_value:
        print("Android SDK not found; skipped Java activity compilation")
        return None

    sdk = Path(sdk_value).resolve()
    android_jar = sdk / "platforms" / "android-34" / "android.jar"
    d8 = sdk / "build-tools" / "34.0.0" / "d8"
    javac = shutil.which("javac")
    if not javac:
        raise ValueError("javac not found while staging the Android activity")
    if not android_jar.is_file():
        raise ValueError(f"Android API 34 platform not found: {android_jar}")
    if not d8.is_file():
        raise ValueError(f"Android build-tools 34.0.0 d8 not found: {d8}")

    apk_root = apk_root.resolve()
    classes_dir = apk_root / ".java-classes"
    dex_dir = apk_root / ".java-dex"
    shutil.rmtree(classes_dir, ignore_errors=True)
    shutil.rmtree(dex_dir, ignore_errors=True)
    classes_dir.mkdir(parents=True)
    dex_dir.mkdir(parents=True)
    try:
        subprocess.run([
            javac, "-source", "8", "-target", "8",
            "-bootclasspath", str(android_jar),
            "-d", str(classes_dir),
            *(str(path) for path in sources),
        ], check=True)
        class_files = sorted(classes_dir.rglob("*.class"))
        if not class_files:
            raise ValueError("javac produced no Android activity classes")
        subprocess.run([
            str(d8), "--min-api", "29", "--output", str(dex_dir),
            *(str(path) for path in class_files),
        ], check=True)
        generated = dex_dir / "classes.dex"
        if not generated.is_file():
            raise ValueError("d8 did not produce classes.dex")
        destination = apk_root / "classes.dex"
        shutil.copy2(generated, destination)
        return destination
    finally:
        shutil.rmtree(classes_dir, ignore_errors=True)
        shutil.rmtree(dex_dir, ignore_errors=True)


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Copy game assets into an APK staging directory.")
    parser.add_argument("source", nargs="?", default="game/assets")
    parser.add_argument("destination", nargs="?", default="staging/assets")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        destination = Path(args.destination)
        staged = stage_assets(Path(args.source), destination)
        # Звуки (game/sounds) кладём в APK рядом с ассетами: assets/sounds/.
        sounds_dir = Path(__file__).resolve().parent / "game" / "sounds"
        staged += stage_sounds(sounds_dir, destination / "sounds")
        java_source = Path(__file__).resolve().parent / "game" / "java"
        dex = build_android_activity(java_source, destination.parent)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"Error: {error}", file=sys.stderr)
        return 1

    print(f"Staged {len(staged)} asset(s) in {args.destination}")
    for path in staged:
        print(f"  {path.as_posix()}")
    if dex:
        print(f"Built Android activity: {dex}")
    return 0


if __name__ == '__main__':
    sys.exit(main())
