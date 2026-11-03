#!/bin/bash
# Windows cross-build with mingw-w64.  Run inside wint14syssys, where the
# mingw RtMidi development files are installed alongside the compiler.
set -e

cd "$(dirname "$0")"

if command -v podman >/dev/null 2>&1 &&
   podman container exists wint14syssys 2>/dev/null; then
    podman exec --user "$(id -u):$(id -g)" -w "$PWD" wint14syssys bash -c \
        'cmake -S . -B build-windows -DCMAKE_BUILD_TYPE=Release \
             -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake && \
         cmake --build build-windows -j$(nproc)'
else
    cmake -S . -B build-windows -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake
    cmake --build build-windows -j"$(nproc 2>/dev/null || echo 2)"
fi

echo
echo "Built: build-windows/rgrv.exe"
