/* Track R1 round 4: the renderer with dirty-page marks on its write-backs into guest memory (runtime.h R1M). Those
 * write-backs are in the front end, which is nvn_front.c now (r1src/nvn_front.c); this file is the OpenGL back end. */
#define R1_MARK 1
#include "../nvn_gl.c"
