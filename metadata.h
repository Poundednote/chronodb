/* TODO(RAY): Need to refactor the functions to take in the precomupted hashes
 * Most of the lookup functions end up rehahsing column values. For tables that already exist we tend to check the 
 * current global schema first for the column and then we also rehash again into the local schema if we cant find it
 
*/ 
#pragma once

#define MAX_TABLES (16384)
#define DEFAULT_TABLE_CAPACITY (512u)
#define TABLE_PAGE_LIMIT (8192)
#define PER_REQUEST_INFO_LIMIT (256)
#define LOCAL_TABLE_PAGE_MAP_LIMIT (512)
#define MAX_COLUMNS (1024)
#define DATA_PAGE_SIZE (KILOBYTES(64))

#include <stdint.h>

#include "utils.h"
#include "chrono_platform.h"

#define MAX_TAGS (256)
#define MAX_TABLE_NAME_SIZE (256u)
#define MAX_PATH_SIZE (1024u)
#define COLUMN_CAPACITY_DEFAULT (1024)
#define COLUMN_MAX_NAME_SIZE (63u)
#define COLUMN_ID_MAP_SIZE (1024u)
#define DATA_PAGE_HEADER_SIZE (512)


#if defined(__x86_64__) || defined(_M_X64)
#include <intrin.h>
typedef __m128i int128_t;
typedef __m128i uint128_t;
// INTEL / AMD (SSE 4.1)

inline int compare128(const void *a, const void *b)
{
	__m128i v_a = _mm_loadu_si128((const __m128i *)a);
	__m128i v_b = _mm_loadu_si128((const __m128i *)b);

	// Compare 64-bit lanes. Returns 0xFF.. if equal, 0 otherwise.
	__m128i cmp = _mm_cmpeq_epi64(v_a, v_b);

	// Move High bits to integer. If all are 1, result is 0xFFFF.
	return _mm_movemask_epi8(cmp) == 0xFFFF;
}

/*
inline __m128i int128_from_two_int64(uint64_t a, uint64_t b) 
{
	return _mm_set_epi64((__m64)a, (__m64)b);
}


inline __m128i int128_broadcast(uint64_t a)
{
	return _mm_set1_epi64((__m64)a);
}
*/

#elif defined(__aarch64__) || defined(_M_ARM64)
// ARM64 (NEON)
#include <arm_neon.h>

typedef uint64x2_t int128_t;
inline int compare128(const void *a, const void *b)
{
	uint64x2_t v_a = vld1q_u64((const uint64_t *)a);
	uint64x2_t v_b = vld1q_u64((const uint64_t *)b);

	// Compare lanes. Sets all bits to 1 if equal.
	uint64x2_t cmp = vceqq_u64(v_a, v_b);

	// Reinterpret as 32-bit and find min. If equal, min is 0xFFFFFFFF.
	return vminvq_u32(vreinterpretq_u32_u64(cmp)) == 0xFFFFFFFF;
}

#else
// FALLBACK
inline int compare128(const void *a, const void *b)
{
	uint64_t v_a[2], v_b[2];
	memcpy(v_a, a, 16);
	memcpy(v_b, b, 16);
	return (v_a[0] == v_b[0]) && (v_a[1] == v_b[1]);
}

#endif

enum class TableSchemaChangeOp : uint32_t {
	UPDATE_COL,
	DELETE_COL,
	INSERT_COL,
};

struct TableSchemaChangeUpdate {
	String8 previous_name;
	String8 new_name;
};

struct TableSchemaChangeInsert {
	String8 column_name;
};

struct TableSchemaChangeDelete {
	String8 column_name;
};

struct TableSchemaChange {
	TableSchemaChangeOp op;
	union {
		TableSchemaChangeUpdate update;
		TableSchemaChangeInsert insert;
		TableSchemaChangeDelete del;
	};
};

enum class ColumnDataType : uint32_t {
	INVALID,
	TIMESTAMP,
	INT32,
	FLOAT,
	DOUBLE,
	INT64,
	VARCHAR,
	COUNT,
};

