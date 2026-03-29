#include "chrono_platform.cpp"
#include "context.h"
#include "metadata.cpp"
#include <stdint.h>
#include <stdio.h>

int main() {
  MemoryMappedFile active_partition_shen;
  memory_map_entire_file_read_only(&active_partition_shen, "TEST_DB/tables/table0/active_partition.data");

  auto top_of_page = (uint8_t *)active_partition_shen.mapping;
  auto count = 0;
  while (top_of_page < (uint8_t *)active_partition_shen.mapping + active_partition_shen.mapping_size) {
    auto header = (DataPageHeader *)top_of_page;
    auto data = top_of_page + sizeof(DataPageHeader);
    for (int i = 0; i < header->column_count; ++i) {
      auto column_id_and_type = header->column_data[i];
      auto column_offset_from_row = header->column_offsets[i];
      printf("Column header id: %llu, type: %s, offset: %d\n", column_id_and_type.id.id, COLUMN_TYPE_STRINGS[(uint32_t)column_id_and_type.type], column_offset_from_row);
    }

    auto pointer_in_row = data;
    assert(pointer_in_row < data + active_partition_shen.mapping_size);
    auto row_size = *(uint64_t *)pointer_in_row;
    while (pointer_in_row < top_of_page + header->row_write_offset) {
      auto row_data_ptr = pointer_in_row + 8;
      printf("Row data size: %llu", *(uint64_t *)pointer_in_row);
      for (int i = 0; i < header->column_count; ++i) {
        auto column_id_and_type = header->column_data[i];
        auto col_ptr = (uint64_t *)(row_data_ptr + header->column_offsets[i]);
        printf(", column data at id: %llu, %lli", column_id_and_type.id.id, *(int64_t *)col_ptr);
      }
      pointer_in_row += row_size;
      printf(", timestamp: %llu\n",
             *(uint64_t *)(pointer_in_row - sizeof(uint64_t))); // go back 1 u64 to get the timestamp
      row_size = *(uint64_t *)pointer_in_row;
      count++;
    }

  top_of_page += header->next_page;
}

 printf("\nReader finished processed: %d rows\n", count);
}

