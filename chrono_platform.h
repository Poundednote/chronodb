#pragma once
#include <stdint.h>

#if defined(_WIN32)
#include <intrin.h>
#define cpu_pause() _mm_pause()
#elif defined(_M_ARM64) || defined(__aarch64__)
#define cpu_pause() __asm__ __volatile__("yield")
#endif

enum class MMFileAccess {
	NONE,
	READ,
	WRITE,
};


struct OSHandle {
  bool valid;
	void *os_handle;
};

struct MemoryMappedFile {
	OSHandle handle;
	void *mapping;
	int64_t mapping_size;
	int64_t filesize;
	MMFileAccess access;
};

struct ASIOBatch;

bool create_directory(const char *path);
OSHandle create_file(const char *path, bool is_async);
int64_t get_filesize(const char *path);
int read_entire_file(const char *path, void *buffer, size_t buffer_size);
void memory_map_file_handle_read_only(MemoryMappedFile *mmf, OSHandle &handle, int64_t filesize);
void memory_map_entire_file_read_only(MemoryMappedFile *mmf, const char *filepath);
void memory_map_file_handle_append(MemoryMappedFile *mmf, OSHandle handle, int64_t append_size);
void memory_map_entire_file_append(MemoryMappedFile *mmf, const char *filepath, int64_t append_size);
void *mmf_mapping_offset_ptr(MemoryMappedFile *mmf, int64_t offset);
void unmap_file(MemoryMappedFile *mmf);
void mmf_write(MemoryMappedFile *mmf, int64_t offset, char *data, uint64_t data_size);
void mmf_append(MemoryMappedFile *mmf, void *data, uint64_t data_size);
uint64_t platform_get_high_res_timer_stamp();
uint64_t platform_high_res_timer_freq();
bool platform_os_handle_is_valid(OSHandle handle);
#define mmf_append_struct(mmf, struct_data) mmf_append(mmf, (void *)(struct_data), sizeof(*struct_data))

#define PACKED_STRUCT_START
#define PACKED_STRUCT_END
