# ours-blanc Drum Kit (libretro core)
CFLAGS ?= -O2 -Wall -Wextra
SRC = src/drumkit.c engine/dk_engine.c
HDR = engine/dk_engine.h src/libretro.h src/font8x8_basic.h

all: dist/drumkit_libretro.so

# R36S / aarch64 Linux (cross compile)
dist/drumkit_libretro.so: $(SRC) $(HDR)
	mkdir -p dist
	aarch64-linux-gnu-gcc $(CFLAGS) -shared -fPIC -o $@ $(SRC) -lm

# host build for tests
build/drumkit_libretro.so: $(SRC) $(HDR)
	mkdir -p build
	$(CC) $(CFLAGS) -shared -fPIC -o $@ $(SRC) -lm

build/front: test/front.c src/libretro.h
	mkdir -p build
	$(CC) -O1 -w -o $@ test/front.c -ldl

build/engine_test: test/engine_test.c engine/dk_engine.c engine/dk_engine.h
	mkdir -p build
	$(CC) $(CFLAGS) -o $@ test/engine_test.c engine/dk_engine.c -lm

engine-test: build/engine_test
	./build/engine_test

test: build/drumkit_libretro.so build/front engine-test
	python3 -I test/run_tests.py build/drumkit_libretro.so

package: dist/drumkit_libretro.so
	rm -rf build/pkg && mkdir -p build/pkg && cp -r r36s/ports r36s/README.txt build/pkg/
	cp dist/drumkit_libretro.so build/pkg/ports/drumkit/
	cd build/pkg && rm -f ../../dist/r36s-drumkit.zip && zip -q -X -r ../../dist/r36s-drumkit.zip README.txt ports

# web version: engine → WebAssembly (pip install ziglang), plus the page, UI and AudioWorklet
ZIGCC ?= python3 -m ziglang cc
web/drumkit.wasm: web/wasm_api.c engine/dk_engine.c engine/dk_engine.h
	$(ZIGCC) -target wasm32-wasi -O2 -mexec-model=reactor -s -o $@ web/wasm_api.c engine/dk_engine.c

web: web/drumkit.wasm
	rm -rf dist/web && mkdir -p dist/web && cp web/index.html web/app.js web/worklet.js web/drumkit.wasm dist/web/

clean:
	rm -rf build

.PHONY: all test engine-test package web clean
