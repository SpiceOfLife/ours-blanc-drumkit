# ours-blanc Drum Kit (libretro core)
CFLAGS ?= -O2 -Wall -Wextra
SRC = src/drumkit.c

all: dist/drumkit_libretro.so

# R36S / aarch64 Linux (cross compile)
dist/drumkit_libretro.so: $(SRC)
	mkdir -p dist
	aarch64-linux-gnu-gcc $(CFLAGS) -shared -fPIC -o $@ $(SRC) -lm

# host build for tests
build/drumkit_libretro.so: $(SRC)
	mkdir -p build
	$(CC) $(CFLAGS) -shared -fPIC -o $@ $(SRC) -lm

build/front: test/front.c src/libretro.h
	mkdir -p build
	$(CC) -O1 -w -o $@ test/front.c -ldl

test: build/drumkit_libretro.so build/front
	python3 -I test/run_tests.py build/drumkit_libretro.so

package: dist/drumkit_libretro.so
	rm -rf build/pkg && mkdir -p build/pkg && cp -r r36s/ports r36s/README.txt build/pkg/
	cp dist/drumkit_libretro.so build/pkg/ports/drumkit/
	cd build/pkg && rm -f ../../dist/r36s-drumkit.zip && zip -q -X -r ../../dist/r36s-drumkit.zip README.txt ports

clean:
	rm -rf build

.PHONY: all test package clean
