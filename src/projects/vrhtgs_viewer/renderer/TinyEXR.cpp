// We use zlib for TinyEXR since including MiniZ conflicts with another dependency. 
// MiniZ is disabled at compile time, and we need to include zlib.h before tinyexr.h.
#include <zlib.h>

#define TINYEXR_IMPLEMENTATION
#include <tinyexr.h>
