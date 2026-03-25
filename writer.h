#pragma once
#include "metadata.h"
#include "workqueue.h"
#include "utils.h"

struct DatabaseContext;

struct WriterQueues {
  MPSCWriterQueue queue_arr[3];
  alignas(64) std::atomic<uint64_t> active_version;
};

struct HotPartitionInfo {
  OSHandle file_handle;
  uint64_t file_offset;
};

struct WriterContext {
  Arena transient_arena;
  uint64_t prev_timestamp;
  double previous_additional_work_time_taken;
  ASIOContext asio_context;
  HotPartitionInfo hot_partition_file_handles[MAX_TABLES];
};

MPSCWriterQueue *_get_writer_queue_from_version(WriterQueues *queues, uint64_t index);
MPSCWriterQueue *_get_worker_submit_queue(WriterQueues *queues);
void writer_queues_enqueue_entry(WriterQueues *queues, MPSCWriterQueueEntry entry);
MPSCWriterQueue *writer_queues_swap_active_queue_return_previous(WriterQueues *queues);
void writer_queue_start_routine(DatabaseContext *db_context, WriterContext *writer_context);
