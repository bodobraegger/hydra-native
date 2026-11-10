QUICKJS_VERSION := 2026-06-04
QUICKJS_DIR := vendor/quickjs
QUICKJS_SOURCES := $(addprefix $(QUICKJS_DIR)/,quickjs.c dtoa.c libregexp.c libunicode.c cutils.c quickjs-libc.c)
QUICKJS_OBJECTS := $(patsubst $(QUICKJS_DIR)/%.c,build/quickjs/%.o,$(QUICKJS_SOURCES))
OBJECTS := build/main.o build/video.o build/audio.o build/fetch.o build/editor.o build/ui.o build/overlay.o build/keymap.o

CFLAGS := -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -I$(QUICKJS_DIR) -Ivendor/stb $(shell pkg-config --cflags sdl2)
LDLIBS := $(shell pkg-config --libs sdl2 glesv2 libpulse-simple) -lm -ldl -lpthread

all: build/hydra-native build/hydra-bundle.js

$(QUICKJS_DIR)/quickjs.c:
	mkdir -p vendor
	curl -fsSL https://bellard.org/quickjs/quickjs-$(QUICKJS_VERSION).tar.xz | tar -xJ -C vendor
	mv vendor/quickjs-$(QUICKJS_VERSION) $(QUICKJS_DIR)

STB_COMMIT := 4e9b368bfc071e97206ea5c6adba68dd0cbf8897

vendor/stb/stb_truetype.h:
	mkdir -p vendor/stb
	curl -fsSL https://raw.githubusercontent.com/nothings/stb/$(STB_COMMIT)/stb_truetype.h -o $@

$(filter-out $(QUICKJS_DIR)/quickjs.c,$(QUICKJS_SOURCES)): $(QUICKJS_DIR)/quickjs.c

build/quickjs/%.o: $(QUICKJS_DIR)/%.c
	@mkdir -p build/quickjs
	$(CC) -O2 -w -D_GNU_SOURCE -DCONFIG_VERSION=\"$(QUICKJS_VERSION)\" -c $< -o $@

build/%.o: src/%.c src/*.h $(QUICKJS_DIR)/quickjs.c vendor/stb/stb_truetype.h
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

build/hydra-native: $(OBJECTS) $(QUICKJS_OBJECTS)
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
