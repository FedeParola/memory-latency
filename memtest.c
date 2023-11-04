#include <errno.h>
#include <getopt.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DEFAULT_ACCESSES 10000000
#define WARMUP_ACCESSES 10000000
#define DEFAULT_STRIDE 512
#define DEFAULT_CACHE_LINE_SIZE 64
#define MIN_SIZE 512
#define DEFAULT_MAX_SIZE (1024 * 1024 * 1024)

/* Spin-polling synchronization variable for concurrent tests */
static volatile int ready[2];
static unsigned opt_line_sz = DEFAULT_CACHE_LINE_SIZE;
static unsigned long opt_accesses = DEFAULT_ACCESSES;
static unsigned opt_stride = DEFAULT_STRIDE;
static unsigned long opt_max_size = DEFAULT_MAX_SIZE;
static int opt_forward = 0;
static int opt_index_based = 0;
static int opt_concurrent = 0;
static struct option long_options[] = {
	{"line-size", optional_argument, 0, 'l'},
	{"accesses", optional_argument, 0, 'a'},
	{"stride", optional_argument, 0, 's'},
	{"max-size", optional_argument, 0, 'm'},
	{"forward", optional_argument, 0, 'f'},
	{"index", optional_argument, 0, 'i'},
	{"concurrent", optional_argument, 0, 'c'},
	{0, 0, 0, 0}
};

static void usage(const char *prog)
{
	fprintf(stderr,
		"  Usage: %s [OPTIONS]\n"
		"  Options:\n"
		"  -l, --line-size	Size of a cache line in B (default %u)\n"
		"  -a, --accesses	Number of memory accesses for each memory size (default %u)\n"
		"  -s, --stride		Stride between two consecutive memory accesses in B (default %u)\n"
		"  -m, --max-size	Maximum size of tested memory in MiB (default %u)\n"
		"  -f, --forward		Forward memory scan (default backward)\n"
		"  -i, --index		Index-based memory scan (default pointer-based)\n"
		"  -c, --concurrent	Run the test on two concurrent threads\n",
		prog, DEFAULT_CACHE_LINE_SIZE, DEFAULT_ACCESSES, DEFAULT_STRIDE,
		DEFAULT_MAX_SIZE / (1024 * 1024));

	exit(1);
}

static void parse_command_line(int argc, char **argv)
{
	int option_index, c;

	for (;;) {
		c = getopt_long(argc, argv, "l:a:s:m:fic", long_options,
				&option_index);
		if (c == -1)
			break;

		switch (c) {
		case 'l':
			opt_line_sz = strtoul(optarg, NULL, 0);
			break;
		case 'a':
			opt_accesses = strtoul(optarg, NULL, 0);
			break;
		case 's':
			opt_stride = strtoul(optarg, NULL, 0);
			break;
		case 'm':
			opt_max_size = strtod(optarg, NULL) * 1024 * 1024;
			break;
		case 'f':
			opt_forward = 1;
			break;
		case 'i':
			opt_index_based = 1;
			break;
		case 'c':
			opt_concurrent = 1;
			break;
		default:
			usage(argv[0]);
		}
	}

	if (opt_line_sz == 0) {
		fprintf(stderr, "Cache line size must be > 0\n");
		usage(argv[0]);
	}

	if (opt_max_size < MIN_SIZE) {
		fprintf(stderr, "Max size can't be smaller than %u\n",
			MIN_SIZE);
		usage(argv[0]);
	}

	if (opt_max_size < opt_stride) {
		fprintf(stderr, "Max size can't be smaller than the stride "
			"(%u B)\n", opt_stride);
		usage(argv[0]);
	}

	if (opt_stride % opt_line_sz != 0) {
		fprintf(stderr, "Stride must be a multiple of the cache line "
			"size (%u B)\n", opt_line_sz);
		usage(argv[0]);
	}

	if (!opt_index_based && opt_stride < sizeof(void *)) {
		fprintf(stderr, "In pointer-based memory scan stride can't be "
			"smaller than the size of a pointer (%lu B)\n",
			sizeof(void *));
		usage(argv[0]);
	}
}

static unsigned long step(unsigned long size)
{
	if (size < 1024) {
		size = size * 2;
	} else if (size < 4 * 1024) {
		size += 1024;
	} else {
		unsigned long s;
		for (s = 4 * 1024; s <= size; s *= 2);
		size += s / 4;
	}

	/* Round to the next multiple of the cache line size */
	size = ((size - 1) / opt_line_sz + 1) * opt_line_sz;

	return size;
}

/* Uses the value of a variable so that the optimizer doesn't remove operations
 * on it
 */
static void use_value(long value)
{
	volatile long sink = value;
}

static void warmup_memory(char *mem, unsigned long size)
{
	long *data = (long *)mem;
	unsigned long nitems = size / sizeof(long);

	for (unsigned long i = 0; i < nitems; i++)
		use_value(data[i]);
}

static void compute_forward_pointers(char *mem, unsigned long size)
{
	unsigned long i;

	/* This loop creates N=(stride/line_size) chains of forward pointers */
	for (i = 0; i < size - opt_stride; i += opt_line_sz)
		*(void **)&mem[i] = (void *)&mem[i + opt_stride];

	/* This loop makes the last pointer of each chain point to the next
	 * chain (with proper wrapping)
	 */
	for (; i < size; i += opt_line_sz) {
		*(void **)&mem[i]
			= (void *)&mem[(i + opt_line_sz) % opt_stride];
	}
}

