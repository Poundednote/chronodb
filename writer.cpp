/* TODO(Ray) 
 *
 * Writer needs a completion queue. The reader can peek into the writer queue for already validated data pages and read those
 * Since the writer has multiple queues this might be hard to validate. I think a seperate global completion queue would 
 * be better avoids the worker doing a bunch of queue loops. once data has been written to the disk via io uring we can just remove that index from the queue
 * Either that or the writer just deactivates a queue in batch. Its technicall faster than issuing a disk read for data that was just in memeoyr
 *
 * */

#include "writer.h"
#include "workqueue.h"

MPSCWriterQueue *_get_worker_submit_queue_from_current_index(WriterQueues *queues)
{
  uint64_t current_index = queues->active_index.load(std::memory_order::acquire);
  return &queues->queue_arr[(current_index - 2) % 3];
}

void writer_queues_enqueue_entry(WriterQueues *queues, MPSCWriterQueueEntry entry)
{

  auto queue_to_submit = _get_worker_submit_queue_from_current_index(queues);

  uint64_t tail = queue_to_submit->tail.load(std::memory_order::acquire);

  while (tail >= queue_to_submit->capacity) {
    queues->active_index.wait(tail);
    queue_to_submit = _get_worker_submit_queue_from_current_index(queues);
  }
  
  mpsc_writer_enqueue(queue_to_submit, entry);
}

void writer_queues_swap_active_queue(WriterQueues *queues)
{
  uint64_t old_index = queues->active_index.fetch_add(1, std::memory_order::release);

  auto *queue = &queues->queue_arr[old_index];
  queue->tail.store(0, std::memory_order::release);
}
