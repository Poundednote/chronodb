/* TODO(Ray) 
 *
 * Writer needs a completion queue. The reader can peek into the writer queue for already validated data pages and read those
 * Since the writer has multiple queues this might be hard to validate. I think a seperate global completion queue would 
 * be better avoids the worker doing a bunch of queue loops. once data has been written to the disk via io uring we can just remove that index from the queue
 * Either that or the writer just deactivates a queue in batch. Its technicall faster than issuing a disk read for data that was just in memeoyr
 *
 * */

#include "chrono.h"
#include "context.h"
#include "writer.h"
#include "workqueue.h"
#include "src/optick.h"

MPSCWriterQueue *_get_writer_queue_from_version(WriterQueues *queues, uint64_t version) 
{
  return &queues->queue_arr[version % 3];
}
MPSCWriterQueue *_get_worker_submit_queue(WriterQueues *queues)
{
  uint64_t active_version = queues->active_version.load(std::memory_order::acquire);
  return _get_writer_queue_from_version(queues, active_version - 2);
}

void writer_queues_enqueue_entry(WriterQueues *queues, MPSCWriterQueueEntry *entry)
{

  auto queue_to_submit = _get_worker_submit_queue(queues);

	while (queue_to_submit->tail.load(std::memory_order::acquire) >= queue_to_submit->capacity) {

		queue_to_submit = _get_worker_submit_queue(queues);
	}

	mpsc_writer_enqueue(queue_to_submit, entry);
}

MPSCWriterQueue *writer_queues_swap_active_queue_return_previous(WriterQueues *queues)
{
  uint64_t prev_version = queues->active_version.fetch_add(1, std::memory_order::release);
  auto *queue = _get_writer_queue_from_version(queues, prev_version);
  return queue;
}


/* NOTE(Ray)
    *
    * The writer thread swaps out the writer queues buffer every 10 ms so if it can clear the queue faster it
    * might be useful for it to do some other work. Can probably just time the aditional work, cache it and if it has time it can
    * can complete the work. The work can probably be some ongoing disk compaction that isn't time dependent - need to 
    * make the compaction reentrent though
    *
*/


//TODO(Ray)

SchemaMaps *copy_old_map_to_new_slot(SchemaCacheTrippleBuffer *maps) 
{
	auto version = maps->version_number.load(std::memory_order_relaxed);
	auto new_map = _get_map_at_version(maps, version + 1);
	auto old_map = _get_map_at_version(maps, version);

  while (maps->refcounts[(version + 1) % 3].load(std::memory_order_acquire) > 0) {
    cpu_pause();
  }

	std::memcpy(new_map->arena.memory, old_map->arena.memory, old_map->arena.size);
	auto new_map_arena_ptr = new_map->arena.memory;
	*new_map = *old_map; //
	new_map->arena.memory = new_map_arena_ptr;
  return new_map;
}

void process_completed_writes(ASIOContext *asio_context, PoolAllocator<BatchedIOInfo> *batched_info_pool, IngestionWorkerContext *iw_ctx_arr) 
{
  for (auto entry = platform_asio_completed_entry_dequeue(asio_context);
       entry != nullptr; entry = platform_asio_completed_entry_dequeue(asio_context)) {
    if (entry->buffer_size != platform_asio_completed_entry_get_bytes_transfered(entry)) {
      // try again
			platform_asio_submit_write_buffer_info_array(asio_context, entry->file_handle, entry->offset_to_write,
																									 (BufferInfo *)entry->buffer, entry->buffer_size, entry->user_data);

		} else {
      auto ingestion_worker_schema_maps = &iw_ctx_arr[entry->user_data].schema_maps;
      auto batched_io_info = (BatchedIOInfo *)entry->buffer; 
      auto full_pages_count = batched_io_info->total_bytes / DATA_PAGE_SIZE;
      auto full_page_entry_count = platform_asio_get_buffer_info_entry_count_from_buffer_size(DATA_PAGE_SIZE);
      for (int i = 0; i < batched_io_info->buffer_info_count; i += full_page_entry_count) {
        auto buffer = platform_asio_get_buffer_from_info(&batched_io_info->buffer_info_array[i]);
        pool_atomic_dealloc(&ingestion_worker_schema_maps->data_page_pool, buffer);
      }

      
			if (batched_io_info->total_bytes % DATA_PAGE_SIZE) {
        auto last_page_idx = full_pages_count * full_page_entry_count;
        auto buffer = platform_asio_get_buffer_from_info(&batched_io_info->buffer_info_array[last_page_idx]);
        pool_atomic_dealloc(&ingestion_worker_schema_maps->data_page_pool, buffer);
      };

			pool_dealloc(batched_info_pool, batched_io_info);
    }
  }
}


