#pragma once

#include <stdio.h>
#include <stdint.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <assert.h>

#include "chrono_platform.h"


#define mmf_append_struct(mmf, struct_data) mmf_append(mmf, (void *)(struct_data), sizeof(*struct_data))
#define yield_processor() YieldProcessor()

#if defined(_M_ARM64)
    #define IS_ARM64 1
    #define IS_X64 0
#elif defined(_M_X64) || defined(_M_AMD64)
    #define IS_ARM64 0
    #define IS_X64 1
#endif

#include <intrin.h>
#if IS_ARM64
#define store_release_8(ptr, val)  __strl8((volatile int *__int8)ptr, val)
#define store_release_16(ptr, val) __strl16((volatile int * __int16)ptr, val)
#define store_release_32(ptr, val) __strl32((volatile int * __int32)ptr, val)
#define store_release_64(ptr, val) __strl64((volatile int * __int32)ptr, val)
#define load_acquire_8(ptr, val)   __ldar8((volatile int *__int8)ptr)
#define load_acquire_16(ptr)  __ldar16((volatile int * __int16)ptr)
#define load_acquire_32(ptr)  __ldar32((volatile int * __int32)ptr)
#define load_acquire_64(ptr)  __ldar64((volatile int * __int32)ptr)
#else
#define store_release_8(ptr, val)  (*ptr = val)
#define store_release_16(ptr, val) (*ptr = val)
#define store_release_32(ptr, val) (*ptr = val)
#define store_release_64(ptr, val) (*ptr = val)
#define load_acquire_8(ptr)   (*ptr)
#define load_acquire_16(ptr)  (*ptr)
#define load_acquire_32(ptr)  (*ptr)
#define load_acquire_64(ptr)  (*ptr)
#endif



#if IS_ARM64
    #define atomic_fetch_add_s64_rlxd(ptr, val)    _InterlockedExchangeAdd64_nf((volatile __int64*)(ptr), (val))
    #define atomic_fetch_add_s64_acq(ptr, val)     _InterlockedExchangeAdd64_acq((volatile __int64*)(ptr), (val))
    #define atomic_fetch_add_s64_rel(ptr, val)     _InterlockedExchangeAdd64_rel((volatile __int64*)(ptr), (val))
    #define atomic_fetch_add_s64_acq_rel(ptr, val) _InterlockedExchangeAdd64((volatile __int64*)(ptr), (val)) 
    #define atomic_fetch_add_s64_seq_cst(ptr, val) _InterlockedExchangeAdd64((volatile __int64*)(ptr), (val))
#else
    #define atomic_fetch_add_s64_rlxd(ptr, val)    InterlockedExchangeAdd64((volatile __int64*)(ptr), (val))
    #define atomic_fetch_add_s64_acq(ptr, val)     InterlockedExchangeAdd64((volatile __int64*)(ptr), (val))
    #define atomic_fetch_add_s64_rel(ptr, val)     InterlockedExchangeAdd64((volatile __int64*)(ptr), (val))
    #define atomic_fetch_add_s64_acq_rel(ptr, val) InterlockedExchangeAdd64((volatile __int64*)(ptr), (val))
    #define atomic_fetch_add_s64_seq_cst(ptr, val) InterlockedExchangeAdd64((volatile __int64*)(ptr), (val))
#endif

#if IS_ARM64
    #define atomic_fetch_add_u64_rlxd(ptr, val)    (uint64_t)_InterlockedExchangeAdd64_nf((volatile __int64*)(ptr), (__int64)(val))
    #define atomic_fetch_add_u64_acq(ptr, val)     (uint64_t)_InterlockedExchangeAdd64_acq((volatile __int64*)(ptr), (__int64)(val))
    #define atomic_fetch_add_u64_rel(ptr, val)     (uint64_t)_InterlockedExchangeAdd64_rel((volatile __int64*)(ptr), (__int64)(val))
    #define atomic_fetch_add_u64_acq_rel(ptr, val) (uint64_t)_InterlockedExchangeAdd64((volatile __int64*)(ptr), (__int64)(val))
    #define atomic_fetch_add_u64_seq_cst(ptr, val) (uint64_t)_InterlockedExchangeAdd64((volatile __int64*)(ptr), (__int64)(val))
