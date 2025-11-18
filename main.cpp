#include <stdio.h>
#include <thread>
#include <ctype.h>

#include "utils.h"
#include "workqueue.cpp"
#include "city.c"

#include "chrono_platform.cpp"

#define MAX_TAGS (256)
#define MAX_COLUMNS (64)
#define MAX_TABLE_NAME_SIZE (256)
#define MAX_PATH_SIZE (1024)

#define TEST_DATABASE_NAME "TEST_DB"

struct DatabaseContext {
	String8 db_name;
};

struct DataDictSchema {
	// strings are max 255 characters
	char table_name[MAX_TABLE_NAME_SIZE]; 
	uint8_t name_length;

	volatile uint64_t number_of_rows_in_table;
};

struct DataDictHeader {
	volatile uint64_t number_of_rows;
	uint64_t row_size_in_bytes;
	uint32_t first_row_offset;
};

struct ProcessRequestArgs {
	Arena *arena;
	String8 string;
};

static thread_local Arena thread_local_arena;

void mpmc_work_queue_thread_start_routine(MPMCWorkQueue *wq, size_t arena_size,
					  std::thread *t)
{
	arena_init(&thread_local_arena, arena_size);

	while (!wq->stop_flag.load(std::memory_order_acquire)) {
		mpmc_work_queue_dequeue_entry(wq);
	}
}

bool string_sort_cmp(StringSlice8 a, StringSlice8 b)
{
	if (a.content == 0) {
		return false;
	}

	if (b.content == 0) {
		return true;
	}

	for (int i = 0; i < a.length; ++i) {
		if (i >= b.length) {
			return false;
		}

		char lower_char_a = tolower(a[i]);
		char lower_char_b = tolower(b[i]);

		if (lower_char_a == lower_char_b) {
			continue;
		} else if (lower_char_a > lower_char_b) {
			return false;
		} else {
			return true;
		}
	}

	return true;
}

struct DataDictIndexSet {
	StringBuilder8 *keys;
	uint64_t max_buckets;
};

void data_dict_index_set_init(Arena *a, DataDictIndexSet *ddis, uint64_t max_buckets = 1024) 
{
	ddis->keys = arena_alloc_struct_array(a, StringBuilder8, max_buckets);
	ddis->max_buckets = max_buckets;
	
	// init all string builders
	for (int i = 0; i < max_buckets; ++i) {
		string_builder8_init(a, ddis->keys + i, MAX_TABLE_NAME_SIZE + 1);	
	}
}

void data_dict_index_set_lookup(DataDictIndexSet *ddis, String8 s)
{
	uint64_t hash = CityHash64((const char *)s.content, s.length);
}

String8 data_dict_index_set_lookup(DataDictIndexSet *ddis, StringSlice8 s)
{
	assert(s.length >= MAX_TABLE_NAME_SIZE && "Attempting to lookup a table name that is too large");
	if (s.length == 0) {
		return String8{};
	}

	uint64_t hash = CityHash64((const char *)s.content, s.length);
	uint64_t bucket_idx = hash % ddis->max_buckets;

	for (;bucket_idx < ddis->max_buckets; ++bucket_idx) {
		StringBuilder8 bucket_key = ddis->keys[bucket_idx];
		if (bucket_key.length == 0) {
			return String8{};
		}

		if (bucket_key == s) {
			return string_builder8_to_string(&bucket_key);
		}
	}

	return String8{};
}

void data_dict_index_set_insert(DataDictIndexSet *ddis, StringSlice8 s)
{
	assert(s.length >= MAX_TABLE_NAME_SIZE && "Attempting to insert a table string that is too large");
	uint64_t hash = CityHash64((const char *)s.content, s.length);
	uint64_t bucket_idx = hash % ddis->max_buckets;

	StringBuilder8 bucket_key;
	for (;bucket_idx < ddis->max_buckets; ++bucket_idx) {
		StringBuilder8 bucket_key = ddis->keys[bucket_idx];
		if (bucket_key == s) {
			return; // return if already in map
		}

		bucket_idx++;
	}

	// found empty slot
	memcpy_s(bucket_key.content, bucket_key.capacity, s.content, s.length);
	bucket_key.length = s.length;
}

