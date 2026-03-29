#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <assert.h>
#include <mach/mach_time.h>

#include "chrono_platform.h"

#define mmf_append_struct(mmf, struct_data) mmf_append(mmf, (void *)(struct_data), sizeof(*struct_data))

#define atomic_fetch_add_s64_rlxd(ptr, val)    __atomic_fetch_add((int64_t*)(ptr), (int64_t)(val), __ATOMIC_RELAXED)
#define atomic_fetch_add_s64_acq(ptr, val)     __atomic_fetch_add((int64_t*)(ptr), (int64_t)(val), __ATOMIC_ACQUIRE)
#define atomic_fetch_add_s64_rel(ptr, val)     __atomic_fetch_add((int64_t*)(ptr), (int64_t)(val), __ATOMIC_RELEASE)
#define atomic_fetch_add_s64_acq_rel(ptr, val) __atomic_fetch_add((int64_t*)(ptr), (int64_t)(val), __ATOMIC_ACQ_REL)
#define atomic_fetch_add_s64_seq_cst(ptr, val) __atomic_fetch_add((int64_t*)(ptr), (int64_t)(val), __ATOMIC_SEQ_CST)

#define atomic_fetch_add_u64_rlxd(ptr, val)    __atomic_fetch_add((uint64_t*)(ptr), (uint64_t)(val), __ATOMIC_RELAXED)
#define atomic_fetch_add_u64_acq(ptr, val)     __atomic_fetch_add((uint64_t*)(ptr), (uint64_t)(val), __ATOMIC_ACQUIRE)
#define atomic_fetch_add_u64_rel(ptr, val)     __atomic_fetch_add((uint64_t*)(ptr), (uint64_t)(val), __ATOMIC_RELEASE)
#define atomic_fetch_add_u64_acq_rel(ptr, val) __atomic_fetch_add((uint64_t*)(ptr), (uint64_t)(val), __ATOMIC_ACQ_REL)
#define atomic_fetch_add_u64_seq_cst(ptr, val) __atomic_fetch_add((uint64_t*)(ptr), (uint64_t)(val), __ATOMIC_SEQ_CST)

#define atomic_compare_and_swap_pointer_internal(dst, exc, cmp, ord) \
({ \
    __typeof__(*(dst)) _old = (cmp); \
    __atomic_compare_exchange_n((dst), &_old, (exc), false, ord, ord); \
    _old; \
})

#define atomic_compare_and_swap_pointer_rlxd(dst, exc, cmp)    atomic_compare_and_swap_pointer_internal((void**)(dst), (void*)(exc), (void*)(cmp), __ATOMIC_RELAXED)
#define atomic_compare_and_swap_pointer_acq(dst, exc, cmp)     atomic_compare_and_swap_pointer_internal((void**)(dst), (void*)(exc), (void*)(cmp), __ATOMIC_ACQUIRE)
#define atomic_compare_and_swap_pointer_rel(dst, exc, cmp)     atomic_compare_and_swap_pointer_internal((void**)(dst), (void*)(exc), (void*)(cmp), __ATOMIC_RELEASE)
#define atomic_compare_and_swap_pointer_acq_rel(dst, exc, cmp) atomic_compare_and_swap_pointer_internal((void**)(dst), (void*)(exc), (void*)(cmp), __ATOMIC_ACQ_REL)
#define atomic_compare_and_swap_pointer_seq_cst(dst, exc, cmp) atomic_compare_and_swap_pointer_internal((void**)(dst), (void*)(exc), (void*)(cmp), __ATOMIC_SEQ_CST)

inline int atomic_compare_and_swap_128_internal(volatile int64_t* dst, int64_t hi, int64_t lo, int64_t* cmp, int mem_ord) {
    __int128 desired = ((__int128)hi << 64) | (uint64_t)lo;
    __int128 expected = ((__int128)cmp[1] << 64) | (uint64_t)cmp[0];
    
    bool success = __atomic_compare_exchange_n((__int128*)dst, &expected, desired, false, mem_ord, mem_ord);
    
    if (!success) {
        cmp[0] = (int64_t)(expected & 0xFFFFFFFFFFFFFFFF);
        cmp[1] = (int64_t)(expected >> 64);
    }
    return success ? 1 : 0;
}

#define atomic_compare_and_swap_128_rlxd(dst, hi, lo, cmp)    atomic_compare_and_swap_128_internal((volatile int64_t *)dst, hi, lo, cmp, __ATOMIC_RELAXED)
#define atomic_compare_and_swap_128_acq(dst, hi, lo, cmp)     atomic_compare_and_swap_128_internal((volatile int64_t *)dst, hi, lo, cmp, __ATOMIC_ACQUIRE)
#define atomic_compare_and_swap_128_rel(dst, hi, lo, cmp)     atomic_compare_and_swap_128_internal((volatile int64_t *)dst, hi, lo, cmp, __ATOMIC_RELEASE)
#define atomic_compare_and_swap_128_acq_rel(dst, hi, lo, cmp) atomic_compare_and_swap_128_internal((volatile int64_t *)dst, hi, lo, cmp, __ATOMIC_ACQ_REL)
#define atomic_compare_and_swap_128_seq_cst(dst, hi, lo, cmp) atomic_compare_and_swap_128_internal((volatile int64_t *)dst, hi, lo, cmp, __ATOMIC_SEQ_CST)

