#pragma once
#include "metadata.h"
#include "workqueue.h"
#include "utils.h"
#include <atomic>

constexpr static int BUFFER_INFO_ARRAY_SIZE = 128;

struct DatabaseContext;

struct WriterQueues {
  MPSCWriterQueue queue_arr[3];
  alignas(64) std::atomic<uint64_t> active_version;
};

struct HotPartitionInfo {
  OSHandle file_handle;
  int64_t file_offset;
};

struct BatchedIOInfo {
  BufferInfo buffer_info_array[BUFFER_INFO_ARRAY_SIZE];
  int64_t buffer_info_count;
  uint64_t total_bytes;
};

struct WriterContext {
  Arena transient_arena;
  uint64_t prev_timestamp;
  double previous_additional_work_time_taken;
  ASIOContext asio_context;
  int64_t asio_queue_size;
  HotPartitionInfo hot_partition_file_handles[MAX_TABLES];
  PoolAllocator<BatchedIOInfo> batched_info_pool;

  volatile uint64_t timer_diffs;
  volatile int64_t run_count;

  alignas(std::hardware_destructive_interference_size) std::atomic<bool> stop_flag;
  alignas(std::hardware_destructive_interference_size) std::atomic<bool> finished;
};

MPSCWriterQueue *_get_writer_queue_from_version(WriterQueues *queues, uint64_t index);
MPSCWriterQueue *_get_worker_submit_queue(WriterQueues *queues);
void writer_queues_enqueue_entry(WriterQueues *queues, MPSCWriterQueueEntry *entry);
MPSCWriterQueue *writer_queues_swap_active_queue_return_previous(WriterQueues *queues);
void writer_queue_start_routine(DatabaseContext *db_context, WriterContext *writer_context);
