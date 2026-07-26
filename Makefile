.PHONY: build test itest run run-console

build:
	cmake --preset ninja
	cmake --build --preset ninja

test: build
	ctest --test-dir build/ninja --output-on-failure

itest: build
	cd workspace && ../build/ninja/itest

run: build
	cd workspace && ../build/ninja/cha

run-console: build
	cd workspace && ../build/ninja/chacon --room lobby --new dev
