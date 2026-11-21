#!/bin/bash
# Host-only binary-format and actual loader-allocation checks.
set -euo pipefail
cd "$(dirname "$0")/../.."
python3 -B -m unittest discover -s tools/cellular/tests -v
CELLULAR_CHECK_DIR=$(mktemp -d "${TMPDIR:-/tmp}/cellular-check.XXXXXX")
trap 'rm -rf "$CELLULAR_CHECK_DIR"' EXIT
"${CC:-clang}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
    tools/cellular/tests/memory_allocator_test.c -o "$CELLULAR_CHECK_DIR/allocator-test"
"$CELLULAR_CHECK_DIR/allocator-test"
"${CC:-clang}" -std=c11 -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
    tools/cellular/tests/spi_pio_test.c -o "$CELLULAR_CHECK_DIR/spi-test"
"$CELLULAR_CHECK_DIR/spi-test"
"${CC:-clang}" -std=c11 -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
    tools/cellular/tests/boot_lifecycle_test.c -o "$CELLULAR_CHECK_DIR/boot-test"
"$CELLULAR_CHECK_DIR/boot-test"
"${CC:-clang}" -std=c11 -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
    tools/cellular/tests/live_pcie_test.c -o "$CELLULAR_CHECK_DIR/live-pcie-test"
"$CELLULAR_CHECK_DIR/live-pcie-test"
"${CC:-clang}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
    tools/cellular/tests/test_spi8_contract.c -o "$CELLULAR_CHECK_DIR/spi8-contract"
"$CELLULAR_CHECK_DIR/spi8-contract"
"${CC:-clang}" -std=c11 -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
    tools/cellular/tests/live_spi_test.c -o "$CELLULAR_CHECK_DIR/live-spi-test"
"$CELLULAR_CHECK_DIR/live-spi-test"
"${CC:-clang}" -std=c11 -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
    tools/cellular/tests/live_modem_test.c -o "$CELLULAR_CHECK_DIR/live-modem-test"
"$CELLULAR_CHECK_DIR/live-modem-test"
"${CC:-clang}" -std=c11 -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
    tools/cellular/tests/live_link_test.c -o "$CELLULAR_CHECK_DIR/live-link-test"
"$CELLULAR_CHECK_DIR/live-link-test"
"${CC:-clang}" -std=c11 -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
    tools/cellular/tests/ring_test.c -o "$CELLULAR_CHECK_DIR/ring-test"
"$CELLULAR_CHECK_DIR/ring-test"
"${CC:-clang}" -std=c11 -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
    tools/cellular/tests/cp_memory_test.c -o "$CELLULAR_CHECK_DIR/cp-memory-test"
"$CELLULAR_CHECK_DIR/cp-memory-test"
"${CC:-clang}" -std=c11 -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
    tools/cellular/tests/udl_test.c -o "$CELLULAR_CHECK_DIR/udl-test"
"$CELLULAR_CHECK_DIR/udl-test"
"${CC:-clang}" -std=c11 -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
    tools/cellular/tests/cp_ipc_test.c -o "$CELLULAR_CHECK_DIR/cp-ipc-test"
"$CELLULAR_CHECK_DIR/cp-ipc-test"
