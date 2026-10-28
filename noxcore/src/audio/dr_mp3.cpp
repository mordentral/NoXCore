// The dr_mp3 implementation (public domain / MIT-0), compiled once: the dialog\*.wav speech is
// MPEG layer 3. Third-party code: its warnings aren't ours.
#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wshadow"
#endif

#define DR_MP3_IMPLEMENTATION
#include <dr_mp3.h>

#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