void *process_write_request(DatabaseContext *context, MemoryMappedFile data_dict_file, String8 data_to_write)
{
	for (int request_index = 0; request_index < data_to_write.length; ++request_index) {
		if (data_to_write[request_index] == '\n') {
			continue;
		}

		StringSlice8 measurement = string8_slice_to(
			data_to_write, string8_from_cstring("["));

		//create measurement file if there isn't one
		StringSlice8 tags = string_slice_length(
			data_to_write, measurement.length,
			string_index_of(data_to_write, ']') -
				measurement.length);

		// need to sort the tags by alphabetical order
		StringSlice8 tag_arr[MAX_TAGS] = {};
		int tag_arr_size = 0;
		uint32_t start_index = 1;
		// printf("PRINT THE THING %.*s\n", tags.length, tags.content);
		// fflush(stdout);



		//NOTE(Ray): Parsing needs error handling at some point
		assert(tags[0] == '[');
		for (uint64_t i = 0; i < tags.length; ++i) {
			if (tags[i] == ' ') {
				start_index++;
				continue;
			}

			if (tags[i] == '[') {
				continue;
			}

			if (tags[i] == ',') {
				tag_arr[tag_arr_size++] =
					StringSlice8{ tags.content +
							      start_index,
						      i - start_index };
				start_index = i + 1;
			}
		}

		// save the last tag if there was one
		if (tags.length) {
			tag_arr[tag_arr_size++] =
				StringSlice8{ tags.content + start_index,
					      tags.length - start_index };
		}

		std::sort(std::begin(tag_arr), std::end(tag_arr),
			  string_sort_cmp);

		for (int i = 0; i < tag_arr_size; ++i) {
		}

		// join the array without the comma
		//
		uint64_t final_tags_size = 0;
		for (auto i = 0; i < tag_arr_size; ++i) {
			final_tags_size += tag_arr[i].length;
		}

		StringBuilder8 final_tags_string = {};
		string_builder8_init(&thread_local_arena, &final_tags_string,
				     final_tags_size + 1);

		for (int i = 0; i < tag_arr_size; ++i) {
			string_builder8_append(&final_tags_string, tag_arr[i]);
		}

		StringSlice8 columns = string_slice_length(
			data_to_write, string_index_of(data_to_write, ']'),
			data_to_write.length);
		
		StringSlice8 columns_arr[MAX_COLUMNS] = {};
		auto columns_arr_length = 0;
		start_index = 0;
		for (int i = 0; i < columns.length; ++i) {
			if (columns[i] == ' ') {
				start_index++;
				continue;
			}

			if (columns[i] == '\n') {
				columns_arr[columns_arr_length++] = StringSlice8{ columns.content + start_index, i - start_index };
				break;
			}

			if (tags[i] == ',') {
				tag_arr[tag_arr_size++] =
					StringSlice8{ columns.content +
							      start_index,
						      i - start_index };
				start_index = i + 1;
			}
		}

		// Should use tokeniser abstraction
		for (auto i = 0; i < columns_arr_length; ++i) {
			StringSlice8 column = columns_arr[i];
			// need to do a lookup of the column name in the table dict for the datatyps of columns to parse
			// 
			auto index_of_equal = string_index_of(column, '=');

			for (auto j = 0; column.length; ++j) {
				if (column[i] != ' ') {
					break;
				}
			}

			if (index_of_equal == -1) {
				fprintf(stderr, "expected '=' after column name");
				return 0;
			}

			StringSlice8 value =
				string_slice_length(column, index_of_equal + 1);

			// parse out the value
		}

		DataDictHeader *data_dict_header =
			(DataDictHeader *)data_dict_file.mapping;
		uint8_t *rows_start = (uint8_t *)data_dict_file.mapping +
				      data_dict_header->first_row_offset;
		int row_stride = data_dict_header->row_size_in_bytes;

		DataDictSchema *row;
		bool found_row = false;
		for (auto i = 0; i < data_dict_header->number_of_rows; ++i) {
			DataDictSchema *row =
				(DataDictSchema *)(rows_start + i * row_stride);

			if (measurement == row->table_name) {
				found_row = true;
				break;
			}
		}

		if (!found_row) {
			uint64_t previous_row_index = atomic_fetch_add_u64(&data_dict_header->number_of_rows);
			// write new table entry into data dict
			DataDictSchema *row =
				(DataDictSchema *)(rows_start +
						   previous_row_index *
							   row_stride);
			assert(measurement.length <= 255 &&
			       "Measurment name too large must be no larger than 256 bytes");
			memcpy_s(row->table_name,
				 MAX_TABLE_NAME_SIZE, measurement.content
				 measurement.length);
			row->name_length = measurement.length;

			// have to actually setup the table now 
			StringBuilder8 table_directory;
			uint32_t table_file_path_length 
				= context->db_name.length + 1 + measurement.length + 1 + sizeof("tables") - 1; 

			string_builder8_init(&thread_local_arena, &table_directory, table_file_path_length + 1);
			string_builder8_append(&table_directory, context->db_name);
			string_builder8_append(&table_directory, string8_from_cstring("/"));
			string_builder8_append(&table_directory, string8_from_cstring("tables/"));
			string_builder8_append(&table_directory, measurement);
		}

		// perform the writes

		arena_clear(&thread_local_arena);

		while (data_to_write[request_index] != '\n') {
			++request_index;
		}
	}

	return 0;
}

