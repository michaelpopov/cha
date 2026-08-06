.PHONY: build build-web web-check web-stage web-e2e test itest run run-console run-web clean-san

build:
	cmake --preset ninja
	cmake --build --preset ninja

build-web:
	cmake --preset ninja
	cmake --build --preset ninja --target chaweb_app

web-check:
	cd src/resources/webapp && npm run check

web-stage:
	cd src/resources/webapp && npm run stage

web-e2e:
	cd src/resources/webapp && npm run e2e

test: build
	ctest --test-dir build/ninja --output-on-failure

itest: build
	cd workspace && ../build/ninja/itest

run: build
	cd workspace && ../build/ninja/cha

run-console: build
	cd workspace && ../build/ninja/chacon

run-web: build-web web-stage
	./bin/start-cha.sh

clean-san:
	rm -rf build/console-asan-ubsan build/console-tsan
