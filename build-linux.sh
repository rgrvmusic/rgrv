#!/bin/bash
# Native Linux build.  The wint14syssys container is where RtMidi and the
# ALSA headers live, and ~/Projects is mounted there, so this is the build
# the other tools in this tree use.  Falls back to a plain local cmake when
# podman/the container is not around.
set -e

cd "$(dirname "$0")"

if command -v podman >/dev/null 2>&1 &&
   podman container exists wint14syssys 2>/dev/null; then
    podman exec --user "$(id -u):$(id -g)" -w "$PWD" wint14syssys bash -c \
        'cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && \
         cmake --build build -j$(nproc)'
else
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build -j"$(nproc 2>/dev/null || echo 2)"
fi

echo
echo "Built: build/rgrv"
