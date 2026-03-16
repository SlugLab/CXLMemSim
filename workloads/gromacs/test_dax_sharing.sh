#!/bin/bash
# test_dax_sharing.sh - Verify that /dev/dax0.0 is truly shared between two VMs
#
# VM0 writes a known pattern, VM1 reads it back (and vice versa).
# If both sides see each other's writes, the DAX region is shared.
#
# Usage: bash test_dax_sharing.sh [vm0_ip] [vm1_ip]
#   Defaults: vm0=192.168.100.11  vm1=192.168.100.10

set -euo pipefail

VM0="${1:-192.168.100.11}"
VM1="${2:-192.168.100.10}"
DAX="/dev/dax0.0"
OFFSET=4096          # skip first page (shim uses cacheline 0 as alloc counter)
PATTERN_SIZE=64      # bytes

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

ok()   { echo -e "${GREEN}[PASS]${NC} $*"; }
fail() { echo -e "${RED}[FAIL]${NC} $*"; FAILURES=$((FAILURES+1)); }
info() { echo -e "${YELLOW}[INFO]${NC} $*"; }

FAILURES=0

# ---------- helper: inline C program that writes/reads the DAX device ----------
# We compile on each VM so there are no cross-arch issues.
WRITER_SRC='
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

#define DAX_ALIGN (2UL * 1024 * 1024)  /* 2 MB — DAX device alignment */

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: %s <dax> <offset> <hex-pattern>\n", argv[0]); return 1; }
    const char *dax = argv[1];
    size_t off = strtoul(argv[2], NULL, 0);
    const char *hex = argv[3];
    size_t len = strlen(hex) / 2;

    int fd = open(dax, O_RDWR);
    if (fd < 0) { perror("open"); return 1; }
    size_t map_size = off + len + DAX_ALIGN;
    /* round up to DAX alignment */
    map_size = (map_size + DAX_ALIGN - 1) & ~(DAX_ALIGN - 1);
    void *base = mmap(NULL, map_size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) { perror("mmap"); close(fd); return 1; }

    unsigned char *p = (unsigned char *)base + off;
    /* decode hex string into bytes */
    for (size_t i = 0; i < len; i++) {
        unsigned int b;
        sscanf(hex + 2*i, "%02x", &b);
        p[i] = (unsigned char)b;
    }
    /* explicit cache flush */
    for (size_t i = 0; i < len; i += 64)
        __builtin_ia32_clflush(p + i);
    __sync_synchronize();

    munmap(base, map_size);
    close(fd);
    printf("WROTE %zu bytes at offset %zu\n", len, off);
    return 0;
}
'

READER_SRC='
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

#define DAX_ALIGN (2UL * 1024 * 1024)  /* 2 MB — DAX device alignment */

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: %s <dax> <offset> <nbytes>\n", argv[0]); return 1; }
    const char *dax = argv[1];
    size_t off = strtoul(argv[2], NULL, 0);
    size_t len = strtoul(argv[3], NULL, 0);

    int fd = open(dax, O_RDONLY);
    if (fd < 0) { perror("open"); return 1; }
    size_t map_size = off + len + DAX_ALIGN;
    map_size = (map_size + DAX_ALIGN - 1) & ~(DAX_ALIGN - 1);
    void *base = mmap(NULL, map_size, PROT_READ, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) { perror("mmap"); close(fd); return 1; }

    unsigned char *p = (unsigned char *)base + off;
    /* invalidate cache lines so we see latest data */
    for (size_t i = 0; i < len; i += 64)
        __builtin_ia32_clflush(p + i);
    __sync_synchronize();

    for (size_t i = 0; i < len; i++)
        printf("%02x", p[i]);
    printf("\n");

    munmap(base, map_size);
    close(fd);
    return 0;
}
'

# ---------- connectivity check ----------
info "Checking connectivity..."
for vm in "$VM0" "$VM1"; do
    if ! ssh -o ConnectTimeout=5 "root@${vm}" "true" 2>/dev/null; then
        fail "Cannot SSH to ${vm}"
        echo "Aborting — both VMs must be reachable."
        exit 1
    fi
done
ok "Both VMs reachable"

# ---------- compile helpers on each VM ----------
info "Compiling test programs on both VMs..."
for vm in "$VM0" "$VM1"; do
    ssh "root@${vm}" "cat > /tmp/dax_writer.c && gcc -O2 -o /tmp/dax_writer /tmp/dax_writer.c" <<< "$WRITER_SRC" 2>/dev/null
    ssh "root@${vm}" "cat > /tmp/dax_reader.c && gcc -O2 -o /tmp/dax_reader /tmp/dax_reader.c" <<< "$READER_SRC" 2>/dev/null
done
ok "Compiled on both VMs"