bool validate_existsing_table(DatabaseContext *db_context, WriterContext *writer_context,
															PerTableRequestInfo *request_info, SchemaMaps *schema_maps, DataPageHeader *page_header,
															TableID table_id)
{
	bool table_modified = false;
  bool should_inc_maps_version = false;
	for (int hash_tbl_idx = 0; hash_tbl_idx < MAX_COLUMNS; ++hash_tbl_idx) {
		auto hash = request_info->new_column_hashes[hash_tbl_idx];
		if (hash != 0) {
			auto column_string_ptr = request_info->string_ptrs[hash_tbl_idx];
			auto column_str_slice = StringSlice8{ column_string_ptr->buffer, column_string_ptr->length };

      // lookup in global maps because another entry may have inserted this column first
			auto column_id = schema_maps_lookup_column_id(schema_maps, table_id, column_str_slice);
      auto local_id = ColumnID{ .index = request_info->new_column_id_idxs[hash_tbl_idx], .local_flag = 1};
			auto column_data_type = request_info->new_column_types[local_id.index];
			if (column_id.id == 0) {
				if (!should_inc_maps_version) {
					schema_maps = copy_old_map_to_new_slot(&db_context->schema_maps_tripple_buffer);
					should_inc_maps_version = true;
				}

				column_id =
					schema_maps_create_column(schema_maps, table_id, column_str_slice,
																		request_info->new_column_types[request_info->new_column_id_idxs[hash_tbl_idx]]);

				table_modified = true;
			} else {
				auto actual_data_type = schema_maps_get_column_data_type(schema_maps, table_id, column_id);
				if (actual_data_type != column_data_type) {
					fprintf(stderr, "Mismatch in final writer thread between column types\n");
				}
			}

			auto header_idx = page_header->column_count++;
			page_header->column_data[header_idx] = { .id = column_id, .type = column_data_type };
			page_header->column_offsets[header_idx] = request_info->column_offsets[get_offset_idx_from_id(local_id)];
		}
	}

	if (table_modified) {
		schema_maps_lookup_table_schema_by_id(schema_maps, table_id)->version++;
	}

	for (int global_column_idx = 0; global_column_idx < DATA_PAGE_HEADER_SIZE; ++global_column_idx) {
		auto column_id = request_info->global_column_ids[global_column_idx];
		auto old_id = column_id;
		if (column_id.id != 0) {
			auto data_type = schema_maps_get_column_data_type(schema_maps, table_id, column_id);
      if (0) {
        if (data_type == ColumnDataType::INVALID) {
          // just insert it since we schema on write, the columm  may have been deleted previously but we don't care
          // we can lookup the string from the schema tagged in the global schema version

          //TODO(Ray) Think about this later think i gotta store the global strings to. If one gets deleted then im shit outta luck
          //i have the id of the column i should be able to lookup the string. 
          //I know ids are totally unique so i can constrain the problem for my hash map I have to check the strings for inserting keys
          //but i know my values are totally unique so i can just look through the map and compare ids
          //alternitavely can blitz through the hash map checking column ids and then get the string 
          /*
          auto table_map =
            _get_map_at_version(&db_context->schema_cache_triple_buffer, request_info->global_schema_version);
          if (!should_inc_maps_version) {
            schema_maps = copy_old_map_to_new_slot(&db_context->schema_maps_tripple_buffer);
            column_id = schema_maps_create_column(schema_maps, table_id, column_id, data_);
            should_inc_maps_version = true;
            */
          }
        }

			auto header_idx = page_header->column_count++;
			page_header->column_data[header_idx] = { .id = column_id, .type = data_type };
			page_header->column_offsets[header_idx] = request_info->column_offsets[get_offset_idx_from_id(old_id)];
		}
	}

  return should_inc_maps_version;
}

bool any_queue_has_work_left(WriterQueues *writer_queues)
{
    auto current_version = writer_queues->active_version.load(std::memory_order::relaxed);
    auto all_zeros = true;
    for (int i = 0; i < 3; ++i) {
		  auto queue = _get_writer_queue_from_version(writer_queues, current_version - i);
      auto queue_entries = queue->tail.load(std::memory_order::acquire);
      if (queue_entries > 0) {
        all_zeros = false;
      }
		}
  return !all_zeros;
}

