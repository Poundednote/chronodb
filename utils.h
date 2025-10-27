#pragma once

#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>
#include <atomic>

#define DEFAULT_ARENA_SIZE (4096)

#define KILOBYTES(n) (1024 * n)
#define MEGABYTES(n) (1024 * KILOBYTES(n))
#define GIGABYTES(n) (1024 * MEGABYTES(n))
#define arraycount(arr) (sizeof(arr) / sizeof(arr[0]))

struct Arena {
	void *memory;
	uint64_t size;
	uint64_t capacity;
};

struct ThreadSafeArena {
        void *memory;
        std::atomic<uint64_t> size;
        uint64_t capacity;
};

void arena_init(Arena *a, uint32_t capacity);
void arena_clear(Arena *a);
void arena_destroy(Arena *a);

struct String8 {
	uint8_t *content;
	uint64_t length;

	bool operator==(String8 &rhs);
	bool operator==(const char *rhs);
	bool operator!=(const char *rhs);
	bool operator!=(String8 &rhs);
	uint8_t &operator[](int rhs);
};

struct StringSlice8 {
	uint8_t *content;
	uint64_t length;
	bool operator==(StringSlice8 &rhs);
	bool operator==(const char *rhs);
	bool operator!=(const char *rhs);
	bool operator!=(StringSlice8 &rhs);
	uint8_t &operator[](int rhs);
};

struct StringBuilder8 {
	uint8_t *content;
	uint64_t length;
	uint64_t capacity;
};

template <typename T> int64_t __string_to_int_template(T s);

#define string8_from_cstring(cstring) \
	(String8{ (uint8_t *)(cstring), sizeof(cstring) - 1 })
int string8_index_from_match_end(String8 string, String8 match_string);
String8 string8_view_after_match_end(String8 string, String8 match_string);
String8 inline string8_view_from_match_end(String8 string,
					   String8 match_string);
String8 string8_concat(Arena *a, String8 s1, String8 s2);
int64_t string_to_int(String8 s);

void string_builder8_append(StringBuilder8 *sb, String8 string);
String8 string_builder_to_string8(StringBuilder8 *sb);

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

	uint64_t old_size = a->size.fetch_add(size, std::memory_order_acq_rel);
	void *ptr = (char *)a->memory + old_size;
	return ptr;
}

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

bool String8::operator!=(const char *rhs)
{
	return !(*this == rhs);
}

bool String8::operator!=(String8 &rhs)
{
	return !(*this == rhs);
}

uint8_t &String8::operator[](int rhs)
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

bool StringSlice8::operator==(const char *rhs)
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

int string8_index_from_match_end(String8 string, String8 match_string)
{
	assert(match_string.length < string.length);

	uint32_t match_index = 0;
	for (int i = 0; i < string.length; ++i) {
		if (string[i] == match_string[match_index]) {
			match_index++;
			if (match_index == match_string.length) {
				return i;
			}
		} else {
			match_index = 0;
		}
	}

	return -1;
}

String8 inline string8_view_from_match_end(String8 string, String8 match_string)
{
	int new_start_index =
		string8_index_from_match_end(string, match_string);

	if (new_start_index == -1) {
		string.length = 0;
	} else {
		string.content += new_start_index;
		string.length -= new_start_index;
	}

	assert(string.length >= 0);

	return string;
}

String8 string8_view_after_match_end(String8 string, String8 match_string)
{
	int new_start_index =
		string8_index_from_match_end(string, match_string) + 1;

	if (string.length <= new_start_index) {
		string.length = 0;
	} else if (new_start_index == 0) {
		string.length = 0;
	} else {
		string.content += new_start_index;
		string.length -= new_start_index;
	}
	return string;
}

int64_t string_to_int(String8 s)
{
	return __string_to_int_template<String8>(s);
}

StringSlice8 string_slice_length(String8 s, int start_index = 0,
				 uint32_t length = 0)
{
	return __string_slice_length_template<StringSlice8>(
		*(StringSlice8 *)&s, start_index, length);
}

StringSlice8 string8_slice_to(String8 s, String8 to_string)
{
	int index = string8_index_from_match_end(s, to_string);
	return string_slice_length(s, 0, index);
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
	int index = string8_index_from_match_end(s, after_string);
	return string_slice_length(s, index + 1);
}

String8 string8_concat(Arena *a, String8 s1, String8 s2)
{
	String8 result = {};
	result.length = s1.length + s2.length;
	result.content = (uint8_t *)arena_alloc(a, result.length + 1);
	uint8_t *current_ptr = result.content;
	for (int i = 0; i < s1.length; ++i) {
		*current_ptr++ = s1.content[i];
	}

	for (int i = 0; i < s2.length; ++i) {
		*current_ptr++ = s2.content[i];
	}

	result.content[result.length] = 0;

	return result;
}

void inline string_builder8_init(Arena *a, StringBuilder8 *sb,
				 uint64_t capacity)
{
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
	String8 result = {};
	result.content = sb->content;
	result.length = sb->length;

	return result;
}
