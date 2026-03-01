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

bool create_directory(const char *path);
FileHandle create_file(const char *path);
size_t get_filesize(const char *path);
int read_entire_file(const char *path, void *buffer, size_t buffer_size);
void memory_map_file_handle_read_only(MemoryMappedFile *mmf, FileHandle handle, uint64_t filesize);
void memory_map_entire_file_read_only(MemoryMappedFile *mmf, const char *filepath);
void memory_map_file_handle_append(MemoryMappedFile *mmf, FileHandle handle, uint64_t append_size);
void memory_map_entire_file_append(MemoryMappedFile *mmf, const char *filepath, uint64_t append_size);
void *mmf_mapping_offset_ptr(MemoryMappedFile *mmf, uint64_t offset);
void unmap_file(MemoryMappedFile *mmf);
void mmf_write(MemoryMappedFile *mmf, uint64_t offset, char *data, uint64_t data_size);
void mmf_append(MemoryMappedFile *mmf, void *data, uint64_t data_size);
uint64_t platform_get_high_res_timer_stamp();
uint64_t platform_high_res_timer_freq();

#define atomic_fetch_add_s64_rlxd
#define atomic_fetch_add_s64_acq
#define atomic_fetch_add_s64_rel
#define atomic_fetch_add_s64_acq_rel
#define atomic_fetch_add_s64_seq_cst

#define atomic_fetch_add_u64_rlxd
#define atomic_fetch_add_u64_acq
#define atomic_fetch_add_u64_rel
#define atomic_fetch_add_u64_acq_rel
#define atomic_fetch_add_u64_seq_cst

#define atomic_compare_and_swap_pointer_rlxd
#define atomic_compare_and_swap_pointer_acq
#define atomic_compare_and_swap_pointer_rel
#define atomic_compare_and_swap_pointer_acq_rel
#define atomic_compare_and_swap_pointer_seq_cst

#define atomic_compare_and_swap_128_rlxd
#define atomic_compare_and_swap_128_acq
#define atomic_compare_and_swap_128_rel
#define atomic_compare_and_swap_128_acq_rel
#define atomic_compare_and_swap_128_seq_cst

#define mmf_append_struct(mmf, struct_data) mmf_append(mmf, (void *)(struct_data), sizeof(*struct_data))

#define PACKED_STRUCT_START
#define PACKED_STRUCT_END
