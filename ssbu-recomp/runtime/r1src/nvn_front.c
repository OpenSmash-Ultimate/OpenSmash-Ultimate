/* Track R1 round 4: the renderer front end with dirty-page marks on its write-backs into guest memory (runtime.h R1M).
 * It was part of nvn_gl.c (r1src/nvn_gl.c) until the front end got its own file. Nothing else differs from the tree file. */
#define R1_MARK 1
#include "../nvn_front.c"
