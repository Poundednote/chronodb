#pragma once

#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <concepts>
#include <type_traits>
#include <functional>
#include <cstddef>
#include <new>
#include <string>
#include <algorithm>

#include "chrono_platform.cpp"

#define DEFAULT_ARENA_SIZE (4096)

#define KILOBYTES(n) (1024ull * n)
#define MEGABYTES(n) (1024ull * KILOBYTES(n))
#define GIGABYTES(n) (1024ull * MEGABYTES(n))
#define arraycount(arr) (sizeof(arr) / sizeof(arr[0]))
#define REQUEST_POOL_CHUNK_SIZE (MEGABYTES(3))

#include <algorithm> 

template <typename T>
struct DynamicArray {
    T* data;
    size_t size;
    size_t capacity;

    T& operator[](size_t index) {
        return data[index];
    }
};

typedef void *(AllocFunction)(size_t);
template <typename T>
void dynamic_array_init(DynamicArray<T> *da, AllocFunction f, size_t capacity = 4) {
	da = {};
	da->data = f(capacity);
	da->capacity = capacity;
}

template <typename T>
void dynamic_array_push(DynamicArray<T> *da, const T& item) {
	da->data[da->size] = item;
	da->size += sizeof(T);
}

#define string8_to_cstring(string8) ((const char *)string8.content)


struct Arena {
	void *memory;
	volatile int64_t size;
	uint64_t capacity;
};

void arena_init(Arena *a, uint32_t capacity)
{
	a->size = 0;
	a->capacity = capacity;

	a->memory = page_allocator_alloc(capacity);
	assert(a->memory != nullptr);
}

void arena_clear(Arena *a)
{
	a->size = 0;
}

void arena_destroy(Arena *a)
{
	free(a->memory);
}

void *arena_alloc(Arena *a, uint64_t size, uint64_t alignment = 8)
{
	assert(a->memory != nullptr);
  size_t aligned_offset = (a->size + (alignment - 1)) & ~(alignment - 1);
  void *ptr = (uint8_t *)a->memory + aligned_offset;

  assert(aligned_offset + size <= a->capacity);
  assert(aligned_offset % alignment == 0);
	a->size = aligned_offset + size;
	return ptr;
}

/*
void *arena_atomic_alloc(Arena *a, uint64_t size, uint64_t alignment) {
	assert(a->memory != nullptr);
	assert(a->size + size < a->capacity);

  size_t aligned_offset = (a->size + (alignment - 1)) & ~(alignment - 1);
  void *ptr = (uint8_t *)a->memory + aligned_offset;
	a->size = aligned_offset + size;
	uint64_t old_size = atomic_fetch_add_u64_rlxd(a->size, size);  
	void *ptr = (char *)a->memory + old_size;
	return ptr;
}
*/

void *arena_alloc_zero(Arena *a, uint64_t size, uint64_t alignment) 
{
  auto ptr = arena_alloc(a, size, alignment);
  std::memset(ptr, 0, size);
  return ptr;
}

#define arena_alloc_struct(a, struct) (struct *)arena_alloc(a, sizeof(struct), alignof(struct))
#define arena_alloc_struct_array(a, struct, n) (struct *)arena_alloc(a, sizeof(struct) * (n), alignof(struct))

#define arena_atomic_alloc_struct(a, struct) (struct *)arena_atomic_alloc(a, sizeof(struct), alignof(struct))
#define arena_atomic_alloc_struct_array(a, struct, n) (struct *)arena_atomic_alloc(a, sizeof(struct) * n)
template <typename T>
/* NOTE(Ray):
 * This pool allocator uses an SPSC free list array to track free entries instead of the more traditional linked list approach
 * The array is initiliezed with pointers to the blocks and they are popped from the head. Its a queue so free entries just get added to the end
 * in whatever order they get deallocated. This is used in the ingestion worker and writer thread to reduce MESI contention
 * on the single head of the traditional pool allocators. Instead we have bassically 0 contention 
 * because head and tail are on seperate lines we use a shadow tail to make sure we only check the queue is empty every once 
 * in a while
 *
 *
 *
*/
struct PoolAllocatorSPSCFreeList {
  alignas(std::hardware_destructive_interference_size) std::atomic<uint64_t> head;
  alignas(std::hardware_destructive_interference_size) std::atomic<uint64_t> shadow_tail;
  alignas(std::hardware_destructive_interference_size) std::atomic<uint64_t> tail;
  T **free_list_entries;
  uint64_t block_count;
  void *memory;
};

