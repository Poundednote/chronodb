#include "chrono_platform.h"

#if defined(_WIN32) 
#include "win32_chrono.cpp"
#undef max
#undef min
#elif defined(__unix)
#define cpu_pause() _mm_pause()
#elif defined(_M_ARM64) || defined(__aarch64__)
#define cpu_pause() __asm__ __volatile__("yield")
#endif