#else
    #define atomic_fetch_add_u64_rlxd(ptr, val)    (uint64_t)InterlockedExchangeAdd64((volatile __int64*)(ptr), (__int64)(val))
    #define atomic_fetch_add_u64_acq(ptr, val)     (uint64_t)InterlockedExchangeAdd64((volatile __int64*)(ptr), (__int64)(val))
    #define atomic_fetch_add_u64_rel(ptr, val)     (uint64_t)InterlockedExchangeAdd64((volatile __int64*)(ptr), (__int64)(val))
    #define atomic_fetch_add_u64_acq_rel(ptr, val) (uint64_t)InterlockedExchangeAdd64((volatile __int64*)(ptr), (__int64)(val))
    #define atomic_fetch_add_u64_seq_cst(ptr, val) (uint64_t)InterlockedExchangeAdd64((volatile __int64*)(ptr), (__int64)(val))
#endif

#if IS_ARM64
    #define atomic_compare_and_swap_pointer_rlxd(dst, exc, cmp)    _InterlockedCompareExchangePointer_nf((void* volatile*)(dst), (void*)(exc), (void*)(cmp))
    #define atomic_compare_and_swap_pointer_acq(dst, exc, cmp)     _InterlockedCompareExchangePointer_acq((void* volatile*)(dst), (void*)(exc), (void*)(cmp))
    #define atomic_compare_and_swap_pointer_rel(dst, exc, cmp)     _InterlockedCompareExchangePointer_rel((void* volatile*)(dst), (void*)(exc), (void*)(cmp))
    #define atomic_compare_and_swap_pointer_acq_rel(dst, exc, cmp) _InterlockedCompareExchangePointer((void* volatile*)(dst), (void*)(exc), (void*)(cmp))
    #define atomic_compare_and_swap_pointer_seq_cst(dst, exc, cmp) _InterlockedCompareExchangePointer((void* volatile*)(dst), (void*)(exc), (void*)(cmp))
#else
    #define atomic_compare_and_swap_pointer_rlxd(dst, exc, cmp)    InterlockedCompareExchangePointer((void* volatile*)(dst), (void*)(exc), (void*)(cmp))
    #define atomic_compare_and_swap_pointer_acq(dst, exc, cmp)     InterlockedCompareExchangePointer((void* volatile*)(dst), (void*)(exc), (void*)(cmp))
    #define atomic_compare_and_swap_pointer_rel(dst, exc, cmp)     InterlockedCompareExchangePointer((void* volatile*)(dst), (void*)(exc), (void*)(cmp))
    #define atomic_compare_and_swap_pointer_acq_rel(dst, exc, cmp) InterlockedCompareExchangePointer((void* volatile*)(dst), (void*)(exc), (void*)(cmp))
    #define atomic_compare_and_swap_pointer_seq_cst(dst, exc, cmp) InterlockedCompareExchangePointer((void* volatile*)(dst), (void*)(exc), (void*)(cmp))
#endif


// -----------------------------------------------------------------------------------------
// SECTION 4: 128-BIT COMPARE AND SWAP
// Signature: (Destination, ExchangeHigh, ExchangeLow, ComparandResult)
// Note: ComparandResult is a pointer to an array of two int64s. It is IN/OUT.
// Returns: 1 on success, 0 on failure.
// -----------------------------------------------------------------------------------------
#if IS_ARM64
    #define atomic_compare_and_swap_128_rlxd(dst, hi, lo, cmp)    _InterlockedCompareExchange128_nf((volatile __int64*)(dst), (hi), (lo), (__int64 *)(cmp))
    #define atomic_compare_and_swap_128_acq(dst, hi, lo, cmp)     _InterlockedCompareExchange128_acq((volatile __int64*)(dst), (hi), (lo), (__int64 *)(cmp))
    #define atomic_compare_and_swap_128_rel(dst, hi, lo, cmp)     _InterlockedCompareExchange128_rel((volatile __int64*)(dst), (hi), (lo), (__int64 *)(cmp))
    #define atomic_compare_and_swap_128_acq_rel(dst, hi, lo, cmp) _InterlockedCompareExchange128((volatile __int64*)(dst), (hi), (lo), (__int64 *)(cmp)) #define atomic_compare_and_swap_128_seq_cst(dst, hi, lo, cmp) _InterlockedCompareExchange128((volatile __int64*)(dst), (hi), (lo), (cmp))
#else
    // On x64, 'cmpxchg16b' is always implicitly locked.
    #define atomic_compare_and_swap_128_rlxd(dst, hi, lo, cmp)    InterlockedCompareExchange128((volatile __int64*)(dst), (hi), (lo), (__int64 *)(cmp))
    #define atomic_compare_and_swap_128_acq(dst, hi, lo, cmp)     InterlockedCompareExchange128((volatile __int64*)(dst), (hi), (lo), (__int64 *)(cmp))
    #define atomic_compare_and_swap_128_rel(dst, hi, lo, cmp)     InterlockedCompareExchange128((volatile __int64*)(dst), (hi), (lo), (__int64 *)(cmp))
    #define atomic_compare_and_swap_128_acq_rel(dst, hi, lo, cmp) InterlockedCompareExchange128((volatile __int64*)(dst), (hi), (lo), (__int64 *)(cmp))
    #define atomic_compare_and_swap_128_seq_cst(dst, hi, lo, cmp) InterlockedCompareExchange128((volatile __int64*)(dst), (hi), (lo), (__int64 *)(cmp))
