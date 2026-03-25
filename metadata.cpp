#include "chrono_platform.h"
#include "metadata.h"

inline int calculate_schema_padding_on_name_length(int64_t name_length) 
{
	// add 5 because we need to add an extra 4 to get the required byte padding for multiple of 4 but we also
	// need to have space for the null terminator
	return 4 - ((name_length + 1) % 4);
}

void put_column_info_on_disk_schema_column(ColumnInfo *info, MemoryMappedFile *schema_file)
{

	auto padding_to_add = calculate_schema_padding_on_name_length(info->name.length);
	auto location_to_write = (TableDictSchema *)mmf_mapping_offset_ptr(schema_file, schema_file->filesize);
  TableDictSchema schema{};
	schema.type = info->type;
	schema.name_length = info->name.length;
	std::memcpy(schema.column_name, info->name.content, info->name.length);

	mmf_append_struct(schema_file, &schema);

	auto *header = (TableDictHeader *)schema_file->mapping;
	header->number_of_columns++;
}

TableNameSlot *_get_table_name_slot_ptr(SchemaMaps *schema_maps, uint32_t index) 
{
		
	auto *table_slots =
		(TableNameSlot *)((uint8_t *)schema_maps->arena.memory + schema_maps->table_first_name_slot_offset);

	return &table_slots[index];
}

TableSchemaSlot *_get_table_schema_slot_ptr(SchemaMaps *schema_maps, uint32_t index)
{

	TableSchemaSlot *table_slots =
		(TableSchemaSlot *)((uint8_t *)schema_maps->arena.memory + schema_maps->table_first_schema_slot_offset);

	return &table_slots[index];
}

ColumnSlot *_get_column_slot_ptr(SchemaMaps *schema_maps, TableID table_id, uint32_t index) 
{
	auto table_slot = _get_table_schema_slot_ptr(schema_maps, table_id.index);
	ColumnSlot *column_slots = (ColumnSlot *)((uint8_t *)schema_maps->arena.memory + table_slot->schema.column_schema_offset);

	if (index > table_slot->schema.max_columns) {
		return 0;
	}

	return &column_slots[index];
}

template <typename T>
ptrdiff_t arena_alloc_struct_array_and_get_offset_from_base(Arena *a, uint32_t array_length) 
{
	return (ptrdiff_t)((uint8_t *)arena_alloc_struct_array(a, T, array_length) - (uint8_t *)a->memory);
}

ColumnHashTableBucket *_get_table_column_hash_map(SchemaMaps *schema_maps, uint32_t index) 
{

	auto table_slot = _get_table_schema_slot_ptr(schema_maps, index);
	return (ColumnHashTableBucket *)((uint8_t *)schema_maps->arena.memory +
					 table_slot->schema.id_hash_table_offset);
}

void schema_maps_init(SchemaMaps *schema_maps, uint32_t table_capacity) {
	*schema_maps = {};
	auto maps_arena = &schema_maps->arena;
	auto table_schema_maps_size =
		table_capacity * (sizeof(SchemaTableMapBucket) + sizeof(TableSchema) + sizeof(TableNameSlot));
  
	auto column_arrays_size =
		table_capacity * MAX_COLUMNS * (sizeof(ColumnHashTableBucket) + sizeof(ColumnSlot));

	// Double it and give it the next person
	auto arena_capacity = table_schema_maps_size + column_arrays_size * 2; 
	arena_init(maps_arena, arena_capacity);
	std::memset(maps_arena->memory, 0, arena_capacity);

	schema_maps->table_id_map_first_bucket_offset =
		arena_alloc_struct_array_and_get_offset_from_base<SchemaTableMapBucket>(maps_arena, table_capacity);
	schema_maps->table_first_name_slot_offset =
		arena_alloc_struct_array_and_get_offset_from_base<TableNameSlot>(maps_arena, table_capacity);
	schema_maps->table_first_schema_slot_offset =
		arena_alloc_struct_array_and_get_offset_from_base<TableSchemaSlot>(maps_arena, table_capacity);

	schema_maps->table_capacity = table_capacity;


	// TODO(Ray):
	// Init pool allocator
}