static void compute_backward_pointers(char *mem, unsigned long size)
{
	unsigned long i;

	/* This loop creates N=(stride/line_size) chains of backward pointers */
	for (i = 0; i < size - opt_stride; i += opt_line_sz)
		*(void **)&mem[i + opt_stride] = (void *)&mem[i];

	/* This loop makes the first pointer of each chain point to the previous
	 * chain (with proper wrapping)
	 */
	for (; i < size; i += opt_line_sz) {
		*(void **)&mem[(i + opt_line_sz) % opt_stride]
			= (void *)&mem[i];
	}
}

static double pointer_scan(char *mem)
{
	void **ptr = (void **)&mem[0];

	struct timespec start;
	if (clock_gettime(CLOCK_MONOTONIC, &start)) {
		fprintf(stderr, "Error getting time: %s\n", strerror(errno));
		exit(1);
	}

	for (unsigned long i = 0; i < opt_accesses; i++)
		ptr = *ptr;

	struct timespec stop;
	if (clock_gettime(CLOCK_MONOTONIC, &stop)) {
		fprintf(stderr, "Error getting time: %s\n",
			strerror(errno));
		exit(1);
	}

	use_value(*(long *)ptr);

	unsigned long elapsed = (stop.tv_sec - start.tv_sec)
				* 1000000000
				+ stop.tv_nsec - start.tv_nsec;

	return (double)elapsed / opt_accesses;
}

static double index_scan(char *mem, unsigned long size)
{
	unsigned noffsets = opt_stride / opt_line_sz;
	unsigned long nreads = size / opt_stride;
	unsigned long repetitions =
		(opt_accesses - 1) / (nreads * noffsets) + 1;

	struct timespec start;
	if (clock_gettime(CLOCK_MONOTONIC, &start)) {
		fprintf(stderr, "Error getting time: %s\n", strerror(errno));
		exit(1);
	}

	unsigned long i, j;
	unsigned offset;
	long total = 0;
	for (i = 0; i < repetitions; i++) {
		for (offset = 0; offset < opt_stride; offset += opt_line_sz) {
			for (j = offset; j < size; j += opt_stride)
				total += *(long *)(mem + j);
		}
	}

	struct timespec stop;
	if (clock_gettime(CLOCK_MONOTONIC, &stop)) {
		fprintf(stderr, "Error getting time: %s\n",
			strerror(errno));
		exit(1);
	}

	use_value(total);

	unsigned long elapsed = (stop.tv_sec - start.tv_sec)
				* 1000000000
				+ stop.tv_nsec - start.tv_nsec;
	unsigned long accesses = repetitions * noffsets * nreads;

	return (double)elapsed / accesses;
}

static double scan(long id, char *mem, unsigned long size)
{
	if (!opt_index_based) {
		if (opt_forward)
			compute_forward_pointers(mem, size);
		else
			compute_backward_pointers(mem, size);
	}

	if (opt_concurrent) {
		/* Wait for other thread to start the test */
		__atomic_store_n(&ready[id], 1, __ATOMIC_SEQ_CST);
		while (!__atomic_load_n(&ready[id ^ 1], __ATOMIC_SEQ_CST));
	}

	double latency;
	if (opt_index_based)
		latency = index_scan(mem, size);
	else
		latency = pointer_scan(mem);

	if (opt_concurrent) {
		/* Wait for other thread to complete the test */
		__atomic_store_n(&ready[id ^ 1], 0, __ATOMIC_SEQ_CST);
		while (__atomic_load_n(&ready[id], __ATOMIC_SEQ_CST));
	}

	return latency;
}

static void *run_test(void *arg)
{
	long id = (long)arg;

	char *mem = malloc(opt_max_size);
	if (!mem) {
		fprintf(stderr, "Error allocating memory: %s\n",
			strerror(errno));
		exit(1);
	}

	warmup_memory(mem, opt_max_size);

	unsigned long size;
	for (size = MIN_SIZE < opt_stride ? opt_stride : MIN_SIZE;
	     size <= opt_max_size;
	     size = step(size)) {
		double latency = scan(id, mem, size);

		printf("%ld, %.5f, %.3f\n", id, (double)size / (1024 * 1024),
		       latency);
	}

	free(mem);
}

int main(int argc, char *argv[])
{
	pthread_t sec_thread;

	parse_command_line(argc, argv);

	printf("Running %s %s scan on %s.\n",
	       opt_forward ? "forward" : "backward",
	       opt_index_based ? "index-based" : "pointer-based",
	       opt_concurrent ? "two threads" : "one thread");
	printf("Cache line size %u B, max memory size %lu MiB.\n", opt_line_sz,
	       opt_max_size / (1024 * 1024));
	printf("Performing %lu accesses per size.\n\n", opt_accesses);

	printf("Thread, Mem size (MiB), Access latency (ns)\n");

	if (opt_concurrent) {
		if (pthread_create(&sec_thread, NULL, run_test, (void *)1)) {
			fprintf(stderr, "Error creating secondary thread: %s\n",
				strerror(errno));
			exit(1);
		}
	}

	run_test((void *)0);

	if (opt_concurrent) {
		if (pthread_join(sec_thread, NULL)) {
			fprintf(stderr, "Error joining secondary thread: %s\n",
				strerror(errno));
			exit(1);
		}
	}

	return 0;
}