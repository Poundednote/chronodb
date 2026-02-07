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

	auto bytes_to_write = sizeof(TableDictSchema) + info->name.length + padding_to_add + 1;
	auto location_to_write = (TableDictSchema *)mmf_mapping_offset_ptr(schema_file, schema_file->filesize);
	mmf_append(schema_file, 0, bytes_to_write);
		
	std::memset(location_to_write, 0, bytes_to_write); // set null terminator and padding on the struct
	location_to_write->type = info->type;
	location_to_write->name_length = info->name.length;
	std::memcpy(location_to_write->column_name, info->name.content, info->name.length);

	auto *header = (TableDictHeader *)schema_file->mapping;
	header->number_of_columns++;
}

String8 create_table_dict_file_path_from_name(Arena *a,
					      DatabaseContext *context,
					      StringSlice8 table_name)
{
	StringBuilder8 table_directory;
	uint32_t table_file_path_length = context->db_name.length + 1 +
					  table_name.length + 1 +
					  sizeof("/tables/dict.data") - 1;

	string_builder8_init(a, &table_directory, table_file_path_length + 1);
	string_builder8_append(&table_directory, context->db_name);
	string_builder8_append(&table_directory,
			       string8_from_cstring("/tables/"));
	string_builder8_append(&table_directory, table_name);
	string_builder8_append(&table_directory, string8_from_cstring("/dict.data"));


	return string_builder8_to_string(table_directory);
}
