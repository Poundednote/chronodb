
#include "metadata.h"
#include "writer.h"

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
  WriterQueues writer_queues;
};