template <typename T>
void pool_init(PoolAllocatorSPSCFreeList<T> *p, size_t block_count, void *memory, size_t memory_size) 
{

	assert((sizeof(T) * block_count) + (sizeof(void *) * block_count) <= memory_size);
  assert(((block_count - 1) & block_count) == 0); // make sure pow 2 size
  p->block_count = block_count;
  p->memory = memory;
  p->free_list_entries = (T **)((uint8_t *)memory + (sizeof(T) * block_count));
	for (auto i = 0; i < block_count; ++i) {
    T *memory_block = (T *)((uint8_t *)p->memory + (sizeof(T) * i));
    p->free_list_entries[i] = memory_block;
	}

  p->shadow_tail = block_count;
  p->tail.store(block_count, std::memory_order::release);

}


template <typename T>
void pool_init(PoolAllocatorSPSCFreeList<T> *p, size_t block_count, size_t alignment = 8) 
{
	size_t memory_size = (sizeof(T) * block_count) + (sizeof(void *) * block_count);
	void *memory = page_allocator_alloc(memory_size);

	pool_init(p, block_count, memory, memory_size);
}

template <typename T>
void pool_init(PoolAllocatorSPSCFreeList<T> *p, Arena *a, size_t block_count, size_t alignment = 8) 
{
	size_t memory_size = (sizeof(T) * block_count) + (sizeof(void *) * block_count);
	auto memory = arena_alloc(a, memory_size, alignment);
  pool_init(p, block_count, memory, memory_size);
}


template <typename T>
T *pool_atomic_alloc(PoolAllocatorSPSCFreeList<T> *p, bool zeroed = true) 
{
  auto head = p->head.load(std::memory_order::relaxed);
  if (head == p->shadow_tail) {
    p->shadow_tail = p->tail.load(std::memory_order::acquire);
    if (p->shadow_tail == head) {
      return nullptr;
    }
  }

  auto idx = (head & (p->block_count - 1));
  auto ptr = p->free_list_entries[idx];
  p->free_list_entries[idx] = (T *)0xFFFFFFFFFFFFFFFF;

  p->head.fetch_add(1, std::memory_order::release);
  if (zeroed) {std::memset(ptr, 0, sizeof(T));}
  return ptr;
}

template <typename T>
void pool_atomic_dealloc(PoolAllocatorSPSCFreeList<T> *p, void *ptr)
{
  auto tail = p->tail.load(std::memory_order::relaxed);
  auto head = p->head.load(std::memory_order::acquire);
  assert(tail - head < p->block_count);

  auto idx = (tail & (p->block_count - 1));
  assert(p->free_list_entries[idx] == (T *)0xFFFFFFFFFFFFFFFF);
  p->free_list_entries[idx] = (T *)ptr;


  p->tail.fetch_add(1, std::memory_order::release);
}

struct PoolAllocatorFreeListNode {
	PoolAllocatorFreeListNode *next;
  uint32_t sig;
};

template <typename T>
struct PoolAllocator {
	alignas(16) PoolAllocatorFreeListNode *head;
	int64_t generation;
	void *memory;
};

template <typename T>
void pool_init(PoolAllocator<T> *p, size_t block_count, void *memory, size_t memory_size) {

	auto chunk_size = std::max(sizeof(T), sizeof(PoolAllocatorFreeListNode *));
	assert((chunk_size * block_count) <= memory_size);
	p->memory = memory;

	auto free_list_node = (PoolAllocatorFreeListNode *)p->memory;
	p->head = free_list_node;
  free_list_node->sig = 0xFEFEFEFE;
	for (auto i = 1; i < block_count - 1; ++i) {
		free_list_node->next = (PoolAllocatorFreeListNode *)((uint8_t *)p->memory + chunk_size * i);
		free_list_node = free_list_node->next;
    free_list_node->sig = 0xFEFEFEFE;
	}

	free_list_node->next = nullptr;
	
}


template <typename T>
void pool_init(PoolAllocator<T> *p, size_t block_count) {
	size_t memory_size = (std::max(sizeof(T), sizeof(PoolAllocatorFreeListNode))) * block_count;
	void *memory = page_allocator_alloc(memory_size);

	pool_init(p, block_count, memory, memory_size);
}

