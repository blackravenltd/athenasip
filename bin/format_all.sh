#!/bin/sh
find src -type f \( -name "*.cpp" -o -name "*.h" \) -exec clang-format -i {} +