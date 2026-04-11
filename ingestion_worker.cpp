#include <ctype.h>
#include <charconv>
#include <system_error>
#include <algorithm>

#include "utils.h"
#include "chrono.h"
#include "workqueue.h"
#include "metadata.h"
#include "ingestion_worker.h"
#include "writer.h"

#if defined (__APPLE__)
#include <stdlib.h>
#include <xlocale.h>

inline bool parse_double(char* str, char* end_ptr, double* out_value) {
    // Cache the C locale globally to avoid reallocation overhead
    static locale_t c_locale = newlocale(LC_ALL_MASK, "C", NULL);
    
    *out_value = strtod_l(str, &end_ptr, c_locale);
    
    // Returns true if parsing advanced the pointer
    return (end_ptr != str);
}

#else
void thread_local_schema_maps_init(ThreadLocalSchemaMaps *schema_maps)
{
  std::memset(schema_maps, 0, sizeof(ThreadLocalSchemaMaps));
  auto arena = &schema_maps->arena;

  arena_init(arena, TABLE_PAGE_MAP_SIZE + TABLE_PAGES_SIZE +
             LOCAL_TABLE_PAGE_MAP_SIZE + TABLE_REQUEST_INFO_SIZE +
             ACTIVE_GLOBAL_TABLE_PAGES_SIZE + MEGABYTES(1));

	pool_init(&schema_maps->data_page_pool, arena, DATA_PAGE_LIMIT, DATA_PAGE_SIZE, KILOBYTES(4));
	pool_init(&schema_maps->per_table_request_info_pool, arena, TABLE_PAGE_LIMIT);

	schema_maps->active_global_table_pages = arena_alloc_struct_array(arena, TableID, TABLE_PAGE_LIMIT);
	schema_maps->table_page_map_array = arena_alloc_struct_array(arena, RequestInfoAndPage, MAX_TABLES);
	schema_maps->local_table_page_map.buckets =
		arena_alloc_struct_array(arena, LocalTablePageMapBucket, DEFAULT_TABLE_CAPACITY);
	schema_maps->local_table_page_map.strings = arena_alloc_struct_array(arena, SchemaString, DEFAULT_TABLE_CAPACITY);
  schema_maps->local_table_page_map.info_and_page_arr = arena_alloc_struct_array(arena, RequestInfoAndPage, DEFAULT_TABLE_CAPACITY);
  schema_maps->local_table_page_map.capacity = DEFAULT_TABLE_CAPACITY;

}

void ingestion_worker_submit_buffered_work_to_writer_and_clear_local_maps(DatabaseContext *db_context,
																																					IngestionWorkerContext *t_ctx,
																																					bool clear_buckets = true)
{
	auto active_global_table_id_arr = t_ctx->schema_maps.active_global_table_pages;
	auto global_pages = t_ctx->schema_maps.table_page_map_array;
	for (auto i = 0; i < t_ctx->schema_maps.active_global_table_pages_count; ++i) {
		auto id = active_global_table_id_arr[i];
		MPSCWriterQueueEntry entry = {};
		entry.table_id = id;
		entry.request_info_and_page = global_pages[id.index];
		entry.thread_id = t_ctx->thread_id;
    entry.start_timestamp = ((DataPageHeader *)global_pages[id.index].page_head)->start_timestamp;
    entry.end_timestamp = ((DataPageHeader *)global_pages[id.index].current_page)->end_timestamp;
    auto current_header = (DataPageHeader *)entry.request_info_and_page.current_page;
    assert(current_header->next_page == 0);
		writer_queues_enqueue_entry(&db_context->writer_queues, &entry);
	}

	std::memset(t_ctx->schema_maps.table_page_map_array, 0, TABLE_PAGE_MAP_SIZE);
	t_ctx->schema_maps.active_global_table_pages_count = 0;

	auto local_pages = t_ctx->schema_maps.local_table_page_map;
	for (uint32_t i = 0; i < t_ctx->schema_maps.table_id_count; ++i) {
		MPSCWriterQueueEntry entry = {};
		auto actual_idx = i + 1;
		entry.table_id = { .index = actual_idx, .local_flag = 1 };
		entry.request_info_and_page = local_pages.info_and_page_arr[actual_idx];
		entry.thread_id = t_ctx->thread_id;
    entry.start_timestamp = ((DataPageHeader *)local_pages.info_and_page_arr[actual_idx].page_head)->start_timestamp;
    entry.end_timestamp = ((DataPageHeader *)local_pages.info_and_page_arr[actual_idx].page_head)->end_timestamp;

    auto current_header = (DataPageHeader *)entry.request_info_and_page.current_page;
    assert(current_header->next_page == 0);
		writer_queues_enqueue_entry(&db_context->writer_queues, &entry);
	}

	t_ctx->prev_timestamp = platform_get_high_res_timer_stamp();

	if (clear_buckets) {
		std::memset(local_pages.buckets, 0, sizeof(LocalTablePageMapBucket) * local_pages.capacity);
    t_ctx->schema_maps.table_id_count = 0;
	}

	std::memset(local_pages.info_and_page_arr, 0, sizeof(RequestInfoAndPage) * local_pages.capacity);
}

