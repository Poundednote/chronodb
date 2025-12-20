#pragma once
#include <stdint.h>

enum class MMFileAccess {
	NONE,
	READ,
	WRITE,
};


struct FileHandle {
	void *os_handle;
};

struct MemoryMappedFile {
	FileHandle handle;
	void *mapping;
	uint64_t mapping_size;
	uint64_t filesize;
	MMFileAccess access;
};

void memory_map_entire_file_read_only(MemoryMappedFile *mmf, const char *filepath, uint64_t mapping_size);
void memory_map_file_handle_read_only(MemoryMappedFile *mmf, FileHandle handle);
void memory_map_entire_file_append(MemoryMappedFile *mmf, const char *filepath, uint64_t mapping_size);
void mmf_write(MemoryMappedFile *mmf, uint64_t offset, char *data, uint64_t data_size);
void unmap_file(MemoryMappedFile *mmf);
bool create_directory(const char *path);
FileHandle create_file(const char *path);
