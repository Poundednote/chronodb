//TODO(Ray) store the hash value of the key inside the hash table
// currently we are comparing string_views which is O(n) even though we already know the hash
// we can just store the hash alongside the key
// We probably don't need to iterate often over the keys but if it is required we should begin storing the keys in a seperate array for spatial locality
//
//
//
//
//
//
//
//
//
//
//
//
//
//
//
//
//
//
//
// TODO(Ray):
//	- Make sure all memory is initilised and caches are copied into correctly
//      - Work on the local schema changes and submitting data to pool allocators

#include <stdio.h>
#include <thread>
#include <ctype.h>
#include <charconv>
#include <system_error>
#include <string>
#include <algorithm>

#include "chrono_platform.cpp"

#include "utils.h"
#include "metadata.cpp"
#include "workqueue.cpp"
#include "city.c"


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

struct ThreadContext {
	uint16_t thread_id;
	Arena transient_arena;
	SchemaMaps schema_maps;
};

void mpmc_work_queue_thread_start_routine(ThreadContext *t_ctx, uint16_t thread_id, MPMCWorkQueue *wq)
{
	t_ctx->thread_id = thread_id;

	while (!wq->stop_flag.load(std::memory_order_acquire)) {
		mpmc_work_queue_dequeue_entry(t_ctx, wq);
	}
}

