#pragma once

#include "writer.h"
#include "ingestion_worker.h"
#include "utils.h"

struct DatabaseContext {
      String8 db_name;
      IngestionWorkerContext *thread_context_array;
      uint16_t thread_count;

      MemoryMappedFile data_dict_file;

      ThreadSafeMap<TableID, MemoryMappedFile> table_meta_file_map;
      SchemaCacheTrippleBuffer schema_maps_tripple_buffer;
      WriterQueues writer_queues;
};