// -----------------------------------------------------------------------------------------
// SECTION 4: STRUCT PACKING
// -----------------------------------------------------------------------------------------

#define PACKED_STRUCT_START 
#define PACKED_STRUCT_END __attribute__((packed))

size_t get_filesize(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) return (size_t)st.st_size;
    return 0;
}

bool create_directory(const char *path) {
    return mkdir(path, 0777) == 0;
}

FileHandle create_file(const char *path) {
    int fd = open(path, O_RDWR | O_CREAT, 0666);
    return (fd == -1) ? FileHandle{} : FileHandle{(void*)(intptr_t)fd};
}

int read_entire_file(const char *path, void *buffer, size_t buffer_size) {
    int fd = open(path, O_RDONLY);
    if (fd == -1) return 0;

    struct stat st;
    fstat(fd, &st);
    assert((uint64_t)st.st_size <= 0xFFFFFFFF && "File size too large to be read at once");
    
    size_t to_read = (st.st_size < (off_t)buffer_size) ? (size_t)st.st_size : buffer_size;
    ssize_t result = read(fd, buffer, to_read);
    
    close(fd);
    return (result > 0) ? (int)result : 0;
}

void memory_map_file_handle_read_only(MemoryMappedFile *mmf, FileHandle handle, uint64_t filesize) 
{
    int fd = (int)(intptr_t)handle.os_handle;

    if (filesize == 0) {
        struct stat st;
        fstat(fd, &st);
        filesize = (int64_t)st.st_size;
    }

    void *file_memory = mmap(NULL, filesize, PROT_READ, MAP_PRIVATE, fd, 0);

    if (file_memory == MAP_FAILED) {
        fprintf(stderr, "Error: Could not mmap file\n");
        return; 
    }

    *mmf = MemoryMappedFile{handle, file_memory, filesize, filesize, MMFileAccess::READ};
}

void memory_map_entire_file_read_only(MemoryMappedFile *mmf, const char *filepath) 
{
    int fd = open(filepath, O_RDONLY);
    if (fd == -1) {
        fprintf(stderr, "Error: Could not open file\n");
        return; 
    }
    memory_map_file_handle_read_only(mmf, FileHandle{(void*)(intptr_t)fd}, 0);
}

void memory_map_file_handle_append(MemoryMappedFile *mmf, FileHandle handle, uint64_t append_size) 
{
    int fd = (int)(intptr_t)handle.os_handle;
    *mmf = {};

    if (fd < 0) {
        fprintf(stderr, "Error: Invalid file handle\n");
        return;
    }

    struct stat st;
    fstat(fd, &st);
    uint64_t current_size = (uint64_t)st.st_size;
    uint64_t mapping_size = current_size + append_size;

    if (ftruncate(fd, mapping_size) != 0) {
        fprintf(stderr, "Error: Could not resize file\n");
        return;
    }

    void *file_memory = mmap(NULL, mapping_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

    if (file_memory == MAP_FAILED) {
        fprintf(stderr, "Error: Could not mmap file for append\n");
        return;
    }

    *mmf = MemoryMappedFile{handle, file_memory, mapping_size, current_size, MMFileAccess::WRITE};
}

void memory_map_entire_file_append(MemoryMappedFile *mmf, const char *filepath, uint64_t append_size) 
{
    *mmf = {};
    int fd = open(filepath, O_RDWR | O_CREAT, 0666);
    
    if (fd == -1) {
        fprintf(stderr, "Error: Could not open file\n");
        return;
    }

    memory_map_file_handle_append(mmf, FileHandle{(void*)(intptr_t)fd}, append_size);
}

void unmap_file(MemoryMappedFile *mmf)
{
    if (mmf->mapping) {
        munmap(mmf->mapping, mmf->mapping_size);
        int fd = (int)(intptr_t)mmf->handle.os_handle;
        
        if (mmf->access == MMFileAccess::WRITE) {
            ftruncate(fd, mmf->filesize);
        }
        
        close(fd);
    }
    *mmf = {};
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
        
        uint64_t new_extent = offset + data_size;
        mmf->filesize = (new_extent > mmf->filesize) ? new_extent : mmf->filesize;
        
        memcpy((uint8_t *)mmf->mapping + offset, data, data_size);
    }
}

void mmf_append(MemoryMappedFile *mmf, void *data, uint64_t data_size)
{
    if (mmf->mapping && mmf->access == MMFileAccess::WRITE) {
        assert(mmf->mapping_size >= mmf->filesize + data_size);
        if (data) {
            memcpy((uint8_t *)(mmf->mapping) + mmf->filesize, data, data_size);
        }
        mmf->filesize += data_size;
    }
}

uint64_t platform_get_high_res_timer_stamp() 
{
    return mach_absolute_time();
}

uint64_t platform_high_res_timer_freq()
{
    static mach_timebase_info_data_t info;
    if (info.denom == 0) {
        mach_timebase_info(&info);
    }
    // Convert Mach timebase (ns per tick) to frequency (ticks per second)
    return (uint64_t)(1000000000ULL * info.denom / info.numer);
}
