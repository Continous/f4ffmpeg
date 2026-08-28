// libplacebo's FFmpeg interoperability helpers are implemented as a single
// header. They intentionally need exactly one C translation unit with this set.
#define PL_LIBAV_IMPLEMENTATION 1
#include <libplacebo/utils/libav.h>
