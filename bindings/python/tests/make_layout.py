"""Writes tests/layout.c from the ctypes declarations (ffaudio/_native.py): run it after changing a struct there.

    FFAUDIO_LIBRARY=<path to libffaudio> python3 tests/make_layout.py
"""
import ctypes
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
from ffaudio import _native as n  # noqa: E402

TEMPLATE = """/*
 * Prints the size of every public struct and the offset of every field, as the C compiler lays them out, for
 * test_bindings.py to compare with the ctypes declarations (ffaudio/_native.py). Made from those declarations by
 * tests/make_layout.py: run it again when a struct changes.
 */
#include <stddef.h>
#include <stdio.h>
#include "ffaudio.h"
#include "ffaudio_filters.h"
#include "ffaudio_loudness.h"
#include "ffaudio_meters.h"
#include "ffaudio_playlist.h"

#define S(type) printf("%s %zu\\n", #type, sizeof(type))
#define F(type, field) printf("%s.%s %zu\\n", #type, #field, offsetof(type, field))

int main(void) {
BODY
    return 0;
}
"""


def main():
    lines = []

    def walk(cname, cls, prefix):
        for name, t in cls._fields_:
            path = prefix + name
            lines.append(f"    F({cname}, {path});")
            if isinstance(t, type) and issubclass(t, (ctypes.Structure, ctypes.Union)):
                walk(cname, t, path + ".")

    for s in n.STRUCTS:
        lines.append(f"    S({s.__name__});")
        walk(s.__name__, s, "")
    with open(os.path.join(HERE, "layout.c"), "w") as f:
        f.write(TEMPLATE.replace("BODY", "\n".join(lines)))


if __name__ == "__main__":
    main()
