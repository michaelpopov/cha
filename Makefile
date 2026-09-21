.PHONY: build package-macos package-windows web-check web-stage test itest run-native-dev clean-san

build:
	cmake --preset ninja
	cmake --build --preset ninja

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

test: build
	ctest --test-dir build/ninja --output-on-failure

ifeq ($(OS),Windows_NT)
itest:
	$(error itest is not supported on Windows)

run-native-dev:
	$(error run-native-dev is currently supported only on macOS)
else
itest: build
	cmake -E chdir workspace ../build/ninja/itest

run-native-dev: build
	$(if $(strip $(CONFIG)),,$(error usage: make run-native-dev CONFIG=/path/to/cha-config))
	./scripts/run-native-dev.sh "$(CONFIG)"
endif

clean-san:
	cmake -E remove_directory build/asan-ubsan
	cmake -E remove_directory build/tsan
