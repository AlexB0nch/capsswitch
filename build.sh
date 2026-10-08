#!/usr/bin/env bash
# Сборка CapsSwitch.exe и CapsSwitch.msi на Linux (Ubuntu/Debian):
#   sudo apt-get install -y gcc-mingw-w64-x86-64 wixl
#   ./build.sh
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p build
x86_64-w64-mingw32-windres src/capsswitch.rc -O coff -o build/res.o
x86_64-w64-mingw32-gcc -municode -mwindows -Os -s -Wall -Wextra \
  -o build/CapsSwitch.exe src/capsswitch.c build/res.o -luser32 -lkernel32
wixl -a x64 -o build/CapsSwitch.msi installer/CapsSwitch.wxs
ls -l build/CapsSwitch.exe build/CapsSwitch.msi