struct FakeRequestHandleArgs {
	DatabaseContext *context;
	char *data;
	uint64_t data_size;
	MemoryMappedFile data_dict_file;
};

void *fake_request_handle(void *args)
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
	for (; data[start_offset] != '\n' && data[start_offset] != '\0';
	     ++start_offset);

	if (data[start_offset] == '\n') {
		++start_offset;
	}

	char *trimmed_start = data + start_offset;
	auto final_string_size = data_size - start_offset;

	process_write_request(context, data_dict_file, String8{ (uint8_t *)trimmed_start,
						       final_string_size });
	//printf("taGas\n");
	//fflush(stdout);

	return 0;
}

int main(int argc, char *argv[])
{
	ThreadSafeArena main_arena;
	arena_init(&main_arena, GIGABYTES(1));

	DatabaseContext *db_context = arena_alloc_struct(&main_arena, DatabaseContext);
	if (argc >= 2) {
		db_context->db_name = string8_from_cstring(argv[1]);
	} else {
		db_context->db_name = string8_from_cstring(TEST_DATABASE_NAME);
	}

	// memory map the data dict file
	// nneed to just read the header to find out how much of the file to map and then we can map 
	// the filesize if there is enough space left on the file or we can add an append size to the size
	
	MemoryMappedFile data_dict_file =  memory_map_entire_file_append("./database/datadict.data", MEGABYTES(1));

	DataDictHeader *data_dict_header = (DataDictHeader *)data_dict_file.mapping;
	if (data_dict_file.filesize == 0) {
		*data_dict_header = DataDictHeader{ 0, sizeof(DataDictSchema),
						    sizeof(DataDictHeader) };
	}

	if (data_dict_header->first_row_offset != sizeof(DataDictHeader)) {
		assert(false && "Corrupted data dict header");
	}

	MPMCWorkQueue io_queue = {};
	mpmc_work_queue_init(&io_queue, &main_arena, 512, MEGABYTES(1));
	//
	// start threads
	int thread_count = 2;
	std::thread *io_threads = arena_alloc_struct_array(
		&main_arena, std::thread, thread_count);

	for (int i = 0; i < thread_count; ++i) {
		io_threads[i] =
			std::thread(mpmc_work_queue_thread_start_routine,
				    &io_queue, MEGABYTES(1), io_threads + i);
	}

	// read entire file
	FILE *fd = fopen("outfile.data", "rb");
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

	
	mpmc_begin_producer(&io_queue);

	int max_chunks = 4;
	for (int i = 2; i < max_chunks; ++i) {
		int chunk_size = filesize / max_chunks;
		char *buffer_chunk_start = buffer + i * chunk_size;
		MPMCWorkQueueEntry entry = {};
		FakeRequestHandleArgs *args =
			arena_alloc_struct(&main_arena, FakeRequestHandleArgs);
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
	mpmc_work_queue_stop(&io_queue);

	for (int i = 0; i < thread_count; ++i) {
		io_threads[i].join();
	}

	return 0;
}