#endif

#define PACKED_STRUCT_START __pragma(pack(push, 1))
#define PACKED_STRUCT_END __pragma(pack(pop))

void set_end_of_file(OSHandle file_handle)
{
  SetEndOfFile(file_handle.os_handle);
}

int32_t set_file_pointer_from_start(OSHandle file_handle, int64_t offset)
{
  LONG offset_high = offset >> 32;
  auto new_pointer_low = SetFilePointer(file_handle.os_handle, offset & 0xFFFFFFFF, &offset_high, FILE_BEGIN);
  return (new_pointer_low & (offset_high << 31));
}

int64_t get_filesize(const char *path) {

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


OSHandle create_file(const char *path, bool is_async)
{
  
  DWORD flags = is_async ? FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED : FILE_ATTRIBUTE_NORMAL;
	HANDLE fh = CreateFile(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
		   OPEN_ALWAYS, flags, NULL);

	return fh == INVALID_HANDLE_VALUE ? OSHandle{false, (void *)GetLastError()} : OSHandle{true, fh};
}

OSHandle create_file_direct_asio(const char *path)
{
  
	HANDLE fh = CreateFile(path, GENERIC_WRITE | GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
		   OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED | FILE_FLAG_NO_BUFFERING, NULL);

	return fh == INVALID_HANDLE_VALUE ? OSHandle{false, (void *)GetLastError()} : OSHandle{true, fh};
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



void memory_map_file_handle_read_only(MemoryMappedFile *mmf, OSHandle handle, int64_t offset, int64_t size) 
{
	HANDLE fh = handle.os_handle;

	HANDLE mapping_handle =
		CreateFileMappingA(fh, NULL, PAGE_READONLY,
				   size >> 32, size & 0xFFFFFFF, NULL);


	if (mapping_handle == NULL) {
		fprintf(stderr, "Error: Could not create file mapping\n");
		CloseHandle(fh);
		return; 
	}

	void *file_memory = MapViewOfFile(
		mapping_handle,
		FILE_MAP_READ,
		offset >> 32, offset & 0xFFFFFFFF, size);

	CloseHandle(mapping_handle);
	*mmf = MemoryMappedFile{handle, file_memory, offset, offset, MMFileAccess::READ};
}

void memory_map_entire_file_read_only(MemoryMappedFile *mmf, const char *filepath) 
{
  auto filesize = get_filesize(filepath);
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

	memory_map_file_handle_read_only(mmf, OSHandle{true, fh}, 0, filesize);
}


void memory_map_file_handle_append(MemoryMappedFile *mmf, OSHandle handle, int64_t append_size) 
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

	auto mapping_size = file_size.QuadPart + append_size;
	HANDLE mapping_handle =
		CreateFileMappingA(fh, NULL, PAGE_READWRITE,
				   mapping_size >> 32, mapping_size & 0x00000000FFFFFFFF, NULL);


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

	*mmf = MemoryMappedFile{handle, file_memory, mapping_size, *(int64_t *)(&file_size.QuadPart), MMFileAccess::WRITE};
}

void memory_map_file_handle_read_write(MemoryMappedFile *mmf, OSHandle file_handle, int64_t offset, int64_t size) 
{
	HANDLE fh = file_handle.os_handle;
	*mmf = {};

	if (!fh) {
		fprintf(stderr, "Error: Could not open file\n");
		return;
	}

	HANDLE mapping_handle =
		CreateFileMappingA(fh, NULL, PAGE_READWRITE,
				   size >> 32, size & 0x00000000FFFFFFFF, NULL);


	if (mapping_handle == NULL) {
		fprintf(stderr, "Error: Could not create file mapping\n");
		CloseHandle(fh);
		return;
	}

	void *file_memory = MapViewOfFile(
		mapping_handle,
		FILE_MAP_READ | FILE_MAP_WRITE,
		offset >> 32, offset & 0xFFFFFFFF, size);

	CloseHandle(mapping_handle);

	*mmf = MemoryMappedFile{file_handle, file_memory, size, size, MMFileAccess::WRITE};
}


void memory_map_entire_file_append(MemoryMappedFile *mmf, const char *filepath, int64_t append_size) 
{
	*mmf = {};
	HANDLE fh = CreateFileA(filepath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_WRITE, NULL,
				OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

	if (fh == INVALID_HANDLE_VALUE) {
		fprintf(stderr, "Error: Could not open file\n");
		return;
	}

	memory_map_file_handle_append(mmf, OSHandle{true, fh}, append_size);
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

void *mmf_mapping_offset_ptr(MemoryMappedFile *mmf, int64_t offset) 
{
  assert(offset > 0);
	if (mmf->filesize < offset) {
		return 0;
	}

	return (char *)mmf->mapping + offset;
}

void mmf_write(MemoryMappedFile *mmf, int64_t offset, char *data, uint64_t data_size)
{
  assert(offset > 0);
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

enum class SubmissionQueueOPS: uint32_t {
  READ,
  WRITE,
};
struct SubmissionQueueEntry {
  uint64_t user_data;
  OSHandle file_handle;
  OVERLAPPED overlapped;
  void *buffer;
  uint64_t buffer_size;
  uint64_t offset;
  SubmissionQueueOPS op_type;
  bool active;
};

typedef SubmissionQueueEntry CompletionQueueEntry;

struct SubmissionQueue {
  SubmissionQueueEntry *entries;
  ULONGLONG tail;
  LONGLONG capacity;
};

typedef SubmissionQueue CompletionQueue;

struct ASIOContext {
  SubmissionQueue *submission_queue;
  CompletionQueue *completion_queue;
};

typedef FILE_SEGMENT_ELEMENT BufferInfo;

void platform_asio_create(ASIOContext *context, uint32_t max_threads = 1, size_t queue_size = 4096) {
  auto queue_memory =
    VirtualAlloc(0, sizeof(SubmissionQueue) + queue_size * sizeof(SubmissionQueueEntry), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);

  context->submission_queue = (SubmissionQueue *)queue_memory;
  context->completion_queue = (SubmissionQueue *)queue_memory;
  context->submission_queue->entries = (SubmissionQueueEntry *)((uint8_t *)queue_memory + sizeof(SubmissionQueue));
  context->submission_queue->capacity = queue_size;
  context->completion_queue->capacity = queue_size;

  if (!queue_memory) {
    fprintf(stderr, "Failed to allocate enough memory for asio queues\n");
    *context = {};
  }

	/* NOTE(Ray) 
   *
   * The reason these are the same is because we are doing manual polling of the WriteFile calls
   * We can iterate the list and just see which ones are completed in userspace. The reason the completion queue exists
   * is to provide an abstraction over IORING which uses a completion queue to handle finished IOops this allows the code to 
   * implement an IORING backend for Win11 and io_uring for linux.
   *
  */

}

inline uint64_t _platform_get_asio_queue_slot_idx(uint64_t counting_index, uint64_t capacity) {
  return counting_index & (capacity - 1);
}

bool platform_asio_submit_write(ASIOContext *context, OSHandle file_handle, uint64_t file_offset, void *buffer,
																size_t buffer_size, uint64_t user_data)
{
  assert(buffer_size % 4096 == 0); // must be a multiple of sector size
  assert((uintptr_t)buffer % 4096  == 0);
  assert(file_offset % 4096 == 0);
	DWORD bytes_written = 0;

  auto &submission_queue = context->submission_queue;
  auto queue_slot = &submission_queue->entries[_platform_get_asio_queue_slot_idx(submission_queue->tail, submission_queue->capacity)];

	if (queue_slot->active) {
		return false;
	}

  queue_slot->op_type = SubmissionQueueOPS::WRITE;
  queue_slot->overlapped.Offset = file_offset & 0x00000000FFFFFFFF;
  queue_slot->overlapped.OffsetHigh = (file_offset) >> 32;
  queue_slot->user_data = user_data;
  queue_slot->file_handle = file_handle;
  queue_slot->buffer_size = buffer_size;
  queue_slot->buffer = buffer;
  queue_slot->offset = file_offset;
  queue_slot->active = true;

  auto result = WriteFile(
    (HANDLE)file_handle.os_handle,
    buffer,
    buffer_size,
    nullptr,
    &queue_slot->overlapped
  );
  ++submission_queue->tail;

  return true;
}

int64_t platform_asio_get_buffer_info_entry_count_from_buffer_size(size_t buffer_size) 
{
  assert(buffer_size % 4096 == 0);
  return buffer_size / 4096;
}
void platform_asio_fill_multiple_buffer_info(BufferInfo *info, void *buffer, size_t count) 
{
  for (auto i = 0; i < count; ++i) {
    auto buffer_ptr = (uint8_t *)buffer + 4096 * i;
    info[i].Buffer = buffer_ptr;
  }
}

void **platform_asio_get_buffer_from_info(BufferInfo *info) 
{
  return &info->Buffer;
}


bool platform_asio_submit_write_buffer_info_array(ASIOContext *context, OSHandle file_handle, uint64_t file_offset,
																									BufferInfo *buffer_array, size_t total_bytes_to_write,
																									uint64_t user_data)
{
	assert(file_offset % 4096 == 0);
  assert(total_bytes_to_write % 4096 == 0); // must be a multiple of sector size
	DWORD bytes_written = 0;

	auto &submission_queue = context->submission_queue;

  auto queue_slot = &submission_queue->entries[_platform_get_asio_queue_slot_idx(submission_queue->tail, submission_queue->capacity)];
	if (queue_slot->active) {
		return false;
	}

  queue_slot->op_type = SubmissionQueueOPS::WRITE;
	queue_slot->overlapped.Offset = file_offset & 0x00000000FFFFFFFF;
	queue_slot->overlapped.OffsetHigh = (file_offset) >> 32;
	queue_slot->user_data = user_data;
	queue_slot->file_handle = file_handle;
	queue_slot->buffer_size = total_bytes_to_write;
	queue_slot->buffer = buffer_array;
	queue_slot->offset = file_offset;
	queue_slot->active = true;

	auto result = WriteFileGather((HANDLE)file_handle.os_handle, (FILE_SEGMENT_ELEMENT *)buffer_array,
																total_bytes_to_write, NULL, &queue_slot->overlapped);
	auto err = GetLastError();
	if (err != ERROR_IO_PENDING) {
    fprintf(stderr, "Failed to submit batched IO operation check the disk is not full\n");
		return false;
	}

	return true;
  ++submission_queue->tail;
}

inline CompletionQueueEntry *platform_asio_completed_entry_dequeue(ASIOContext *context) {
  auto &completion_queue = context->completion_queue;
	for (auto i = 0; i < completion_queue->capacity; ++i) {
		auto queue_slot = &completion_queue->entries[i];
    auto bytes_transfered = 0;
    if (queue_slot->active && HasOverlappedIoCompleted(&queue_slot->overlapped)) {
      queue_slot->active = false;
      return queue_slot;
    }
	}

  return nullptr;
}

bool platform_asio_submit_read(ASIOContext *context, OSHandle file_handle, uint64_t file_offset, size_t bytes_to_read, void *buffer, size_t buffer_size, uint64_t user_data) 
{ 
  assert(buffer_size % 4096 == 0); // must be a multiple of sector size
  assert((uintptr_t)buffer % 4096  == 0);
  assert(file_offset % 4096 == 0);
	DWORD bytes_written = 0;

  auto &submission_queue = context->submission_queue;
  auto queue_slot = &submission_queue->entries[_platform_get_asio_queue_slot_idx(submission_queue->tail, submission_queue->capacity)];

	if (queue_slot->active) {
		return false;
	}

  queue_slot->overlapped.Offset = file_offset & 0x00000000FFFFFFFF;
  queue_slot->overlapped.OffsetHigh = (file_offset) >> 32;
  queue_slot->user_data = user_data;
  queue_slot->file_handle = file_handle;
  queue_slot->buffer_size = buffer_size;
  queue_slot->buffer = buffer;
  queue_slot->offset = file_offset;
  queue_slot->active = true;

  auto result = ReadFile(
    (HANDLE)file_handle.os_handle,
    buffer,
    bytes_to_read,
    NULL,
    &queue_slot->overlapped
  );

	auto err = GetLastError();
	if (err != ERROR_IO_PENDING) {
    fprintf(stderr, "Failed to submit batched IO operation check the disk is not full\n");
		return false;
	}

  ++submission_queue->tail;

  return true;
}

uint64_t platform_asio_completed_entry_get_bytes_transfered(CompletionQueueEntry *entry) {
  DWORD bytes_transfered = 0;
  GetOverlappedResult(entry->file_handle.os_handle, &entry->overlapped, &bytes_transfered,
                      FALSE); 

  return bytes_transfered;
}

void *page_allocator_commit(void *address, size_t bytes) 
{
  return VirtualAlloc(address, bytes, MEM_COMMIT, PAGE_READWRITE);
}

void *page_allocator_alloc(size_t bytes) 
{
  return VirtualAlloc(0, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}

void *page_allocator_reserve(size_t bytes) 
{
  return VirtualAlloc(0, bytes, MEM_RESERVE, PAGE_READWRITE);
}
