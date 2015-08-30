#!/bin/sh

# This script is only to help Travis CI.  You shouldn't be using it
# for anything.

git submodule update --init --recursive || exit 1

case "${CI_ENV}" in
    "gcc")
        CMAKE_EXTRA_FLAGS="-DCMAKE_C_COMPILER=gcc-4.8 -DCMAKE_CXX_COMPILER=g++-4.8"
        ;;
    "clang")
        CMAKE_EXTRA_FLAGS="-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++"
        ;;
    "gcov")
        CMAKE_EXTRA_FLAGS="-DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ -DDISABLE_BROTLI=yes -DCMAKE_BUILD_TYPE=Coverage"
        ;;
esac

cmake . \
      -DCMAKE_C_FLAGS="-Werror" \
      -DCMAKE_CXX_FLAGS="-Werror" \
      ${CMAKE_EXTRA_FLAGS} || exit 1
make VERBOSE=1 || exit 1
CTEST_OUTPUT_ON_FAILURE=TRUE make test || exit 1

if [ "${CI_ENV}" = "gcov" ]; then
    coveralls --exclude tests --exclude squash/tinycthread
fi
