#pragma once
#include "workqueue.h"

struct WriterQueues {
  MPSCWriterQueue queue_arr[3];
  alignas(64) std::atomic<uint64_t> active_index;
};
