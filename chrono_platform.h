#pragma once
#include <stdint.h>

enum class MMFileAccess {
	NONE,
	READ,
	WRITE,
};

struct MemoryMappedFile {
	void *mapping;
	uint64_t mapping_size;
	uint64_t filesize;
	MMFileAccess access;
};

MemoryMappedFile memory_map_entire_file_read_only(const char *filepath, uint64_t mapping_size);
MemoryMappedFile memory_map_entire_file_append(const char *filepath, uint64_t mapping_size);
void mmf_write(MemoryMappedFile *mmf, uint64_t offset, char *data, uint64_t data_size);
void unmap_file(MemoryMappedFile *mmf);
int64_t atomic_fetch_add(int64_t *i);
