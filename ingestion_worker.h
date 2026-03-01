#pragma once

#include "utils.h"
#include "metadata.h"
#include "context.h"

struct Tokeniser {
  char *at;
};

struct ParseColumnResult {
	String8 err_msg;
	StringSlice8 name;
	ColumnDataType type;
	ColumnData data;
};

struct ParseValueResult {
	String8 err_msg;
	ColumnDataType type;
	ColumnData data;
};

struct ParseColumnResultList {
	ParseColumnResult *results;
	uint32_t n_results;
};

struct TagsList {
	StringSlice8 *tags;
	uint32_t n_tags;
};

//TODO make sure multiple columns are not inserted in a single row
StringSlice8 tokeniser_get_slice_at(Tokeniser *t, String8 data, char target); 
StringSlice8 tokeniser_get_slice_to(Tokeniser *t, String8 data, char target);

bool string_sort_cmp(StringSlice8 a, StringSlice8 b);
ParseValueResult parse_string(StringSlice8 string);
ParseValueResult parse_value(StringSlice8 value);
ParseColumnResult parse_column(StringSlice8 col);
TagsList parse_tags(Arena *a, StringSlice8 tags);
void parse_columns(Arena *a, ParseColumnResultList *results_array, StringSlice8 columns);

uint32_t get_offset_idx_from_id(ColumnID id);

void request_info_assign_offset(PerTableRequestInfo *request_info, ColumnID id, ColumnDataType type);
ColumnID request_info_insert_or_match(PerTableRequestInfo *request_info, ParseColumnResult *result);
ColumnID request_info_insert(PerTableRequestInfo *request_info, ParseColumnResult *result);

void insert_col_info_to_row_cache_at_index(PrevRowColumnCache *cache, ColumnID id, ParseColumnResult parsed_column,
																					 int index);

void write_parsed_data(ThreadLocalSchemaMaps *schema_maps, uint64_t timestamp,
											 RequestInfoAndPage *request_info_and_page, PrevRowColumnCache cache,
											 ParseColumnResultList parsed_columns);
void *process_write_request(ThreadContext *t_ctx, DatabaseContext *context, String8 data_to_write);
uint32_t get_data_size_from_col_type(ColumnDataType type);
