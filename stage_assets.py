#!/usr/bin/env python3
import os, shutil, sys

def main():
    src = sys.argv[1] if len(sys.argv) > 1 else "game/assets"
    dst = sys.argv[2] if len(sys.argv) > 2 else "staging/assets"
    if not os.path.isdir(src):
        print(f"Error: {src} not found", file=sys.stderr); return 1
    if os.path.exists(dst): shutil.rmtree(dst)
    shutil.copytree(src, dst)
    print(f"Staged assets from {src} to {dst}")
    return 0

if __name__ == '__main__':
    sys.exit(main())
