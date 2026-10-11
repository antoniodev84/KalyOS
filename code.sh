#!/bin/bash
for file in $(find "${1:-.}" -type f \( -name "*.c" -o -name "*.h" -o -name "*.asm" -o -name "*.md" -o -name "*.txt" -o -name "*.sh" -o -name "Makefil*" -o -name "*.S" -o -name "*.s" \)); do
    echo "===== start of $file =====" && cat "$file" && echo "===== end of $file ====="
done