uint8_t *schema_maps_get_new_page_and_metadata(DatabaseContext *db_context, IngestionWorkerContext *t_ctx, ThreadLocalSchemaMaps *schema_maps) 
{
  auto data = (uint8_t *)pool_atomic_alloc(&schema_maps->data_page_pool, false); // its too big to warrant a full memset 0

  if (!data) {
    return data;
  }

	auto data_page_header = (DataPageHeader *)data;
  data_page_header->is_out_of_order = 0;
  data_page_header->start_timestamp = 0;
  data_page_header->end_timestamp = 0;
  data_page_header->bytes_written = sizeof(DataPageHeader);
  data_page_header->next_page = 0;
  return data;
}

uint8_t *schema_maps_get_new_page_with_flush_and_spin(DatabaseContext *db_context, IngestionWorkerContext *t_ctx, ThreadLocalSchemaMaps *schema_maps) {
	auto data_page = schema_maps_get_new_page_and_metadata(db_context, t_ctx, schema_maps);
	if (!data_page) {
		ingestion_worker_submit_buffered_work_to_writer_and_clear_local_maps(db_context, t_ctx, false);
		while (!data_page) {
			data_page = schema_maps_get_new_page_and_metadata(db_context, t_ctx, schema_maps);
			cpu_pause();
		}
	}

  return data_page;
}

PerTableRequestInfo *schema_maps_get_new_request_info(ThreadLocalSchemaMaps *schema_maps) 
{
  auto request_info = (PerTableRequestInfo *)pool_atomic_alloc(&schema_maps->per_table_request_info_pool);
  request_info->strings_arena.memory = request_info->arena_backing;
  request_info->strings_arena.capacity = sizeof(request_info->arena_backing);
  return request_info;
}

RequestInfoAndPage *local_table_page_map_insert_new_page_and_info(DatabaseContext *db_context, IngestionWorkerContext *t_ctx, ThreadLocalSchemaMaps *schema_maps, StringSlice8 table_name) 
{
  auto page_and_metadata = schema_maps_get_new_page_with_flush_and_spin(db_context, t_ctx, schema_maps); 
	auto new_request_info = schema_maps_get_new_request_info(schema_maps);

	auto &page_map = schema_maps->local_table_page_map;
	auto hash = std::hash<StringSlice8>{}(table_name);
	auto index = hash % page_map.capacity;
	auto &bucket = page_map.buckets[index];

	while (bucket.hash != 0) {
		if (bucket.hash == hash) {
			auto &string = page_map.strings[index];
			if (StringSlice8{string.buffer, string.length} == table_name) {
				break;
			}
		}

		index = (index + 1) % page_map.capacity;
		bucket = page_map.buckets[index];
	}
	auto id = TableID{ .index = ++schema_maps->table_id_count, .local_flag = 1 };
  bucket.hash = hash;
	bucket.table_id = id;

	auto &string = page_map.strings[index];
	string.length = table_name.length;
	std::memcpy(string.buffer, table_name.content, table_name.length);
  auto &request_info_and_page = page_map.info_and_page_arr[id.index];

	request_info_and_page.request_info = new_request_info;
  request_info_and_page.current_page = page_and_metadata;
  request_info_and_page.page_head = page_and_metadata;

  return &request_info_and_page;
}

RequestInfoAndPage *local_table_page_map_lookup(ThreadLocalSchemaMaps *schema_maps, StringSlice8 table_name)
{
	auto &page_map = schema_maps->local_table_page_map;
	auto hash = std::hash<StringSlice8>{}(table_name);
	auto index = hash % page_map.capacity;
	auto &bucket = page_map.buckets[index];

	auto search_count = 0;
	while (bucket.hash != 0) {
		if (bucket.hash == hash) {
			auto string = page_map.strings[index];
			if (StringSlice8{string.buffer, string.length} == table_name) {
        auto id = bucket.table_id;
				return &page_map.info_and_page_arr[id.index];
			}
		}

		if (search_count == page_map.capacity) {
			return {};
		}

		index = (index + 1) % page_map.capacity;
		bucket = page_map.buckets[index];
	}

	return {};
}

