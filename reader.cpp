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
  auto prev_timestamp = 0;
  while (top_of_page < (uint8_t *)active_partition_shen.mapping + active_partition_shen.mapping_size) {
		auto header = (DataPageHeader *)top_of_page;
		auto data = top_of_page + sizeof(DataPageHeader);
		for (int i = 0; i < header->column_count; ++i) {
			auto column_id_and_type = header->column_data[i];
			auto column_offset_from_row = header->column_offsets[i];
		}

		auto pointer_in_row = data;
		assert(pointer_in_row < data + active_partition_shen.mapping_size);
		auto row_size = *(uint64_t *)pointer_in_row;
		while (pointer_in_row < top_of_page + header->bytes_written) {
			auto row_data_ptr = pointer_in_row + 8;
			for (int i = 0; i < header->column_count; ++i) {
				auto column_id_and_type = header->column_data[i];
				auto col_ptr = (uint64_t *)(row_data_ptr + header->column_offsets[i]);
			}

			pointer_in_row += row_size;
			auto timestamp = *(uint64_t *)(pointer_in_row - sizeof(uint64_t));
			row_size = *(uint64_t *)pointer_in_row;
      count++;
			if (!prev_timestamp) {
				prev_timestamp = timestamp;
			} else {
				assert(prev_timestamp < timestamp);
				prev_timestamp = timestamp;
			}
		}

		top_of_page += header->next_page;
	}

	printf("last_ts: %d\n", prev_timestamp);
  assert(prev_timestamp == 9999999);
  printf("\nReader finished processed: %d rows\n", count);
}

