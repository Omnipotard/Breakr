#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# BREAKR's engine on a computer, under UBSan and ASan (tests/engine_test.c): fidelity, timing, the dice,
# every effect at the ends of the knobs, the slicing, IMA, the generators.
set -e
cd "$(dirname "$0")/.."
mkdir -p build/test
cc -O1 -g -fsanitize=undefined,address -fno-sanitize-recover=undefined -Wall -Wextra -Wno-unused-function \
   -Wno-unused-parameter -Ifirmware/src -Ifirmware/gen -o build/test/engine tests/engine_test.c -lm
build/test/engine