RequestInfoAndPage *table_page_map_lookup(ThreadLocalSchemaMaps *schema_maps, TableID id)
{
	return &schema_maps->table_page_map_array[id.index];
}

RequestInfoAndPage *table_page_map_insert_new_page_and_info(DatabaseContext *db_context, IngestionWorkerContext *t_ctx,
																														ThreadLocalSchemaMaps *schema_maps, TableID id)
{
	auto &slot = schema_maps->table_page_map_array[id.index];

	auto new_page = schema_maps_get_new_page_with_flush_and_spin(db_context, t_ctx, schema_maps);
	slot.request_info = schema_maps_get_new_request_info(schema_maps);
	slot.current_page = new_page;
	slot.page_head = new_page;

	schema_maps->active_global_table_pages[schema_maps->active_global_table_pages_count++] = id;

	return &slot;
}

void ingestion_worker_do_work_and_submit_to_writer_periodically(DatabaseContext *db_context, IngestionWorkerContext *t_ctx, MPMCWorkQueue *wq) 
{
	mpmc_work_queue_dequeue_entry(t_ctx, wq);

	if (compute_time_in_ms(t_ctx->prev_timestamp, platform_get_high_res_timer_stamp()) > 10) {
		ingestion_worker_submit_buffered_work_to_writer_and_clear_local_maps(db_context, t_ctx);
	}
}

void ingestion_worker_start_routine(DatabaseContext *db_context, IngestionWorkerContext *t_ctx, uint16_t thread_id,
																		MPMCWorkQueue *wq)
{
	t_ctx->thread_id = thread_id;

  t_ctx->prev_timestamp = platform_get_high_res_timer_stamp();
	while (!wq->stop_flag.load(std::memory_order::acquire)) {
    ingestion_worker_do_work_and_submit_to_writer_periodically(db_context, t_ctx, wq);
	}


  while (wq->head.load(std::memory_order::acquire) != wq->tail.load(std::memory_order::acquire)) {
    ingestion_worker_do_work_and_submit_to_writer_periodically(db_context, t_ctx, wq);
  }

  ingestion_worker_submit_buffered_work_to_writer_and_clear_local_maps(db_context, t_ctx);
  wq->stop_count.fetch_add(1, std::memory_order::release);
  wq->stop_count.notify_all();
}

inline bool parse_double(const char *str, char *end_ptr, double *out_value)
{
	auto parsing_res = std::from_chars((char *)str, (char *)end_ptr, *out_value);
	return parsing_res.ec == std::errc::invalid_argument;
}
#endif

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


ParseValueResult parse_string(StringSlice8 string)
{
	ParseValueResult result = {};
	result.data.type = ColumnDataType::VARCHAR;
	result.data.varchar.content = string.content + 1;
	bool double_quote = string[0] == '"';
	for (auto i = 1; i < string.length; ++i) {
		if (string[i] == '\n') {
			result.err_msg = string8_from_cstring("Unterminated string literal");
			return result;
		}

		if (double_quote && string[i] == '"') {
			result.data.varchar.length = i - 1;
			return result;
		} else if (!double_quote && string[i] == '\'') {
			result.data.varchar.length = i - 1;
			return result;
		}
	}

	return result;
}

ParseValueResult parse_value(StringSlice8 value)
{
	ParseValueResult result = {};
	bool is_string = value[0] == '"' || value[0] == '\'';

	if (!is_string) {
		result.data.type = ColumnDataType::INT64;
		auto parsing_res = std::from_chars((char *)value.content, (char *)value.content + value.length, result.data.int64);

		if (parsing_res.ec == std::errc::invalid_argument) {
      if (parse_double((char *)value.content, (char *)value.content + value.length, &result.data.dbl)) {
        result.data.type = ColumnDataType::INVALID;
			} else {
				result.err_msg = string8_from_cstring("Error parsing column value as int or float");
      }


		}
	} else {
		result = parse_string(value);
	}

	return result;
}

ParseColumnResult parse_column(StringSlice8 col)
{

	ParseColumnResult result = {};
  result.name.content = col.content;
  int index_of_equal = 0; 
  for (int i = 0; i < col.length; i++) {
    if (col.content[i] == '=') {
      index_of_equal = i;
      break;
    }
	}

	auto parsed_val = parse_value(StringSlice8{col.content + index_of_equal + 1, col.length - index_of_equal});
	StringSlice8 value = string_slice_length(col, index_of_equal + 1);

	if (parsed_val.err_msg.length != 0) {
		result.err_msg = parsed_val.err_msg;
		return result;
	}

	result.data = parsed_val.data;
	result.name = string_slice_length(col, 0, index_of_equal);

	return result;
}

