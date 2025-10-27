#include "utils.h"
#include "workqueue.cpp"
#include "city.c"

#include <stdio.h>
#include <thread>


#define MAX_TAGS (256)
struct ProcessRequestArgs {
        Arena *arena;
        String8 string;
};

thread_local Arena thread_local_arena;

void mpmc_work_queue_thread_start_routine(MPMCWorkQueue *wq, size_t arena_size, std::thread *t)
{
	arena_init(&thread_local_arena, arena_size);

	while (!wq->stop_flag.load(std::memory_order_acquire)) {
		mpmc_work_queue_dequeue_entry(wq);
	}
}

bool string_sort_cmp(StringSlice8 a, StringSlice8 b)
{
        if (a.content == 0) {
            return false;
        }

        if (b.content == 0) {
            return true;
        }

        for (int i = 0; i < a.length; ++i) {
                if (i >= b.length) {
                        return false;
                }

                char lower_char_a = tolower(a[i]);
                char lower_char_b = tolower(b[i]);

                if (lower_char_a == lower_char_b) {
                        continue;
                } else if (lower_char_a > lower_char_b) {
                        return false;
                } else {
                        return true;
                }
        }

        return true;
}

void *process_write_request(void *args)
{
	ProcessRequestArgs *typed_args = (ProcessRequestArgs *)args;
	String8 request_string = typed_args->string;
	Arena *arena = typed_args->arena;

	for (int request_index = 0; request_index < request_string.length;
	     ++request_index) {
		if (request_string[request_index] == '\n') {
			continue;
		}

		StringSlice8 measurement = string8_slice_to(
			request_string, string8_from_cstring("["));
		StringSlice8 tags = string_slice_length(
			request_string, measurement.length,
			string_index_of(request_string, ']') -
				measurement.length);

		// need to sort the tags by alphabetical order
		StringSlice8 tag_arr[MAX_TAGS] = {};
		int tag_arr_size = 0;
		int start_index = 1;
		printf("PRINT THE THING %.*s\n", tags.length, tags.length,
		       tags.content);
		fflush(stdout);

		assert(tags[0] == '[');
		for (uint64_t i = 0; i < tags.length; ++i) {
			if (tags[i] == ' ') {
				start_index++;
				continue;
			}

			if (tags[i] == '[') {
				continue;
			}

			if (tags[i] == ',') {
				tag_arr[tag_arr_size++] =
					StringSlice8{ tags.content +
							      start_index,
						      i - start_index };
				start_index = i + 1;
			}
		}

		// save the last tag if there was one
		if (tags.length) {
			tag_arr[tag_arr_size++] =
				StringSlice8{ tags.content + start_index,
					      tags.length - start_index };
		}

		std::sort(std::begin(tag_arr), std::end(tag_arr),
			  string_sort_cmp);

		printf("taGas\n");
		fflush(stdout);
		for (int i = 0; i < tag_arr_size; ++i) {
			printf("tag: %.*s\n", tag_arr[i].length,
			       tag_arr[i].content);
			fflush(stdout);
		}

		// join the array without the comma
		//
		uint64_t final_tags_size = 0;
		for (auto i = 0; i < tag_arr_size; ++i) {
			final_tags_size += tag_arr[i].length;
		}

		StringBuilder8 final_tags_string;
		string_builder8_init(&thread_local_arena, &final_tags_string,
				     final_tags_size + 1);

		for (int i = 0; i < tag_arr_size; ++i) {
			string_builder8_append(&final_tags_string, tag_arr[i]);
		}

		CityHash128((char *)final_tags_string.content,
			    final_tags_string.length);
	}
	return 0;
}

struct ChronoMemtable {
};

struct FakeRequestHandleArgs {
        ThreadSafeArena *arena;
        int file_offset;
        MPMCWorkQueue *processing_queue;
};


void *fake_request_handle(void *args) {
        auto fake_request_handle_args = (FakeRequestHandleArgs *)args;
        int file_offset = fake_request_handle_args->file_offset;
        MPMCWorkQueue *processing_queue = fake_request_handle_args->processing_queue;

        FILE *fd = fopen("outfile.data", "r");

        if (fd == NULL) {
                perror("can't open file");
        }

        size_t string_size = 36*1000; 
        char *buffer =  (char *)arena_alloc(&thread_local_arena, string_size + 1);
        fseek(fd, 36*1000*file_offset, SEEK_SET);
        size_t bytes_read = fread(buffer, 1, string_size, fd);
        assert(bytes_read == 36 * 1000);

        buffer[string_size] = 0; // null terminator
        for (int i = string_size; i >= 0; --i) {
                if (buffer[i] != '\n') {
                        string_size--;
                }
        }

        for (;*buffer != '\n'; ++buffer);
        ++buffer;

        MPMCWorkQueueEntry entry = {};
        
        mpmc_begin_producer(processing_queue);

        ProcessRequestArgs *process_request = (ProcessRequestArgs *)arena_alloc(&thread_local_arena, sizeof(*process_request));
        entry.callback_args = (void *)process_request;
        process_request->string = String8{(uint8_t *)buffer, string_size};
        entry.callback = process_write_request;
        mpmc_work_queue_enqueue_entry(processing_queue, entry);
        printf("taGas\n");
        fflush(stdout);

	mpmc_end_producer(processing_queue);
        
        return 0;
}

int main(int argv, char *argc[]) {
        ThreadSafeArena main_arena; 
        arena_init(&main_arena, MEGABYTES(1));
        
        MPMCWorkQueue io_queue = {};
        static MPMCWorkQueue processing_queue = {};
        mpmc_work_queue_init(&io_queue, &main_arena, 512, MEGABYTES(1));
        mpmc_work_queue_init(&processing_queue, &main_arena, 512, MEGABYTES(1));

        // start threads
	int thread_count = 2;
	std::thread *io_threads = (std::thread *)arena_alloc(
		&main_arena, sizeof(*io_threads) * thread_count);
	std::thread *processing_threads = (std::thread *)arena_alloc(
		&main_arena, sizeof(*processing_threads) * thread_count);

	for (int i = 0; i < thread_count; ++i) {
		io_threads[i] = std::thread(mpmc_work_queue_thread_start_routine,
					 &io_queue, MEGABYTES(1), io_threads + i);
	}

	for (int i = 0; i < thread_count; ++i) {
		processing_threads[i] = std::thread(mpmc_work_queue_thread_start_routine,
					 &processing_queue, MEGABYTES(1), processing_threads + i);
	}

	mpmc_begin_producer(&io_queue);
    
        for (int i = 0; i < 4; ++i) {
                MPMCWorkQueueEntry entry = {};
                FakeRequestHandleArgs *args = (FakeRequestHandleArgs *)arena_alloc(&main_arena, sizeof(*args));
                args->file_offset = i+1;
                args->processing_queue = &processing_queue;
                args->arena = &main_arena;
        
                entry.callback_args = args;
                entry.callback = fake_request_handle;
                mpmc_work_queue_enqueue_entry(&io_queue, entry);
        }

        mpmc_end_producer(&io_queue);

        mpmc_work_queue_spinlock_till_finished(&io_queue);
        mpmc_work_queue_spinlock_till_finished(&processing_queue);
        mpmc_work_queue_stop(&io_queue);
        mpmc_work_queue_stop(&processing_queue);
        printf("WHAT\n");

        for (int i = 0; i < thread_count; ++i) {
                processing_threads[i].join();
        }

        for (int i = 0; i < thread_count; ++i) {
                io_threads[i].join();
        }

        return 0;
}


