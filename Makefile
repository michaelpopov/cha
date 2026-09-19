.PHONY: build package-macos package-windows web-check web-stage test itest run-native-dev clean-san

build:
	cmake --preset ninja
	cmake --build --preset ninja

package-macos:
	@test -n "$(VERSION)" || (echo "usage: make package-macos VERSION=<version>" >&2; exit 2)
	./packaging/macos/package.sh "$(VERSION)"

package-windows:
	@test -n "$(VERSION)" || (echo "usage: make package-windows VERSION=<version>" >&2; exit 2)
	powershell -NoProfile -ExecutionPolicy Bypass -File packaging/windows/package.ps1 -Version "$(VERSION)"

web-check:
	cd webapp && npm run check

web-stage:
	cd webapp && npm run stage

test: build
	ctest --test-dir build/ninja --output-on-failure

itest: build
	cd workspace && ../build/ninja/itest

run-native-dev: build
	@test -n "$(CONFIG)" || (echo "usage: make run-native-dev CONFIG=/path/to/cha-config" >&2; exit 2)
	./scripts/run-native-dev.sh "$(CONFIG)"

clean-san:
	rm -rf build/asan-ubsan build/tsan
