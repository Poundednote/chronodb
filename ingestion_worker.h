#pragma once

#include "utils.h"
#include "metadata.h"

struct DatabaseContext;

struct PerTableRequestInfo {
  // Hash table info
  Arena strings_arena; // make sure memory is 16 byte aligned for simd
	uint64_t table_schema_version;
  uint64_t global_schema_version;
  SchemaString table_name;
  SchemaString arena_backing[MAX_COLUMNS];
	uint64_t new_column_hashes[MAX_COLUMNS];
  VariableSchemaString *string_ptrs[MAX_COLUMNS];
  uint32_t new_column_id_idxs[MAX_COLUMNS];
	ColumnDataType new_column_types[MAX_COLUMNS];
  uint32_t new_column_id_count; 
  
  //Global Column info
  ColumnID global_column_ids[MAX_COLUMNS];

  uint32_t column_offsets[MAX_COLUMNS];
  uint32_t running_offset;
  int64_t row_count;
};

struct DataPageHeader {
  bool is_out_of_order; 
  uint64_t start_timestamp;
  uint64_t end_timestamp;
  uint32_t bytes_written;
  uint64_t next_page;

	int64_t column_count;
	ColumnIDAndType column_data[DATA_PAGE_HEADER_SIZE];
  uint32_t column_offsets[DATA_PAGE_HEADER_SIZE];
};


struct RequestInfoAndPage {
  PerTableRequestInfo *request_info;
  uint8_t *page_head;
  uint8_t *current_page;
};

struct LocalTablePageMapBucket {
  uint64_t hash;
	TableID table_id;
};

struct LocalTablePageMap {
  SchemaString *strings;
  LocalTablePageMapBucket *buckets;
	RequestInfoAndPage *info_and_page_arr;
  uint32_t capacity;
};

struct ThreadLocalSchemaMaps {
	Arena arena;
  TableID *active_global_table_pages; // NOTE(Ray) This is only for actual tables 
	PoolAllocatorSPSCFreeListSize data_page_pool;
  PoolAllocatorSPSCFreeList<PerTableRequestInfo> per_table_request_info_pool;
	RequestInfoAndPage *table_page_map_array;
	LocalTablePageMap local_table_page_map;
	uint32_t table_id_count;
  uint32_t active_global_table_pages_count;
};



struct IngestionWorkerContext {
	uint16_t thread_id;
	Arena transient_arena;
	ThreadLocalSchemaMaps schema_maps;
  volatile uint64_t timer_diffs;
  volatile int64_t run_count;
  uint64_t prev_timestamp;
};

struct Tokeniser {
  char *at;
};

struct PrevRowColumnCacheEntry {
  TableID table_id;
  ColumnID column_id;
  StringSlice8 prev_string;
};

struct PrevRowColumnCache {
  PrevRowColumnCacheEntry *entries;
  uint32_t n_entries;
};


struct ParseColumnResult {
	String8 err_msg;
	StringSlice8 name;
	ColumnData data;
};

struct ParseValueResult {
	String8 err_msg;
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

struct GetNewPageResult {
  uint8_t *page;
  bool flushed;
};

static constexpr auto TABLE_PAGE_MAP_SIZE = MAX_TABLES * sizeof(RequestInfoAndPage);
static constexpr auto TABLE_PAGES_SIZE = (DATA_PAGE_SIZE + sizeof(void *)) * DATA_PAGE_LIMIT;
static constexpr auto LOCAL_TABLE_PAGE_MAP_SIZE = DEFAULT_TABLE_CAPACITY * (sizeof(LocalTablePageMapBucket) + sizeof(SchemaString) + sizeof(RequestInfoAndPage));
static constexpr auto TABLE_REQUEST_INFO_SIZE = TABLE_PAGE_LIMIT * (sizeof(PerTableRequestInfo) + sizeof(void *));
static constexpr auto ACTIVE_GLOBAL_TABLE_PAGES_SIZE = TABLE_PAGE_LIMIT * sizeof(TableID);


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

void request_info_assign_offset(PerTableRequestInfo *request_info, ColumnID id, ColumnData data);
ColumnID request_info_insert_or_match(PerTableRequestInfo *request_info, ParseColumnResult *result);
ColumnID request_info_insert(PerTableRequestInfo *request_info, ParseColumnResult *result);

void insert_col_info_to_row_cache_at_index(PrevRowColumnCache *cache, TableID table_id, ColumnID column_id, ParseColumnResult parsed_column,
																					 int index);

void write_parsed_data(DatabaseContext *db_context, IngestionWorkerContext *t_ctx, ThreadLocalSchemaMaps *schema_maps,
											 uint64_t timestamp, RequestInfoAndPage *request_info_and_page, PrevRowColumnCache cache,
											 ParseColumnResultList parsed_columns);
void *process_write_request(IngestionWorkerContext *t_ctx, DatabaseContext *context, String8 data_to_write);
void thread_local_schema_maps_init(ThreadLocalSchemaMaps *schema_maps);
uint8_t *schema_maps_get_new_page_and_metadata(DatabaseContext *db_context, IngestionWorkerContext *t_ctx, ThreadLocalSchemaMaps *schema_maps);
PerTableRequestInfo *schema_maps_get_new_request_info(ThreadLocalSchemaMaps *schema_maps); 
RequestInfoAndPage *local_table_page_map_insert_new_page_and_info(DatabaseContext *db_context, IngestionWorkerContext *t_ctx, ThreadLocalSchemaMaps *schema_maps, StringSlice8 table_name);
RequestInfoAndPage *local_table_page_map_lookup(ThreadLocalSchemaMaps *schema_maps, StringSlice8 table_name);
RequestInfoAndPage *table_page_map_lookup(ThreadLocalSchemaMaps *schema_maps, TableID id); 
RequestInfoAndPage *table_page_map_insert_new_page_and_info(DatabaseContext *db_context, IngestionWorkerContext *t_ctx, ThreadLocalSchemaMaps *schema_maps, TableID id);
