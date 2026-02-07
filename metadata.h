#pragma once

#include <stdint.h>

#include "utils.h"
#include "chrono_platform.h"

#define MAX_TAGS (256)
#define MAX_COLUMNS (64)
#define MAX_TABLE_NAME_SIZE (256)
#define MAX_PATH_SIZE (1024)


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
	String8 name;
	ColumnDataType type;
};

struct TableSchema {
	Arena string_data_arena;
	uint64_t n_columns;
	uint64_t max_columns;
	String8 *column_names;
	ColumnDataType *column_data_types;
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
	int64_t number_of_rows;
	uint64_t row_size_in_bytes;
	uint32_t first_row_offset;
};

struct TableDictHeader {
	int64_t number_of_columns;
	uint32_t first_row_offset;
};

PACKED_STRUCT_START
struct TableDictSchema {
	ColumnDataType type;	
	uint16_t name_length;
	char column_name[];
}; 
PACKED_STRUCT_END

static_assert(sizeof(TableDictSchema) == 6);

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

typedef uint64_t TableDescriptor;
struct SchemaMaps {
	Arena arena; // this arena holds the schema_cache descriptor_map and the table_pool
	TableSchema *table_schemas;
	HashMapClosedAddr<StringSlice8, TableDescriptor> table_descriptor_map;
	void *table_pool_map;

	// NOTE(Ray):
	//
	// These descriptors function as indexes into the schema cache and the
	// pool map. We need to track the max limit because the arrays need to be realloced if we hit it
	TableDescriptor last_descriptor;
	TableDescriptor max_descriptor_limit; 
	uint64_t version;
};

struct DatabaseContext {
	String8 db_name;
	MemoryMappedFile data_dict_file;
	ThreadSafeMap<TableDescriptor, MemoryMappedFile> table_meta_file_map;
	PoolAllocator schema_maps_pool;
	SchemaMaps *schema_maps;
	PoolAllocator writer_thread_data_pool;
};


void put_column_info_on_disk_schema_column(const ColumnInfo *info, MemoryMappedFile *schema_file);
String8 create_table_dict_file_path_from_name(Arena *a, DatabaseContext *context, StringSlice8 table_name);
