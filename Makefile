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
# The audio processing of Chrome from the system (not downloaded); only its headers come from upstream.
WEBRTC_APM_LIBRARY := $(firstword $(wildcard /usr/lib/*/libwebrtc-audio-processing-1.so.3 /usr/lib/libwebrtc-audio-processing-1.so.3))
WEBRTC_APM_VERSION := 1.3
WEBRTC_APM_DIR := vendor/webrtc-audio-processing
ABSEIL_VERSION := 20230125.1
ABSEIL_DIR := vendor/abseil-cpp
SHEENBIDI_VERSION := 2.9.0
SHEENBIDI_DIR := vendor/sheenbidi
EMOJI_FONT_COMMIT := 8b0a1d0f5983c89bc2b93f1b5fb55f9e252744b5
EMOJI_FONT_URL := https://raw.githubusercontent.com/google/fonts/$(EMOJI_FONT_COMMIT)/ofl/notoemoji
OBJECTS := build/main.o build/canvas2d.o build/svg_filter.o build/webgl.o build/video.o build/audio.o build/audio_processing.o build/analog_agc.o build/fetch.o build/thumbnails.o build/midi.o build/recorder.o build/patch_bay.o build/editor.o build/bidi.o build/ui.o build/settings.o build/overlay.o build/keymap.o

CFLAGS := -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -I$(QUICKJS_DIR) -Ivendor/stb -I$(PLUTOVG_DIR)/include -DPLUTOVG_BUILD_STATIC -I$(HARFBUZZ_DIR)/src -I$(SHEENBIDI_DIR)/Headers $(shell pkg-config --cflags sdl2)
LDLIBS := $(shell pkg-config --libs sdl2 glesv2 libpulse-simple libpulse alsa) -lm -ldl -lpthread

all: build/hydra-native build/hydra-bundle.js build/p5.min.js vendor/fonts/NotoEmoji.ttf $(if $(WEBRTC_APM_LIBRARY),build/libhydra-webrtc-apm.so)

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

$(WEBRTC_APM_DIR)/webrtc/modules/audio_processing/include/audio_processing.h:
	mkdir -p vendor
	curl -fsSL https://freedesktop.org/software/pulseaudio/webrtc-audio-processing/webrtc-audio-processing-$(WEBRTC_APM_VERSION).tar.xz | tar -xJ -C vendor
	mv vendor/webrtc-audio-processing-$(WEBRTC_APM_VERSION) $(WEBRTC_APM_DIR)
	touch $@

$(ABSEIL_DIR)/absl/types/optional.h:
	mkdir -p vendor
	curl -fsSL https://github.com/abseil/abseil-cpp/archive/$(ABSEIL_VERSION).tar.gz | tar -xz -C vendor
	mv vendor/abseil-cpp-$(ABSEIL_VERSION) $(ABSEIL_DIR)
	touch $@

# C++17 makes absl::optional the std::optional that the system library uses.
build/libhydra-webrtc-apm.so: src/webrtc_apm.cc src/webrtc_apm.h $(WEBRTC_APM_DIR)/webrtc/modules/audio_processing/include/audio_processing.h $(ABSEIL_DIR)/absl/types/optional.h
	@mkdir -p build
	$(CXX) -std=c++17 -O2 -fPIC -shared -w -DWEBRTC_POSIX -DWEBRTC_LINUX -I$(WEBRTC_APM_DIR)/webrtc -I$(ABSEIL_DIR) -o $@ $< $(WEBRTC_APM_LIBRARY)

$(SHEENBIDI_DIR)/Source/SheenBidi.c:
	mkdir -p vendor
	curl -fsSL https://github.com/Tehreer/SheenBidi/archive/refs/tags/v$(SHEENBIDI_VERSION).tar.gz | tar -xz -C vendor
	mv vendor/SheenBidi-$(SHEENBIDI_VERSION) $(SHEENBIDI_DIR)

build/sheenbidi.o: $(SHEENBIDI_DIR)/Source/SheenBidi.c
	@mkdir -p build
	$(CC) -O2 -w -DSB_CONFIG_UNITY -I$(SHEENBIDI_DIR)/Headers -c $< -o $@

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

build/%.o: src/%.c src/*.h $(QUICKJS_DIR)/quickjs.c $(PLUTOVG_DIR)/include/plutovg.h $(HARFBUZZ_DIR)/src/harfbuzz.cc $(SHEENBIDI_DIR)/Source/SheenBidi.c vendor/stb/stb_truetype.h vendor/stb/stb_image_write.h vendor/stb/stb_image.h
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

build/hydra-native: $(OBJECTS) $(QUICKJS_OBJECTS) $(PLUTOVG_OBJECTS) build/harfbuzz.o build/sheenbidi.o
	$(CXX) -o $@ $^ $(LDLIBS)

build/p5.min.js: node_modules/hydra-synth/package.json
	@mkdir -p build
	cp node_modules/p5/lib/p5.min.js $@

node_modules/hydra-synth/package.json:
	pnpm install

build/hydra-bundle.js: js/*.js package.json node_modules/hydra-synth/package.json
	node_modules/.bin/esbuild js/main.js --bundle --format=iife --outfile=$@ \
		--alias:regl=./js/regl-shim.js --alias:raf-loop=./js/raf-loop-shim.js --alias:meyda=./js/meyda-shim.js

build/editor-test: test/editor_test.c src/editor.c src/editor.h src/bidi.c src/bidi.h build/sheenbidi.o
	@mkdir -p build
	$(CC) -O1 -g -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined -I$(SHEENBIDI_DIR)/Headers -o $@ \
		test/editor_test.c src/editor.c src/bidi.c build/sheenbidi.o

build/ui-test: test/ui_test.c src/ui.c src/keymap.c src/editor.c src/bidi.c src/settings.c src/*.h build/sheenbidi.o $(QUICKJS_OBJECTS)
	@mkdir -p build
	$(CC) -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -fsanitize=address,undefined \
		$(shell pkg-config --cflags sdl2) -I$(SHEENBIDI_DIR)/Headers -I$(QUICKJS_DIR) -o $@ test/ui_test.c src/ui.c src/keymap.c \
		src/editor.c src/bidi.c src/settings.c build/sheenbidi.o $(QUICKJS_OBJECTS) $(shell pkg-config --libs sdl2) -lm -ldl -lpthread

build/audio-test: test/audio_test.c src/audio_processing.c src/audio_processing.h src/analog_agc.c src/analog_agc.h
	@mkdir -p build
	$(CC) -O1 -g -Wall -Wextra -fsanitize=address,undefined -o $@ test/audio_test.c src/audio_processing.c src/analog_agc.c -lm -ldl -lpthread

test: build/editor-test build/ui-test build/audio-test $(if $(WEBRTC_APM_LIBRARY),build/libhydra-webrtc-apm.so)
	build/editor-test
	build/ui-test
	build/audio-test
	python3 test/portal_test.py
	python3 test/gallery_test.py

check: all test
	XDG_CONFIG_HOME=$(CURDIR)/build/test-config build/hydra-native --hidden --no-vsync --no-analog-agc test/check.js
	XDG_CONFIG_HOME=$(CURDIR)/build/test-config python3 test/fetch_test.py
	XDG_CONFIG_HOME=$(CURDIR)/build/test-config python3 test/patch_bay_test.py

clean:
	rm -rf build

.PHONY: all test check clean