template <typename T>
void pool_init(PoolAllocator<T> *p, Arena *a, size_t block_count) {
	size_t memory_size = std::max(sizeof(T), sizeof(PoolAllocatorFreeListNode)) * block_count;
	assert((memory_size) <= a->capacity - a->size);
	auto memory = arena_alloc(a, memory_size);
  pool_init(p, block_count, memory, memory_size);
}

template <typename T>
T *pool_alloc(PoolAllocator<T> *p, bool zeroed=true)
{
	if (!p->head)
		return nullptr;

	PoolAllocatorFreeListNode *block = p->head;
	p->head = p->head->next;
	p->generation++;

 // memset the thing here i've been screwed over by stale data before 
  if (zeroed) { std::memset(block, 0, sizeof(T)); }

	return (T *)block;
}

template <typename T>
__declspec(noinline) T *pool_atomic_alloc(PoolAllocator<T> *p, bool zeroed=true) {

  /* NOTE(Ray)
    * The compiler only requires 8 bit alignment for these array fields, but cmpxchng16b requires 
    * 16 byte alignment so need to manually align
  */

	alignas(16) uint64_t block[2] = {};
	// need to make sure we do an atomic 128 load of both values 
	atomic_compare_and_swap_128_rlxd(&p->head, 0,  0, block);
	for (;;) {
		PoolAllocatorFreeListNode* node_ptr = reinterpret_cast<PoolAllocatorFreeListNode*>(block[0]);
		if (!node_ptr) {
			return nullptr;
		}

		alignas(16) uint64_t next_head[2];
		next_head[0] = reinterpret_cast<uint64_t>(node_ptr->next);
		next_head[1] = block[1] + 1;

		if (atomic_compare_and_swap_128_acq_rel(&p->head, next_head[1], next_head[0], block)) {
      node_ptr->sig = 0;
      if (zeroed) {std::memset(node_ptr, 0, sizeof(T));}
			return (T *)node_ptr; 
		} 
	}
}

template <typename T>
void pool_dealloc(PoolAllocator<T> *p, void *ptr)
{
	auto block = (PoolAllocatorFreeListNode *)ptr;
	block->next = p->head;
	p->head = block;
}

template <typename T>
void pool_atomic_dealloc(PoolAllocator<T> *p, void *ptr) {
	auto node_to_free = reinterpret_cast<PoolAllocatorFreeListNode*>(ptr);

	alignas(16) uint64_t expected_head[2] = {};
	atomic_compare_and_swap_128_rlxd(&p->head, 0,  0, expected_head);
	for (;;) {
		node_to_free->next = reinterpret_cast<PoolAllocatorFreeListNode*>(expected_head[0]);

		alignas(16) uint64_t block[2];
		block[0] = reinterpret_cast<uint64_t>(ptr);
		block[1] = expected_head[1] + 1;
    if (node_to_free->sig == 0xFEFEFEFE) {
      assert(false);
      __debugbreak();
    }


		if (atomic_compare_and_swap_128_acq_rel(&p->head, block[1],
						    block[0], expected_head)) {
      node_to_free->sig = 0xFEFEFEFE;
			return;
		}
	}
}

struct String8;
struct StringSlice8;
struct StringBuilder8;

struct String8 {
	const uint8_t *content;
	int64_t length;

	bool operator==(String8 &rhs);
	bool operator==(const char *rhs);
	bool operator!=(const char *rhs);
	bool operator!=(String8 &rhs);
	uint8_t operator[](int rhs);
};

struct StringSlice8 {
	uint8_t *content;
	int64_t length;
	
	StringSlice8() : content(0), length(0) {};
	StringSlice8(String8 s) : content(const_cast<uint8_t *>(s.content)), length(s.length) {};
	StringSlice8(uint8_t *content, int64_t length) : content(content), length(length) {}
	StringSlice8(char *content, int64_t length) : content((uint8_t *)content), length(length) {}
	bool operator==(const StringSlice8 &rhs) const;
	bool operator==(const StringBuilder8 &rhs) const;
	bool operator==(const char *rhs) const;
	uint8_t &operator[](int rhs);
};

struct StringBuilder8 {
	uint8_t *content;
	int64_t length;
	int64_t capacity;

