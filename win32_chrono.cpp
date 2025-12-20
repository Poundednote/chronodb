#include <stdio.h>
#include <stdint.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <assert.h>

#include "chrono_platform.h"

bool create_directory(const char *path)
{
	BOOL result = CreateDirectory(path, NULL);
	return result;
};	


FileHandle create_file(const char *path)
{
	HANDLE fh = CreateFile(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL,
		   OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

	return fh == INVALID_HANDLE_VALUE ? FileHandle{} : FileHandle{fh};
}

void memory_map_file_handle_read_only(MemoryMappedFile *mmf, FileHandle handle) 
{
	HANDLE fh = handle.os_handle;
	// get file size
	LARGE_INTEGER file_size = {};
	GetFileSizeEx(fh, &file_size);

	HANDLE mapping_handle =
		CreateFileMappingA(fh, NULL, PAGE_READWRITE,
				   file_size.HighPart, file_size.LowPart, NULL);


	if (mapping_handle == NULL) {
		fprintf(stderr, "Error: Could not create file mapping\n");
		CloseHandle(fh);
		return; 
	}

	void *file_memory = MapViewOfFile(
		mapping_handle,
		FILE_MAP_READ,
		0, 0, 0);

	CloseHandle(mapping_handle);
	CloseHandle(fh);

	uint64_t mapping_size = *(uint64_t *)(&file_size.QuadPart);
	*mmf = MemoryMappedFile{handle, file_memory, mapping_size, mapping_size, MMFileAccess::READ};
}

void memory_map_entire_file_read_only(MemoryMappedFile *mmf, const char *filepath) 
{
	HANDLE fh = CreateFileA(
		filepath,
		GENERIC_READ, 
		0, 
		NULL,
		OPEN_ALWAYS,
		FILE_ATTRIBUTE_NORMAL, NULL);

	if (fh == INVALID_HANDLE_VALUE) {
		fprintf(stderr, "Error: Could not open file\n");
		return; 
	}

	memory_map_file_handle_read_only(mmf, FileHandle{fh});
}


void memory_map_file_handle_append(MemoryMappedFile *mmf, FileHandle handle, uint64_t append_size) 
{
	HANDLE fh = handle.os_handle;
	*mmf = {};

	if (!fh) {
		fprintf(stderr, "Error: Could not open file\n");
		return;
	}

	// get file size
	LARGE_INTEGER file_size = {};
	GetFileSizeEx(fh, &file_size);

	uint64_t mapping_size = file_size.QuadPart + append_size;
	HANDLE mapping_handle =
		CreateFileMappingA(fh, NULL, PAGE_READWRITE,
				   mapping_size >> 32, mapping_size & 0xFFFFFFFF, NULL);


	if (mapping_handle == NULL) {
		fprintf(stderr, "Error: Could not create file mapping\n");
		CloseHandle(fh);
		return;
	}

	void *file_memory = MapViewOfFile(
		mapping_handle,
		FILE_MAP_READ | FILE_MAP_WRITE,
		0, 0, mapping_size);

	CloseHandle(mapping_handle);

	*mmf = MemoryMappedFile{handle, file_memory, mapping_size, *(uint64_t *)(&file_size.QuadPart), MMFileAccess::WRITE};
}

void memory_map_entire_file_append(MemoryMappedFile *mmf, const char *filepath, uint64_t append_size) 
{
	*mmf = {};
	HANDLE fh = CreateFileA(filepath, GENERIC_READ | GENERIC_WRITE, 0, NULL,
				OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

	if (fh == INVALID_HANDLE_VALUE) {
		fprintf(stderr, "Error: Could not open file\n");
		return;
	}

	memory_map_file_handle_append(mmf, FileHandle{fh}, append_size);
}

void unmap_file(MemoryMappedFile *mmf)
{
	UnmapViewOfFile(mmf->mapping);
	HANDLE fh = mmf->handle.os_handle;
	LARGE_INTEGER bytes_to_move = {};
	bytes_to_move.QuadPart = mmf->filesize;
	SetFilePointerEx(fh, bytes_to_move, NULL, FILE_BEGIN);
	SetEndOfFile(mmf->handle.os_handle);
	CloseHandle(fh);
	mmf = {};
}

void mmf_write(MemoryMappedFile *mmf, uint64_t offset, char *data, uint64_t data_size)
{
	if (mmf->mapping && mmf->access == MMFileAccess::WRITE) {
		assert(mmf->mapping_size > offset);
		mmf->filesize = max(offset + data_size, mmf->filesize);
		memcpy_s((uint8_t *)mmf->mapping + offset, mmf->mapping_size - offset, data, data_size);
	}
}

void mmf_append(MemoryMappedFile *mmf, void *data, uint64_t data_size)
{
	if (mmf->mapping && mmf->access == MMFileAccess::WRITE) {
		assert(mmf->mapping_size > mmf->filesize + data_size);
		mmf->filesize += data_size;
		memcpy_s((uint8_t *)(mmf->mapping) + mmf->filesize,
			 mmf->mapping_size - mmf->filesize, data, data_size);
	}
}

#define mmf_append_struct(mmf, struct_data) mmf_append(mmf, (void *)(struct_data), sizeof(*struct_data))

#define atomic_fetch_add_s64(i) InterlockedIncrement64(i)
#define atomic_fetch_add_u64(i) InterlockedIncrement64((volatile int64_t *)(i))