//TOOD(Ray): X macros 

struct ColumnInfo {
	StringSlice8 name;
	ColumnDataType type;
};


struct SchemaColumnInfo {
	uint8_t name_length;
	ColumnDataType type;
};


// TODO(Ray) Sort out memeory allocation for stuff like this and for schema_cache
// can just allocate thread context on the main arena at a startup
// can also just use the permenent storage mechanism
//
// If i want to add a column I can add it to my schema cache and push to the writer thread an add column for a given string
// to add it to the cache i would need some growable array
PACKED_STRUCT_START
struct DataDictSchema {
	uint64_t number_of_rows_in_table;
	uint32_t name_length;
	char table_name[];
};
PACKED_STRUCT_END

struct DataDictHeader {
	uint32_t number_of_rows;
	uint32_t first_row_offset;
	uint32_t row_size_in_bytes;
};

struct TableDictHeader {
	uint32_t number_of_columns;
	uint32_t first_row_offset;
};

struct TableDictSchema {
	ColumnDataType type;	
	uint16_t name_length;
	char column_name[COLUMN_MAX_NAME_SIZE];
}; 

struct ColumnDataVarChar {
	const char *content;
	uint32_t length;
};

struct ColumnDataInt64 {
	int64_t value;
};

struct ColumnDataInt32 {
	int64_t value;
};

struct ColumnDataFloat {
	float value;
};

struct ColumnDataDouble {
	double value;
};

struct ColumnData {
	ColumnDataType type;
	union {
		String8 varchar;
		double dbl;
		float flt;
		int64_t int64;
		int32_t int32;
	};
};

struct ColumnID {
	union {
		uint64_t id;
		struct {
			uint32_t index;
			uint32_t generation : 30;
			uint32_t local_flag : 1;
			uint32_t fake_slot : 1;
		};
	};
};

struct TableID {
	union {
		uint64_t id;
		struct {
			uint32_t index;
			uint32_t generation : 30;
			uint32_t local_flag : 1;
			uint32_t fake_slot : 1;
		};
	};

	bool operator==(const TableID &rhs) const {return this->id == rhs.id;}
};

namespace std {
	template<>
    struct hash<TableID> {
        std::size_t operator()(const TableID& k) const noexcept {
            return std::hash<std::uint64_t>{}(k.id);
        }
    };
}

namespace std {
	template<>
    struct hash<ColumnID> {
        std::size_t operator()(const ColumnID& k) const noexcept {
            return std::hash<std::uint64_t>{}(k.id);
        }
    };
}




//TODO(Ray) We can split this data up
// we should store strings in a seperate pool arena
// the hot write path only cares about column_offset and id_hash_table_offset
// the allocator/ID producer only cares about column_counts and optimistic column count and the last free head
//
// table_names is mainly used once by to look up the table id 
//
//
struct TableSchema {
	uint64_t version;
	uint32_t column_schema_offset;
	uint32_t id_hash_table_offset;

	uint32_t column_count;
	uint32_t max_columns;

	uint32_t column_last_free_head;
};

struct SchemaString {
	uint8_t length;
	char buffer[63];
};

struct VariableSchemaString {
  uint8_t length;
  char buffer[];
};

struct SchemaTableMapBucket {
	uint64_t hash; 
	TableID table_id; 
};

// NOTE(Ray):
// These slots are actually pool allocators inside our schema arena
// f defined(__x86_64__) || defined(_M_X64)
//     #include <emmintrin.h>
//         #include <smmintrin.h>
//             #define SIMD_INTEL 1
//             #elif defined(__aarch64__) || defined(_M_ARM64)
//                 #include <arm_neon.h>
//                     #define SIMD_ARM 1
//                     #endifif tables are deleted we want to reuse old cache slots 
// the generation makes sure each slot gets a unique id
//
//
// TODO(Ray) template would go hard here
struct TableNameSlot {
	uint32_t generation;
	union {
		SchemaString table_name;
		uint32_t next_free_index;
	};
};