ColumnID schema_maps_create_column(SchemaMaps *schema_maps, TableID id, StringSlice8 column_string,
																	 ColumnDataType data_type)
{
	if (id.id == 0) {
		return {};
	}

	auto table_slot = _get_table_schema_slot_ptr(schema_maps, id.index);
	if (table_slot->generation != id.generation) {
		return ColumnID{};
	}

	ColumnID result = {};
	auto &table_schema = table_slot->schema;
	ColumnSlot *column_slot = nullptr;

  ++table_schema.version;
	auto last_free_slot = table_schema.column_last_free_head;
	if (last_free_slot) {
		column_slot =_get_column_slot_ptr(schema_maps, id, last_free_slot);
		result.index = last_free_slot;
		result.generation = ++column_slot->generation;
	} else {

		auto desired_index = ++table_schema.column_count;
		result.index = desired_index;
		if (desired_index >= table_schema.max_columns) {
      //TODO(Ray): Return an error if we reach max capacity 
			result.fake_slot = 1;
			return result;
		}

		column_slot = _get_column_slot_ptr(schema_maps, id, desired_index);
	}

	auto &column_schema = column_slot->schema;
	column_schema.type = data_type;

	auto hash_table_buckets = _get_table_column_hash_map(schema_maps, id.index);
	auto hash = std::hash<StringSlice8>{}(column_string);
	auto hash_table_index = hash % table_schema.max_columns * 2;
	// TODO(Ray): Make safe lol
	auto bucket = &hash_table_buckets[hash_table_index];
	while (bucket->column_id.id != 0) {
		++bucket;
	}

	bucket->column_id = result;
	bucket->hash = hash;
	bucket->name_length = column_string.length;
	std::memcpy((void *)bucket->name, column_string.content, column_string.length);


	return result;
}

//TODO(Ray) need to compute the column hash anyway so i want a function to get the id with the hash
ColumnID schema_maps_lookup_column_id(SchemaMaps *schema_maps, TableID id, StringSlice8 column_string) 
{
	if (id.id == 0) {
		return {};
	}

	auto hash_table_buckets = _get_table_column_hash_map(schema_maps, id.index);
	auto table_slot = _get_table_schema_slot_ptr(schema_maps, id.index);

	auto hash = std::hash<StringSlice8>{}(column_string);
	auto hash_table_index = hash % table_slot->schema.max_columns * 2;
	auto bucket = &hash_table_buckets[hash_table_index];
	auto &table_schema = table_slot->schema;

	auto column_slots = (ColumnSlot *)(uint8_t *)schema_maps->arena.memory + table_schema.column_schema_offset;
	while (bucket->column_id.id != 0) {
		if (bucket->hash == hash) {
			if (StringSlice8{bucket->name, bucket->name_length} == column_string) {
				return bucket->column_id;
			}
		}

		++bucket;
	}

	return {};
}

ColumnID schema_maps_lookup_column_id(SchemaMaps *schema_maps, TableID id, StringSlice8 column_string, uint64_t hash) 
{
	if (id.id == 0) {
		return {};
	}

	auto hash_table_buckets = _get_table_column_hash_map(schema_maps, id.index);
	auto table_slot = _get_table_schema_slot_ptr(schema_maps, id.index);

	auto hash_table_index = hash % table_slot->schema.max_columns * 2;
	auto bucket = &hash_table_buckets[hash_table_index];
	auto &table_schema = table_slot->schema;

	auto column_slots = (ColumnSlot *)(uint8_t *)schema_maps->arena.memory + table_schema.column_schema_offset;
	while (bucket->column_id.id != 0) {
		if (bucket->hash == hash) {
			if (StringSlice8{bucket->name, bucket->name_length} == column_string) {
				return bucket->column_id;
			}
		}

		++bucket;
	}

	return {};
}

