.PHONY: build package package-macos package-windows web-check web-stage test test-ui itest itest-local run-native-dev clean-san

build:
ifeq ($(OS),Windows_NT)
	powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build-windows.ps1
else
	cmake --preset ninja
	cmake --build --preset ninja
endif

package:
	$(if $(strip $(VERSION)),,$(error usage: make package VERSION=<version>))
	./packaging/macos/release.sh check
	$(MAKE) package-macos VERSION="$(VERSION)"
	./packaging/macos/release.sh upload "$(VERSION)" "packages/CHA-macos-$(VERSION).tar.gz"

ifeq ($(OS),Windows_NT)
package-macos:
	$(error package-macos must be run on macOS)
else
package-macos:
	$(if $(strip $(VERSION)),,$(error usage: make package-macos VERSION=<version>))
	./packaging/macos/package.sh "$(VERSION)"
endif

ifeq ($(OS),Windows_NT)
package-windows:
	$(if $(strip $(VERSION)),,$(error usage: make package-windows VERSION=<version>))
	powershell -NoProfile -ExecutionPolicy Bypass -File packaging/windows/package.ps1 -Version "$(VERSION)"
else
package-windows:
	$(error package-windows must be run on Windows)
endif

web-check:
	npm --prefix webapp run check

web-stage:
	npm --prefix webapp run stage

ifneq ($(OS),Windows_NT)
CHA_TEST_PLATFORM := $(shell uname -s)
endif

ifeq ($(CHA_TEST_PLATFORM),Linux)
test:
	cmake --preset ninja
	cmake --build --preset ninja --target cha_tests cha_app_tests
	ctest --test-dir build/ninja -L backend --output-on-failure
else
test: build
	ctest --test-dir build/ninja --output-on-failure
endif

test-ui:
	./tests/native/macos/test-ui.sh

ifeq ($(OS),Windows_NT)
itest itest-local:
	$(error $@ is not supported on Windows)

run-native-dev:
	$(error run-native-dev is currently supported only on macOS)
else
itest: build
	./build/ninja/itest

itest-local: build
	./build/ninja/itest --gtest_filter='-Integration.*:R2Integration.*'

run-native-dev: build
	$(if $(strip $(CONFIG)),,$(error usage: make run-native-dev CONFIG=/path/to/cha-config))
	./scripts/run-native-dev.sh "$(CONFIG)"
endif

clean-san:
	cmake -E remove_directory build/asan-ubsan
	cmake -E remove_directory build/tsan
