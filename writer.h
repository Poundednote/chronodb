#pragma once
#include "metadata.h"
#include "workqueue.h"
#include "utils.h"
#include <atomic>

constexpr static int BUFFER_INFO_ARRAY_SIZE = 128; // each entry must contain 4KB
constexpr static int BUFFER_INFO_SLOT_DATA_SIZE = KILOBYTES(4);
constexpr static int PAGES_PER_INFO_ARRAY = BUFFER_INFO_ARRAY_SIZE / (DATA_PAGE_SIZE / BUFFER_INFO_SLOT_DATA_SIZE);
constexpr static int MAX_PAGES_PER_HOT_PARTITION = 16384;

struct WriterQueues {
  MPSCWriterQueue queue_arr[3];
  alignas(64) std::atomic<uint64_t> active_version;
};

struct TimestampInterval {
  uint64_t start_timestamp;
  uint64_t end_timestamp;
};

struct HotPartitionHeader {
  volatile int64_t total_bytes_written;

  volatile int64_t data_page_count;
  int64_t data_page_offsets[MAX_PAGES_PER_HOT_PARTITION];
  int64_t data_page_sizes[MAX_PAGES_PER_HOT_PARTITION];
  TimestampInterval timestamp_intervals[MAX_PAGES_PER_HOT_PARTITION];
};

struct HotPartitionInfo {
  MemoryMappedFile header_mapping;
  OSHandle file_handle;
  int64_t file_offset;
  uint64_t end_timestamp;
};

struct BatchedIOInfo {
  BufferInfo buffer_info_array[BUFFER_INFO_ARRAY_SIZE];
  TimestampInterval timestamp_intervals[PAGES_PER_INFO_ARRAY];
  int64_t page_sizes[PAGES_PER_INFO_ARRAY];
  TableID table_id;
  int64_t file_offset;
  int64_t buffer_info_count;
  uint64_t total_bytes;
  int64_t page_slot_in_array_index_start;
};

struct WriterContext {
  Arena transient_arena;
  uint64_t prev_timestamp;
  double previous_additional_work_time_taken;
  ASIOContext asio_context;
  int64_t asio_queue_size;
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
