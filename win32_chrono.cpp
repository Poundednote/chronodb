#include <stdio.h>
#include <stdint.h>
#include <atomic>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <assert.h>

#include "chrono_platform.h"


#define mmf_append_struct(mmf, struct_data) mmf_append(mmf, (void *)(struct_data), sizeof(*struct_data))

#if defined(_M_ARM64)
    #define IS_ARM64 1
    #define IS_X64 0
#elif defined(_M_X64) || defined(_M_AMD64)
    #define IS_ARM64 0
    #define IS_X64 1
#endif


#if IS_ARM64
    #define atomic_fetch_add_s64_rlxd(ptr, val)    _InterlockedExchangeAdd64_nf((volatile __int64*)(ptr), (val))
    #define atomic_fetch_add_s64_acq(ptr, val)     _InterlockedExchangeAdd64_acq((volatile __int64*)(ptr), (val))
    #define atomic_fetch_add_s64_rel(ptr, val)     _InterlockedExchangeAdd64_rel((volatile __int64*)(ptr), (val))
    #define atomic_fetch_add_s64_acq_rel(ptr, val) _InterlockedExchangeAdd64((volatile __int64*)(ptr), (val)) 
    #define atomic_fetch_add_s64_seq_cst(ptr, val) _InterlockedExchangeAdd64((volatile __int64*)(ptr), (val))
#else
    #define atomic_fetch_add_s64_rlxd(ptr, val)    _InterlockedExchangeAdd64((volatile __int64*)(ptr), (val))
    #define atomic_fetch_add_s64_acq(ptr, val)     _InterlockedExchangeAdd64((volatile __int64*)(ptr), (val))
    #define atomic_fetch_add_s64_rel(ptr, val)     _InterlockedExchangeAdd64((volatile __int64*)(ptr), (val))
    #define atomic_fetch_add_s64_acq_rel(ptr, val) _InterlockedExchangeAdd64((volatile __int64*)(ptr), (val))
    #define atomic_fetch_add_s64_seq_cst(ptr, val) _InterlockedExchangeAdd64((volatile __int64*)(ptr), (val))
#endif

#if IS_ARM64
    #define atomic_fetch_add_u64_rlxd(ptr, val)    (uint64_t)_InterlockedExchangeAdd64_nf((volatile __int64*)(ptr), (__int64)(val))
    #define atomic_fetch_add_u64_acq(ptr, val)     (uint64_t)_InterlockedExchangeAdd64_acq((volatile __int64*)(ptr), (__int64)(val))
    #define atomic_fetch_add_u64_rel(ptr, val)     (uint64_t)_InterlockedExchangeAdd64_rel((volatile __int64*)(ptr), (__int64)(val))
    #define atomic_fetch_add_u64_acq_rel(ptr, val) (uint64_t)_InterlockedExchangeAdd64((volatile __int64*)(ptr), (__int64)(val))
    #define atomic_fetch_add_u64_seq_cst(ptr, val) (uint64_t)_InterlockedExchangeAdd64((volatile __int64*)(ptr), (__int64)(val))
#else
    #define atomic_fetch_add_u64_rlxd(ptr, val)    (uint64_t)_InterlockedExchangeAdd64((volatile __int64*)(ptr), (__int64)(val))
    #define atomic_fetch_add_u64_acq(ptr, val)     (uint64_t)_InterlockedExchangeAdd64((volatile __int64*)(ptr), (__int64)(val))
    #define atomic_fetch_add_u64_rel(ptr, val)     (uint64_t)_InterlockedExchangeAdd64((volatile __int64*)(ptr), (__int64)(val))
    #define atomic_fetch_add_u64_acq_rel(ptr, val) (uint64_t)_InterlockedExchangeAdd64((volatile __int64*)(ptr), (__int64)(val))
    #define atomic_fetch_add_u64_seq_cst(ptr, val) (uint64_t)_InterlockedExchangeAdd64((volatile __int64*)(ptr), (__int64)(val))
#endif

#if IS_ARM64
    #define atomic_compare_and_swap_pointer_rlxd(dst, exc, cmp)    _InterlockedCompareExchangePointer_nf((void* volatile*)(dst), (void*)(exc), (void*)(cmp))
    #define atomic_compare_and_swap_pointer_acq(dst, exc, cmp)     _InterlockedCompareExchangePointer_acq((void* volatile*)(dst), (void*)(exc), (void*)(cmp))
    #define atomic_compare_and_swap_pointer_rel(dst, exc, cmp)     _InterlockedCompareExchangePointer_rel((void* volatile*)(dst), (void*)(exc), (void*)(cmp))
    #define atomic_compare_and_swap_pointer_acq_rel(dst, exc, cmp) _InterlockedCompareExchangePointer((void* volatile*)(dst), (void*)(exc), (void*)(cmp))
    #define atomic_compare_and_swap_pointer_seq_cst(dst, exc, cmp) _InterlockedCompareExchangePointer((void* volatile*)(dst), (void*)(exc), (void*)(cmp))
#else
    #define atomic_compare_and_swap_pointer_rlxd(dst, exc, cmp)    _InterlockedCompareExchangePointer((void* volatile*)(dst), (void*)(exc), (void*)(cmp))
    #define atomic_compare_and_swap_pointer_acq(dst, exc, cmp)     _InterlockedCompareExchangePointer((void* volatile*)(dst), (void*)(exc), (void*)(cmp))
    #define atomic_compare_and_swap_pointer_rel(dst, exc, cmp)     _InterlockedCompareExchangePointer((void* volatile*)(dst), (void*)(exc), (void*)(cmp))
    #define atomic_compare_and_swap_pointer_acq_rel(dst, exc, cmp) _InterlockedCompareExchangePointer((void* volatile*)(dst), (void*)(exc), (void*)(cmp))
    #define atomic_compare_and_swap_pointer_seq_cst(dst, exc, cmp) _InterlockedCompareExchangePointer((void* volatile*)(dst), (void*)(exc), (void*)(cmp))