	const bool operator==(const StringSlice8 rhs);
	const bool operator==(const StringBuilder8 rhs);
	const bool operator==(const char *rhs);
};

template <typename T> int64_t __string_to_int_template(T s);

#define string8_from_cstring(cstring) \
	(String8{ (uint8_t *)(cstring), sizeof(cstring) - 1 })
#define string8_from_std_string(string) \
	(String8{ (uint8_t *)(string.c_str()), (int64_t)string.length()})
#define str_view_from_slice(slice) \
	(std::string_view{(const char *)slice.content, (size_t)slice.length})
StringSlice8 string8_view_after_match_end(String8 string, String8 match_string);
StringSlice8 inline string8_view_from_match_end(String8 string,
						String8 match_string);
String8 string8_concat(Arena *a, String8 s1, String8 s2);
int64_t string_to_int(String8 s);

void string_builder8_append(StringBuilder8 *sb, String8 string);
String8 string_builder_to_string8(StringBuilder8 *sb);

template <typename T> int64_t __string_to_int_template(T s)
{
	int64_t accum = 0;
	int minus_multiplier = 1;
	for (int i = 0; i < s.length; ++i) {
		uint16_t digit = s[i];

		if (i == 0 && digit == '-') {
			minus_multiplier = -1;
		} else if (digit >= '0' && digit <= '9') {
			accum = accum * 10 + (digit - '0');
		} else {
			return 0;
		}
	}

	return accum * minus_multiplier;
}

template <typename T>
T __string_slice_length_template(T s, int64_t start_index, int64_t length = 0)
{
	assert(start_index < s.length);
	T result = s;
	result.content = s.content + start_index;

	assert(length <= s.length - start_index);
	if (length != 0) {
		result.length = length;
	} else {
		result.length -= start_index;
	}

	return result;
}

// TODO(Ray): maybe simdizeee
bool String8::operator==(String8 &rhs)
{
	if (this->length != rhs.length) {
		return false;
	}

	for (int i = 0; i < this->length; ++i) {
		if (this->content[i] != rhs.content[i]) {
			return false;
		}
	}

	return true;
}

bool String8::operator==(const char *rhs)
{
	const char *character = rhs;
	int index = 0;
	while (*character != '\0') {
		if (index >= this->length) {
			return false;
		}

		if (this->content[index] != *character) {
			return false;
		}

		++character;
		++index;
	}

	if (*character != '\0')
		return false;

	return true;
}

bool String8::operator!=(const char *rhs)
{
	return !(*this == rhs);
}

bool String8::operator!=(String8 &rhs)
{
	return !(*this == rhs);
}

uint8_t String8::operator[](int rhs)
{
	assert(this->length > rhs);
	return this->content[rhs];
}

bool StringSlice8::operator==(const StringSlice8 &rhs) const
{
	if (this->length != rhs.length) {
		return false;
	}

	for (int i = 0; i < this->length; ++i) {
		if (this->content[i] != rhs.content[i]) {
			return false;
		}
	}

	return true;
}

bool StringSlice8::operator==(const StringBuilder8 &rhs) const
{
	if (this->length != rhs.length) {
		return false;
	}

	for (int i = 0; i < this->length; ++i) {
		if (this->content[i] != rhs.content[i]) {
			return false;
		}
	}

	return true;
}

bool StringSlice8::operator==(const char *rhs) const 
{
	const char *character = rhs;
	int index = 0;
	while (*character != '\0') {
		if (index >= this->length) {
			return false;
		}

		if (this->content[index] != *character) {
			return false;
		}

		++character;
		++index;
	}

	if (*character != '\0')
		return false;

	return true;
}

uint8_t &StringSlice8::operator[](int rhs)
{
	assert(this->length > rhs);
	return this->content[rhs];
}

int64_t string_to_int(String8 s)
{
	return __string_to_int_template<String8>(s);
}

StringSlice8 string_slice_length(String8 s, int64_t start_index = 0,
				 int64_t length = 0)
{
	return __string_slice_length_template<StringSlice8>(
		*(StringSlice8 *)&s, start_index, length);
}

StringSlice8 string_slice_length(StringSlice8 s, int64_t start_index = 0,
				 int64_t length = 0)
{
	return __string_slice_length_template<StringSlice8>(
		*(StringSlice8 *)&s, start_index, length);
}

