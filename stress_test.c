#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>

#define BUFFER_SIZE (64 * 1024 * 1024)
#define CACHE_LINE_SIZE 64
#define NUM_THREADS 2

typedef struct {
    int thread_id;
    volatile uint64_t *buffer;
    size_t buffer_size;
} thread_data_t;

void *cache_thrashing_thread(void *arg) {
    thread_data_t *data = (thread_data_t *)arg;
    size_t num_elements = data->buffer_size / sizeof(uint64_t);
    size_t stride = CACHE_LINE_SIZE / sizeof(uint64_t);
    uint64_t sum = 0;
    int iterations = 0;

    printf("[Thread %d] Started on CPU %d\n", data->thread_id, sched_getcpu());

    while (1) {
        // Write phase (dirty the cache lines)
        for (size_t i = 0; i < num_elements; i += stride) {
            data->buffer[i] += 1;
            sum += data->buffer[i];
        }
        // Read phase
        for (size_t i = 0; i < num_elements; i += stride) {
            sum += data->buffer[i];
        }
        iterations++;
        if (iterations % 50 == 0) {
            printf("[Thread %d] Running on CPU %d (sum: %lu)\n", 
                   data->thread_id, sched_getcpu(), sum);
        }
        // Small sleep to allow migration to register in logs without flooding
        usleep(1000); 
    }
    return NULL;
}

int main(void) {
    pthread_t threads[NUM_THREADS];
    thread_data_t thread_data[NUM_THREADS];
    cpu_set_t cpuset;
    void *buffer;
    int ret;

    printf("==============================================\n");
    printf("CTAE Cache Thrashing Stress Test\n");
    printf("==============================================\n");
    printf("Creating %d threads pinned to CPU 0\n", NUM_THREADS);
    printf("Each thread will access %d MB buffer\n", BUFFER_SIZE / (1024 * 1024));
    printf("==============================================\n\n");

    // Align buffer to cache line size
    buffer = aligned_alloc(CACHE_LINE_SIZE, BUFFER_SIZE * NUM_THREADS);
    if (!buffer) {
        perror("Failed to allocate buffer");
        return 1;
    }
    memset(buffer, 0, BUFFER_SIZE * NUM_THREADS);

    for (int i = 0; i < NUM_THREADS; i++) {
        thread_data[i].thread_id = i;
        thread_data[i].buffer = (volatile uint64_t *)((uint8_t *)buffer + (i * BUFFER_SIZE));
        thread_data[i].buffer_size = BUFFER_SIZE;

        ret = pthread_create(&threads[i], NULL, cache_thrashing_thread, &thread_data[i]);
        if (ret != 0) {
            fprintf(stderr, "Failed to create thread %d\n", i);
            return 1;
        }

        // Pin everything to CPU 0 initially to cause contention
        CPU_ZERO(&cpuset);
        CPU_SET(0, &cpuset);
        ret = pthread_setaffinity_np(threads[i], sizeof(cpu_set_t), &cpuset);
        if (ret != 0) {
            fprintf(stderr, "Failed to set affinity for thread %d\n", i);
            return 1;
        }
        printf("Thread %d created and pinned to CPU 0\n", i);
    }

    printf("\nMonitor with: sudo dmesg -w | grep CTAE\n");
    printf("Press Ctrl+C to stop.\n\n");

    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }
    free(buffer);
    return 0;
}