#endif


// -----------------------------------------------------------------------------------------
// SECTION 4: 128-BIT COMPARE AND SWAP
// Signature: (Destination, ExchangeHigh, ExchangeLow, ComparandResult)
// Note: ComparandResult is a pointer to an array of two int64s. It is IN/OUT.
// Returns: 1 on success, 0 on failure.
// -----------------------------------------------------------------------------------------
#if IS_ARM64
    #define atomic_compare_and_swap_128_rlxd(dst, hi, lo, cmp)    _InterlockedCompareExchange128_nf((volatile __int64*)(dst), (hi), (lo), (cmp))
    #define atomic_compare_and_swap_128_acq(dst, hi, lo, cmp)     _InterlockedCompareExchange128_acq((volatile __int64*)(dst), (hi), (lo), (cmp))
    #define atomic_compare_and_swap_128_rel(dst, hi, lo, cmp)     _InterlockedCompareExchange128_rel((volatile __int64*)(dst), (hi), (lo), (cmp))
    #define atomic_compare_and_swap_128_acq_rel(dst, hi, lo, cmp) _InterlockedCompareExchange128((volatile __int64*)(dst), (hi), (lo), (cmp))
    #define atomic_compare_and_swap_128_seq_cst(dst, hi, lo, cmp) _InterlockedCompareExchange128((volatile __int64*)(dst), (hi), (lo), (cmp))
#else
    // On x64, 'cmpxchg16b' is always implicitly locked.
    #define atomic_compare_and_swap_128_rlxd(dst, hi, lo, cmp)    _InterlockedCompareExchange128((volatile __int64*)(dst), (hi), (lo), (cmp))
    #define atomic_compare_and_swap_128_acq(dst, hi, lo, cmp)     _InterlockedCompareExchange128((volatile __int64*)(dst), (hi), (lo), (cmp))
    #define atomic_compare_and_swap_128_rel(dst, hi, lo, cmp)     _InterlockedCompareExchange128((volatile __int64*)(dst), (hi), (lo), (cmp))
    #define atomic_compare_and_swap_128_acq_rel(dst, hi, lo, cmp) _InterlockedCompareExchange128((volatile __int64*)(dst), (hi), (lo), (cmp))
    #define atomic_compare_and_swap_128_seq_cst(dst, hi, lo, cmp) _InterlockedCompareExchange128((volatile __int64*)(dst), (hi), (lo), (cmp))
#endif

#define PACKED_STRUCT_START __pragma(pack(push, 1))
#define PACKED_STRUCT_END __pragma(pack(pop))


size_t get_filesize(const char *path) {

	HANDLE fh = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, NULL,
		   OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	LARGE_INTEGER file_size; 
	GetFileSizeEx(fh, &file_size);

	CloseHandle(fh);
	return file_size.QuadPart;
}
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


int read_entire_file(const char *path, void *buffer, size_t buffer_size) {
	DWORD result = 0;
	HANDLE fh = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, NULL,
		   OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

	if (fh == INVALID_HANDLE_VALUE) return result;

	LARGE_INTEGER file_size;
	GetFileSizeEx(fh, &file_size);
	assert(file_size.QuadPart <= 0xFFFFFFFF && "File size too large to be read at once");
		
	bool success = ReadFile(fh, buffer, file_size.QuadPart, &result, NULL);

	CloseHandle(fh);
	return success ? result : 0;
}

void memory_map_file_handle_read_only(MemoryMappedFile *mmf, FileHandle handle, uint64_t filesize) 
{
	HANDLE fh = handle.os_handle;

	HANDLE mapping_handle =
		CreateFileMappingA(fh, NULL, PAGE_READONLY,
				   filesize >> 32, filesize & 0x00000000FFFFFFFF, NULL);


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

	uint64_t mapping_size = *(uint64_t *)(&filesize);
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

	memory_map_file_handle_read_only(mmf, FileHandle{fh}, 0);
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

void *mmf_mapping_offset_ptr(MemoryMappedFile *mmf, uint64_t offset) 
{
	if (mmf->filesize < offset) {
		return 0;
	}

	return (char *)mmf->mapping + offset;
}

void mmf_write(MemoryMappedFile *mmf, uint64_t offset, char *data, uint64_t data_size)
{
	if (mmf->mapping && mmf->access == MMFileAccess::WRITE) {
		assert(mmf->mapping_size > offset);
		mmf->filesize = max(offset + data_size, mmf->filesize);
		memcpy_s((uint8_t *)mmf->mapping + offset, mmf->mapping_size - offset, data, data_size);
	}
}

// NOTE(Ray) Can pass in the 0 ptr to this function and will instead just grow the mapping size
void mmf_append(MemoryMappedFile *mmf, void *data, uint64_t data_size)
{
	if (mmf->mapping && mmf->access == MMFileAccess::WRITE) {
		assert(mmf->mapping_size > mmf->filesize + data_size);
		if (data) {
			memcpy_s((uint8_t *)(mmf->mapping) + mmf->filesize,
				 mmf->mapping_size - mmf->filesize, data, data_size);
		}

		mmf->filesize += data_size;
	}
}

uint64_t platform_get_high_res_timer_stamp() 
{
  LARGE_INTEGER timestamp;
  QueryPerformanceCounter(&timestamp);
  return timestamp.QuadPart;
}

uint64_t platform_high_res_timer_freq()
{
  LARGE_INTEGER freq;
  QueryPerformanceFrequency(&freq);
  return freq.QuadPart;
}
