# A thin wrapper around the CMake build (CMakeLists.txt). BUILD is a direct child of this folder, because the
# program finds its icon one directory above its own.
BUILD ?= build

all: $(BUILD)/build.ninja
	cmake --build $(BUILD)

$(BUILD)/build.ninja: CMakeLists.txt
	cmake -S . -B $(BUILD) -G Ninja

test: all
	cmake --build $(BUILD) --target unit-tests
	ctest --test-dir $(BUILD) --label-regex unit --output-on-failure

check: test
	ctest --test-dir $(BUILD) --label-regex check --output-on-failure

clean:
	rm -rf $(BUILD)

.PHONY: all test check sanitize clean

# The whole program with the address and undefined-behavior sanitizers, on the render check.
sanitize:
	cmake -S . -B build-sanitize -G Ninja -DCMAKE_BUILD_TYPE=Debug \
		-DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
		-DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
	cmake --build build-sanitize
	HYDRA_NATIVE_TEST=1 XDG_CONFIG_HOME=build-sanitize/test-config ASAN_OPTIONS=detect_leaks=0 \
		build-sanitize/hydra-native --hidden test/check.js
