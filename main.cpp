/* 
 i
 * TODO(Ray):
 *
 *  Use tombstones for global schema id map
 *  Error handling on max column create limit and table create limit
 *  
 *
 *
 *
 *
 *
 *
 *
 *
 *
*/

#include <stdio.h>
#include <thread>
#include <stdlib.h>
#include <charconv>

#include "utils.h"
#include "chrono_platform.cpp"
#include "chrono.h"
#include "context.h"
#include "metadata.cpp"
#include "workqueue.cpp"
#include "ingestion_worker.cpp"
#include "writer.cpp"


#define TEST_DATABASE_NAME "TEST_DB"
#define NANOSECONDS_IN_NS(n) (n)
#define MICROSECONDS_IN_NS(n) (100ull * NANOSECONDS(n))
#define MILISECONDS_IN_NS(n) (100ull * MICROSECONDS(n))
#define SECONDS_IN_NS(n) (100ull * MILISECONDS(n))

#define MAX_BLOCK_TIME_DURATION_NS (SECONDS_IN_NS(10) * 60)

struct ProcessRequestArgs {
	Arena *arena;
	String8 string;
};

struct FakeRequestHandleArgs {
	DatabaseContext *context;
	char *data;
	int64_t data_size;
	MemoryMappedFile data_dict_file;
};


WQ_TASK(fake_request_handle)
{
	FakeRequestHandleArgs *typed_args = (FakeRequestHandleArgs *)args;
	auto *context = typed_args->context;
	auto *data = typed_args->data;
	auto data_size = typed_args->data_size;
	MemoryMappedFile data_dict_file = typed_args->data_dict_file;

	auto string_size = data_size;
	for (int i = data_size; i >= 0; --i) {
		if (data[i] != '\n') {
			--string_size;
		}
	}

	auto start_offset = 0;
	for (; data[start_offset] != '\n' && data[start_offset] != '\0'; ++start_offset)
		;

	if (data[start_offset] == '\n') {
		++start_offset;
	}

	char *trimmed_start = data + start_offset;
	int64_t final_string_size = data_size - start_offset;

	process_write_request(t_ctx, context, String8{ (uint8_t *)trimmed_start, final_string_size });
	//printf("taGas\n");
	//fflush(stdout);

	return 0;
}

TableDictSchema *table_dict_schema_next_row_ptr(TableDictSchema *current)
{
	int padding = (current->name_length + 1) % 4;
	return (TableDictSchema *)(current->column_name + current->name_length + padding);
}

