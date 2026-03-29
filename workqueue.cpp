#include <atomic>

#include "workqueue.h"
#include "utils.h"

#define WQ_TASK(name, ...) void *name(IngestionWorkerContext *t_ctx, void *args)

void mpmc_work_queue_init(MPMCWorkQueue *wq, Arena *a, uint32_t work_capacity, int64_t consumer_count)
{
	memset(wq, 0, sizeof(MPMCWorkQueue));
	wq->capacity = work_capacity;
	wq->entries = arena_alloc_struct_array(a, MPMCWorkQueueEntry, work_capacity);
  wq->consumer_count = consumer_count;

  for (int i = 0; i < wq->capacity; ++i) {
    wq->entries[i].seq_num = i;
  }

}

void mpmc_work_queue_dequeue_entry(IngestionWorkerContext *t_ctx, MPMCWorkQueue *wq)
{
	uint64_t head = wq->head.load(std::memory_order::relaxed);
	uint64_t next_head_index = head + 1;
	uint64_t tail = wq->tail.load(std::memory_order::relaxed);

	if (head < wq->tail.load(std::memory_order::acquire)) {
		bool success = wq->head.compare_exchange_strong(
			head, next_head_index, std::memory_order::acq_rel,
			std::memory_order::relaxed);

		if (success) {
			uint64_t queue_mask = wq->capacity - 1;
			auto *entry =
				&wq->entries[head & queue_mask];
			while (entry->seq_num.load(std::memory_order::acquire) != head + 1) {
				cpu_pause();
			}

			entry->payload.callback((IngestionWorkerContext *)t_ctx, entry->payload.callback_args);
			entry->seq_num.store(head + wq->capacity, std::memory_order::release);
			wq->completion_count.fetch_add(
				1, std::memory_order::release);

		}
	} else {
    if (wq->stop_flag.load(std::memory_order::acquire)) {
      return;
    }
		uint64_t tail = wq->tail.load(std::memory_order::relaxed);
		wq->tail.wait(tail); // wait while tail is the same
	}
}

void inline mpmc_begin_producer(MPMCWorkQueue *wq)
{
	wq->producer_count.fetch_add(1, std::memory_order::release);
}

void inline mpmc_end_producer(MPMCWorkQueue *wq)
{
	wq->producer_count.fetch_sub(1, std::memory_order::release);
}

void mpmc_work_queue_enqueue_entry(MPMCWorkQueue *wq, MPMCWorkQueuePayload payload)
{
	// TODO(Ray): Make make multiproducer safe safe
	// need bounds check on size of queue and just block if you can't add anymore data

	assert(wq->tail >= wq->head); // assume the queue is never full

	for (;;) {
		uint64_t tail = wq->tail.load(std::memory_order::relaxed);
		uint64_t head = wq->head.load(std::memory_order::acquire);
		uint64_t next_tail_index = tail + 1;
		uint64_t mask = wq->capacity - 1;

		// need to use relative offsets because tail may wrap before head
		if ((tail - head) < wq->capacity) {
			if (wq->tail.compare_exchange_weak(tail, next_tail_index, std::memory_order::relaxed,
							   std::memory_order::relaxed)) {
				MPMCWorkQueueEntry *wq_entry = &wq->entries[tail & mask];
				// need to make sure the consumer has actaully finished using this data
				while (wq_entry->seq_num.load(std::memory_order::acquire) != tail) {
					cpu_pause();
				}
				wq_entry->payload = payload; // write the data
				wq_entry->seq_num.store(tail + 1, std::memory_order::release);
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
	while (wq->stop_count.load(std::memory_order::acquire) != wq->consumer_count) {
    wq->stop_count.wait(wq->stop_count);
	}
}

void mpmc_work_queue_stop(MPMCWorkQueue *wq)
{
	wq->stop_flag.store(true, std::memory_order::release);
	wq->tail.notify_all();
}

void mpsc_writer_init(MPSCWriterQueue *wq, Arena *a, int64_t work_capacity)
{
  assert(work_capacity > 0 && (work_capacity & (work_capacity - 1)) == 0);
	memset(wq, 0, sizeof(MPMCWorkQueue));
	wq->capacity = work_capacity;
	wq->entries = arena_alloc_struct_array(a, MPSCWriterQueueEntry, work_capacity);
  wq->seq_nums = arena_alloc_struct_array(a, std::atomic<int64_t>, work_capacity);
}

void mpsc_writer_enqueue(MPSCWriterQueue *wq, MPSCWriterQueueEntry *entry)
{
	auto tail = wq->tail.load(std::memory_order::relaxed);
	auto next_tail_index = tail + 1;
	auto mask = wq->capacity - 1;

	for (;;) {
		if (wq->tail.compare_exchange_weak(tail, next_tail_index, std::memory_order::relaxed, std::memory_order::relaxed)) {

      auto idx = tail & mask;
      auto wq_entry = &wq->entries[idx];
			wq_entry->request_info_and_page = entry->request_info_and_page;
			wq_entry->table_id = entry->table_id;
			wq_entry->thread_id = entry->thread_id;

			wq->seq_nums[idx].store(tail + 1, std::memory_order::release);
			wq->tail.notify_all();
			break;
		}
	}
}