TagsList parse_tags(Arena *a, StringSlice8 tags)
{
	TagsList result = {};
	result.tags = arena_alloc_struct_array(a, StringSlice8, 256);
	result.n_tags = 0;
	//
	//TODO(Ray): Parsing needs error handling at some point
  //
	uint32_t start_index = 0;

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
			result.tags[result.n_tags++] = StringSlice8{ tags.content + start_index, i - start_index };
			start_index = i + 1;
		}
	}

	// save the last tag if there was one
	if (tags.length) {
		result.tags[result.n_tags++] = StringSlice8{ tags.content + start_index, tags.length - start_index };
	}

	std::sort(result.tags, result.tags + result.n_tags, string_sort_cmp);

	// join the array without the comma
	//
	int64_t final_tags_size = 0;
	for (auto i = 0; i < result.n_tags; ++i) {
		final_tags_size += result.tags[i].length;
	}

	StringBuilder8 final_tags_string = {};
	string_builder8_init(a, &final_tags_string, final_tags_size + 1);

	for (int i = 0; i < result.n_tags; ++i) {
		string_builder8_append(&final_tags_string, result.tags[i]);
	}

	return result;
}


//TODO make sure multiple columns are not inserted in a single row
void parse_columns(Arena *a, ParseColumnResultList *results_array, StringSlice8 columns)
{
	auto columns_arr_length = 0;

	uint32_t start_index = 0;
  for (int i = 0; i < columns.length; ++i)  { 
    if (columns[i] != ' ') {
      start_index = i;
      break;
    }
  }

	for (int i = 0; i < columns.length; ++i) {
		if (columns[i] == '\n') {
			results_array->results[results_array->n_results++] = parse_column(StringSlice8{ columns.content + start_index, i - start_index });
			break;
		} else if (columns[i] == ',') {
			results_array->results[results_array->n_results++] = parse_column(StringSlice8{ columns.content + start_index, i - start_index });
			start_index = i + 1;
      for (;start_index < columns.length; ++start_index)  { 
        if (columns[start_index] != ' ') {
          break;
        }
      }
		}
	}
}

void request_info_assign_offset(PerTableRequestInfo *request_info, ColumnID id, ColumnData data)
{
  auto &offset_slot = request_info->column_offsets[get_offset_idx_from_id(id)];
	if (!offset_slot) {
		offset_slot = request_info->running_offset;
		request_info->running_offset += std::max(8u, get_size_from_col_data(data));
	}
}

void output_column_error_message(const char *prepend_string, StringSlice8 table_name, ColumnDataType actual_type, ColumnDataType expected_type) {
  auto expected_type_str = COLUMN_TYPE_STRINGS[static_cast<int32_t>(expected_type)];
  auto actual_type_str = COLUMN_TYPE_STRINGS[static_cast<int32_t>(actual_type)];
	fprintf(stderr, "%s\nMismatch column type in table: %.*s, expected type: %s, parsed type: %s\n", prepend_string, table_name.length,
					table_name.content, expected_type_str, actual_type_str);
}

ColumnID request_info_insert_or_match(PerTableRequestInfo *request_info, ParseColumnResult *result) {
	auto hash = std::hash<StringSlice8>{}(result->name);
	auto index = hash % DATA_PAGE_HEADER_SIZE;

	while (request_info->new_column_hashes[index] != 0) {
		auto &bucket_hash = request_info->new_column_hashes[index];
		if (hash == bucket_hash) {
			auto name = (VariableSchemaString *)request_info->string_ptrs[index];
			if (StringSlice8{ name->buffer, name->length } == result->name) {
				auto &id_index = request_info->new_column_id_idxs[index];
				if (request_info->new_column_types[id_index] != result->data.type) {
					return {};
				} else {
          return {.index = id_index, .local_flag = 1};
        }
			}
		}
		index = (index + 1) % DATA_PAGE_HEADER_SIZE;
	}

	request_info->new_column_hashes[index] = hash;

  auto id = ColumnID{.index = ++request_info->new_column_id_count, .local_flag = 1};
  request_info->new_column_id_idxs[index] = id.index;
	request_info->new_column_types[id.index] = result->data.type;

  auto string_ptr = &request_info->string_ptrs[index];
	*string_ptr = arena_alloc_struct_array(&request_info->strings_arena, VariableSchemaString, result->name.length + 1);
	assert(result->name.length <= 63);
	(*string_ptr)->length = result->name.length;
	std::memcpy((*string_ptr)->buffer, result->name.content, result->name.length);

  request_info_assign_offset(request_info, id, result->data);
  return id;
}

