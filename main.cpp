/* 
 *
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
	uint64_t data_size;
	MemoryMappedFile data_dict_file;
};


WQ_TASK(fake_request_handle)
{
	FakeRequestHandleArgs *typed_args = (FakeRequestHandleArgs *)args;
	DatabaseContext *context = typed_args->context;
	char *data = typed_args->data;
	uint64_t data_size = typed_args->data_size;
	MemoryMappedFile data_dict_file = typed_args->data_dict_file;

	uint64_t string_size = data_size;
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
	Arena main_arena;
	arena_init(&main_arena, GIGABYTES(1));

	DatabaseContext *db_context = arena_alloc_struct(&main_arena, DatabaseContext);
	// init maps
	auto schema_maps_buffer = &db_context->schema_maps_tripple_buffer;
	auto table_meta_file_map = &db_context->table_meta_file_map;


	if (argc >= 2) {
		db_context->db_name = string8_from_cstring(argv[1]);
	} else {
		db_context->db_name = string8_from_cstring(TEST_DATABASE_NAME);
	}

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

	int thread_count = 2;
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
	mpmc_work_queue_init(&io_queue, &main_arena, 512);

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

  auto iterations = 1;
  for (int i = 0; i < iterations; ++i) {
		mpmc_begin_producer(&io_queue);

		int max_chunks = 1000;
		auto submision_start = platform_get_high_res_timer_stamp();
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
		mpmc_work_queue_spinlock_till_finished(&io_queue);
		auto submission_end = platform_get_high_res_timer_stamp();
		auto ms_total_work_time = compute_time_in_ms(submision_start, submission_end);
    for (int thread = 0; thread < thread_count; ++thread) {
      thread_time_acc[thread] += thread_context_array[thread].ms_time_taken;
      if (0) {
				arena_destroy(&thread_context_array[thread].schema_maps.arena);
				arena_destroy(&thread_context_array[thread].transient_arena);
				arena_init(&thread_context_array[thread].transient_arena, MEGABYTES(32));
				thread_local_schema_maps_init(&thread_context_array[thread].schema_maps);
			}
		}

    submission_time_acc += ms_total_work_time;
	}

  fprintf(stderr, "\n\nTotal time elapsed from submision start to end: %fms\n", submission_time_acc / iterations);
  fprintf(stderr, "Avg time thread 0: %fms\n", thread_time_acc[0] / (double)iterations);
  fprintf(stderr, "Avg time thread 1: %fms\n", thread_time_acc[1] / (double)iterations);
	mpmc_work_queue_stop(&io_queue);


	for (int i = 0; i < thread_count; ++i) {
		io_threads[i].join();
	}

	return 0;
}