StringSlice8 string8_slice_to(String8 s, String8 to_string)
{
	StringSlice8 result = {};
	auto match_idx = 0;
	for (auto i = 0; i < s.length; ++i) {
		if (to_string[match_idx] == s.content[i]) {
			match_idx++;
			if (match_idx == to_string.length) {
				result.length = i - match_idx + 1;
				result.content = (uint8_t *)s.content;
				return result;
			}
		} else {
			match_idx = 0;
		}
	}

	return result;
}

String8 string8_from_slice(StringSlice8 s, Arena *a) {
	String8 result{};
	result.content = arena_alloc_struct_array(a, uint8_t, s.length + 1);
	result.length = s.length;
	std::memset((void *)result.content, 0, s.length);
	std::memcpy((void *)result.content, s.content, s.length);

	return result;
}

template <typename T> int64_t __index_of_template(T s, char c)
{
	for (int i = 0; i < s.length; ++i) {
		if (s.content[i] == c) {
			return i;
		}
	}

	return -1;
}

inline int64_t string_index_of(String8 s, char c)
{
	return __index_of_template<String8>(s, c);
}

inline int64_t string_index_of(StringSlice8 s, char c)
{
	return __index_of_template<StringSlice8>(s, c);
}

inline int64_t string_index_of(StringBuilder8 s, char c)
{
	return __index_of_template<StringBuilder8>(s, c);
}

StringSlice8 string8_slice_after(String8 s, String8 after_string)
{
	StringSlice8 result = {};
	auto match_idx = 0;
	for (auto i = 0; i < s.length; ++i) {
		if (after_string[match_idx] == s.content[i]) {
			match_idx++;
			if (match_idx == after_string.length) {
				result.content = (uint8_t *)s.content + i + 1;
				result.length = s.length - i + 1;
				return result;
			}
		} else {
			match_idx = 0;
		}
	}

	return result;
}

String8 string8_from_char_buff(const char *buffer, size_t length) {
	
	int64_t real_length = 0;
	const char *start = buffer;
	for (; *buffer != 0; ++buffer) {
		++real_length;
	}

	assert (real_length <= length);
	return String8{(uint8_t *)start, real_length};
}
String8 string8_concat(Arena *a, String8 s1, String8 s2)
{
	String8 result = {};
	result.length = s1.length + s2.length;
	result.content = arena_alloc_struct_array(a, uint8_t, result.length + 1);
	uint8_t *current_ptr = (uint8_t *)result.content;
	for (int i = 0; i < s1.length; ++i) {
		*current_ptr++ = s1.content[i];
	}

	for (int i = 0; i < s2.length; ++i) {
		*current_ptr++ = s2.content[i];
	}

	uint8_t *null_terminator = (uint8_t *)result.content + result.length;
	*null_terminator = 0;

	return result;
}

void inline string_builder8_init(Arena *a, StringBuilder8 *sb,
				 int64_t capacity)
{
	*sb = {};
	sb->capacity = capacity;
	sb->content = arena_alloc_struct_array(a, uint8_t, sb->capacity);
	memset(sb->content, 0, sb->capacity);
}

void string_builder8_append(StringBuilder8 *sb, String8 string)
{
	assert(sb->length + string.length < sb->capacity);

	for (auto i = 0; i < string.length; ++i) {
		sb->content[sb->length++] = string.content[i];
	}
}

void string_builder8_append(StringBuilder8 *sb, StringBuilder8 string)
{
	assert(sb->length + string.length < sb->capacity);

	for (auto i = 0; i < string.length; ++i) {
		sb->content[sb->length++] = string.content[i];
	}
}

void string_builder8_append(StringBuilder8 *sb, StringSlice8 string)
{
	assert(sb->length + string.length < sb->capacity);

	for (auto i = 0; i < string.length; ++i) {
		sb->content[sb->length++] = string.content[i];
	}
}

String8 string_builder8_to_string(StringBuilder8 *sb)
{
	assert(sb->length < sb->capacity - 1 &&
	       "No space for null terminator in string builder to string conversion");

	String8 result = {};
	result.content = sb->content;
	result.length = sb->length;

	return result;
}