HotPartitionInfo *get_hot_partition_file_info_or_create(HotPartitionInfo *hot_partition_file_handles,
																												DatabaseContext *db_context, Arena *a, TableID id,
																												StringSlice8 table_name)
{
	auto &hot_partition_info = hot_partition_file_handles[id.index];
	if (!hot_partition_info.file_handle.valid) {
		auto hot_partition_path =
			create_table_hot_partition_path_from_name(a, db_context, table_name);

		StringBuilder8 table_dir_name = {};
		string_builder8_init(a, &table_dir_name, 1024);
		string_builder8_append(&table_dir_name, db_context->db_name);
		string_builder8_append(&table_dir_name, string8_from_cstring("/tables/"));
		string_builder8_append(&table_dir_name, table_name);

		create_directory((const char *)table_dir_name.content); // assume it doesn't exist so create it

		hot_partition_info.file_offset = get_filesize((const char *)hot_partition_path.content);
		hot_partition_info.file_handle = create_file_direct_asio((const char *)hot_partition_path.content);

		if (!hot_partition_info.file_handle.valid) {
			fprintf(stderr, "Writer thread error: Unable to open hot partition file\n");
		}
	}

  return &hot_partition_info;
}

void writer_queues_advance_version_and_process_queue(DatabaseContext *db_context, WriterContext *writer_context) 
{
  auto start_processing_time = platform_get_high_res_timer_stamp();
  auto processing_queue = writer_queues_swap_active_queue_return_previous(&db_context->writer_queues);
  auto tail = processing_queue->tail.load(std::memory_order::acquire);

  auto schema_maps = get_latest_schema_maps(&db_context->schema_maps_tripple_buffer);

  std::sort(processing_queue->entries,
            processing_queue->entries + tail,
            [](MPSCWriterQueueEntry a, MPSCWriterQueueEntry b) { return a.table_id.index > b.table_id.index; });

  bool should_inc_maps_version = false;
  for (int i = 0; i < tail; ++i) {

    auto &entry = processing_queue->entries[i];
    auto &seq_num = processing_queue->seq_nums[i];

    while (seq_num.load(std::memory_order::acquire) != i + 1) {
      cpu_pause();
    }

    auto request_info = entry.request_info_and_page.request_info;
    auto current_page = entry.request_info_and_page.page_head;


    auto table_id = entry.table_id;
    auto batched_io_info = pool_alloc(&writer_context->batched_info_pool);
    while (current_page) {
      auto total_bytes = 0;
      auto page_header = (DataPageHeader *)current_page;
      page_header->column_count = 0; // always reset to 0
      if (entry.table_id.local_flag) {
        table_id = schema_maps_lookup_table_id(schema_maps, { request_info->table_name.buffer,
                                                              request_info->table_name.length });
        if (table_id.id == 0) {
          if (!should_inc_maps_version) {
            schema_maps = copy_old_map_to_new_slot(&db_context->schema_maps_tripple_buffer);
            should_inc_maps_version = true;
            table_id = schema_maps_create_table(schema_maps, { request_info->table_name.buffer,
                                                               request_info->table_name.length });
          } else { 

            table_id = schema_maps_create_table(schema_maps, { request_info->table_name.buffer,
                                                               request_info->table_name.length });
          }

          for (int hash_tbl_idx = 0; hash_tbl_idx < MAX_COLUMNS; ++hash_tbl_idx) {
            auto hash = request_info->new_column_hashes[hash_tbl_idx];

            if (hash != 0) {
              auto column_string_ptr = request_info->string_ptrs[hash_tbl_idx];
              auto column_str_slice = StringSlice8{ column_string_ptr->buffer, column_string_ptr->length };
              auto local_id = ColumnID{.index = request_info->new_column_id_idxs[hash_tbl_idx], .local_flag = 1};
              auto column_data_type =
                request_info->new_column_types[request_info->new_column_id_idxs[hash_tbl_idx]];

              auto column_id = schema_maps_create_column(schema_maps, table_id, column_str_slice, column_data_type);

              auto header_idx = page_header->column_count++;
              page_header->column_data[header_idx] = { .id = column_id, .type = column_data_type };
              page_header->column_offsets[header_idx] =
              request_info->column_offsets[get_offset_idx_from_id(local_id)];
            }
          }

        } else {
          should_inc_maps_version |=
            validate_existsing_table(db_context, writer_context, request_info, schema_maps, page_header, table_id);
        }
      } else {
        if (schema_maps_check_table_id_exists(schema_maps, entry.table_id)) {
          should_inc_maps_version |=
            validate_existsing_table(db_context, writer_context, request_info, schema_maps, page_header, table_id);
        }
      }

      
      // TODO(Ray): Get the sector size from the OS at some point

      // prepare the page
      total_bytes += page_header->bytes_written - sizeof(DataPageHeader);
      size_t aligned_size = (page_header->bytes_written + (KILOBYTES(4) - 1)) & ~(KILOBYTES(4) - 1);
      auto bytes_to_zero = aligned_size - page_header->bytes_written;
      std::memset(current_page->data + page_header->row_write_offset, 0, bytes_to_zero);
      auto ingestion_schema_maps = &db_context->thread_context_array[entry.thread_id].schema_maps;
      auto next_page = (DataPage *)page_header->next_page;
      assert(*(current_page->data + page_header->row_write_offset) == 0);

      auto buffer_info_entry_count = platform_asio_get_buffer_info_entry_count_from_buffer_size(aligned_size);
			if (batched_io_info->buffer_info_count + buffer_info_entry_count > BUFFER_INFO_ARRAY_SIZE) {
				auto hot_partition_info = get_hot_partition_file_info_or_create(
					writer_context->hot_partition_file_handles, db_context, &writer_context->transient_arena, table_id,
					StringSlice8{ request_info->table_name.buffer, request_info->table_name.length });

				while (!platform_asio_submit_write_buffer_info_array(
					&writer_context->asio_context, hot_partition_info->file_handle, hot_partition_info->file_offset,
					batched_io_info->buffer_info_array, batched_io_info->total_bytes, entry.thread_id)) {
					process_completed_writes(&writer_context->asio_context, &writer_context->batched_info_pool, db_context->thread_context_array);
				}

				hot_partition_info->file_offset += batched_io_info->total_bytes;
        batched_io_info = pool_alloc(&writer_context->batched_info_pool);
			}

			auto *buffer_info = &batched_io_info->buffer_info_array[batched_io_info->buffer_info_count];
      platform_asio_fill_multiple_buffer_info(buffer_info, current_page, buffer_info_entry_count);
      batched_io_info->total_bytes += aligned_size;
      batched_io_info->buffer_info_count += buffer_info_entry_count;
      
      page_header->next_page = aligned_size;
      current_page = next_page;
    }

		auto hot_partition_info = get_hot_partition_file_info_or_create(
			writer_context->hot_partition_file_handles, db_context, &writer_context->transient_arena, table_id,
			StringSlice8{ request_info->table_name.buffer, request_info->table_name.length });

		while (!platform_asio_submit_write_buffer_info_array(
			&writer_context->asio_context, hot_partition_info->file_handle, hot_partition_info->file_offset,
			batched_io_info->buffer_info_array, batched_io_info->total_bytes, entry.thread_id)) {
      process_completed_writes(&writer_context->asio_context, &writer_context->batched_info_pool, db_context->thread_context_array);
		}

		auto ingestion_schema_maps = &db_context->thread_context_array[entry.thread_id].schema_maps;
    pool_atomic_dealloc(&ingestion_schema_maps->per_table_request_info_pool, request_info);
  }

  if (should_inc_maps_version) {
      db_context->schema_maps_tripple_buffer.version_number.fetch_add(1, std::memory_order::release);
  }

  processing_queue->tail.store(0, std::memory_order::release);
  db_context->writer_queues.active_version.notify_all();

  arena_clear(&writer_context->transient_arena);
  writer_context->timer_diffs += platform_get_high_res_timer_stamp() - start_processing_time;
  writer_context->run_count += 1;
}

