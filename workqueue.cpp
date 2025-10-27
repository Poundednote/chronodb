#include <atomic>

#include "workqueue.h"
#include "utils.h"

#if defined(_M_X64) || defined(__x86_64__)
#include <intrin.h>
#define cpu_pause() _mm_pause()
#elif defined(_M_ARM64) || defined(__aarch64__)
#define cpu_pause() __asm__ __volatile__("yield")
#endif

void mpmc_work_queue_init(MPMCWorkQueue *wq, Arena *a, uint32_t work_capacity,
			  uint64_t thread_arena_capacity)
{
	memset(wq, 0, sizeof(MPMCWorkQueue));
	wq->capacity = work_capacity;
	wq->entries = (MPMCWorkQueueEntry *)arena_alloc(
		a, sizeof(*wq->entries) * work_capacity);
}

void mpmc_work_queue_init(MPMCWorkQueue *wq, ThreadSafeArena *a, uint32_t work_capacity,
			  uint64_t thread_arena_capacity)
{
	memset(wq, 0, sizeof(MPMCWorkQueue));
	wq->capacity = work_capacity;
	wq->entries = (MPMCWorkQueueEntry *)arena_alloc(
		a, sizeof(*wq->entries) * work_capacity);
}

void mpmc_work_queue_dequeue_entry(MPMCWorkQueue *wq)
{
	uint64_t head = wq->head.load(std::memory_order_relaxed);
	uint64_t next_head_index = head + 1;
	uint64_t tail = wq->tail.load(std::memory_order_acquire);
	if (tail == 0xFFFFFFFFFFFFFFFF) {
		return;
	}

	if (head < wq->tail.load(std::memory_order_acquire)) {
		bool success = wq->head.compare_exchange_strong(
			head, next_head_index, std::memory_order_relaxed,
			std::memory_order_relaxed);

		if (success) {
			uint64_t queue_mask = wq->capacity - 1;
			MPMCWorkQueueEntry *entry =
				&wq->entries[head & queue_mask];

			entry->callback(entry->callback_args);

			wq->completion_count.fetch_add(
				1, std::memory_order_release);
		}
	} else {
		uint64_t tail = wq->tail.load(std::memory_order_relaxed);
		wq->tail.wait(tail); // wait while tail is the same
	}
}

void inline mpmc_begin_producer(MPMCWorkQueue *wq)
{
	wq->producer_count.fetch_add(1, std::memory_order_release);
}

void inline mpmc_end_producer(MPMCWorkQueue *wq)
{
	wq->producer_count.fetch_sub(1, std::memory_order_release);
}

void mpmc_work_queue_enqueue_entry(MPMCWorkQueue *wq, MPMCWorkQueueEntry entry)
{
	// TODO(Ray): Make make multiproducer safe safe
	// need bounds check on size of queue and just block if you can't add anymore data

	assert(wq->tail >= wq->head); // assume the queue is never full

	for (;;) {
		uint64_t tail = wq->tail.load(std::memory_order_relaxed);
		uint64_t head = wq->head.load(std::memory_order_acquire);
		uint64_t next_tail_index = tail + 1;
		uint64_t mask = wq->capacity - 1;

		// need to use relative offsets because tail may wrap before head
		if ((tail - head) < wq->capacity) {
			if (wq->tail.compare_exchange_strong(
				    tail, next_tail_index,
				    std::memory_order_relaxed,
				    std::memory_order_relaxed)) {
				wq->entries[tail & mask] = entry;
				// make sure this write happens before the notify
				std::atomic_thread_fence(
					std::memory_order_release);
				wq->tail.notify_all();
				break; // succesful can stop trying
			}
		} else {
			cpu_pause();
		}
	}
}

void mpmc_work_queue_spinlock_till_finished(MPMCWorkQueue *wq)
{
	while (wq->producer_count.load(std::memory_order_acquire) != 0 ||
	       wq->completion_count.load(std::memory_order_acquire) !=
		       wq->tail.load(std::memory_order_relaxed)) {
		cpu_pause();
	}
}

void mpmc_work_queue_stop(MPMCWorkQueue *wq)
{
        wq->stop_flag.store(true, std::memory_order_release);
        wq->tail.store(0xFFFFFFFFFFFFFFFF, std::memory_order_relaxed);
        wq->tail.notify_all();
}
