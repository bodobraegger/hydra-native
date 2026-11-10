QUICKJS_VERSION := 2026-06-04
QUICKJS_DIR := vendor/quickjs
QUICKJS_SOURCES := $(addprefix $(QUICKJS_DIR)/,quickjs.c dtoa.c libregexp.c libunicode.c cutils.c quickjs-libc.c)
QUICKJS_OBJECTS := $(patsubst $(QUICKJS_DIR)/%.c,build/quickjs/%.o,$(QUICKJS_SOURCES))
PLUTOVG_VERSION := 1.3.3
PLUTOVG_DIR := vendor/plutovg
PLUTOVG_SOURCES := $(addprefix $(PLUTOVG_DIR)/source/,plutovg-blend.c plutovg-canvas.c plutovg-font.c plutovg-ft-math.c \
	plutovg-ft-raster.c plutovg-ft-stroker.c plutovg-matrix.c plutovg-paint.c plutovg-path.c plutovg-rasterize.c \
	plutovg-surface.c)
PLUTOVG_OBJECTS := $(patsubst $(PLUTOVG_DIR)/source/%.c,build/plutovg/%.o,$(PLUTOVG_SOURCES))
OBJECTS := build/main.o build/canvas2d.o build/video.o build/audio.o build/fetch.o build/editor.o build/ui.o build/overlay.o build/keymap.o

CFLAGS := -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -I$(QUICKJS_DIR) -Ivendor/stb -I$(PLUTOVG_DIR)/include -DPLUTOVG_BUILD_STATIC $(shell pkg-config --cflags sdl2)
LDLIBS := $(shell pkg-config --libs sdl2 glesv2 libpulse-simple) -lm -ldl -lpthread

all: build/hydra-native build/hydra-bundle.js

$(QUICKJS_DIR)/quickjs.c:
	mkdir -p vendor
	curl -fsSL https://bellard.org/quickjs/quickjs-$(QUICKJS_VERSION).tar.xz | tar -xJ -C vendor
	mv vendor/quickjs-$(QUICKJS_VERSION) $(QUICKJS_DIR)

STB_COMMIT := 4e9b368bfc071e97206ea5c6adba68dd0cbf8897

vendor/stb/%.h:
	mkdir -p vendor/stb
	curl -fsSL https://raw.githubusercontent.com/nothings/stb/$(STB_COMMIT)/$*.h -o $@

$(PLUTOVG_DIR)/include/plutovg.h:
	mkdir -p vendor
	curl -fsSL https://github.com/sammycage/plutovg/archive/refs/tags/v$(PLUTOVG_VERSION).tar.gz | tar -xz -C vendor
	mv vendor/plutovg-$(PLUTOVG_VERSION) $(PLUTOVG_DIR)

$(PLUTOVG_SOURCES): $(PLUTOVG_DIR)/include/plutovg.h

build/plutovg/%.o: $(PLUTOVG_DIR)/source/%.c
	@mkdir -p build/plutovg
	$(CC) -O2 -w -DPLUTOVG_BUILD -DPLUTOVG_BUILD_STATIC -DPLUTOVG_DISABLE_FONT_FACE_CACHE_LOAD -I$(PLUTOVG_DIR)/include -c $< -o $@

$(filter-out $(QUICKJS_DIR)/quickjs.c,$(QUICKJS_SOURCES)): $(QUICKJS_DIR)/quickjs.c

build/quickjs/%.o: $(QUICKJS_DIR)/%.c
	@mkdir -p build/quickjs
	$(CC) -O2 -w -D_GNU_SOURCE -DCONFIG_VERSION=\"$(QUICKJS_VERSION)\" -c $< -o $@

build/%.o: src/%.c src/*.h $(QUICKJS_DIR)/quickjs.c $(PLUTOVG_DIR)/include/plutovg.h vendor/stb/stb_truetype.h vendor/stb/stb_image_write.h
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

build/hydra-native: $(OBJECTS) $(QUICKJS_OBJECTS) $(PLUTOVG_OBJECTS)
	$(CC) -o $@ $^ $(LDLIBS)

node_modules/hydra-synth/package.json:
	pnpm install

build/hydra-bundle.js: js/*.js package.json node_modules/hydra-synth/package.json
	node_modules/.bin/esbuild js/main.js --bundle --format=iife --outfile=$@ \
		--alias:regl=./js/regl-shim.js --alias:raf-loop=./js/raf-loop-shim.js --alias:meyda=./js/meyda-shim.js

build/editor-test: test/editor_test.c src/editor.c src/editor.h
	@mkdir -p build
	$(CC) -O1 -g -Wall -Wextra -fsanitize=address,undefined -o $@ test/editor_test.c src/editor.c

build/ui-test: test/ui_test.c src/ui.c src/keymap.c src/editor.c src/*.h
	@mkdir -p build
	$(CC) -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -fsanitize=address,undefined \
		$(shell pkg-config --cflags sdl2) -o $@ test/ui_test.c src/ui.c src/keymap.c src/editor.c \
		$(shell pkg-config --libs sdl2) -lm

test: build/editor-test build/ui-test
	build/editor-test
	build/ui-test

check: all test
	build/hydra-native --hidden --no-vsync test/check.js

clean:
	rm -rf build

.PHONY: all test check clean