bool schema_maps_check_column_id_exists(SchemaMaps *schema_maps, TableID table_id, ColumnID column_id) 
{

  auto column_slot = _get_column_slot_ptr(schema_maps, table_id, column_id.index);
  if (!column_slot) {
    return false;
  }

  return column_slot->generation == column_id.generation;
}

bool schema_maps_check_table_id_exists(SchemaMaps *schema_maps, TableID table_id) 
{
  if (table_id.index >= schema_maps->table_count) {
    return false;
  }

  auto table_slot = _get_table_schema_slot_ptr(schema_maps, table_id.index);
  return table_slot->generation == table_id.generation;
}

ColumnDataType schema_maps_get_column_data_type(SchemaMaps *schema_maps, TableID table_id, ColumnID column_id) 
{
	auto column_slot = _get_column_slot_ptr(schema_maps, table_id, column_id.index);
  if (!column_slot || column_slot->generation != column_id.generation) {
    return ColumnDataType::INVALID;
  }

  return column_slot->schema.type;
}

SchemaTableMapBucket *_get_table_hash_map_first_bucket(SchemaMaps *schema_maps) 
{
	return (SchemaTableMapBucket *)((uint8_t *)schema_maps->arena.memory + schema_maps->table_id_map_first_bucket_offset);
}

TableID schema_maps_lookup_table_id(SchemaMaps *schema_maps, StringSlice8 table_string) 
{
	auto buckets = _get_table_hash_map_first_bucket(schema_maps);
	auto hash = std::hash<StringSlice8>{}(table_string);
	auto hash_table_index = hash % schema_maps->table_capacity;
	auto bucket = &buckets[hash_table_index];

	while (bucket->table_id.id != 0) {
		if (bucket->hash == hash) {
			auto name_ptr = _get_table_name_slot_ptr(schema_maps, bucket->table_id.index);
			auto &schema_string = name_ptr->table_name;
			if (StringSlice8{ schema_string.buffer, schema_string.length } == table_string) {
				return bucket->table_id;
			}
		}

		++bucket;
	}

	return {};
}

TableID schema_maps_create_table(SchemaMaps *schema_maps, StringSlice8 table_name)
{
	TableSchemaSlot *table_slot = nullptr;
	auto last_free_slot = schema_maps->table_slot_free_head;
	TableID result = {};

	if (last_free_slot) {
		table_slot = _get_table_schema_slot_ptr(schema_maps, last_free_slot);
		table_slot->schema.column_count = 0;
		result.index = last_free_slot;
		result.generation = ++table_slot->generation;
	} else { 
		auto desired_index = ++schema_maps->table_count;
		result.index = desired_index;
		if (desired_index >= schema_maps->table_capacity) {
      // TODO(Ray) REEEEEEEalloc
		} else {
			schema_maps->table_count++;

			table_slot = _get_table_schema_slot_ptr(schema_maps, desired_index);
			auto &schema = table_slot->schema;
			schema = {};
			uint8_t *base_addr = (uint8_t *)schema_maps->arena.memory;
			schema.column_schema_offset =
				(ptrdiff_t)((uint8_t *)arena_alloc_struct_array(&schema_maps->arena, ColumnSlot, MAX_COLUMNS) - base_addr);
			schema.id_hash_table_offset =
				(ptrdiff_t)((uint8_t *)arena_alloc_struct_array(&schema_maps->arena, ColumnHashTableBucket, MAX_COLUMNS * 2) -
										base_addr);
			schema.max_columns = MAX_COLUMNS;
		}
	}

	auto table_map_buckets = _get_table_hash_map_first_bucket(schema_maps);
	auto hash = std::hash<StringSlice8>{}(table_name);
	auto hash_table_index = hash % schema_maps->table_capacity;

	
	// TODO(Ray): Make safe lol
	auto bucket = &table_map_buckets[hash_table_index];
	while (bucket->table_id.id != 0) {
		++bucket;
	}

	bucket->table_id = result;
	bucket->hash = hash;
	auto name_slot_ptr = _get_table_name_slot_ptr(schema_maps, result.index);
	auto &slot_name = name_slot_ptr->table_name;
	slot_name.length = table_name.length;
	std::memcpy((void *)slot_name.buffer, table_name.content, table_name.length);

	return result;
}

