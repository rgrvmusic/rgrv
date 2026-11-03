# SPDX-License-Identifier: MIT
# RGRV -- plain CMake underneath, with the container targets this tree uses.
#
#   make                  native Linux build in ./build
#   make windows          mingw-w64 cross-build in ./build-windows
#   make container        native build inside wint14syssys (has RtMidi dev)
#   make container-windows  Windows cross-build inside wint14syssys
#   make wasm             WebMIDI page's rgrv.js/rgrv.wasm in web/wasm
#   make serve            serve web/ on http://localhost:8080

BUILD ?= build
WINBUILD ?= build-windows
WASMBUILD ?= build-wasm
CONTAINER ?= wint14syssys
EMSDK ?= $(HOME)/Applications/emsdk
PODMAN_USER ?= $(shell id -u):$(shell id -g)
JOBS ?= $(shell nproc)

.PHONY: all build windows container container-windows wasm serve clean

all: build

build:
	cmake -S . -B $(BUILD) -DCMAKE_BUILD_TYPE=Release
	cmake --build $(BUILD) -j$(JOBS)

windows:
	cmake -S . -B $(WINBUILD) -DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake
	cmake --build $(WINBUILD) -j$(JOBS)

container:
	podman exec --user $(PODMAN_USER) -w "$(CURDIR)" $(CONTAINER) bash -c \
		'cmake -S . -B $(BUILD) -DCMAKE_BUILD_TYPE=Release && \
		 cmake --build $(BUILD) -j$$(nproc)'

container-windows:
	podman exec --user $(PODMAN_USER) -w "$(CURDIR)" $(CONTAINER) bash -c \
		'cmake -S . -B $(WINBUILD) -DCMAKE_BUILD_TYPE=Release \
		 -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake && \
		 cmake --build $(WINBUILD) -j$$(nproc)'

wasm:
	@if [ ! -f "$(EMSDK)/emsdk_env.sh" ]; then \
		echo "rgrv: emsdk not found at $(EMSDK) (set EMSDK=/path/to/emsdk)" >&2; \
		exit 1; \
	fi
	bash -c '. "$(EMSDK)/emsdk_env.sh" >/dev/null 2>&1 && \
		emcmake cmake -S . -B $(WASMBUILD) -DCMAKE_BUILD_TYPE=Release && \
		cmake --build $(WASMBUILD) -j$(JOBS)'
	@echo "Built: web/wasm/rgrv.js and web/wasm/rgrv.wasm"

serve:
	python3 -m http.server 8080 --directory web

clean:
	rm -rf $(BUILD) $(WINBUILD) $(WASMBUILD) web/wasm