const bool StringBuilder8::operator==(const StringSlice8 rhs)
{
	if (this->length != rhs.length) {
		return false;
	}

	for (int i = 0; i < this->length; ++i) {
		if (this->content[i] != rhs.content[i]) {
			return false;
		}
	}

	return true;
}

const bool StringBuilder8::operator==(const StringBuilder8 rhs)
{
	if (this->length != rhs.length) {
		return false;
	}

	for (int i = 0; i < this->length; ++i) {
		if (this->content[i] != rhs.content[i]) {
			return false;
		}
	}

	return true;
}

const bool StringBuilder8::operator==(const char *rhs)
{
	const char *character = rhs;
	uint32_t index = 0;
	while (*character != '\0') {
		if (index >= this->length) {
			return false;
		}

		if (this->content[index] != *character) {
			return false;
		}

		++character;
		++index;
	}

	if (*character != '\0')
		return false;

	return true;
}

namespace std {
    template<>
    struct hash<StringSlice8> {
        std::size_t operator()(const StringSlice8& k) const noexcept {
            return std::hash<std::string_view>{}(std::string_view(reinterpret_cast<const char *>(k.content), k.length));
        }
    };
}

namespace std {
    template<>
    struct hash<String8> {
        std::size_t operator()(const String8& k) const noexcept {
            // Delegate to std::string_view for a zero-copy hash
            // This treats your raw data as a string without allocating new memory
            return std::hash<std::string_view>{}(std::string_view(reinterpret_cast<const char *>(k.content), k.length));
        }
    };
}



template <typename K>
concept MapKey = std::semiregular<std::hash<K>> && 
requires(std::hash<K> h, K k)
{
	{ h(k) }->std::convertible_to<std::size_t>;
} && 
std::equality_comparable<K>;

template <MapKey K, typename V> struct ThreadSafeMapBucketNode {
	K key;
	V value;
	ThreadSafeMapBucketNode<K, V> *prev;
	ThreadSafeMapBucketNode<K, V> *next;
	uint64_t hash;
};

template <MapKey K, typename V>
struct alignas(std::hardware_destructive_interference_size) ThreadSafeMapBucket {
	std::shared_mutex lock;
	ThreadSafeMapBucketNode<K, V> head;
	bool active;
};

template <MapKey K, typename V>
struct ThreadSafeMap {
	ThreadSafeMapBucket<K, V> *buckets;
	int64_t bucket_count;
	PoolAllocator<ThreadSafeMapBucket<K, V>> *a; // make clear that pool is owned by map

	void insert(const K &k, const V &v)
	{
		auto hash = std::hash<K>{}(k);
		auto idx =  hash % this->bucket_count;

		ThreadSafeMapBucket<K, V> &bucket = this->buckets[idx];
		std::unique_lock<std::shared_mutex> lock{bucket.lock};

		if (!bucket.active) {
			bucket.head.key = k;
			bucket.head.value = v;
			bucket.active = true;
		} else {
			ThreadSafeMapBucketNode<K, V> *current = &bucket.head;
			ThreadSafeMapBucketNode<K, V> *prev = nullptr;
			while (current != nullptr) {
				// check hash first faster for complext types e.g. string_view
				if (current->hash == hash) {
					if (current->key == k) {
						current->value = v;
						return;
					}
				}

				prev = current;
				current = current->next;
			}

			// it is safe to cast to a bucket node only because the pool allocates in type of 
			// ThreadSafeMapBucket chunks and the ThreadSafeMapBucket contains the ThreadSafeMapBucketNode

			prev->next = (ThreadSafeMapBucketNode<K, V> *)pool_alloc(a);
			current = prev->next;

			current->prev = prev;
			current->hash = hash;
			current->key = k;
			current->value = v;
		}
	}

	V insert_or_get(const K &k, const V &v) 
	{
		auto hash = std::hash<K>{}(k);
		auto idx = hash % this->bucket_count;

		ThreadSafeMapBucket<K, V> &bucket = this->buckets[idx];
		std::unique_lock<std::shared_mutex> lock{bucket.lock};

		if (!bucket.active) {
			bucket.head.key = k;
			bucket.head.value = v;
			bucket.active = true;

		} else {

			// start the walk
			ThreadSafeMapBucketNode<K, V> *current = &bucket.head;
			ThreadSafeMapBucketNode<K, V> *prev = nullptr;
			while(current != nullptr) {
				if (current->hash == hash) {
					if (current->key == k) {
						return current->value;
					}
				}
			}

			prev->next = (ThreadSafeMapBucketNode<K, V> *)pool_alloc(a);
			current = prev->next;

			current->prev = prev;
			current->hash = hash;
			current->key = k;
			current->value = v;
		}

		return V{};

	}

