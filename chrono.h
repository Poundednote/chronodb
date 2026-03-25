#pragma once

#include <stdint.h>
#include "utils.h"

#include "chrono_platform.h"

inline double compute_time_in_ms(uint64_t start_timestamp, uint64_t end_timestamp) 
{
		return ((double)(end_timestamp - start_timestamp) / (double)platform_high_res_timer_freq()) * 1000;
}
