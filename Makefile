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
HARFBUZZ_VERSION := 14.6.0
HARFBUZZ_DIR := vendor/harfbuzz
EMOJI_FONT_COMMIT := 8b0a1d0f5983c89bc2b93f1b5fb55f9e252744b5
EMOJI_FONT_URL := https://raw.githubusercontent.com/google/fonts/$(EMOJI_FONT_COMMIT)/ofl/notoemoji
OBJECTS := build/main.o build/canvas2d.o build/webgl.o build/video.o build/audio.o build/audio_processing.o build/fetch.o build/thumbnails.o build/midi.o build/recorder.o build/editor.o build/ui.o build/overlay.o build/keymap.o

CFLAGS := -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -I$(QUICKJS_DIR) -Ivendor/stb -I$(PLUTOVG_DIR)/include -DPLUTOVG_BUILD_STATIC -I$(HARFBUZZ_DIR)/src $(shell pkg-config --cflags sdl2)
LDLIBS := $(shell pkg-config --libs sdl2 glesv2 libpulse-simple alsa) -lm -ldl -lpthread

all: build/hydra-native build/hydra-bundle.js build/p5.min.js vendor/fonts/NotoEmoji.ttf

$(QUICKJS_DIR)/quickjs.c:
	mkdir -p vendor
	curl -fsSL https://bellard.org/quickjs/quickjs-$(QUICKJS_VERSION).tar.xz | tar -xJ -C vendor
	mv vendor/quickjs-$(QUICKJS_VERSION) $(QUICKJS_DIR)

STB_COMMIT := 4e9b368bfc071e97206ea5c6adba68dd0cbf8897

vendor/stb/%.h:
	mkdir -p vendor/stb
	curl -fsSL https://raw.githubusercontent.com/nothings/stb/$(STB_COMMIT)/$*.h -o $@

$(HARFBUZZ_DIR)/src/harfbuzz.cc:
	mkdir -p vendor
	curl -fsSL https://github.com/harfbuzz/harfbuzz/releases/download/$(HARFBUZZ_VERSION)/harfbuzz-$(HARFBUZZ_VERSION).tar.xz | tar -xJ -C vendor
	mv vendor/harfbuzz-$(HARFBUZZ_VERSION) $(HARFBUZZ_DIR)

build/harfbuzz.o: $(HARFBUZZ_DIR)/src/harfbuzz.cc
	@mkdir -p build
	$(CXX) -O2 -w -fno-exceptions -fno-rtti -c $< -o $@

vendor/fonts/NotoEmoji.ttf:
	mkdir -p vendor/fonts
	curl -fsSL "$(EMOJI_FONT_URL)/NotoEmoji%5Bwght%5D.ttf" -o $@
	curl -fsSL "$(EMOJI_FONT_URL)/OFL.txt" -o vendor/fonts/NotoEmoji-OFL.txt

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

build/%.o: src/%.c src/*.h $(QUICKJS_DIR)/quickjs.c $(PLUTOVG_DIR)/include/plutovg.h $(HARFBUZZ_DIR)/src/harfbuzz.cc vendor/stb/stb_truetype.h vendor/stb/stb_image_write.h vendor/stb/stb_image.h
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

build/hydra-native: $(OBJECTS) $(QUICKJS_OBJECTS) $(PLUTOVG_OBJECTS) build/harfbuzz.o
	$(CXX) -o $@ $^ $(LDLIBS)

build/p5.min.js: node_modules/hydra-synth/package.json
	@mkdir -p build
	cp node_modules/p5/lib/p5.min.js $@

node_modules/hydra-synth/package.json:
	pnpm install

build/hydra-bundle.js: js/*.js package.json node_modules/hydra-synth/package.json
	node_modules/.bin/esbuild js/main.js --bundle --format=iife --outfile=$@ \
		--alias:regl=./js/regl-shim.js --alias:raf-loop=./js/raf-loop-shim.js --alias:meyda=./js/meyda-shim.js

build/editor-test: test/editor_test.c src/editor.c src/editor.h
	@mkdir -p build
	$(CC) -O1 -g -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined -o $@ test/editor_test.c src/editor.c

build/ui-test: test/ui_test.c src/ui.c src/keymap.c src/editor.c src/*.h
	@mkdir -p build
	$(CC) -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -fsanitize=address,undefined \
		$(shell pkg-config --cflags sdl2) -o $@ test/ui_test.c src/ui.c src/keymap.c src/editor.c \
		$(shell pkg-config --libs sdl2) -lm

build/audio-test: test/audio_test.c src/audio_processing.c src/audio_processing.h
	@mkdir -p build
	$(CC) -O1 -g -Wall -Wextra -fsanitize=address,undefined -o $@ test/audio_test.c src/audio_processing.c -lm

test: build/editor-test build/ui-test build/audio-test
	build/editor-test
	build/ui-test
	build/audio-test
	python3 test/portal_test.py
	python3 test/gallery_test.py

check: all test
	build/hydra-native --hidden --no-vsync test/check.js

clean:
	rm -rf build

.PHONY: all test check clean
