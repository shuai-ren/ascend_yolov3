#!/bin/bash

function detect_compiler() {
  ARCH=$(uname -m)
  case "$ARCH" in
    x86_64)
      echo "g++"
      ;;
    aarch64)
      echo "aarch64-linux-gnu-g++"
      ;;
    *)
      echo "g++"
      ;;
  esac
}

COMPILER=$(detect_compiler)

rm -rf build
mkdir build && cd build
cmake -DCMAKE_SKIP_RPATH=TRUE -Dtarget=SoC -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=$COMPILER -G "Unix Makefiles" ..
make -j12