ColumnID request_info_insert(PerTableRequestInfo *request_info, ParseColumnResult *result) {
  auto id = ColumnID{ .index = ++request_info->new_column_id_count, .local_flag = 1 };
	auto hash = std::hash<StringSlice8>{}(result->name);
	auto index = hash % DATA_PAGE_HEADER_SIZE;

	while (request_info->new_column_hashes[index] != 0) {
		index = (index + 1) % DATA_PAGE_HEADER_SIZE;
	}

	request_info->new_column_hashes[index] = hash;
  request_info->new_column_id_idxs[index] = id.index;

	assert(result->name.length <= 63);
  auto string_ptr = &request_info->string_ptrs[index];
	*string_ptr = (VariableSchemaString *)arena_alloc(&request_info->strings_arena, result->name.length + 1);
	assert(result->name.length <= 63);
	(*string_ptr)->length = result->name.length;
	std::memcpy((*string_ptr)->buffer, result->name.content, result->name.length);

	request_info->new_column_types[id.index] = result->data.type;

  request_info_assign_offset(request_info, id, result->data);
  return id;
}

StringSlice8 tokeniser_get_slice_at(Tokeniser *t, String8 data, char target) 
{
  StringSlice8 result = {};
  result.content = (uint8_t *)t->at;
	for (;t->at - (char *)data.content < data.length; ++t->at) {
		if (*t->at == target) {
			break;
		}
	}

  result.length = ((uint8_t *)t->at - result.content) + 1;
  if (*t->at != target) {
    return {};
  }
  
  return result;
}

StringSlice8 tokeniser_get_slice_to(Tokeniser *t, String8 data, char target)
{
	StringSlice8 result = {};
	result.content = (uint8_t *)t->at;
	for (; t->at - (char *)data.content < data.length; ++t->at) {
		if (*t->at == target) {
			break;
		}
	}

	result.length = (uint8_t *)t->at - result.content;
  
  return result;
}


void insert_col_info_to_row_cache_at_index(PrevRowColumnCache *cache, TableID table_id, ColumnID column_id, ParseColumnResult parsed_column,
																					 int index)
{
  auto &cache_entry = cache->entries[index];
  cache_entry.table_id = table_id;
	cache_entry.column_id = column_id;
	cache_entry.prev_string = parsed_column.name;
}