void push_column_into_schema_maps(SchemaMaps *schema_maps, TableDescriptor table_descriptor, StringSlice8 column_string,
				  ColumnDataType data_type)
{
	auto table_schema = &schema_maps->table_schemas[table_descriptor];
	auto column_idx = table_schema->n_columns++;

	if (column_idx >= table_schema->max_columns) {
		//TODO(Ray) realloc whole arena if we run out of space
		// and memcpy if we run out of space
	}

	auto &target_column_string = table_schema->column_names[column_idx];
	auto string_data_arena = table_schema->string_data_arena;

	auto str_alloc_size = column_string.length + 1; // need space for null term
	target_column_string.content = (uint8_t *)arena_alloc(&string_data_arena, str_alloc_size);
	target_column_string.length = column_string.length;

	std::memset(column_string.content, 0, column_string.length + 1);
	std::memcpy((void *)target_column_string.content, column_string.content, column_string.length);

	table_schema->column_data_types[column_idx] = data_type;
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

struct ParseValueResult {
	String8 err_msg;
	ColumnDataType type;
	ColumnData data;
};

void allocate_table_schema_in_schema_cache(SchemaMaps *schema_maps, TableDescriptor table_descriptor, uint64_t string_bytes_to_allocate,
						  size_t column_capacity)
{
	TableSchema &table_schema = schema_maps->table_schemas[table_descriptor];
	Arena &string_data_arena = table_schema.string_data_arena;

	// over allocate for number of columns
	auto capacity_to_alloc = string_bytes_to_allocate * 2;
	string_data_arena.memory = arena_alloc(&schema_maps->arena, capacity_to_alloc);
	string_data_arena.capacity = capacity_to_alloc;

	table_schema.max_columns = column_capacity;
	table_schema.column_data_types = arena_alloc_struct_array(&schema_maps->arena, ColumnDataType, table_schema.max_columns);
	table_schema.column_names = arena_alloc_struct_array(&schema_maps->arena, String8, table_schema.max_columns);
}

ParseValueResult parse_string(DatabaseContext *context, StringSlice8 string)
{
	ParseValueResult result = {};
	result.type = ColumnDataType::VARCHAR;
	result.data.varchar.content = string.content;
	bool double_quote = string[0] == '"';
	for (auto i = 0; i < string.length; ++i) {
		if (string[i] == '\n') {
			result.err_msg = string8_from_cstring("Unterminated string literal");
			return result;
		}

		if (double_quote && string[i] == '"') {
			result.data.varchar.length = i + 1;
			return result;
		} else if (!double_quote && string[i] == '\'') {
			result.data.varchar.length = i + 1;
			return result;
		}
	}

	return result;
}

ParseValueResult parse_value(DatabaseContext *context, StringSlice8 value)
{
	ParseValueResult result = {};
	bool is_string = value[0] == '"' || value[0] == '\'';

	if (!is_string) {
		result.type = ColumnDataType::INT64;
		auto parsing_res =
			std::from_chars((char *)value.content, (char *)value.content + value.length, result.data.int64);

		if (parsing_res.ec == std::errc::invalid_argument) {
			auto parsing_res = std::from_chars((char *)value.content, (char *)value.content + value.length,
							   result.data.dbl);

			result.type = ColumnDataType::DOUBLE;
			if (parsing_res.ec == std::errc::invalid_argument) {
				result.type = ColumnDataType::INVALID;
				result.err_msg = string8_from_cstring("Error parsing column value as int or float");
			}
		}
	} else {
		result = parse_string(context, value);
	}

	return result;
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

TableDescriptor create_table_in_schema_maps(SchemaMaps *schema_maps, StringSlice8 measurement,
								uint64_t default_columns_capacity = 8,
								uint64_t string_bytes_to_allocate = 0)
{
	auto table_descriptor = schema_maps->last_descriptor++;
	if (schema_maps->last_descriptor >= schema_maps->max_descriptor_limit) {
		// TODO(Ray) need to resize all arrays to fit since we will excede memory bounds
	}
	schema_maps->table_descriptor_map.insert(measurement, table_descriptor);

	string_bytes_to_allocate = string_bytes_to_allocate ? string_bytes_to_allocate : string_bytes_to_allocate * 32;
	allocate_table_schema_in_schema_cache(schema_maps, table_descriptor, default_columns_capacity,
					      string_bytes_to_allocate);

	return table_descriptor;
}

void *process_write_request(ThreadContext *t_ctx, DatabaseContext *context, String8 data_to_write)
{
	auto schema_maps = &t_ctx->schema_maps;

	int64_t column_data_to_write = 0;
	int64_t total_column_index = 0;
	for (int request_index = 0; request_index < data_to_write.length; ++request_index) {
		if (data_to_write[request_index] == '\n') {
			continue;
		}

		StringSlice8 measurement = string8_slice_to(data_to_write, string8_from_cstring("["));
		auto table_descriptor_ptr = schema_maps->table_descriptor_map.get(measurement);
		TableDescriptor table_descriptor = {};
		if (!table_descriptor_ptr) {
			table_descriptor = create_table_in_schema_maps(schema_maps, measurement);
		} else {
			table_descriptor = *table_descriptor_ptr;
		}

		TableSchema &table_schema = t_ctx->schema_maps.table_schemas[table_descriptor]; 

		StringSlice8 tags = string_slice_length(data_to_write, measurement.length,
							string_index_of(data_to_write, ']') - measurement.length);

		// need to sort the tags by alphabetical order
		StringSlice8 tag_arr[MAX_TAGS] = {};
		int tag_arr_size = 0;
		uint32_t start_index = 1;
		//
		//TODO(Ray): Parsing needs error handling at some point
		assert(tags[0] == '[');
		for (auto i = 0; i < tags.length; ++i) {
			if (tags[i] == ' ') {
				start_index++;
				continue;
			}

			if (tags[i] == '[') {
				continue;
			}

			if (tags[i] == ',') {
				tag_arr[tag_arr_size++] = StringSlice8{ tags.content + start_index, i - start_index };
				start_index = i + 1;
			}
		}

		// save the last tag if there was one
		if (tags.length) {
			tag_arr[tag_arr_size++] = StringSlice8{ tags.content + start_index, tags.length - start_index };
		}

		std::sort(std::begin(tag_arr), std::end(tag_arr), string_sort_cmp);

		for (int i = 0; i < tag_arr_size; ++i) {
		}

		// join the array without the comma
		//
		uint64_t final_tags_size = 0;
		for (auto i = 0; i < tag_arr_size; ++i) {
			final_tags_size += tag_arr[i].length;
		}

		StringBuilder8 final_tags_string = {};
		string_builder8_init(&t_ctx->transient_arena, &final_tags_string, final_tags_size + 1);

		for (int i = 0; i < tag_arr_size; ++i) {
			string_builder8_append(&final_tags_string, tag_arr[i]);
		}

		StringSlice8 columns = string_slice_length(data_to_write, string_index_of(data_to_write, ']') + 1);

		StringSlice8 columns_arr[MAX_COLUMNS] = {};
		auto columns_arr_length = 0;
		start_index = 0;
		for (int i = 0; i < columns.length; ++i) {
			if (columns[i] == ' ') {
				start_index++;
				continue;
			}

			if (columns[i] == '\r') {
				continue;
			}

			if (columns[i] == '\n') {
				columns_arr[columns_arr_length++] =
					StringSlice8{ columns.content + start_index, i - start_index };
				break;
			}

			if (columns[i] == ',') {
				columns_arr[columns_arr_length++] =
					StringSlice8{ columns.content + start_index, i - start_index };
				start_index = i + 1;
			}
		}

		// Should use tokeniser abstraction
		for (auto i = 0; i < columns_arr_length; ++i) {
			StringSlice8 column = columns_arr[i];
			// need to do a lookup of the column name in the table dict for the datatyps of columns to parse
			// table
			auto index_of_equal = string_index_of(column, '=');

			if (index_of_equal == -1) {
				fprintf(stderr, "expected '=' after column name");
				return 0;
			}

			StringSlice8 value = string_slice_length(column, index_of_equal + 1);
			ParseValueResult parsed_val = parse_value(context, value);

			ColumnData data = parsed_val.data;

			auto parsed_column_name = string_slice_length(column, 0, index_of_equal);

			ColumnDataType column_data_type;
			String8 column_name;
			for (int j = 0; j < table_schema.n_columns; ++j) {
				String8 schema_name = table_schema.column_names[j];
				if (parsed_column_name == schema_name) {
					column_data_type = table_schema.column_data_types[j];
					column_name = table_schema.column_names[j];
				}
			}

			if (column_name.length == 0) {
				int column_index = table_schema.n_columns++;
				push_column_into_schema_maps(schema_maps, table_descriptor, parsed_column_name,
							     parsed_val.type);
			} else if (parsed_val.type != column_data_type) {
				fprintf(stderr, "Mismatch in column data type");
				return 0;
			}

			// check the timestamp to the latest. if its o3 then shove it into a buffer for sorting 
			// another thread that isn't the writer thread can sort through this data.
			// If its in order just continue to append it to the pool in the binary format
			// get a pool for each column and just append to that and send that through to the writer thread
			// use a hashmap to get a pool block and store it. we can clear this map every request
		}

		while (data_to_write[request_index] != '\n') {
			++request_index;
		}
	}


	arena_clear(&t_ctx->transient_arena);

	return 0;
}

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
	auto global_schema_maps = (SchemaMaps *)pool_alloc(&db_context->schema_maps_pool);
	auto global_table_schemas = global_schema_maps->table_schemas;
	auto global_table_descriptor = &global_schema_maps->table_descriptor_map;
	auto table_meta_file_map = &db_context->table_meta_file_map;
	hash_map_init(global_table_descriptor, &main_arena, 256);
	thread_safe_map_init(table_meta_file_map, &main_arena, 256);

	pool_init(&db_context->schema_maps_pool, 8, sizeof(SchemaMaps));

	// Init pool allocator
	{
		auto block_size = MEGABYTES(1);
		auto block_count = 512;
		auto pool_slots_memory_size = block_size * block_count;
		auto memory = arena_alloc(&main_arena, pool_slots_memory_size);

		pool_init(&db_context->writer_thread_data_pool, block_count, block_size, memory, pool_slots_memory_size);
	}

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
	memory_map_entire_file_append(&data_dict_file, (const char *)database_data_dict.content, KILOBYTES(256));

	if (data_dict_file.filesize == 0) {
		auto data_dict_header = DataDictHeader{ 0, sizeof(DataDictSchema), sizeof(DataDictHeader) };
		mmf_append_struct(&data_dict_file, &data_dict_header);
	}

	DataDictHeader *data_dict_header = (DataDictHeader *)data_dict_file.mapping;
	if (data_dict_header->first_row_offset != sizeof(DataDictHeader)) {
		assert(false && "Corrupted data dict header");
	}

	auto max_descriptor_limit = std::max(data_dict_header->number_of_rows * 2, 10ll);
	global_table_schemas = arena_alloc_struct_array(&global_schema_maps->arena, TableSchema, max_descriptor_limit);

	int thread_count = 2;
	std::thread *io_threads = arena_alloc_struct_array(&main_arena, std::thread, thread_count);
	ThreadContext *thread_context_array = arena_alloc_struct_array(&main_arena, ThreadContext, thread_count);
	Arena *thread_context_permanent_arenas = arena_alloc_struct_array(&main_arena, Arena, thread_count);

	for (auto thread_idx = 0; thread_idx < thread_count; ++thread_idx) {
		auto &thread_context = thread_context_array[thread_idx];
		arena_init(&thread_context.transient_arena, MEGABYTES(32));
		auto &schema_maps = thread_context.schema_maps;
		arena_init(&thread_context.schema_maps.arena, MEGABYTES(512));
		
		schema_maps.max_descriptor_limit = max_descriptor_limit;
		schema_maps.table_schemas = arena_alloc_struct_array(&schema_maps.arena, TableSchema,
								       max_descriptor_limit);
		schema_maps.table_pool_map =
			arena_alloc_struct_array(&schema_maps.arena, void *, max_descriptor_limit);
	}

	// Fill schema_caches
	auto ptr = (DataDictSchema *)((uint8_t *)data_dict_header + data_dict_header->first_row_offset);
	for (auto i = 0; i < data_dict_header->number_of_rows; ++i) {
		auto &row = ptr[i];

		String8 table_name = string8_from_char_buff(row.table_name, row.name_length);

		// map file
		String8 file_path = create_table_dict_file_path_from_name(
			&main_arena, db_context, StringSlice8{ (uint8_t *)table_name.content, table_name.length });
		size_t file_size = get_filesize((const char *)file_path.content);
		MemoryMappedFile file_mapping;
		memory_map_entire_file_append(&file_mapping, (const char *)file_path.content, file_size + KILOBYTES(128));

		auto header = (TableDictHeader *)file_mapping.mapping;
		auto first_row = (TableDictSchema *)((char *)header + header->first_row_offset);
		auto current_row = first_row;
		auto string_bytes_to_allocate = 0;

		for (auto col = 0; col < header->number_of_columns; ++col) {
			string_bytes_to_allocate += current_row->name_length + 1;
			current_row = table_dict_schema_next_row_ptr(current_row);
		}
		string_bytes_to_allocate *= 2;

		auto table_schema_column_capacity = header->number_of_columns * 2;

		{
			TableDescriptor table_descriptor = create_table_in_schema_maps(
				global_schema_maps, table_name, table_schema_column_capacity, string_bytes_to_allocate);

			table_meta_file_map->insert(table_descriptor, file_mapping);

			for (auto col = 0; col < header->number_of_columns; ++col) {
				auto column_name_slice =
					StringSlice8{ current_row->column_name, current_row->name_length };
				push_column_into_schema_maps(global_schema_maps, table_descriptor, column_name_slice,
							     current_row->type);
			}
		}

		{
			for (auto thread_idx = 0; thread_idx < thread_count; ++thread_idx) {
				auto &thread_context = thread_context_array[thread_idx];
				auto schema_maps = &thread_context.schema_maps;
				TableDescriptor table_descriptor =
					create_table_in_schema_maps(schema_maps, table_name,
								   string_bytes_to_allocate, header->number_of_columns);

				for (auto col = 0; col < header->number_of_columns; ++col) {
					auto column_name_slice =
						StringSlice8{ current_row->column_name, current_row->name_length };
					push_column_into_schema_maps(schema_maps, table_descriptor,
								      column_name_slice, current_row->type);
				}
			}
		}

		current_row = table_dict_schema_next_row_ptr(current_row);
	}

	MPMCWorkQueue io_queue = {};
	mpmc_work_queue_init(&io_queue, &main_arena, 512, MEGABYTES(1));

	for (int i = 0; i < thread_count; ++i) {
		new (io_threads + i) std::thread(mpmc_work_queue_thread_start_routine, thread_context_array + i, i,
						 &io_queue);
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

	mpmc_begin_producer(&io_queue);

	int max_chunks = 4;
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
	mpmc_work_queue_stop(&io_queue);

	for (int i = 0; i < thread_count; ++i) {
		io_threads[i].join();
	}

	return 0;
}