	// only insert if the thing doesn't exists
	//void insert_if_no_exists(

	void erase(const K &k)
	{
		auto hash = std::hash<K>{}(k);
		auto idx = hash % this->bucket_count;

		ThreadSafeMapBucket<K, V> &bucket = this->buckets[idx];
		std::unique_lock<std::shared_mutex> lock{bucket.lock};

		ThreadSafeMapBucketNode<K, V> *current = bucket->head;
		ThreadSafeMapBucketNode<K, V> *prev = current->prev;
		while (current != nullptr) {
			if (current->key == k) {
				break;
			}

			prev = current;
			current = current->next;
		}

		if (!current) {
			return; // nothing to erase
		}

		auto next_node = current->next;
		if (prev) {
			prev->next = next_node;
		} else {
			bucket->active = false;
		}

		if (next_node) {
			next_node->prev = prev;
		}

		pool_dealloc(a, current);
	}

	V get(const K &k) {
		auto hash = std::hash<K>{}(k);
		auto idx = hash % this->bucket_count;

		ThreadSafeMapBucket<K, V> &bucket = this->buckets[idx];
		std::shared_lock<std::shared_mutex> lock{bucket.lock};

		if (!bucket.active) {
			return V{};
		}

		ThreadSafeMapBucketNode<K, V> *current = &bucket.head;
		ThreadSafeMapBucketNode<K, V> *prev = current->prev;

		while (current != nullptr) {
			if (current->key == k) {
				return current->value;
			}
		}

		return V{}; // return the zero value
	}
		
};

template <MapKey K, typename V>
void thread_safe_map_init(ThreadSafeMap<K, V> *m, PoolAllocator<ThreadSafeMap<K, V>> *a, size_t buckets)
{
	m->a = a;
	m->bucket_count = buckets;
	m->buckets = (ThreadSafeMapBucket<K, V> *)pool_alloc(a);
	for (auto i = 1; i < buckets; ++i) {
		pool_alloc(a);
	}
}

template <MapKey K, typename V>
void thread_safe_map_init(ThreadSafeMap<K, V> *m, Arena *a, size_t buckets) 
{
	auto p = (PoolAllocator<ThreadSafeMapBucket<K, V>> *)arena_alloc(a, sizeof(PoolAllocator<ThreadSafeMapBucket<K, V>>));
	pool_init(p, a, buckets);

	m->a = p;
	m->bucket_count = buckets;
	m->buckets = pool_alloc<ThreadSafeMapBucket<K, V>>(p);
	for (auto i = 1; i < buckets; ++i) {
		pool_alloc<ThreadSafeMapBucket<K, V>>(p);

	}
}

template <MapKey K, typename V> struct HashMapBucketNode {
	K key;
	V value;
	HashMapBucketNode<K, V> *prev;
	HashMapBucketNode<K, V> *next;
	uint64_t hash;
};

template <MapKey K, typename V>
struct HashMapBucket {
	HashMapBucketNode<K, V> head;
	bool active;
};

template <typename V>
struct HashMapClosedAddrInsertOrGetResult {
	V *value;
	bool exists;
};

template <MapKey K, typename V>
struct HashMapClosedAddr {
	HashMapBucket<K, V> *buckets;
	int64_t bucket_count;
	PoolAllocator<HashMapBucket<K, V>> *a; // make clear that pool is owned by map