void write_parsed_data(DatabaseContext *db_context, IngestionWorkerContext *t_ctx, ThreadLocalSchemaMaps *schema_maps,
											 uint64_t timestamp, RequestInfoAndPage *request_info_and_page, PrevRowColumnCache cache,
											 ParseColumnResultList parsed_columns)
{
	// calculate row size

  uint64_t maximum_offset = 0; // theoretically can have a uint16_t but need the 64 for alignment
  uint64_t max_offset_data_size = 0;
  auto request_info = request_info_and_page->request_info;
  for (int i = 0; i < parsed_columns.n_results; ++i) {
    auto &parsed_column = parsed_columns.results[i];
    auto &cache_entry = cache.entries[i];
    auto data_size = std::max(8u, get_size_from_col_data(parsed_column.data));
    auto offset = request_info->column_offsets[get_offset_idx_from_id(cache_entry.column_id)] + sizeof(uint64_t);
		if (maximum_offset < offset) {
			maximum_offset = offset;
      max_offset_data_size = data_size;
		}
  }
  
  maximum_offset = maximum_offset + max_offset_data_size;
  max_offset_data_size = 8;

  auto bytes_to_write = maximum_offset + max_offset_data_size;
  auto current_page = request_info_and_page->current_page;
  auto current_header = (DataPageHeader *)current_page;
	if (current_header->bytes_written + bytes_to_write >= DATA_PAGE_SIZE) {
		auto data_page = schema_maps_get_new_page_and_metadata(db_context, t_ctx, schema_maps);
    if (!data_page) {
      auto new_request_info = schema_maps_get_new_request_info(schema_maps);
      new_request_info->table_schema_version = request_info->table_schema_version;
      new_request_info->table_name.length = request_info->table_name.length;

      std::memcpy(new_request_info->table_name.buffer, request_info->table_name.buffer, sizeof(request_info->table_name));
      std::memcpy(new_request_info->arena_backing, request_info->arena_backing, sizeof(request_info->arena_backing));
      std::memcpy(new_request_info->new_column_hashes, request_info->new_column_hashes, sizeof(request_info->new_column_hashes));
      std::memcpy(new_request_info->string_ptrs, request_info->string_ptrs, sizeof(request_info->string_ptrs));
      std::memcpy(new_request_info->new_column_id_idxs, request_info->new_column_id_idxs, sizeof(uint32_t) * request_info->new_column_id_count);
      std::memcpy(new_request_info->new_column_types, request_info->new_column_types, sizeof(uint32_t) * request_info->new_column_id_count);
      std::memcpy(new_request_info->global_column_ids, request_info->global_column_ids, sizeof(ColumnID) * request_info->new_column_id_count);
      std::memcpy(new_request_info->column_offsets, request_info->column_offsets, sizeof(request_info->column_offsets));
      new_request_info->running_offset = request_info->running_offset;
      new_request_info->row_count = request_info->row_count;

      ingestion_worker_submit_buffered_work_to_writer_and_clear_local_maps(db_context, t_ctx, false);

      while (!data_page) {
        data_page = schema_maps_get_new_page_and_metadata(db_context, t_ctx, schema_maps);
        cpu_pause();
      }

      request_info_and_page->request_info = new_request_info;
      request_info_and_page->page_head = data_page;
      request_info_and_page->current_page = data_page;
      request_info = request_info_and_page->request_info;
    } else { 
      current_header->next_page = reinterpret_cast<uint64_t>(data_page);
      request_info_and_page->current_page = data_page;
    }
	}

  current_page = request_info_and_page->current_page;
  current_header = (DataPageHeader *)current_page;

  if (!current_header->start_timestamp) {
    current_header->start_timestamp = timestamp;
  }

  if (current_header->end_timestamp > timestamp) {
    current_header->is_out_of_order = true;
  } else {
    current_header->end_timestamp = timestamp;  // always set the end timstamp to the latest even for out of order data
  }

  auto write_ptr = current_page + current_header->bytes_written;
  assert(current_header->bytes_written >= sizeof(DataPageHeader));
  assert((uintptr_t)write_ptr % 8 == 0);
  auto row_size_ptr = (uint64_t *)write_ptr;
  *row_size_ptr = maximum_offset + max_offset_data_size;

	for (int i = 0; i < parsed_columns.n_results; ++i) {
    auto &parsed_column = parsed_columns.results[i];
    auto &cache_entry = cache.entries[i];

		auto data_size = std::max(8u, get_size_from_col_data(parsed_column.data));
		auto offset = request_info->column_offsets[get_offset_idx_from_id(cache_entry.column_id)] + sizeof(uint64_t); // add the size of the row length
		auto col_ptr = write_ptr + offset;
    assert((uintptr_t)col_ptr % 8 == 0);

    switch (parsed_column.data.type)
		{
		case ColumnDataType::TIMESTAMP:
			std::memcpy(col_ptr, &parsed_column.data.int64, data_size);
			break;
		case ColumnDataType::INT32:
			std::memcpy(col_ptr, &parsed_column.data.int32, data_size);
			break;
		case ColumnDataType::FLOAT:
			std::memcpy(col_ptr, &parsed_column.data.flt, data_size);
			break;
		case ColumnDataType::DOUBLE:
			std::memcpy(col_ptr, &parsed_column.data.dbl, data_size);
			break;
		case ColumnDataType::INT64:
			std::memcpy(col_ptr, &parsed_column.data.int64, data_size);
			break;
		case ColumnDataType::VARCHAR: {
			*(int64_t *)col_ptr = parsed_column.data.varchar.length; // this address will be 8 byte aligned so cast is legal
			std::memcpy(col_ptr + 8, parsed_column.data.varchar.content, parsed_column.data.varchar.length);
      std::memset(col_ptr + 8 + parsed_column.data.varchar.length, 0, data_size - parsed_column.data.varchar.length - 8);

		} break;
		default:
			break;
		}
	}
  
  auto timestamp_ptr = (uint64_t *)(write_ptr + maximum_offset);
  *timestamp_ptr = timestamp;

  current_header->bytes_written += bytes_to_write;
  request_info->row_count++;
  assert(current_header->bytes_written <= DATA_PAGE_SIZE);
}

