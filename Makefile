.PHONY: build build-web import-dev package-linux package-macos web-check web-stage web-e2e test itest run run-web-dev clean-san

build:
	cmake --preset ninja
	cmake --build --preset ninja

build-web:
	cmake --preset ninja
	cmake --build --preset ninja --target chaweb_app

package-linux:
	@test -n "$(VERSION)" || (echo "usage: make package-linux VERSION=<version>" >&2; exit 2)
	./scripts/package-linux.sh "$(VERSION)"

package-macos:
	@test -n "$(VERSION)" || (echo "usage: make package-macos VERSION=<version>" >&2; exit 2)
	./scripts/package-macos.sh "$(VERSION)"

web-check:
	cd webapp && npm run check

web-stage:
	cd webapp && npm run stage

web-e2e:
	cd webapp && npm run e2e

test: build
	ctest --test-dir build/ninja --output-on-failure

itest: build
	cd workspace && ../build/ninja/itest

run: build-web web-stage
	./bin/start-cha.sh

import-dev: build-web
	@test -n "$(CONFIG)" || (echo "usage: make import-dev CONFIG=/path/to/cha.toml" >&2; exit 2)
	./build/ninja/chaweb --config="$(CONFIG)" --import packaging/linux/import-seed

# The API server behind 'npm run dev'. It listens on the port the Vite proxy
# targets, which is not the port the staged loop above uses; see
# webapp/README.md. Browse the shell at 127.0.0.1:5173, not here.
# It stages only because chaweb refuses to start without a web/index.html; the
# editable loop serves its shell from Vite and never reads the staged one.
run-web-dev: build-web web-stage
	@test -n "$(CONFIG)" || (echo "usage: make run-web-dev CONFIG=/path/to/cha.toml" >&2; exit 2)
	./build/ninja/chaweb --root bin --config="$(CONFIG)"

clean-san:
	rm -rf build/asan-ubsan build/tsan
