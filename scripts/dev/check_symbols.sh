#!/bin/bash
# Compare the functions a built libffaudio exports with abi/ffaudio.symbols: check_symbols.sh <libffaudio.so> [--update]
# Every change to the public API changes this list on purpose; --update rewrites it from the library.
set -euo pipefail
src=$(cd "$(dirname "$0")/../.." && pwd)
lib=${1:?usage: check_symbols.sh <libffaudio.so|.dll> [--update]}
list="$src/abi/ffaudio.symbols"
case "$lib" in
    *.dll) symbols=$(objdump -p "$lib" | awk '/\[Ordinal\/Name Pointer\] Table/ { on = 1; next } on && NF == 0 { on = 0 }
                                               on { print $NF }' | grep '^au_' | sort) ;;
    *)     symbols=$(nm -D --defined-only "$lib" | awk '$2 == "T" { print $3 }' | sort) ;;
esac
if [ "${2:-}" = --update ]; then
    mkdir -p "$src/abi"
    echo "$symbols" > "$list"
    echo "abi/ffaudio.symbols: $(wc -l < "$list") functions"
    exit 0
fi
if ! diff <(echo "$symbols") "$list" > /dev/null; then
    echo "Exported functions differ from abi/ffaudio.symbols (< built library, > list):" >&2
    diff <(echo "$symbols") "$list" >&2 || true
    echo "If the API change is intended: scripts/dev/check_symbols.sh <lib> --update" >&2
    exit 1
fi
echo "exports match abi/ffaudio.symbols ($(echo "$symbols" | wc -l) functions)"
