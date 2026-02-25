#include <ctype.h>
#include <charconv>
#include <system_error>
#include <algorithm>

#include "utils.h"
#include "metadata.h"

#include "ingestion_worker.h"

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

ParseValueResult parse_value(StringSlice8 value)
{
	ParseValueResult result = {};
	bool is_string = value[0] == '"' || value[0] == '\'';

	if (!is_string) {
		result.type = ColumnDataType::INT64;
		auto parsing_res = std::from_chars((char *)value.content, (char *)value.content + value.length, result.data.int64);

		if (parsing_res.ec == std::errc::invalid_argument) {
			auto parsing_res = std::from_chars((char *)value.content, (char *)value.content + value.length, result.data.dbl);

			result.type = ColumnDataType::DOUBLE;
			if (parsing_res.ec == std::errc::invalid_argument) {
				result.type = ColumnDataType::INVALID;
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
	result.type = parsed_val.type;
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
	uint64_t final_tags_size = 0;
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

uint32_t get_offset_idx_from_id(ColumnID id)
{
	return id.local_flag ? id.index + DATA_PAGE_HEADER_SIZE : id.index;
}

void request_info_assign_offset(PerTableRequestInfo *request_info, ColumnID id, ColumnDataType type)
{
  auto &offset_slot = request_info->column_offsets[get_offset_idx_from_id(id)];
	if (!offset_slot) {
		offset_slot = request_info->running_offset;
		request_info->running_offset += std::max(8u, get_data_size_from_col_type(type));
	}
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
				if (request_info->new_column_types[id_index] != result->type) {
					fprintf(stderr, "Mismatch Column Type\n");
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
	request_info->new_column_types[id.index] = result->type;

  auto string_ptr = &request_info->string_ptrs[index];
	*string_ptr = arena_alloc_struct_array(&request_info->strings_arena, VariableSchemaString, result->name.length + 1);
	assert(result->name.length <= 63);
	(*string_ptr)->length = result->name.length;
	std::memcpy((*string_ptr)->buffer, result->name.content, result->name.length);

  request_info_assign_offset(request_info, id, result->type);
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

	request_info->new_column_types[id.index] = result->type;

  request_info_assign_offset(request_info, id, result->type);
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

void insert_col_info_to_row_cache_at_index(PrevRowColumnCache *cache, ColumnID id, ParseColumnResult parsed_column,
																					 int index)
{
  auto &cache_entry = cache->entries[index];
	cache_entry.id = id;
	cache_entry.prev_string = (char *)parsed_column.name.content;
	cache_entry.prev_string_length = parsed_column.name.length;
}

void write_parsed_data(ThreadLocalSchemaMaps *schema_maps, uint64_t timestamp, RequestInfoAndPage *request_info_and_page, PrevRowColumnCache cache,
											 ParseColumnResultList parsed_columns)
{
  // calculate row size
  auto request_info = request_info_and_page->request_info;
  auto data_page = request_info_and_page->current_page;
  if (!data_page->start_timestamp) {
    data_page->start_timestamp = timestamp;
  }

  if (data_page->end_timestamp > timestamp) {
    data_page->is_out_of_order = true;
  } else {
    data_page->end_timestamp = timestamp;  // always set the end timstamp to the latest even for out of order data
  }

  uint64_t maximum_offset = 0; // theoretically can have a uint16_t but need the 64 for alignment
  uint64_t max_offset_data_size = 0;
  for (int i = 0; i < parsed_columns.n_results; ++i) {
    auto &parsed_column = parsed_columns.results[i];
    auto &cache_entry = cache.entries[i];
    auto data_size = std::max(8u, get_data_size_from_col_type(parsed_column.type));
    auto offset = request_info->column_offsets[get_offset_idx_from_id(cache_entry.id)] + sizeof(uint64_t);

		if (maximum_offset < offset) {
			maximum_offset = offset;
      max_offset_data_size = data_size;
		}
  }



  auto current_page_ptr = &request_info_and_page->current_page;
	if ((*current_page_ptr)->bytes_written + maximum_offset + max_offset_data_size >= DATA_PAGE_SIZE) {
		(*current_page_ptr)->next_page = (DataPage *)schema_maps_get_new_page(schema_maps);
		*current_page_ptr = (*current_page_ptr)->next_page;
	}

  auto current_page = request_info_and_page->current_page;
  
  auto write_ptr = current_page->data + current_page->bytes_written;
  *(uint64_t *)write_ptr = maximum_offset + max_offset_data_size;
	for (int i = 0; i < parsed_columns.n_results; ++i) {
    auto &parsed_column = parsed_columns.results[i];
    auto &cache_entry = cache.entries[i];

		auto data_size = std::max(8u, get_data_size_from_col_type(parsed_column.type));
		auto offset = request_info->column_offsets[get_offset_idx_from_id(cache_entry.id)] + sizeof(uint64_t); // add the size of the row length
		auto col_ptr = write_ptr + offset;

    switch (parsed_column.type)
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
		case ColumnDataType::VARCHAR:
			std::memcpy(col_ptr, &parsed_column.data.varchar, data_size);
			break;
		default:
			break;
		}
	}

  current_page->bytes_written += maximum_offset + max_offset_data_size;
  assert(current_page->bytes_written <= DATA_PAGE_SIZE);
}

void *process_write_request(ThreadContext *t_ctx, DatabaseContext *context, String8 data_to_write)
{
	auto time_start = platform_get_high_res_timer_stamp();
	auto schema_maps_result = get_latest_schema_maps_inc_refcount(&context->schema_maps_tripple_buffer);
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

	for (; tokeniser.at - (char *)data_to_write.content < data_to_write.length; ++tokeniser.at) {
		row_count++;
		if (*tokeniser.at == '\n') {
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
      return 0;
    }

		StringSlice8 tags = tokeniser_get_slice_to(&tokeniser, data_to_write, ']');
		tokeniser.at++;
		StringSlice8 columns = tokeniser_get_slice_at(&tokeniser, data_to_write, '\n');

		parse_columns(&t_ctx->transient_arena, &parsed_columns_array, columns);

		auto table_id = schema_maps_lookup_table_id(global_schema_maps, table_name);

		if (!table_id.id) {
			auto page_map_result = local_table_page_map_lookup(local_schema_maps, table_name);

      // might get a valid entry but no page
			if (!page_map_result) {
				table_id = { .index = ++local_schema_maps->table_id_count, .local_flag = 1 };
				page_map_result = local_table_page_map_insert_new_page_and_info(local_schema_maps, table_name, table_id);
        auto request_info = page_map_result->info_and_page.request_info;

				for (int i = 0; i < parsed_columns_array.n_results; ++i) {
					auto &parsed_column = parsed_columns_array.results[i];
					auto id = request_info_insert(request_info, &parsed_column);
					request_info->new_column_types[id.index] = parsed_column.type;
					insert_col_info_to_row_cache_at_index(&prev_row_col_cache, id, parsed_column, i);
				}

				write_parsed_data(local_schema_maps, timestamp, &page_map_result->info_and_page, prev_row_col_cache, parsed_columns_array);

			} else {
        auto request_info = page_map_result->info_and_page.request_info;
        if (!page_map_result->info_and_page.page_head) {
          // if no page, clear the info and get a new page for now
          page_map_result->info_and_page.page_head = schema_maps_get_new_page(local_schema_maps);
          page_map_result->info_and_page.request_info = schema_maps_get_new_request_info(local_schema_maps);
        }

				for (int i = 0; i < parsed_columns_array.n_results; ++i) {
					auto &parsed_column = parsed_columns_array.results[i];
					auto &cache_entry = prev_row_col_cache.entries[i];
					if (StringSlice8{ cache_entry.prev_string, cache_entry.prev_string_length } != parsed_column.name) {
						auto id = request_info_insert_or_match(request_info, &parsed_columns_array.results[i]);
						insert_col_info_to_row_cache_at_index(&prev_row_col_cache, id, parsed_column, i);
					} else if (request_info->new_column_types[cache_entry.id.index] != parsed_column.type) {
						fprintf(stderr, "Mismatch Column Type");
						return 0;
					}

					write_parsed_data(local_schema_maps, timestamp, &page_map_result->info_and_page, prev_row_col_cache, parsed_columns_array);
				}
			}
		} else {
			// table exists can just use the schema_maps for lookups
			auto page_and_info = table_page_map_lookup(local_schema_maps, table_id);
      auto request_info = page_and_info->request_info;
			if (!page_and_info->page_head) {

				page_and_info = table_page_map_insert_new_page_and_info(local_schema_maps, table_id);
        request_info = page_and_info->request_info;
				request_info->table_schema_version = schema_maps_lookup_table_schema_by_id(global_schema_maps, table_id)->version;

				for (int i = 0; i < parsed_columns_array.n_results; ++i) {
					auto &parsed_column = parsed_columns_array.results[i];
					auto column_id = schema_maps_lookup_column_id(global_schema_maps, table_id, parsed_column.name);
					if (!column_id.id) {
						column_id = request_info_insert(request_info, &parsed_column);
					} else {
						if (schema_maps_get_column_data_type(global_schema_maps, table_id, column_id) != parsed_column.type) {
							fprintf(stderr, "Mismatch Column Type");
							return 0;
						};
            request_info->global_column_ids[column_id.index] = column_id;
					}

          request_info_assign_offset(request_info, column_id, parsed_column.type);
          insert_col_info_to_row_cache_at_index(&prev_row_col_cache, column_id, parsed_column, i);
				}

				write_parsed_data(local_schema_maps, timestamp, page_and_info, prev_row_col_cache, parsed_columns_array);

			} else {
				request_info->table_schema_version = schema_maps_lookup_table_schema_by_id(global_schema_maps, table_id)->version;

				for (int i = 0; i < parsed_columns_array.n_results; ++i) {
					auto &parsed_column = parsed_columns_array.results[i];
					auto &cache_entry = prev_row_col_cache.entries[i];
          auto new_col = false;
					if (StringSlice8{ cache_entry.prev_string, cache_entry.prev_string_length } != parsed_column.name) {
            auto column_id = schema_maps_lookup_column_id(global_schema_maps, table_id, parsed_column.name);
            if (!column_id.id) 
            {
							column_id = request_info_insert_or_match(request_info, &parsed_columns_array.results[i]);
              if (!column_id.id) {
                return 0;
              }
						} else {
              if (schema_maps_get_column_data_type(global_schema_maps, table_id, column_id) != parsed_column.type) {
                fprintf(stderr, "Mismatch Column Type");
                return 0;
              }
            }

            insert_col_info_to_row_cache_at_index(&prev_row_col_cache, column_id, parsed_column, i);

					} else {
            ColumnDataType data_type{};
            if (cache_entry.id.local_flag) {
              data_type = request_info->new_column_types[cache_entry.id.index];
            } else {
              data_type = schema_maps_get_column_data_type(global_schema_maps, table_id, cache_entry.id);
            }

            if (data_type != parsed_column.type) {
              fprintf(stderr, "Mismatch Column Type");
              return 0;
            }
          }
				}

				write_parsed_data(local_schema_maps, timestamp, page_and_info, prev_row_col_cache, parsed_columns_array);

			}
		}

		parsed_columns_array.n_results = 0;
	}

	schema_maps_dec_refcount(&context->schema_maps_tripple_buffer, schema_maps_result);
	arena_clear(&t_ctx->transient_arena);

	auto time_end = platform_get_high_res_timer_stamp();
	double ms_time_taken = ((double)(time_end - time_start) / (double)platform_high_res_timer_freq()) * 1000;
  t_ctx->ms_time_taken = ms_time_taken;

	fprintf(stderr, "Thread %d: Ingested %d rows, time taken %f ms\n", t_ctx->thread_id, row_count, ms_time_taken);
	fprintf(stderr, "Timestamp start: %llu, Timestamp end: %llu\n", time_start, time_end);
	return 0;
}