TableSchema *schema_maps_lookup_table_schema_by_id(SchemaMaps *schema_maps, TableID id) 
{
	return &_get_table_schema_slot_ptr(schema_maps, id.index)->schema;
}

void schema_maps_delete_table(SchemaMaps *schema_maps, TableID id) 
{
	if (id.id == 0) {
		return;
	}

	TableSchemaSlot *table_slots =
		(TableSchemaSlot *)(uint8_t *)schema_maps->arena.memory + schema_maps->table_first_schema_slot_offset;

	auto table_slot = &table_slots[id.index];

	if (table_slot->generation != id.generation) {
		return;	
	}

	table_slot->next_free_index = schema_maps->table_slot_free_head;
	schema_maps->table_slot_free_head = id.index;
}


SchemaMaps *get_latest_schema_maps(SchemaCacheTrippleBuffer *maps) 
{
	return &maps->maps[maps->version_number % 3];
}

SchemaMaps *_get_map_at_version(SchemaCacheTrippleBuffer *maps, uint64_t version)
{
  return &maps->maps[version % 3];
}

SchemaMapsResult get_latest_schema_maps_inc_refcount(SchemaCacheTrippleBuffer *maps)
{
	auto version_number = maps->version_number.load(std::memory_order_acquire);
	auto index = version_number % 3;
	auto refcount = maps->refcounts[index].fetch_add(1, std::memory_order_release);
	return { &maps->maps[index], version_number };
}

void schema_maps_dec_refcount(SchemaCacheTrippleBuffer *maps, SchemaMapsResult maps_result)
{
	auto index = maps_result.version_number % 3;
	maps->refcounts[index].fetch_sub(1, std::memory_order_acquire);
}

void thread_local_schema_maps_init(ThreadLocalSchemaMaps *schema_maps)
{
  *schema_maps = {};
  auto arena = &schema_maps->arena;

	arena_init(arena, TABLE_PAGE_MAP_SIZE + LOCAL_TABLE_PAGE_MAP_SIZE + TABLE_PAGES_SIZE + TABLE_REQUEST_INFO_SIZE +
											ACTIVE_GLOBAL_TABLE_PAGES_SIZE);

	pool_init(&schema_maps->data_page_pool, arena, DATA_PAGE_SIZE, TABLE_PAGE_LIMIT);
	pool_init(&schema_maps->per_table_request_info_pool, arena, sizeof(PerTableRequestInfo), TABLE_PAGE_LIMIT);
  pool_init(&schema_maps->data_page_and_metadata_pool, arena, sizeof(DataPageAndMetadata), TABLE_PAGE_LIMIT);

	schema_maps->active_global_table_pages = arena_alloc_struct_array(arena, TableID, TABLE_PAGE_LIMIT);
	schema_maps->table_page_map_array = arena_alloc_struct_array(arena, RequestInfoAndPage, MAX_TABLES);
	schema_maps->local_table_page_map.buckets =
		arena_alloc_struct_array(arena, LocalTablePageMapBucket, DEFAULT_TABLE_CAPACITY);
	schema_maps->local_table_page_map.strings = arena_alloc_struct_array(arena, SchemaString, DEFAULT_TABLE_CAPACITY);
  schema_maps->local_table_page_map.info_and_page_arr = arena_alloc_struct_array(arena, RequestInfoAndPage, DEFAULT_TABLE_CAPACITY);
  schema_maps->local_table_page_map.capacity = DEFAULT_TABLE_CAPACITY;

}

