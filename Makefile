# A thin wrapper around the CMake build (CMakeLists.txt). BUILD is a direct child of this folder, because the
# program finds its tools, fonts and icon one directory above its own.
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

.PHONY: all test check clean
