#!/usr/bin/env bash
set -euo pipefail

MATRIX_DOC="docs/DESKTOP_INTEGRATION_COMPAT_MATRIX.md"

if [[ ! -f "$MATRIX_DOC" ]]; then
    echo "ERROR: missing compatibility matrix: $MATRIX_DOC" >&2
    exit 1
fi

for phase in 1 2 3 4 5 6 7; do
    if ! grep -qE "^\|[[:space:]]*${phase}[[:space:]]*\|" "$MATRIX_DOC"; then
        echo "ERROR: phase ${phase} row is missing from matrix" >&2
        exit 2
    fi

    if ! grep -qE "^- \[ \] Phase ${phase} " "$MATRIX_DOC"; then
        echo "ERROR: phase ${phase} checklist entry is missing" >&2
        exit 3
    fi
done

if ! grep -q "src/dbus.c" "$MATRIX_DOC"; then
    echo "ERROR: matrix notes must reference src/dbus.c foundation" >&2
    exit 4
fi

if ! grep -q "src/dbus_codec.c" "$MATRIX_DOC"; then
    echo "ERROR: matrix notes must reference src/dbus_codec.c foundation" >&2
    exit 5
fi

if ! grep -q "src/xdg.c" "$MATRIX_DOC"; then
    echo "ERROR: matrix notes must reference src/xdg.c foundation" >&2
    exit 6
fi

if ! grep -qE "^\|[[:space:]]*6[[:space:]]*\|.*dbus_encode_wire_header.*dbus_decode_wire_header.*(little-endian|big-endian)" "$MATRIX_DOC"; then
    echo "ERROR: phase 6 row must require explicit encode/decode cross-endian validation" >&2
    exit 7
fi

if ! grep -qE "^\|[[:space:]]*7[[:space:]]*\|.*dbus_bus_endpoint_init.*xdg_runtime_dir_resolve.*(/bus|session path)" "$MATRIX_DOC"; then
    echo "ERROR: phase 7 row must require endpoint init + XDG runtime integration validation" >&2
    exit 8
fi

if ! grep -q "## Phase 6/7 Coverage Checks" "$MATRIX_DOC"; then
    echo "ERROR: matrix must include Phase 6/7 Coverage Checks section" >&2
    exit 9
fi

if ! grep -q "dbus_encode_wire_header|dbus_decode_wire_header|DBUS_ENDIAN_(LITTLE|BIG)" "$MATRIX_DOC"; then
    echo "ERROR: phase 6 coverage command is missing from matrix" >&2
    exit 10
fi

if ! grep -q "dbus_bus_endpoint_init|xdg_runtime_dir_resolve|/bus" "$MATRIX_DOC"; then
    echo "ERROR: phase 7 coverage command is missing from matrix" >&2
    exit 11
fi

echo "Desktop integration compatibility matrix validation passed (phases 1-7 present; phase 6/7 coverage enforced)."