	V *insert(const K &k, const V &v)
	{
		auto hash = std::hash<K>{}(k);
		auto idx =  hash % this->bucket_count;

		HashMapBucket<K, V> &bucket = this->buckets[idx];

		if (!bucket.active) {
			bucket.head.key = k;
			bucket.head.value = v;
			bucket.active = true;
		} else {
			HashMapBucketNode<K, V> *current = &bucket.head;
			HashMapBucketNode<K, V> *prev = nullptr;
			while (current != nullptr) {
				// check hash first faster for complext types e.g. string_view
				if (current->hash == hash) {
					if (current->key == k) {
						current->value = v;
						return current->value;
					}
				}

				prev = current;
				current = current->next;
			}

			//TODO(Ray) find a way to allocate just a node the current way just feels hacky
			prev->next = (HashMapBucketNode<K, V> *)pool_alloc(a);
			current = prev->next;

			current->prev = prev;
			current->hash = hash;
			current->key = k;
			current->value = v;

			return current->value;
		}
	}

	
  HashMapClosedAddrInsertOrGetResult<V> insert_or_get(const K &k, const V &v) 
	{
		auto hash = std::hash<K>{}(k);
		auto idx = hash % this->bucket_count;

		HashMapBucket<K, V> &bucket = this->buckets[idx];

		if (!bucket.active) {
			bucket.head.key = k;
			bucket.head.value = v;
			bucket.active = true;

		} else {

			// start the walk
			HashMapBucketNode<K, V> *current = &bucket.head;
			HashMapBucketNode<K, V> *prev = nullptr;
			while(current != nullptr) {
				if (current->hash == hash) {
					if (current->key == k) {
						return {current->value, true};
					}
				}
			}

			prev->next = (HashMapBucketNode<K, V> *)pool_alloc(a);
			current = prev->next;

			current->prev = prev;
			current->hash = hash;
			current->key = k;
			current->value = v;
			return {current->value, false};
		}
	}

	void erase(const K &k)
	{
		auto hash = std::hash<K>{}(k);
		auto idx = hash % this->bucket_count;

		HashMapBucket<K, V> &bucket = this->buckets[idx];

		HashMapBucketNode<K, V> *current = bucket->head;
		HashMapBucketNode<K, V> *prev = current->prev;
		while (current != nullptr) {
			if (current->key == k) {
				break;
			}

			prev = current;
			current = current->next;
		}

		if (!current) {
			return; // nothing to erase
		}

		auto next_node = current->next;
		if (prev) {
			prev->next = next_node;
		} else {
			bucket->active = false;
		}

		if (next_node) {
			next_node->prev = prev;
		}

		pool_dealloc(a, current);
	}

	V *get(const K &k) {
		auto hash = std::hash<K>{}(k);
		auto idx = hash % this->bucket_count;

		HashMapBucket<K, V> &bucket = this->buckets[idx];

		if (!bucket.active) {
			return nullptr;
		}

		HashMapBucketNode<K, V> *current = &bucket.head;
		HashMapBucketNode<K, V> *prev = current->prev;

		while (current != nullptr) {
			if (current->key == k) {
				return &current->value;
			}
		}

		return nullptr; // return the zero value
	}

	bool in(const K &k) {
		auto hash = std::hash<K>{}(k);
		auto idx = hash % this->bucket_count;

		HashMapBucket<K, V> &bucket = this->buckets[idx];

		if (!bucket.active) {
			return false;
		}

		HashMapBucketNode<K, V> *current = &bucket.head;
		HashMapBucketNode<K, V> *prev = current->prev;

		while (current != nullptr) {
			if (current->key == k) {
				return true;
			}
		}

		return false; // return the zero value

	}
};

template <MapKey K, typename V>
void hash_map_init(HashMapClosedAddr<K, V> *m, PoolAllocator<HashMapBucket<K, V>> *a, size_t buckets)
{
	m->a = a;
	m->bucket_count = buckets;
	m->buckets = pool_alloc<HashMapBucket<K, V>>(a);
	for (auto i = 1; i < buckets; ++i) {
		pool_alloc(a);
	}
}

template <MapKey K, typename V>
void hash_map_init(HashMapClosedAddr<K, V> *m, Arena *a, size_t buckets) 
{
	auto p = arena_alloc_struct(a, PoolAllocator);
	size_t bucket_size = sizeof(*m->buckets);
	size_t max_pool_blocks = buckets * 2;
	size_t pool_memory_size = max_pool_blocks * bucket_size;
	void *pool_memory = arena_alloc(a, pool_memory_size);
	pool_init(p, bucket_size, max_pool_blocks, pool_memory, pool_memory_size);

	m->a = p;
	m->bucket_count = buckets;
	m->buckets = pool_alloc<HashMapBucket<K, V>>(p);
	for (auto i = 1; i < buckets; ++i) {
		pool_alloc<HashMapBucket<K, V>>(p);
	}
}



#define string_builder8_to_string(sb) String8{sb.content, sb.length}