struct TableSchemaSlot {
	uint32_t generation;
	union {
		TableSchema schema;
		uint32_t next_free_index;
	};
};

struct ColumnIDAndType {
	ColumnID id;
	ColumnDataType type;
};


struct PerTableRequestInfo {
  // Hash table info
  Arena strings_arena; // make sure memory is 16 byte aligned for simd
	uint64_t table_schema_version;
  SchemaString arena_backing[DATA_PAGE_HEADER_SIZE];
	uint64_t new_column_hashes[DATA_PAGE_HEADER_SIZE];
  VariableSchemaString *string_ptrs[DATA_PAGE_HEADER_SIZE];
  uint32_t new_column_id_idxs[DATA_PAGE_HEADER_SIZE];
	ColumnDataType new_column_types[DATA_PAGE_HEADER_SIZE];

  // Global Column Info
  uint32_t new_column_id_count; 
  ColumnID global_column_ids[DATA_PAGE_HEADER_SIZE];

  uint32_t column_offsets[MAX_COLUMNS];
  uint32_t running_offset;
};

// TODO(Ray) Take these out from the header and have the writer thread build the header
struct DataPageHeader {
	uint64_t column_count;
	ColumnIDAndType column_data[DATA_PAGE_HEADER_SIZE];
  uint32_t column_offsets[DATA_PAGE_HEADER_SIZE];
};

struct DataPage {
  uint64_t start_timestamp;
  uint64_t end_timestamp;
  DataPage *next_page;
  uint32_t bytes_written;
  bool is_out_of_order;
  char data[];
};

struct RequestInfoAndPage {
  uint64_t start_timestamp;
  uint64_t end_timestamp;
  PerTableRequestInfo *request_info;
  DataPage *page_head;
  DataPage *current_page;
};

struct LocalTablePageMapValue {
	TableID table_id;
	RequestInfoAndPage info_and_page;
};

struct LocalTablePageMapBucket {
  uint64_t hash;
	LocalTablePageMapValue value;
};

struct LocalTablePageMap {
	SchemaString *strings;
	LocalTablePageMapBucket *buckets;
	uint32_t bucket_count;
	uint32_t capacity;
};

struct PrevRowColumnCacheEntry {
  ColumnID id;
  char *prev_string;
  uint32_t prev_string_length;
};

struct PrevRowColumnCache {
  PrevRowColumnCacheEntry *entries;
  uint32_t n_entries;
};

struct ThreadLocalSchemaMaps {
	Arena arena;
	PoolAllocator data_page_pool;
  PoolAllocator per_table_request_info_pool;
	RequestInfoAndPage *table_page_map_array;
	LocalTablePageMap local_table_page_map;
	uint32_t table_id_count;
};

// NOTE(Ray): Column maps are pretty small the chances are that we get the empty slot first time is high
// Storing the string inline means we dont cache miss comparing the string data. 
struct ColumnHashTableBucket {
		uint64_t hash;
		ColumnID column_id;

		uint8_t name_length;
		char name[COLUMN_MAX_NAME_SIZE];
};

struct ColumnHashTable {
	ColumnHashTableBucket *buckets;
	uint64_t capacity;
};

struct ColumnSchema {
	ColumnDataType type;
};

struct ColumnSlot {
	uint32_t generation;
	union {
		ColumnSchema schema;
		uint32_t next_free_index;
	};
};

static_assert(sizeof(SchemaString) == 64);

struct SchemaMaps {
	// NOTE(Ray):
	//
	// These descriptors function as indexes into the in memory schema 
	Arena arena;
	uint32_t table_id_map_first_bucket_offset;
	uint32_t table_first_name_slot_offset;
	uint32_t table_first_schema_slot_offset;
	uint32_t table_count;
	uint32_t table_capacity; 
	uint32_t table_slot_free_head;
};