void writer_queue_start_routine(DatabaseContext *db_context, WriterContext *writer_context) 
{

  arena_init(&writer_context->transient_arena, MEGABYTES(256));
  writer_context->prev_timestamp = platform_get_high_res_timer_stamp();
  writer_context->asio_queue_size = 4096;
  platform_asio_create(&writer_context->asio_context, 1);
  pool_init(&writer_context->batched_info_pool, writer_context->asio_queue_size);

  while (!writer_context->stop_flag.load(std::memory_order::acquire)) {
		auto current_worker_queue = _get_worker_submit_queue(&db_context->writer_queues);
		auto should_swap = false;
    auto current_tail = current_worker_queue->tail.load(std::memory_order::acquire);
    
		should_swap |= compute_time_in_ms(writer_context->prev_timestamp, platform_get_high_res_timer_stamp()) > 10;
		should_swap |=  current_tail >= current_worker_queue->capacity;

    should_swap &= any_queue_has_work_left(&db_context->writer_queues);

		if (should_swap) {
      writer_queues_advance_version_and_process_queue(db_context, writer_context);
		} else {
      process_completed_writes(&writer_context->asio_context, &writer_context->batched_info_pool, db_context->thread_context_array);
    }
	}

	if (any_queue_has_work_left(&db_context->writer_queues)) {
		bool should_swap = compute_time_in_ms(writer_context->prev_timestamp, platform_get_high_res_timer_stamp()) > 10;
    if (should_swap) {
      writer_queues_advance_version_and_process_queue(db_context, writer_context);
    }
	}

  writer_context->finished.store(true, std::memory_order::release);
  writer_context->finished.notify_all();
}

