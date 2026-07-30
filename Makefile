.PHONY: build build-web test itest run run-console run-web

build:
	cmake --preset ninja
	cmake --build --preset ninja

build-web:
	cmake --preset ninja
	cmake --build --preset ninja --target chaweb_app

test: build
	ctest --test-dir build/ninja --output-on-failure

itest: build
	cd workspace && ../build/ninja/itest

run: build
	cd workspace && ../build/ninja/cha

run-console: build
	cd workspace && ../build/ninja/chacon --forum lobby --new dev

run-web: build-web
	cd workspace && ../build/ninja/chaweb
