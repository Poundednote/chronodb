#include <stdio.h>
#include <stdint.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <assert.h>

#include "chrono_platform.h"


MemoryMappedFile memory_map_entire_file_read_only(const char *filepath) 
{
	HANDLE file_handle = CreateFileA(
		filepath,
		GENERIC_READ, 
		0, 
		NULL,
		OPEN_ALWAYS,
		FILE_ATTRIBUTE_NORMAL, NULL);

	if (file_handle == INVALID_HANDLE_VALUE) {
		fprintf(stderr, "Error: Could not open file\n");
		return MemoryMappedFile{};
	}


	// get file size
	LARGE_INTEGER file_size = {};
	GetFileSizeEx(file_handle, &file_size);

	HANDLE mapping_handle =
		CreateFileMappingA(file_handle, NULL, PAGE_READWRITE,
				   file_size.HighPart, file_size.LowPart, NULL);


	if (mapping_handle == NULL) {
		fprintf(stderr, "Error: Could not create file mapping\n");
		CloseHandle(file_handle);
		return MemoryMappedFile{};
	}

	void *file_memory = MapViewOfFile(
		mapping_handle,
		FILE_MAP_READ,
		0, 0, 0);

	CloseHandle(mapping_handle);
	CloseHandle(file_handle);

	uint64_t mapping_size = *(uint64_t *)(&file_size.QuadPart);
	return MemoryMappedFile{file_memory, mapping_size, mapping_size, MMFileAccess::READ};
}

MemoryMappedFile memory_map_entire_file_append(const char *filepath, uint64_t append_size) 
{
	HANDLE file_handle = CreateFileA(
		filepath,
		GENERIC_READ | GENERIC_WRITE, 
		0, 
		NULL,
		OPEN_ALWAYS,
		FILE_ATTRIBUTE_NORMAL, NULL);

	if (file_handle == INVALID_HANDLE_VALUE) {
		fprintf(stderr, "Error: Could not open file\n");
		return MemoryMappedFile{};
	}

	// get file size
	LARGE_INTEGER file_size = {};
	GetFileSizeEx(file_handle, &file_size);

	uint64_t mapping_size = file_size.QuadPart + append_size;
	HANDLE mapping_handle =
		CreateFileMappingA(file_handle, NULL, PAGE_READWRITE,
				   mapping_size >> 32, mapping_size & 0xFFFFFFFF, NULL);


	if (mapping_handle == NULL) {
		fprintf(stderr, "Error: Could not create file mapping\n");
		CloseHandle(file_handle);
		return MemoryMappedFile{};
	}

	void *file_memory = MapViewOfFile(
		mapping_handle,
		FILE_MAP_READ | FILE_MAP_WRITE,
		0, 0, mapping_size);

	CloseHandle(mapping_handle);
	CloseHandle(file_handle);

	return MemoryMappedFile{file_memory, mapping_size, *(uint64_t *)(&file_size.QuadPart), MMFileAccess::WRITE};
}

void unmap_file(MemoryMappedFile *mmf)
{
	UnmapViewOfFile(mmf->mapping);
	mmf = {};
}

void mmf_write(MemoryMappedFile *mmf, uint64_t offset, char *data, uint64_t data_size)
{
	assert(mmf->mapping_size > offset);
	if (mmf->mapping && mmf->access == MMFileAccess::WRITE) {
		memcpy_s((uint8_t *)mmf->mapping + offset, mmf->mapping_size - offset, data, data_size);
	}
}

#define atomic_fetch_add_s64(i) InterlockedIncrement64(i)
#define atomic_fetch_add_u64(i) InterlockedIncrement64((volatile int64_t *)(i))
