#!/usr/bin/env python3
"""Copy a game's assets into the directory that aapt packages into the APK."""

from __future__ import annotations

import argparse
import shutil
import sys
from pathlib import Path


def stage_assets(source: Path, destination: Path) -> list[Path]:
    """Replace *destination* with a clean copy of *source*.

    Paths are returned relative to the destination so callers can report what
    will be stored at the APK asset root.
    """
    source = source.resolve()
    destination = destination.resolve()

    if not source.is_dir():
        raise ValueError(f"asset directory not found: {source}")
    if (
        source == destination
        or source in destination.parents
        or destination in source.parents
    ):
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


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Copy game assets into an APK staging directory."
    )
    parser.add_argument("source", nargs="?", default="game/assets")
    parser.add_argument("destination", nargs="?", default="staging/assets")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        staged = stage_assets(Path(args.source), Path(args.destination))
    except (OSError, ValueError) as error:
        print(f"Error: {error}", file=sys.stderr)
        return 1

    print(f"Staged {len(staged)} asset(s) in {args.destination}")
    for path in staged:
        print(f"  {path.as_posix()}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
