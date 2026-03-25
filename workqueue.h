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
	uint64_t capacity;
	// align all the atomics on cache line boundary
	alignas(std::hardware_destructive_interference_size) std::atomic<uint64_t> head;
	alignas(std::hardware_destructive_interference_size) std::atomic<uint64_t> tail;
	alignas(std::hardware_destructive_interference_size) std::atomic<uint64_t> completion_count;

	std::atomic<uint64_t> producer_count;
	volatile uint64_t entries_in_flight;

	std::atomic<bool32_t> stop_flag;
};

struct MPSCWriterQueueEntry {
  TableID table_id;
  RequestInfoAndPage request_info_and_page; 
  uint16_t thread_id;
};

struct alignas(std::hardware_destructive_interference_size) MPSCWriterQueue {
	MPSCWriterQueueEntry *entries;
	uint64_t capacity;
	alignas(std::hardware_destructive_interference_size) std::atomic<uint64_t> tail;
	alignas(std::hardware_destructive_interference_size) std::atomic<uint64_t> head;
};

void mpmc_work_queue_enqueue_entry(MPMCWorkQueue *wq, MPMCWorkQueueEntry entry);
void inline mpmc_work_queue_begin_producer(MPMCWorkQueue *wq);
void inline mpmc_work_queue_end_producer(MPMCWorkQueue *wq);
void mpmc_work_queue_dequeue_entry(void *ctx, MPMCWorkQueue *wq);
void inline mpmc_work_queue_start_work(MPMCWorkQueue *wq);
void mpmc_work_queue_spinlock_till_finished(MPMCWorkQueue *wq);
void mpmc_work_queue_stop(MPMCWorkQueue *wq);
void mpsc_writer_init(MPSCWriterQueue *wq, Arena *a, uint32_t work_capacity);
void mpsc_writer_enqueue(MPSCWriterQueue *wq, MPSCWriterQueueEntry entry);