void *process_write_request(IngestionWorkerContext *t_ctx, DatabaseContext *context, String8 data_to_write)
{

	auto time_start = platform_get_high_res_timer_stamp();
	auto schema_maps_result = schema_maps_get_latest_version_inc_refcount(&context->schema_maps_tripple_buffer);
	auto global_schema_maps = schema_maps_result.maps;
	auto local_schema_maps = &t_ctx->schema_maps;

	int64_t column_data_to_write = 0;
	int64_t total_column_index = 0;

	ParseColumnResultList parsed_columns_array = {};
	Tokeniser tokeniser = { .at = (char *)data_to_write.content };
	parsed_columns_array.results = arena_alloc_struct_array(&t_ctx->transient_arena, ParseColumnResult, 256);
	auto row_count = 0;

	PrevRowColumnCache prev_row_col_cache = { .entries = arena_alloc_struct_array(
																							&t_ctx->transient_arena, PrevRowColumnCacheEntry, DATA_PAGE_HEADER_SIZE),
																						.n_entries = DATA_PAGE_HEADER_SIZE };

  if (data_to_write.length >= DATA_PAGE_SIZE * TABLE_PAGE_LIMIT) {
    fprintf(stderr, "Rejected: Batch size too large\n");
    goto cleanup;
  }

	for (; tokeniser.at - (char *)data_to_write.content < data_to_write.length; ++tokeniser.at) {
		row_count++;
		if (*tokeniser.at == '\n') {
			continue;
		}

    bool is_full_line_to_parse = false;
    int i = (ptrdiff_t)(tokeniser.at - (char *)data_to_write.content);
    for (int i = (ptrdiff_t)(tokeniser.at - (char *)data_to_write.content); i < data_to_write.length; ++i) {
      if (data_to_write.content[i] == '\n') {
        is_full_line_to_parse = true;
        break;
      }
    }

    if (!is_full_line_to_parse) {
      tokeniser.at += i;
      continue;
    }

		//TODO(Ray) write an actual parser using SIMD - this is painful
		StringSlice8 table_name = tokeniser_get_slice_to(&tokeniser, data_to_write, ' ');
    tokeniser.at++; // skip the whitespace
    StringSlice8 timestamp_str = tokeniser_get_slice_to(&tokeniser, data_to_write, ' ');

		uint64_t timestamp = 0;
    
    auto result = std::from_chars((char *)timestamp_str.content, (char *)timestamp_str.content + timestamp_str.length, timestamp);

    if (result.ec == std::errc::invalid_argument) {
      fprintf(stderr, "Failure to parse timestamp\n");
      goto cleanup;
    }

		StringSlice8 tags = tokeniser_get_slice_to(&tokeniser, data_to_write, ']');
		tokeniser.at++;
		StringSlice8 columns = tokeniser_get_slice_at(&tokeniser, data_to_write, '\n');

		parse_columns(&t_ctx->transient_arena, &parsed_columns_array, columns);

		auto table_id = schema_maps_lookup_table_id(global_schema_maps, table_name);

		if (!table_id.id) {
			auto page_map_result = local_table_page_map_lookup(local_schema_maps, table_name);

			if (!page_map_result) {
				page_map_result = local_table_page_map_insert_new_page_and_info(context, t_ctx, local_schema_maps, table_name);
        auto request_info = page_map_result->request_info;

        request_info->table_name.length = table_name.length;
        std::memcpy(request_info->table_name.buffer, table_name.content, table_name.length);

				for (int i = 0; i < parsed_columns_array.n_results; ++i) {
					auto &parsed_column = parsed_columns_array.results[i];
					auto column_id = request_info_insert(request_info, &parsed_column);
					request_info->new_column_types[column_id.index] = parsed_column.data.type;
					insert_col_info_to_row_cache_at_index(&prev_row_col_cache, table_id, column_id, parsed_column, i);
				}

				write_parsed_data(context, t_ctx, local_schema_maps, timestamp, page_map_result, prev_row_col_cache, parsed_columns_array);

			} else {
        if (!page_map_result->page_head) {
          page_map_result->page_head = schema_maps_get_new_page_with_flush_and_spin(context, t_ctx, local_schema_maps);
          page_map_result->request_info = schema_maps_get_new_request_info(local_schema_maps);
        }

				for (int i = 0; i < parsed_columns_array.n_results; ++i) {
					auto &parsed_column = parsed_columns_array.results[i];
					auto &cache_entry = prev_row_col_cache.entries[i];
          auto &cached_type = page_map_result->request_info->new_column_types[cache_entry.column_id.index];
					if (cache_entry.table_id == table_id && cache_entry.prev_string != parsed_column.name) {
						auto column_id = request_info_insert_or_match(page_map_result->request_info, &parsed_columns_array.results[i]);
						insert_col_info_to_row_cache_at_index(&prev_row_col_cache, table_id, column_id, parsed_column, i);
					} else if (cached_type != parsed_column.data.type) {
						output_column_error_message("Error in the local cache", table_name, parsed_column.data.type, cached_type);
            goto cleanup;
					}

				}
        write_parsed_data(context, t_ctx, local_schema_maps, timestamp, page_map_result, prev_row_col_cache, parsed_columns_array);
			}
		} else {
			// table exists can just use the schema_maps for lookups
			auto page_and_info = table_page_map_lookup(local_schema_maps, table_id);
      auto request_info = page_and_info->request_info;

			if (!page_and_info->request_info) {
				page_and_info = table_page_map_insert_new_page_and_info(context, t_ctx, local_schema_maps, table_id);
        request_info = page_and_info->request_info;
				request_info->table_schema_version = schema_maps_lookup_table_schema_by_id(global_schema_maps, table_id)->version;

				request_info->table_name.length = table_name.length;
				std::memcpy(request_info->table_name.buffer, table_name.content, table_name.length);

				for (int i = 0; i < parsed_columns_array.n_results; ++i) {
					auto &parsed_column = parsed_columns_array.results[i];
					auto column_id = schema_maps_lookup_column_id(global_schema_maps, table_id, parsed_column.name);
          auto expected_type = schema_maps_get_column_data_type(global_schema_maps, table_id, column_id);
					if (!column_id.id) {
						column_id = request_info_insert(request_info, &parsed_column);
					} else {
						if ( expected_type != parsed_column.data.type) {
							output_column_error_message("Error in global table info", table_name, parsed_column.data.type, expected_type);
              goto cleanup;
						};
            request_info->global_column_ids[column_id.index] = column_id;
					}

          request_info_assign_offset(request_info, column_id, parsed_column.data);
          insert_col_info_to_row_cache_at_index(&prev_row_col_cache, table_id, column_id, parsed_column, i);
				}

				write_parsed_data(context, t_ctx, local_schema_maps, timestamp, page_and_info, prev_row_col_cache, parsed_columns_array);

			} else {
				request_info->table_schema_version = schema_maps_lookup_table_schema_by_id(global_schema_maps, table_id)->version;
        assert(request_info->table_name.length > 0);

				for (int i = 0; i < parsed_columns_array.n_results; ++i) {
					auto &parsed_column = parsed_columns_array.results[i];
					auto &cache_entry = prev_row_col_cache.entries[i];
          auto new_col = false;
					if (cache_entry.table_id == table_id && cache_entry.prev_string != parsed_column.name) {
            auto column_id = schema_maps_lookup_column_id(global_schema_maps, table_id, parsed_column.name);
            auto expected_type = schema_maps_get_column_data_type(global_schema_maps, table_id, column_id);
            if (!column_id.id) 
            {
							column_id = request_info_insert_or_match(request_info, &parsed_columns_array.results[i]);

              if (!column_id.id) {
                goto cleanup;
              }

						} else {
              if (expected_type != parsed_column.data.type) {
                output_column_error_message("Erorr in request info", table_name, parsed_column.data.type, expected_type);
                goto cleanup;
              }
            }

            insert_col_info_to_row_cache_at_index(&prev_row_col_cache, table_id, column_id, parsed_column, i);
            // if column isn't yet in request info 
            if (request_info->global_column_ids[column_id.index].id == 0) {
              request_info->global_column_ids[column_id.index] = column_id;
              request_info_assign_offset(request_info, column_id, parsed_column.data);
            }

					} else {
            ColumnDataType data_type{};
            bool is_cached = false;
            if (cache_entry.column_id.local_flag) {
              data_type = request_info->new_column_types[cache_entry.column_id.index];
              if (data_type != parsed_column.data.type) {
                output_column_error_message("Error in cache table", table_name, parsed_column.data.type, data_type);
                goto cleanup;
              }
            } else {
              data_type = schema_maps_get_column_data_type(global_schema_maps, table_id, cache_entry.column_id);
              if (data_type != parsed_column.data.type) {
                output_column_error_message("Error in global table", table_name, parsed_column.data.type, data_type);
                goto cleanup;
              }

            }
          }
				}
				write_parsed_data(context, t_ctx, local_schema_maps, timestamp, page_and_info, prev_row_col_cache, parsed_columns_array);
			}
		}

		parsed_columns_array.n_results = 0;
	}

cleanup:

	schema_maps_dec_refcount(&context->schema_maps_tripple_buffer, schema_maps_result);
	arena_clear(&t_ctx->transient_arena);
  
	auto time_end = platform_get_high_res_timer_stamp();
  t_ctx->timer_diffs += time_end - time_start;
  t_ctx->run_count += 1;
	return 0;
}
