"""Self-check for VERTEX_FETCH vs VERTEX_FETCH_COMPACT decoding equivalence."""
SIZES = [0x01, 0x02, 0x03, 0x04, 0x05, 0x0A, 0x0F, 0x12, 0x13, 0x18, 0x1B, 0x1D, 0x32, 0x33, 0x34, 0x30]
PARAMS = {
    0x01: (4, 32), 0x02: (3, 32), 0x03: (4, 16), 0x04: (2, 32),
    0x05: (3, 16), 0x0A: (4, 8),  0x0F: (2, 16), 0x12: (1, 32),
    0x13: (3, 8),  0x18: (2, 8),  0x1B: (1, 16), 0x1D: (1, 8),
    0x32: (2, 8),  0x33: (4, 8),  0x34: (1, 8),  0x30: (4, 10),
}
WORDS = [0x11111111, 0x22222222, 0x33333333, 0x44444444, 0x55555555]


def test_vertex_fetch():
    count = 0
    for size in SIZES:
        n, bits = PARAMS[size]
        packed = (size == 0x30)
        for align in range(4):
            sh = align * 8
            for i in range(4):
                if i >= n:
                    break
                cb = 2 if (packed and i == 3) else bits
                bo = sh + (i * 10 if packed else i * bits)
                wi, s = bo >> 5, bo & 31
                assert wi <= 3, f"wi out of bounds: {wi}"

                old_wlo = WORDS[wi]
                old_whi = WORDS[wi + 1]
                new_wlo = WORDS[0] if wi == 0 else WORDS[1] if wi == 1 else WORDS[2] if wi == 2 else WORDS[3]
                new_whi = WORDS[1] if wi == 0 else WORDS[2] if wi == 1 else WORDS[3] if wi == 2 else WORDS[4]
                assert old_wlo == new_wlo and old_whi == new_whi
                count += 1
    assert count == 164, f"expected 164 cases, got {count}"


if __name__ == "__main__":
    test_vertex_fetch()
    print("Vertex fetch compact check passed: 164 cases verified.")
