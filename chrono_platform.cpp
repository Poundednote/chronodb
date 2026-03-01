#include "chrono_platform.h"

#if defined(_WIN32) 
#include "win32_chrono.cpp"
#undef max
#undef min
#define cpu_pause() _mm_pause()

#elif defined(__APPLE__)
#include "macos_chrono.cpp"
#define cpu_pause() __asm__ __volatile__("yield")
#endif
