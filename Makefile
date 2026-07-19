.PHONY: build test itest run

build:
	cmake --preset ninja
	cmake --build --preset ninja

test: build
	ctest --test-dir build/ninja --output-on-failure

itest: build
	./build/ninja/itest

run: build
	./build/ninja/cha
