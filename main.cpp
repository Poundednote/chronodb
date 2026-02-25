/* 
 *
 * TODO(Ray):
 *
 *  Refactor The data page and Per table request info so we can chain together an arbitrary ammount of pages with 
 *  a single pertable request info 
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

#include "chrono_platform.cpp"

#include "utils.h"
#include "metadata.cpp"
#include "workqueue.cpp"
#include "ingestion_worker.cpp"

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

void mpmc_work_queue_thread_start_routine(ThreadContext *t_ctx, uint16_t thread_id, MPMCWorkQueue *wq)
{
	t_ctx->thread_id = thread_id;

	while (!wq->stop_flag.load(std::memory_order_acquire)) {
		mpmc_work_queue_dequeue_entry(t_ctx, wq);
	}
}


/*
void create_table(ThreadContext *t_ctx, DatabaseContext *context, StringSlice8 measurement)
{
	auto table_descriptor = context->schema_maps->table_descriptor_count++;
	auto data_dict_header = (DataDictHeader *)context->data_dict_file.mapping;
	auto *rows_start = (DataDictSchema *)((uint8_t *)data_dict_header + data_dict_header->first_row_offset);
	auto row_stride = data_dict_header->row_size_in_bytes;
	uint64_t previous_row_index = atomic_fetch_add_s64_rlxd(&data_dict_header->number_of_rows, 1);

	// write new table entry into data dict
	DataDictSchema *row = (DataDictSchema *)(rows_start + previous_row_index * row_stride);
	assert(measurement.length <= 255 && "Measurment name too large must be no larger than 256 bytes");
	memcpy_s(row->table_name, MAX_TABLE_NAME_SIZE, measurement.content, measurement.length);
	row->name_length = measurement.length;

	StringBuilder8 table_dir;
	string_builder8_init(&t_ctx->transient_arena, &table_dir,
			     context->db_name.length + sizeof("/tables/") + sizeof("/dict.data") + measurement.length);
	string_builder8_append(&table_dir, context->db_name);
	string_builder8_append(&table_dir, string8_from_cstring("/tables/"));
	string_builder8_append(&table_dir, measurement);
	create_directory((const char *)table_dir.content);
	string_builder8_append(&table_dir, string8_from_cstring("/dict.data"));
	FileHandle fh = create_file((const char *)table_dir.content);

	MemoryMappedFile table_map;
	memory_map_file_handle_append(&table_map, fh, MEGABYTES(1));
	context->table_meta_file_map.insert(table_descriptor, table_map);

	TableDictHeader header = {};
	header.number_of_columns = 0;
	header.first_row_offset = sizeof(header);
	mmf_append_struct(&table_map, &header);
}
*/

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
	arena_init(&main_arena, GIGABYTES(2));

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
	string_builder8_init(&main_arena, &database_data_dict, db_context->db_name.length + sizeof("/datadict.data"));
	string_builder8_append(&database_data_dict, db_context->db_name);
	string_builder8_append(&database_data_dict, string8_from_cstring("/datadict.data"));

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
	std::thread *io_threads = arena_alloc_struct_array(&main_arena, std::thread, thread_count);
	auto thread_context_array = arena_alloc_struct_array(&main_arena, ThreadContext, thread_count);

	for (auto thread_idx = 0; thread_idx < thread_count; ++thread_idx) {
		auto &thread_context = thread_context_array[thread_idx];
    thread_context.thread_id = thread_idx;
		arena_init(&thread_context.transient_arena, MEGABYTES(32));
    if ((uintptr_t)&thread_context.schema_maps == (uintptr_t)-1) {
      __debugbreak();
    }
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

	MPMCWorkQueue io_queue = {};
	mpmc_work_queue_init(&io_queue, &main_arena, 512, MEGABYTES(1));

	for (int i = 0; i < thread_count; ++i) {
		new (io_threads + i) std::thread(mpmc_work_queue_thread_start_routine, thread_context_array + i, i, &io_queue);
	}

	// read entire file
	FILE *fd;
	fopen_s(&fd, "outfile.data", "rb");
	if (fd == NULL) {
		perror("can't open file");
	}

	// compute filesize
	fseek(fd, 0, SEEK_END);
	uint64_t filesize = ftell(fd);
	fseek(fd, 0, SEEK_SET);

	char *buffer = (char *)arena_alloc(&main_arena, filesize + 1);
	size_t bytes_read = fread(buffer, 1, filesize, fd);

	assert(bytes_read == filesize);

	fclose(fd);

	buffer[filesize] = 0; // null terminator
	uint64_t string_size = filesize;

  double thread_time_acc[2] = {};
  double submission_time_acc = 0;
  for (int i = 0; i < 100; ++i) {
		mpmc_begin_producer(&io_queue);

		int max_chunks = 4;
		auto submision_start = platform_get_high_res_timer_stamp();
		for (int i = 2; i < max_chunks; ++i) {
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
		auto ms_total_work_time =
			((double)(submission_end - submision_start) / (double)platform_high_res_timer_freq()) * 1000;
    for (int thread = 0; thread < thread_count; ++thread) {
      thread_time_acc[thread] += thread_context_array[thread].ms_time_taken;
      arena_destroy(&thread_context_array[thread].schema_maps.arena);
      arena_destroy(&thread_context_array[thread].transient_arena);
			arena_init(&thread_context_array[thread].transient_arena, MEGABYTES(32));
			thread_local_schema_maps_init(&thread_context_array[thread].schema_maps);
		}

    submission_time_acc += ms_total_work_time;
	}

  fprintf(stderr, "\n\nTotal time elapsed from submision start to end: %fms\n", submission_time_acc / 100);
  fprintf(stderr, "Avg time thread 0: %fms\n", thread_time_acc[0] / (double)100);
  fprintf(stderr, "Avg time thread 1: %fms\n", thread_time_acc[1] / (double)100);
	mpmc_work_queue_stop(&io_queue);


	for (int i = 0; i < thread_count; ++i) {
		io_threads[i].join();
	}

	return 0;
}
