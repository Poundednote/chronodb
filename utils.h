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

#define DEFAULT_ARENA_SIZE (4096)

#define KILOBYTES(n) (1024 * n)
#define MEGABYTES(n) (1024 * KILOBYTES(n))
#define GIGABYTES(n) (1024 * MEGABYTES(n))
#define arraycount(arr) (sizeof(arr) / sizeof(arr[0]))
#define REQUEST_POOL_CHUNK_SIZE (MEGABYTES(3))
#define REQUEST_SLOT_COUNT (64)
#define string8_to_cstring(string8) ((const char *)string8.content)

struct Arena {
	void *memory;
	uint64_t size;
	uint64_t capacity;
};

void arena_init(Arena *a, uint32_t capacity)
{
	a->size = 0;
	a->capacity = capacity;

	a->memory = malloc(capacity);

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

void *arena_alloc(Arena *a, uint64_t size)
{
	assert(a->size + size < a->capacity);

	a->size += size;
	void *ptr = (char *)a->memory + a->size;
	return ptr;
}

struct ThreadSafeArena: public Arena {
        void *memory;
        std::atomic<uint64_t> size;
        uint64_t capacity;
};


void arena_init(ThreadSafeArena *a, uint32_t capacity)
{
	a->size = 0;
	a->capacity = capacity;

	a->memory = malloc(capacity);

	assert(a->memory != nullptr);
}

void arena_clear(ThreadSafeArena *a)
{
	a->size = 0;
}

void arena_destroy(ThreadSafeArena *a)
{
	if (a->memory != nullptr) {
		free(a->memory);
	}
}

void *arena_alloc(ThreadSafeArena *a, uint64_t size)
{
	assert(a->memory != nullptr);
	assert(a->size + size < a->capacity);

	uint64_t old_size = a->size.fetch_add(size, std::memory_order_relaxed);
	void *ptr = (char *)a->memory + old_size;
	return ptr;
}

#define arena_alloc_struct(a, struct) (struct *)arena_alloc(a, sizeof(struct))
#define arena_alloc_struct_array(a, struct, n) (struct *)arena_alloc(a, sizeof(struct) * n)


struct PoolAllocatorFreeListNode {
	PoolAllocatorFreeListNode *next;
};

struct PoolAllocator {
	PoolAllocatorFreeListNode *head;
	void *memory;
};


void pool_init(PoolAllocator *p, size_t block_size, size_t block_count) {
	p->memory = malloc(sizeof(block_size) + sizeof(PoolAllocatorFreeListNode) * block_count);

	uint8_t *ptr = (uint8_t *)p->memory;
	auto chunk_size = sizeof(block_size) + sizeof(PoolAllocatorFreeListNode);
	auto *free_list_node = (PoolAllocatorFreeListNode *)(ptr + chunk_size);
	for (auto i = 0; i < block_count - 1; ++i) {
		free_list_node->next = free_list_node + 1;
		free_list_node = free_list_node->next;
	}

	free_list_node->next = nullptr;

	p->head = (PoolAllocatorFreeListNode *)ptr;
	
}

void *pool_alloc(PoolAllocator *p)
{
	if (!p->head)
		return nullptr;

	PoolAllocatorFreeListNode *block = p->head;
	p->head = p->head->next;
	return block;
}

void pool_dealloc(PoolAllocator *p, void *ptr)
{
	auto block = (PoolAllocatorFreeListNode *)ptr;
	block->next = p->head;
	p->head = block;
}


template <typename K>
concept MapKey = std::regular<std::hash<K>> && 
requires(std::hash<K> h, K k)
{
	{ h(k) }->std::convertible_to<std::size_t>;
} && 
std::equality_comparable<K>;

template <typename K, typename V> struct ThreadSafeMapBucketNode {
	K key;
	V value;
	ThreadSafeMapBucketNode<K, V> *prev;
	ThreadSafeMapBucketNode<K, V> *next;
};

template <typename K, typename V>
struct alignas(std::hardware_destructive_interference_size) ThreadSafeMapBucket {
	std::shared_mutex lock;
	ThreadSafeMapBucketNode<K, V> head;
	bool active;
};

// neeed to impl a pool alloactor with free list
template <typename K, typename V>
struct ThreadSafeMap {
	ThreadSafeMapBucket<K, V> *buckets;
	int64_t bucket_count;
	PoolAllocator *a;

	void insert(const K &k, const V &v)
	{
		auto hash = std::hash<K>{}(k);
		auto idx =  hash % this.bucket_count;

		ThreadSafeMapBucket<K, V> &bucket = this->buckets[idx];
		std::unique_lock<std::shared_mutex> lock{bucket.lock};

		if (!bucket.active) {
			bucket->head.key = k;
			bucket->head.v = v;
			bucket.active = true;
		} else {
			ThreadSafeMapBucketNode<K, V> *current = bucket->head;
			ThreadSafeMapBucketNode<K, V> *prev = nullptr;
			while (current != nullptr) {
				if (current->key == k) {
					current->value = v;
					return;
				}

				prev = current;
				current = current->next;
			}

			prev->next = pool_alloc(a);
			current = prev->next;
			current->prev = prev;
			current->key = k;
			current->value = v;
		}
	}

	void erase(const K &k)
	{
		auto hash = std::hash<K>{}(k);
		auto idx = hash % this.bucket_count;

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
		auto idx = hash % this.bucket_count;

		ThreadSafeMapBucket<K, V> &bucket = this->buckets[idx];
		std::shared_lock<std::shared_mutex> lock{bucket.lock};

		if (!bucket->active) {
			return nullptr;
		}

		ThreadSafeMapBucketNode<K, V> *current = bucket->head;
		ThreadSafeMapBucketNode<K, V> *prev = current->prev;

		while (current != nullptr) {
			if (current->key == k) {
				return *current;
			}
		}

		return {}; // return the zero value
	}
		
};

template <typename K, typename V>
void thread_safe_map_init(ThreadSafeMap<K, V> *m, PoolAllocator *a,
		     size_t bucket)
{
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
	bool operator==(StringSlice8 &rhs);
	bool operator==(StringBuilder8 &rhs);
	bool operator==(const char *rhs);
	bool operator!=(const char *rhs);
	bool operator!=(StringSlice8 &rhs);
	uint8_t &operator[](int rhs);
};

struct StringBuilder8 {
	uint8_t *content;
	int64_t length;
	int64_t capacity;
	bool operator==(StringSlice8 &rhs);
	bool operator==(StringBuilder8 &rhs);
	bool operator==(const char *rhs);
	bool operator!=(const char *rhs);
	bool operator!=(StringSlice8 &rhs);
};

template <typename T> int64_t __string_to_int_template(T s);

#define string8_from_cstring(cstring) \
	(String8{ (uint8_t *)(cstring), sizeof(cstring) - 1 })
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
T __string_slice_length_template(T s, uint32_t start_index, uint32_t length = 0)
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

bool StringSlice8::operator==(StringSlice8 &rhs)
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

bool StringSlice8::operator==(StringBuilder8 &rhs)
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

bool StringSlice8::operator==(const char *rhs)
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

bool StringSlice8::operator!=(const char *rhs)
{
	return !(*this == rhs);
}

bool StringSlice8::operator!=(StringSlice8 &rhs)
{
	return !(*this == rhs);
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

StringSlice8 string_slice_length(String8 s, int start_index = 0,
				 int64_t length = 0)
{
	return __string_slice_length_template<StringSlice8>(
		*(StringSlice8 *)&s, start_index, length);
}

StringSlice8 string_slice_length(StringSlice8 s, int start_index = 0,
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

String8 string8_from_char_buff(const char *buffer, size_t capacity) {
	
	int64_t length = 0;
	for (; *buffer != 0; ++buffer) {
		++length;
	}

	assert (length < capacity);
	return String8{(uint8_t *)buffer, length};
}
String8 string8_concat(Arena *a, String8 s1, String8 s2)
{
	String8 result = {};
	result.length = s1.length + s2.length;
	result.content = (uint8_t *)arena_alloc(a, result.length + 1);
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
	sb->content = (uint8_t *)arena_alloc(a, sb->capacity);
	memset(sb->content, 0, sb->capacity);
}

void string_builder8_append(StringBuilder8 *sb, String8 string)
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

bool StringBuilder8::operator==(StringSlice8 &rhs)
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

bool StringBuilder8::operator==(StringBuilder8 &rhs)
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

bool StringBuilder8::operator==(const char *rhs)
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

bool StringBuilder8::operator!=(const char *rhs)
{
	return !(*this == rhs);
}

bool StringBuilder8::operator!=(StringSlice8 &rhs)
{
	return !(*this == rhs);
}


#define string_builder8_to_string(sb) String8{sb.content, sb.length}
