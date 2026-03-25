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
#include "metadata.h"

MPSCWriterQueue *_get_writer_queue_from_version(WriterQueues *queues, uint64_t version) 
{
  return &queues->queue_arr[version % 3];
}
MPSCWriterQueue *_get_worker_submit_queue(WriterQueues *queues)
{
  uint64_t active_version = queues->active_version.load(std::memory_order::acquire);
  return _get_writer_queue_from_version(queues, active_version - 2);
}

void writer_queues_enqueue_entry(WriterQueues *queues, MPSCWriterQueueEntry entry)
{

  auto queue_to_submit = _get_worker_submit_queue(queues);

  while (queue_to_submit->tail.load(std::memory_order_acquire) >= queue_to_submit->capacity) {
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

bool validate_existsing_table(DatabaseContext *db_context, WriterContext *writer_context,
															PerTableRequestInfo *request_info, SchemaMaps *schema_maps, DataPageHeader *page_header,
															TableID table_id)
{
	bool table_modified = false;
  bool should_inc_maps_version = false;
	for (int hash_tbl_idx = 0; hash_tbl_idx < DATA_PAGE_HEADER_SIZE; ++hash_tbl_idx) {
		auto hash = request_info->new_column_hashes[hash_tbl_idx];
		if (hash != 0) {
			auto column_string_ptr = request_info->string_ptrs[hash_tbl_idx];
			auto column_str_slice = StringSlice8{ column_string_ptr->buffer, column_string_ptr->length };
			auto column_id = schema_maps_lookup_column_id(schema_maps, table_id, column_str_slice);
			auto old_id = column_id;
			auto column_data_type = request_info->new_column_types[request_info->new_column_id_idxs[hash_tbl_idx]];
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
			page_header->column_offsets[header_idx] = request_info->column_offsets[get_offset_idx_from_id(old_id)];
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

void writer_queue_start_routine(DatabaseContext *db_context, WriterContext *writer_context) 
{

  arena_init(&writer_context->transient_arena, MEGABYTES(256));
  writer_context->prev_timestamp = platform_get_high_res_timer_stamp();
  platform_asio_create(&writer_context->asio_context, 1);

  for (;;) {
		auto current_worker_queue = _get_worker_submit_queue(&db_context->writer_queues);
		auto should_swap = false;
    auto current_tail = current_worker_queue->tail.load(std::memory_order::acquire);
    
		should_swap |= compute_time_in_ms(writer_context->prev_timestamp, platform_get_high_res_timer_stamp()) > 10;
		should_swap |=  current_tail >= current_worker_queue->capacity;

    auto current_version = db_context->writer_queues.active_version.load(std::memory_order::relaxed);
    auto all_zeros = true;
    for (int i = 0; i < 3; ++i) {
		  auto queue = _get_writer_queue_from_version(&db_context->writer_queues, current_version - i);
      auto queue_entries = queue->tail.load(std::memory_order::release);
      if (queue_entries != 0) {
        all_zeros = false;
      }
		}

    if (all_zeros) {
      should_swap = false;
    }

		if (should_swap) {
      auto start_processing_time = platform_get_high_res_timer_stamp();
			auto processing_queue = writer_queues_swap_active_queue_return_previous(&db_context->writer_queues);
			// can use a relaxed load here because the queue is effectively closed and canot be touched
			auto entry_count = processing_queue->tail.load(std::memory_order::relaxed);

			auto schema_maps = get_latest_schema_maps(&db_context->schema_maps_tripple_buffer);

			std::sort(processing_queue->entries,
								processing_queue->entries + entry_count,
								[](MPSCWriterQueueEntry a, MPSCWriterQueueEntry b) { return a.table_id.index > b.table_id.index; });

			bool should_inc_maps_version = false;
			for (int i = 0; i < entry_count; ++i) {
				auto &entry = processing_queue->entries[i];
				auto request_info = entry.request_info_and_page.request_info;
        auto current_page_and_metadata = entry.request_info_and_page.page_head;
        while (current_page_and_metadata) {
					auto table_id = entry.table_id;
					auto &page_header = current_page_and_metadata->page->header;
					if (entry.table_id.local_flag) {
						table_id = schema_maps_lookup_table_id(schema_maps, { request_info->table_name.buffer,
																																	request_info->table_name.length });
						if (table_id.id == 0) {
							if (!should_inc_maps_version) {
								schema_maps = copy_old_map_to_new_slot(&db_context->schema_maps_tripple_buffer);
								should_inc_maps_version = true;
								table_id = schema_maps_create_table(schema_maps, { request_info->table_name.buffer,
																																	 request_info->table_name.length });
							}

							for (int hash_tbl_idx = 0; hash_tbl_idx < DATA_PAGE_HEADER_SIZE; ++hash_tbl_idx) {
								auto hash = request_info->new_column_hashes[hash_tbl_idx];

								if (hash != 0) {
									auto column_string_ptr = request_info->string_ptrs[hash_tbl_idx];
									auto column_str_slice = StringSlice8{ column_string_ptr->buffer, column_string_ptr->length };
									auto column_id = schema_maps_lookup_column_id(schema_maps, table_id, column_str_slice);
									auto old_id = column_id;
									auto column_data_type =
										request_info->new_column_types[request_info->new_column_id_idxs[hash_tbl_idx]];

									column_id = schema_maps_create_column(schema_maps, table_id, column_str_slice, column_data_type);

									auto header_idx = page_header.column_count++;
									page_header.column_data[header_idx] = { .id = column_id, .type = column_data_type };
									page_header.column_offsets[header_idx] =
                  request_info->column_offsets[get_offset_idx_from_id(old_id)];
								}
							}

						} else {
							should_inc_maps_version |=
								validate_existsing_table(db_context, writer_context, request_info, schema_maps, &page_header, table_id);
						}
					} else {
						if (schema_maps_check_table_id_exists(schema_maps, entry.table_id)) {
							should_inc_maps_version |=
								validate_existsing_table(db_context, writer_context, request_info, schema_maps, &page_header, table_id);
						}
					}

					auto &hot_partition_info = writer_context->hot_partition_file_handles[table_id.index];
					if (!hot_partition_info.file_handle.valid) {
						auto table_name = StringSlice8{ request_info->table_name.buffer, request_info->table_name.length };
						auto hot_partition_path =
							create_table_hot_partition_path_from_name(&writer_context->transient_arena, db_context, table_name);

						StringBuilder8 table_dir_name = {};
            string_builder8_init(&writer_context->transient_arena, &table_dir_name, 1024);
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

          
          // TODO(Ray): Get the sector size from the OS at some point
          auto &page_metadata = current_page_and_metadata->metadata;
          size_t aligned_size = (page_metadata.bytes_written + (KILOBYTES(4) - 1)) & ~(KILOBYTES(4) - 1);
          auto bytes_to_zero = aligned_size - page_metadata.bytes_written;
          std::memset((uint8_t *)current_page_and_metadata->page + page_metadata.bytes_written, 0, bytes_to_zero);
                      
					platform_asio_submit_write(&writer_context->asio_context, hot_partition_info.file_handle, hot_partition_info.file_offset,
																		 current_page_and_metadata->page, aligned_size, entry.thread_id);

          auto old_metadata = current_page_and_metadata;
          current_page_and_metadata = current_page_and_metadata->next;
          auto ingestion_schema_maps = &db_context->thread_context_array[entry.thread_id].schema_maps;
          pool_atomic_dealloc(&ingestion_schema_maps->data_page_and_metadata_pool, old_metadata);
				}

        auto ingestion_schema_maps = &db_context->thread_context_array[entry.thread_id].schema_maps;
        pool_atomic_dealloc(&ingestion_schema_maps->per_table_request_info_pool, request_info);
			}


      for (auto entry = platform_asio_completed_entry_dequeue(&writer_context->asio_context);
           entry != nullptr; entry = platform_asio_completed_entry_dequeue(&writer_context->asio_context)) {
        if (entry->buffer_size != platform_asio_completed_entry_get_bytes_transfered(entry)) {
          // try again
          platform_asio_submit_write(&writer_context->asio_context, entry->file_handle, entry->offset_to_write, 
                                     entry->buffer, entry->buffer_size, entry->user_data); 

        } else {
          auto ingestion_worker_schema_maps = &db_context->thread_context_array[entry->user_data].schema_maps;
          pool_atomic_dealloc(&ingestion_worker_schema_maps->data_page_pool, entry->buffer);
        }
      }

      if (should_inc_maps_version) {
          db_context->schema_maps_tripple_buffer.version_number.fetch_add(1, std::memory_order::release);
          //fprintf(stderr, "New schema ingest\n"); 
      }

      
      

			processing_queue->tail.store(0, std::memory_order::release);
      db_context->writer_queues.active_version.notify_all();


      auto end_processing_time = platform_get_high_res_timer_stamp();

      arena_clear(&writer_context->transient_arena);
			fprintf(stderr, "Writer thread swap buffer entries: %llu, took: %fms\n", entry_count,
							compute_time_in_ms(start_processing_time, end_processing_time));
		} else {
      cpu_pause();
    }
	}
}