int main(int argc, char *argv[])
{

  auto thread_count = 1;
	if (argc >= 2) {
    std::from_chars(argv[1], argv[1] + 1, thread_count);
	} else {
    auto thread_count = 1;
	}

  FILE *outfile = 0;
  fopen_s(&outfile, "out.txt", "ab");
	Arena main_arena;
	arena_init(&main_arena, GIGABYTES(1));

	DatabaseContext *db_context = arena_alloc_struct(&main_arena, DatabaseContext);
	// init maps
	auto schema_maps_buffer = &db_context->schema_maps_tripple_buffer;
	auto table_meta_file_map = &db_context->table_meta_file_map;

  db_context->db_name = string8_from_cstring(TEST_DATABASE_NAME);
  fprintf(stderr, "Starting DB with %d worker threads\n", thread_count);

	StringBuilder8 database_data_dict;
	string_builder8_init(&main_arena, &database_data_dict, db_context->db_name.length + sizeof("/datadict.meta"));
	string_builder8_append(&database_data_dict, db_context->db_name);
	string_builder8_append(&database_data_dict, string8_from_cstring("/datadict.meta"));

	MemoryMappedFile data_dict_file = {};
	memory_map_entire_file_read_only(&data_dict_file, (const char *)database_data_dict.content);

	if (data_dict_file.filesize == 0) {
		auto data_dict_header = DataDictHeader{ 0, sizeof(DataDictSchema), sizeof(DataDictHeader) };
		mmf_append_struct(&data_dict_file, &data_dict_header);
	}

	DataDictHeader *data_dict_header = (DataDictHeader *)data_dict_file.mapping;
	if (data_dict_header->first_row_offset != sizeof(DataDictHeader)) {
		assert(false && "Corrupted data dict header");
	}

	for (auto &maps : schema_maps_buffer->maps) {
		schema_maps_init(&maps, DEFAULT_TABLE_CAPACITY);
	}

	thread_safe_map_init(table_meta_file_map, &main_arena, DEFAULT_TABLE_CAPACITY);

	auto io_threads = arena_alloc_struct_array(&main_arena, std::thread, thread_count);
	auto thread_context_array = arena_alloc_struct_array(&main_arena, IngestionWorkerContext, thread_count);
  db_context->thread_context_array = thread_context_array;

  auto writer_thread_context = arena_alloc_struct(&main_arena, WriterContext);
  auto writer_thread = arena_alloc_struct(&main_arena, std::thread); 
  
	for (auto thread_idx = 0; thread_idx < thread_count; ++thread_idx) {
		auto &thread_context = thread_context_array[thread_idx];
    thread_context.thread_id = thread_idx;
		arena_init(&thread_context.transient_arena, MEGABYTES(32));
    thread_local_schema_maps_init(&thread_context.schema_maps);
	}

	auto schema_maps = get_latest_schema_maps(schema_maps_buffer);
	auto ptr = (DataDictSchema *)((uint8_t *)data_dict_header + data_dict_header->first_row_offset);
	for (auto i = 0; i < data_dict_header->number_of_rows; ++i) {
		auto &row = ptr[i];

		String8 table_name = string8_from_char_buff(row.table_name, row.name_length);

		// map file
		String8 file_path = create_table_dict_file_path_from_name(
			&main_arena, db_context, StringSlice8{ (uint8_t *)table_name.content, table_name.length });
		size_t file_size = get_filesize((const char *)file_path.content);
		MemoryMappedFile file_mapping;
		memory_map_entire_file_read_only(&file_mapping, (const char *)file_path.content);

		auto header = (TableDictHeader *)file_mapping.mapping;
		auto col_array = (TableDictSchema *)((char *)header + header->first_row_offset);

		TableID table_id = schema_maps_create_table(schema_maps, table_name);
		table_meta_file_map->insert(table_id, file_mapping);

		for (auto col = 0; col < header->number_of_columns; ++col) {
			auto column_name_slice = StringSlice8{ col_array[col].column_name, col_array[col].name_length };
			schema_maps_create_column(schema_maps, table_id, column_name_slice, col_array[col].type);
		}
	}

  // init queues
	MPMCWorkQueue io_queue = {};
	mpmc_work_queue_init(&io_queue, &main_arena, 512, thread_count);

  for (auto &queue: db_context->writer_queues.queue_arr) {
    mpsc_writer_init(&queue, &main_arena, 64);
  }

  // start your engines (threads)
	for (int i = 0; i < thread_count; ++i) {
		new (io_threads + i) std::thread(ingestion_worker_start_routine, db_context, thread_context_array + i, i, &io_queue);
	}

  new (writer_thread) std::thread(writer_queue_start_routine, db_context, writer_thread_context);

	// read entire file
	auto filesize = get_filesize("outfile.data");
	char *buffer = (char *)arena_alloc(&main_arena, filesize + 1);
	size_t bytes_read = read_entire_file("outfile.data", buffer, filesize);
	buffer[filesize] = 0; // null terminator
	uint64_t string_size = filesize;

  double thread_time_acc[2] = {};
  double submission_time_acc = 0;

  mpmc_begin_producer(&io_queue);

  int max_chunks = 20;
  auto submission_start = platform_get_high_res_timer_stamp();
  for (int iter = 0; iter < 1; ++iter) {
    for (int i = 0; i < max_chunks; ++i) {
      int chunk_size = filesize / max_chunks;
      char *buffer_chunk_start = buffer + i * chunk_size;
      MPMCWorkQueuePayload entry = {};
      FakeRequestHandleArgs *args = arena_alloc_struct(&main_arena, FakeRequestHandleArgs);
      args->data = buffer_chunk_start;
      args->data_size = chunk_size;
      args->data_dict_file = data_dict_file;
      args->context = db_context;

      entry.callback = fake_request_handle;
      entry.callback_args = args;
      mpmc_work_queue_enqueue_entry(&io_queue, entry);
    }
    mpmc_end_producer(&io_queue);
  }

	mpmc_work_queue_stop(&io_queue);
	mpmc_work_queue_spinlock_till_finished(&io_queue);


	writer_thread_context->stop_flag.store(true, std::memory_order::release);
	while (!writer_thread_context->finished.load(std::memory_order::acquire)) {
    writer_thread_context->finished.wait(writer_thread_context->finished);
	}

	for (int i = 0; i < thread_count; ++i) {
		io_threads[i].join();
	}

  writer_thread->join();
  auto submission_end = platform_get_high_res_timer_stamp();

  #if 0
  auto page_header = (DataPageHeader *)page_allocator_alloc(DATA_PAGE_SIZE);
  ASIOContext asio_context = {};
  platform_asio_create(&asio_context);

  auto read_max_runs = 100;
  auto read_avg_diff = 0;
	for (int i = 0; i < read_max_runs; ++i) {
		auto start_read_time = platform_get_high_res_timer_stamp();
		auto schema_maps_result = schema_maps_get_latest_version_inc_refcount(&db_context->schema_maps_tripple_buffer);
		auto schema_map = schema_maps_result.maps;
		auto table_id = schema_maps_lookup_table_id(schema_map, string8_from_cstring("table0"));
		assert(table_id.id != 0);
		auto &hot_partition_info = db_context->hot_partition_file_handles[table_id.index];
		auto file_header = (HotPartitionHeader *)hot_partition_info.header_mapping.mapping;
		auto data_page_count = load_acquire_64(&file_header->data_page_count);
    auto timestamp = (11111 + i*10000) % 10000000;
		auto found = false;
		auto found_idx = 0;

		MemoryMappedFile memory_map_page = {};
		for (int i = 0; i < data_page_count; ++i) {
			if (timestamp >= file_header->timestamp_intervals[i].start_timestamp &&
					timestamp <= file_header->timestamp_intervals[i].end_timestamp) {
				found = true;
				found_idx = i;
				break;
			}
		}

		platform_asio_submit_read(&asio_context, hot_partition_info.file_handle, file_header->data_page_offsets[found_idx],
															DATA_PAGE_SIZE, page_header, DATA_PAGE_SIZE, 0);

		auto entry = platform_asio_completed_entry_dequeue(&asio_context);
		while (entry == nullptr) {
			entry = platform_asio_completed_entry_dequeue(&asio_context);
		}

		auto row_ptr = (uint8_t *)page_header + sizeof(DataPageHeader);
		while (row_ptr < row_ptr + page_header->bytes_written) {
			if (*(uint64_t *)(row_ptr + *(uint64_t *)row_ptr - 8) == timestamp) {
				//fprintf(stderr, "found timestamp 11111\n");
				break;
			}
			row_ptr += *(uint64_t *)row_ptr;
		}

		read_avg_diff += platform_get_high_res_timer_stamp() - start_read_time;

    assert(found);
	}
  #endif

	for (int i = 0; i < thread_count; ++i) {
    auto avg_diff = (double)thread_context_array[i].timer_diffs / (double)thread_context_array[i].run_count;
    fprintf(outfile, "Avg time thread %d: %fms, runs: %I64d\n", i, ((double)avg_diff / (double)platform_high_res_timer_freq()) * 1000, thread_context_array[i].run_count);
  }

  auto avg_writer_diff = writer_thread_context->timer_diffs / writer_thread_context->run_count;
  fprintf(outfile, "Avg writer thread time: %fms, runs: %I64d\n", ((double)avg_writer_diff / (double)platform_high_res_timer_freq()) * 1000, writer_thread_context->run_count);
  //read_avg_diff /= read_max_runs;
  //fprintf(stderr, "Point read time: %fms\n", ((double)read_avg_diff / (double)platform_high_res_timer_freq()) * 1000);
	fprintf(outfile, "\n\nTotal time elapsed from submision start to end: %fms, \n", compute_time_in_ms(submission_start, platform_get_high_res_timer_stamp()));
  fclose(outfile);
	return 0;
}
