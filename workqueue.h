#pragma once

#if defined(__APPLE__) && (defined(_M_ARM64) || defined (__aarch64__))
#define CACHE_LINE_SIZE 128
#elif defined (_M_x64) || defined(__x86_64__)
#define CACHE_LINE_SIZE 64
#elif defined(_WIN32) || defined(_WIN64)
#define CACHE_LINE_SIZE 64
#endif 


#include <stdint.h>
#include <atomic>
#include <thread>

typedef void *(WorkQueueFunc)(void *);
typedef uint32_t bool32_t;

struct MPMCWorkQueueEntry {
	WorkQueueFunc *callback;
	void *callback_args;
};

struct alignas(CACHE_LINE_SIZE) MPMCWorkQueue {
	MPMCWorkQueueEntry *entries;
	uint64_t capacity;
        // align all the atomics on cache line boundary
	alignas(CACHE_LINE_SIZE) std::atomic<uint64_t> head;
	alignas(CACHE_LINE_SIZE) std::atomic<uint64_t> tail;
	alignas(CACHE_LINE_SIZE) std::atomic<uint64_t> completion_count;

	std::atomic<uint64_t> producer_count;
	volatile uint64_t entries_in_flight;

        std::atomic<bool32_t> stop_flag;
};

void mpmc_work_queue_enqueue_entry(MPMCWorkQueue *wq, MPMCWorkQueueEntry entry);
void inline mpmc_work_queue_begin_producer(MPMCWorkQueue *wq);
void inline mpmc_work_queue_end_producer(MPMCWorkQueue *wq);
void mpmc_work_queue_dequeue_entry(MPMCWorkQueue *wq);
void inline mpmc_work_queue_start_work(MPMCWorkQueue *wq);
void mpmc_work_queue_spinlock_till_finished(MPMCWorkQueue *wq);
void mpmc_work_queue_stop(MPMCWorkQueue *wq);

