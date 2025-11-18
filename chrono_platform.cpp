#include "chrono_platform.h"

#define max(a, b) (a) < (b) ? (a) : (b)
#define min(a, b) (a) > (b) ? (a) : (b)

#if defined(_WIN32) 
#undef max()
#undef min()
#include "win32_chrono.cpp"
#elif defined(__unix)
#define cpu_pause() _mm_pause()
#elif defined(_M_ARM64) || defined(__aarch64__)
#define cpu_pause() __asm__ __volatile__("yield")
#endif

