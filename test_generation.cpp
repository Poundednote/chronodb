#include <string>

#include "chrono_platform.cpp"

#include "utils.h"
#include "metadata.cpp"


int main(void)
{
	Arena main_arena;
	arena_init(&main_arena, GIGABYTES(1));

	create_directory("./TEST_DB/");
	FileHandle fh = create_file("./TEST_DB/datadict.data");
	MemoryMappedFile data_dict_file = {};
	memory_map_file_handle_append(&data_dict_file, fh, KILOBYTES(256));

	uint32_t num_rows = 2;
	{
		auto data_dict_header = DataDictHeader{ num_rows, sizeof(DataDictSchema), sizeof(DataDictHeader) };
		mmf_append_struct(&data_dict_file, &data_dict_header);
	}

	DataDictHeader *data_dict_header = (DataDictHeader *)data_dict_file.mapping;
	auto ptr = (DataDictSchema *)((uint8_t *)data_dict_header + data_dict_header->first_row_offset);
	for (int i = 0; i < num_rows; ++i) {
		auto &row = ptr[i];
		StringBuilder8 table_name;
		string_builder8_init(&main_arena, &table_name, sizeof("table") + 5); // 5 extra because why not
		string_builder8_append(&table_name, string8_from_cstring("table"));
		std::string table_num_str = std::to_string(i);
		string_builder8_append(&table_name, string8_from_std_string(table_num_str));

		std::memcpy(row.table_name, table_name.content, table_name.length);
		row.name_length = table_name.length;

		StringBuilder8 table_dir;
		string_builder8_init(&main_arena, &table_dir,
				     sizeof("TEST_DB/tables/") + table_name.length + sizeof("/dict.data"));
		string_builder8_append(&table_dir, string8_from_cstring("TEST_DB/tables/"));
		create_directory((const char *)table_dir.content);
		string_builder8_append(&table_dir, table_name);
		create_directory((const char *)table_dir.content);
		string_builder8_append(&table_dir, string8_from_cstring("/dict.data"));
		FileHandle schema_fh = create_file((const char *)table_dir.content);

		MemoryMappedFile schema_file;
		memory_map_file_handle_append(&schema_file, schema_fh, KILOBYTES(1));

		//write header
		TableDictHeader table_header = {0, sizeof(TableDictHeader)};
		mmf_append_struct(&schema_file, &table_header);

		ColumnInfo info;
		info.name = string8_from_cstring("testcolumn0");
		info.type = ColumnDataType::INT64;
		put_column_info_on_disk_schema_column(&info, &schema_file);

		info.name = string8_from_cstring("testcolumn1");
		info.type = ColumnDataType::INT64;
		put_column_info_on_disk_schema_column(&info, &schema_file);

	}

	FlushViewOfFile(data_dict_file.mapping, 0);
	FlushFileBuffers((HANDLE)data_dict_file.handle.os_handle);

	return 0;
}
