#!/bin/bash
mkdir ./build


# Ensure COMMON_COMPILER_FLAGS is defined, e.g., COMMON_COMPILER_FLAGS="-Wall -Wextra"
#COMMON_COMPILER_FLAGS="-mmacosx-version-min=15.0 -Wall -Wextra "

if [ "$1" == "tests" ]; then
    clang++ $COMMON_COMPILER_FLAGS -O0 -g -std=c++20 test_generation.cpp -o ./build/test_generation
else
    clang++ -target arm64-apple-macos15.0 $COMMON_COMPILER_FLAGS -O0 -std=c++20 main.cpp -o ./build/main
fi