DataPageAndMetadata *schema_maps_get_new_page_and_metadata(ThreadLocalSchemaMaps *schema_maps) 
{
  auto data_page = (DataPage *)pool_atomic_alloc(&schema_maps->data_page_pool);
  auto page_and_metadata = (DataPageAndMetadata *)pool_atomic_alloc(&schema_maps->data_page_and_metadata_pool);
  data_page->header.column_count = 0;
  page_and_metadata->metadata = {};
  page_and_metadata->metadata.bytes_written = sizeof(DataPageHeader);
  page_and_metadata->page = data_page;
  return page_and_metadata;
}

PerTableRequestInfo *schema_maps_get_new_request_info(ThreadLocalSchemaMaps *schema_maps) 
{
  auto request_info = (PerTableRequestInfo *)pool_atomic_alloc(&schema_maps->per_table_request_info_pool);
  *request_info = {};
  request_info->strings_arena.memory = request_info->arena_backing;
  request_info->strings_arena.capacity = sizeof(request_info->arena_backing);

  return request_info;
}

RequestInfoAndPage *local_table_page_map_insert_new_page_and_info(ThreadLocalSchemaMaps *schema_maps, StringSlice8 table_name) 
{
  auto page_and_metadata = schema_maps_get_new_page_and_metadata(schema_maps);
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

RequestInfoAndPage *table_page_map_insert_new_page_and_info(ThreadLocalSchemaMaps *schema_maps, TableID id)
{


  auto &slot = schema_maps->table_page_map_array[id.index];

  auto new_page = schema_maps_get_new_page_and_metadata(schema_maps);
	slot.request_info = schema_maps_get_new_request_info(schema_maps);
  slot.current_page = new_page;
  slot.page_head = new_page;

  schema_maps->active_global_table_pages[schema_maps->active_global_table_pages_count++] = id;

  return &slot;
}

uint32_t get_data_size_from_col_type(ColumnDataType type)
{
	switch (type) {
	case ColumnDataType::DOUBLE:
	case ColumnDataType::INT64:
		return 8;

	case ColumnDataType::FLOAT:
	case ColumnDataType::INT32:
		return 4;

	case ColumnDataType::VARCHAR:
		return 255;
	default:
		return 0;
	}
}

uint32_t get_offset_idx_from_id(ColumnID id)
{
	return id.local_flag ? id.index + DATA_PAGE_HEADER_SIZE : id.index;
}

String8 create_table_dict_file_path_from_name(Arena *a,
					      DatabaseContext *context,
					      StringSlice8 table_name)
{
	StringBuilder8 table_directory;
	uint32_t table_file_path_length = context->db_name.length + 1 +
					  table_name.length + 1 +
					  sizeof("/tables/dict.meta") - 1;

	string_builder8_init(a, &table_directory, table_file_path_length + 1);
	string_builder8_append(&table_directory, context->db_name);
	string_builder8_append(&table_directory,
			       string8_from_cstring("/tables/"));
	string_builder8_append(&table_directory, table_name);
	string_builder8_append(&table_directory, string8_from_cstring("/dict.meta"));


	return string_builder8_to_string(table_directory);
}

String8 create_table_hot_partition_path_from_name(Arena *a, 
                                                  DatabaseContext *context, 
                                                  StringSlice8 table_name) {

	StringBuilder8 table_directory;
	uint32_t table_file_path_length = context->db_name.length + 1 +
					  table_name.length + 1 +
					  sizeof("/tables/active_partition.data") - 1;

	string_builder8_init(a, &table_directory, table_file_path_length + 1);
	string_builder8_append(&table_directory, context->db_name);
	string_builder8_append(&table_directory,
			       string8_from_cstring("/tables/"));
	string_builder8_append(&table_directory, table_name);
	string_builder8_append(&table_directory, string8_from_cstring("/active_partition.data"));

  return {table_directory.content, table_directory.length};
}