struct SchemaCacheTrippleBuffer {
	std::atomic<uint64_t> version_number;
	std::atomic<uint32_t> refcounts[3];
	SchemaMaps maps[3];
};

struct ThreadContext {
	uint16_t thread_id;
	Arena transient_arena;
	ThreadLocalSchemaMaps schema_maps;
  double ms_time_taken;
};

struct DatabaseContext {
	String8 db_name;
	ThreadContext *thread_context_array;
	uint16_t thread_count;

	MemoryMappedFile data_dict_file;

	ThreadSafeMap<TableID, MemoryMappedFile> table_meta_file_map;
	PoolAllocator schema_maps_pool;
	SchemaCacheTrippleBuffer schema_maps_tripple_buffer;
};

struct SchemaMapsResult {
	SchemaMaps *maps;	
	uint64_t version_number;
};



inline int calculate_schema_padding_on_name_length(int64_t name_length);
void put_column_info_on_disk_schema_column(ColumnInfo *info, MemoryMappedFile *schema_file);
String8 create_table_dict_file_path_from_name(Arena *a,
					      DatabaseContext *context,
					      StringSlice8 table_name);
TableNameSlot *_get_table_name_slot_ptr(SchemaMaps *schema_maps, uint32_t index);
TableSchemaSlot *_get_table_schema_slot_ptr(SchemaMaps *schema_maps, uint32_t index);
ColumnSlot *_get_column_slot_ptr(SchemaMaps *schema_maps, TableID table_id, uint32_t index);
ptrdiff_t arena_alloc_struct_array_and_get_offset_from_base(Arena *a, uint32_t array_length);
ColumnHashTableBucket *_get_table_column_hash_map(SchemaMaps *schema_maps, uint32_t index);
void schema_maps_init(SchemaMaps *schema_maps, uint32_t table_capacity);
ColumnID schema_maps_create_column(SchemaMaps *schema_maps, TableID id, StringSlice8 column_string,
																	 ColumnDataType data_type);
ColumnID schema_maps_lookup_column_id(SchemaMaps *schema_maps, TableID id, StringSlice8 column_string);
ColumnDataType schema_maps_get_column_data_type(SchemaMaps *schema_maps, TableID table_id, ColumnID column_id);
SchemaTableMapBucket *_get_table_hash_map_first_bucket(SchemaMaps *schema_maps); 
TableID schema_maps_lookup_table_id(SchemaMaps *schema_maps, StringSlice8 table_string); 
TableID schema_maps_create_table(SchemaMaps *schema_maps, StringSlice8 table_name);
TableSchema *schema_maps_lookup_table_schema_by_id(SchemaMaps *schema_maps, TableID id); 
void schema_maps_delete_table(SchemaMaps *schema_maps, TableID id); 
SchemaMaps *get_latest_schema_maps(SchemaCacheTrippleBuffer *maps); 
SchemaMapsResult get_latest_schema_maps_inc_refcount(SchemaCacheTrippleBuffer *maps);
void schema_maps_dec_refcount(SchemaCacheTrippleBuffer *maps, SchemaMapsResult maps_result);
void thread_local_schema_maps_init(ThreadLocalSchemaMaps *schema_maps);
DataPage *schema_maps_get_new_page(ThreadLocalSchemaMaps *schema_maps); 
PerTableRequestInfo *schema_maps_get_new_request_info(ThreadLocalSchemaMaps *schema_maps); 
LocalTablePageMapValue *local_table_page_map_insert_new_page_and_info(ThreadLocalSchemaMaps *schema_maps, StringSlice8 table_name, TableID id); 
LocalTablePageMapValue *local_table_page_map_lookup(ThreadLocalSchemaMaps *schema_maps, StringSlice8 table_name);
RequestInfoAndPage *table_page_map_lookup(ThreadLocalSchemaMaps *schema_maps, TableID id); 
RequestInfoAndPage *table_page_map_insert_new_page_and_info(ThreadLocalSchemaMaps *schema_maps, TableID id);
uint32_t get_data_size_from_col_type(ColumnDataType type); 