# ---------- Test 1: VM0 writes, VM1 reads ----------
info "Test 1: VM0 writes magic pattern, VM1 reads it back"

MAGIC1="deadbeefcafebabe0123456789abcdef"
MAGIC1="${MAGIC1}${MAGIC1}"  # 64 hex chars = 32 bytes

ssh "root@${VM0}" "/tmp/dax_writer ${DAX} ${OFFSET} ${MAGIC1}" 2>/dev/null
sleep 0.2
RESULT1=$(ssh "root@${VM1}" "/tmp/dax_reader ${DAX} ${OFFSET} 32" 2>/dev/null)

if [ "$RESULT1" = "$MAGIC1" ]; then
    ok "VM0->VM1: pattern matches (${MAGIC1})"
else
    fail "VM0->VM1: MISMATCH"
    echo "  expected: ${MAGIC1}"
    echo "  got:      ${RESULT1}"
fi

# ---------- Test 2: VM1 writes, VM0 reads ----------
info "Test 2: VM1 writes different pattern, VM0 reads it back"

OFFSET2=$((OFFSET + 128))
MAGIC2="a5a5a5a5b4b4b4b4c3c3c3c3d2d2d2d2"
MAGIC2="${MAGIC2}${MAGIC2}"

ssh "root@${VM1}" "/tmp/dax_writer ${DAX} ${OFFSET2} ${MAGIC2}" 2>/dev/null
sleep 0.2
RESULT2=$(ssh "root@${VM0}" "/tmp/dax_reader ${DAX} ${OFFSET2} 32" 2>/dev/null)

if [ "$RESULT2" = "$MAGIC2" ]; then
    ok "VM1->VM0: pattern matches (${MAGIC2})"
else
    fail "VM1->VM0: MISMATCH"
    echo "  expected: ${MAGIC2}"
    echo "  got:      ${RESULT2}"
fi

# ---------- Test 3: Overwrite test — VM0 overwrites VM1's data ----------
info "Test 3: VM0 overwrites VM1's region, VM1 sees new data"

MAGIC3="ffffffffffffffffffffffffffffffff"
MAGIC3="${MAGIC3}${MAGIC3}"

ssh "root@${VM0}" "/tmp/dax_writer ${DAX} ${OFFSET2} ${MAGIC3}" 2>/dev/null
sleep 0.2
RESULT3=$(ssh "root@${VM1}" "/tmp/dax_reader ${DAX} ${OFFSET2} 32" 2>/dev/null)

if [ "$RESULT3" = "$MAGIC3" ]; then
    ok "Overwrite: VM1 sees VM0's new data"
else
    fail "Overwrite: MISMATCH"
    echo "  expected: ${MAGIC3}"
    echo "  got:      ${RESULT3}"
fi

# ---------- Test 4: Concurrent write/read (quick stress) ----------
info "Test 4: Rapid alternating writes (8 rounds)"

T4_PASS=0
T4_TOTAL=8
for i in $(seq 1 $T4_TOTAL); do
    OFF=$((OFFSET + 256 + i * 64))
    PAT=$(printf '%02x' $i | head -c 2)
    # repeat pattern to fill 32 bytes
    FULL_PAT=$(printf "${PAT}%.0s" $(seq 1 32))

    if (( i % 2 == 1 )); then
        WRITER_VM=$VM0; READER_VM=$VM1
    else
        WRITER_VM=$VM1; READER_VM=$VM0
    fi

    ssh "root@${WRITER_VM}" "/tmp/dax_writer ${DAX} ${OFF} ${FULL_PAT}" >/dev/null 2>/dev/null
    sleep 0.1
    GOT=$(ssh "root@${READER_VM}" "/tmp/dax_reader ${DAX} ${OFF} 32" 2>/dev/null)

    if [ "$GOT" = "$FULL_PAT" ]; then
        T4_PASS=$((T4_PASS + 1))
    else
        fail "  Round $i: expected ${FULL_PAT:0:16}... got ${GOT:0:16}..."
    fi
done

if [ "$T4_PASS" -eq "$T4_TOTAL" ]; then
    ok "All $T4_TOTAL rapid rounds passed"
fi

# ---------- cleanup ----------
info "Cleaning up test binaries..."
for vm in "$VM0" "$VM1"; do
    ssh "root@${vm}" "rm -f /tmp/dax_writer /tmp/dax_reader /tmp/dax_writer.c /tmp/dax_reader.c" 2>/dev/null || true
done

# ---------- summary ----------
echo ""
echo "=============================="
if [ "$FAILURES" -eq 0 ]; then
    echo -e "${GREEN}ALL TESTS PASSED${NC} — DAX device is shared between VMs"
else
    echo -e "${RED}${FAILURES} TEST(S) FAILED${NC} — DAX sharing may not be working"
fi
echo "=============================="
exit "$FAILURES"
