#pragma once

#include <stdint.h>
#include <atomic>
#include <thread>

#include "utils.h"
#include "metadata.h"

#define WRITER_QUEUE_SIZE (64u)

struct IngestionWorkerContext;
typedef void *(WorkQueueFunc)(IngestionWorkerContext *, void *);
typedef uint32_t bool32_t;

struct MPMCWorkQueuePayload {
	WorkQueueFunc *callback;
	void *callback_args;
};
struct MPMCWorkQueueEntry {
	std::atomic<uint64_t> seq_num;
	MPMCWorkQueuePayload payload;
};

struct alignas(std::hardware_destructive_interference_size) MPMCWorkQueue {
	MPMCWorkQueueEntry *entries;
	int64_t capacity;
  int64_t consumer_count;
	// align all the atomics on cache line boundary
	alignas(std::hardware_destructive_interference_size) std::atomic<uint64_t> head;
	alignas(std::hardware_destructive_interference_size) std::atomic<uint64_t> tail;
	alignas(std::hardware_destructive_interference_size) std::atomic<uint64_t> completion_count;
	alignas(std::hardware_destructive_interference_size) std::atomic<int64_t> producer_count;

	volatile int64_t entries_in_flight;

	std::atomic<bool> stop_flag;
  std::atomic<int64_t> stop_count;
};

struct MPSCWriterQueueEntry {
  TableID table_id;
  RequestInfoAndPage request_info_and_page; 
  uint16_t thread_id;
};

struct alignas(std::hardware_destructive_interference_size) MPSCWriterQueue {
	MPSCWriterQueueEntry *entries;
  std::atomic<int64_t> *seq_nums;
	int64_t capacity;
	alignas(std::hardware_destructive_interference_size) std::atomic<int64_t> tail;

  std::atomic<bool> stop_flag;
};

void mpmc_work_queue_enqueue_entry(MPMCWorkQueue *wq, MPMCWorkQueueEntry *entry);
void inline mpmc_work_queue_begin_producer(MPMCWorkQueue *wq);
void inline mpmc_work_queue_end_producer(MPMCWorkQueue *wq);
void mpmc_work_queue_dequeue_entry(void *ctx, MPMCWorkQueue *wq);
void inline mpmc_work_queue_start_work(MPMCWorkQueue *wq);
void mpmc_work_queue_spinlock_till_finished(MPMCWorkQueue *wq);
void mpmc_work_queue_stop(MPMCWorkQueue *wq);
void mpsc_writer_init(MPSCWriterQueue *wq, Arena *a, int64_t work_capacity);
void mpsc_writer_enqueue(MPSCWriterQueue *wq, MPSCWriterQueueEntry *entry);
void mpsc_writer_stop(MPSCWriterQueue *wq);
