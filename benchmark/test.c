#include <stdlib.h>
#include <time.h>
#include <stdio.h>
#include <stdint.h>

// The timespec struct holds seconds and nanoseconds
struct timespec start_time, end_time;

void start_timer()
{
	clock_gettime(CLOCK_MONOTONIC_RAW, &start_time);
}

void end_timer()
{
	clock_gettime(CLOCK_MONOTONIC_RAW, &end_time);
}

__attribute_noinline__ uint64_t __benchmark_test_function3(const char *a, int b,
							   uint64_t c)
{
	return a[b] + c;
}

__attribute_noinline__ uint64_t __benchmark_test_function2(const char *a, int b,
							   uint64_t c)
{
	return a[b] + c;
}

__attribute_noinline__ uint64_t __benchmark_test_function1(const char *a, int b,
							   uint64_t c)
{
	return a[b] + c;
}

typedef uint64_t (*benchmark_test_function_t)(const char *, int, uint64_t);

static double get_elapsed_time()
{
	long seconds = end_time.tv_sec - start_time.tv_sec;
	long nanoseconds = end_time.tv_nsec - start_time.tv_nsec;
	if (start_time.tv_nsec > end_time.tv_nsec) { // clock underflow
		--seconds;
		nanoseconds += 1000000000;
	}
	printf("Elapsed time: %ld.%09ld seconds\n", seconds, nanoseconds);
	return seconds * 1.0 + nanoseconds / 1000000000.0;
}

static double get_function_time(benchmark_test_function_t func, int iter)
{
	start_timer();
	// test base line
	for (int i = 0; i < iter; i++) {
		func("hello", i % 4, i);
	}
	end_timer();
	double time = get_elapsed_time();
	return time;
}

void do_benchmark_userspace(benchmark_test_function_t func, int iter)
{
	double base_line_time, after_hook_time, total_time;

	printf("a[b] + c for %d times\n", iter);
	base_line_time = get_function_time(func, iter);
	printf("Average time usage %lf ns\n\n",
	       (base_line_time) / iter * 1000000000.0);
}

#define do_benchmark_func(func, iter)                                   \
	do {                                                                   \
		printf("Benchmarking %s\n", #func);                           \
		do_benchmark_userspace(func ,iter);	\
	} while (0)

int main()
{
	puts("");
	int iter = 100 * 1000;
	do_benchmark_func(__benchmark_test_function1, iter);
	do_benchmark_func(__benchmark_test_function2, iter);
	do_benchmark_func(__benchmark_test_function3, iter);
    return 0;
}
