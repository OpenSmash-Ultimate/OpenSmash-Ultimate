/* Plain versions of the Visual C++ library's __std_find_first_of_trivial_pos_N, for toolsets
 * older than the one Khronos built glslang with: their msvcprt.lib has no such functions and
 * ssbu_glslang.dll would not link. tools/glslang_lib.py adds this file only when that link fails
 * for them, so a newer toolset's own (vectorized) versions are used whenever it has them.
 * Each returns the index of the first haystack element equal to any needle element, or SIZE_MAX
 * (the library's static_cast<size_t>(-1)), as microsoft/STL's vector_algorithms.cpp does. */
#include <stddef.h>
#include <stdint.h>

#define FIND_FIRST_OF_POS(n, type)                                                                       \
    size_t __stdcall __std_find_first_of_trivial_pos_##n(const void *haystack, size_t haystack_length,  \
                                                         const void *needle, size_t needle_length) {    \
        const type *h = haystack, *e = needle;                                                           \
        for (size_t i = 0; i < haystack_length; i++)                                                     \
            for (size_t j = 0; j < needle_length; j++)                                                   \
                if (h[i] == e[j]) return i;                                                              \
        return SIZE_MAX;                                                                                 \
    }
FIND_FIRST_OF_POS(1, uint8_t)
FIND_FIRST_OF_POS(2, uint16_t)
FIND_FIRST_OF_POS(4, uint32_t)
FIND_FIRST_OF_POS(8, uint64_t)
