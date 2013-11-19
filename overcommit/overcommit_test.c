
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <pthread.h>

#define WORK_SLEEP_TIME 3
#define MAX_USED_SWAP_PCT 30

static int cpu_workers_pause = 0;

void *cpu_worker(void *arg)
{
	unsigned long *counter = (unsigned long*)arg;
	int outer_counter = 1024;
	while (1) {
		if (cpu_workers_pause)
			sleep(1);
		else if (!--outer_counter) {
			outer_counter = 1024;
			(*counter)++;
		}
	}
}

struct random_page_stats {
	long min;
	long max;
	long avg;
	int count;
	int err;
	int reset;
	unsigned long *page_count;
	void **pages;
};

void *random_page_worker(void *arg)
{
	struct random_page_stats *stats = (struct random_page_stats*)arg;
	int r, v;
	struct timespec before, after;
	long sec, nsec;
	while (1) {
		r = rand() % *stats->page_count;
		v = rand() % 0xff;
		if (clock_gettime(CLOCK_MONOTONIC_RAW, &before)) {
			stats->err++;
			continue;
		}
		memset(stats->pages[r], v, 1);
		if (clock_gettime(CLOCK_MONOTONIC_RAW, &after)) {
			stats->err++;
			continue;
		}
		sec = after.tv_sec - before.tv_sec;
		nsec = (sec * 1000000000) + after.tv_nsec - before.tv_nsec;
		if (stats->reset)
			stats->min = stats->max = stats->avg = stats->count = stats->err = stats->reset = 0;
		if (nsec < stats->min || !stats->min)
			stats->min = nsec;
		if (nsec > stats->max)
			stats->max = nsec;
		stats->avg = ((stats->avg * stats->count) + nsec) / (stats->count + 1);
		stats->count++;
	}
}

void start_cpu_workers(int cpus, unsigned long counters[])
{
	pthread_t workers[cpus];
	int c = 0;

	for (; c<cpus; c++)
		pthread_create(&workers[c], NULL, &cpu_worker, &counters[c]);
}

unsigned long alloc_pages(void *pages[], size_t page_size, unsigned long n)
{
	unsigned long p = 0, total = (n / page_size);
	for (; p<total; p++) {
		pages[p] = malloc(page_size);
		if (pages[p])
			memset(pages[p], 1, 1); /* force allocation of phys page */
		else
			break; /* ENOMEM */
	}
	return p;
}

void get_meminfo_vals(char *keys[], unsigned long vals[])
{
	FILE *fp = fopen("/proc/meminfo", "r");
	char *line = NULL;
	size_t len = 0;
	ssize_t read;
	int k;

	while ((read = getline(&line, &len, fp)) >= 0) {
		for (k=0; keys[k]; k++) {
			size_t klen = strlen(keys[k]);
			size_t l = (len < klen ? len : klen);
			if (!strncmp(line, keys[k], l))
				sscanf(line, "%*s %ld kB", &vals[k]);
		}
		free(line);
		line = NULL;
		len = 0;
	}
	fclose(fp);
}

double calc_used_mem()
{
	char *keys[] = { "MemTotal", "AnonPages", "SwapTotal", "SwapFree", NULL };
	unsigned long vals[4] = { 0 };
	unsigned long used;

	get_meminfo_vals(keys, vals);

	used = vals[1] + vals[2] - vals[3];
	return ((double)(used * 100) / (double)vals[0]);
}

double calc_used_swap()
{
	char *keys[] = { "MemTotal", "SwapTotal", "SwapFree", NULL };
	unsigned long vals[3] = { 0 };
	unsigned long used;

	get_meminfo_vals(keys, vals);

	return (double)((vals[1] - vals[2]) * 100) / (double)vals[0];
}

double calc_max_mem(int pct_swap)
{
	char *keys[] = { "MemTotal", "SwapTotal", NULL };
	unsigned long vals[2] = { 0 };
	unsigned long max_swap;

	get_meminfo_vals(keys, vals);

	max_swap = (vals[1] * pct_swap) / 100;
	return ((double)((vals[0] + max_swap) * 100) / (double)vals[0]);
}

unsigned long calc_init_mem_size()
{
	char *keys[] = { "MemTotal", "AnonPages", "SwapTotal", "SwapFree", NULL };
	unsigned long vals[4] = { 0 };
	unsigned long used;

	get_meminfo_vals(keys, vals);

	used = vals[1] + vals[2] - vals[3];
	if (used > vals[0])
		return 1024;
	else
		return (((vals[0] - used) * 80) / 100) * 1024;
}

unsigned long calc_inc_mem_size()
{
	char *keys[] = { "MemTotal", NULL };
	unsigned long vals[1] = { 0 };

	get_meminfo_vals(keys, vals);

	return ((vals[0] * 2) / 100) * 1024;
}

unsigned long calc_counter(unsigned long counters[], int cpus)
{
	unsigned long total = 0;
	int i = 0;
	for (; i<cpus; i++)
		total += counters[i];
	return total;
}

double pct(unsigned long n, unsigned long d)
{
	return (double)(n * 100) / (double)d;
}

void main(int argc, char *argv[])
{
	int cpus = (int)sysconf(_SC_NPROCESSORS_CONF);
	size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
	unsigned long phys_pages = sysconf(_SC_PHYS_PAGES);
	unsigned long max_pages = (phys_pages * 2); /* 2x is arbitrary */
	void **pages = calloc(max_pages, sizeof(void*));
	unsigned long counters[cpus];
	unsigned long current_pages = 0;
	double used_mem;
	unsigned long init_mem_size = calc_init_mem_size();
	unsigned long inc_mem_size = calc_inc_mem_size();
	pthread_t worker;
	struct random_page_stats stats;
	unsigned long bl_count, bl_min, bl_avg, bl_max, bl_page_count;
	int bl_set = 0;

	memset(&stats, 0, sizeof(struct random_page_stats));
	stats.page_count = &current_pages;
	stats.pages = pages;

	printf("Getting baseline numbers\n");
	fflush(NULL);
	start_cpu_workers(cpus, counters);
	bzero(counters, cpus * sizeof(unsigned long));
	sleep(WORK_SLEEP_TIME);
	bl_count = calc_counter(counters, cpus);

	cpu_workers_pause = 1;

	current_pages += alloc_pages(&pages[current_pages], page_size, page_size * 1024);
	pthread_create(&worker, NULL, &random_page_worker, &stats);
	stats.reset = 1;
	sleep(WORK_SLEEP_TIME);
	bl_avg = stats.avg;
	bl_page_count = stats.count;

	printf("Baseline CPU count %ld MEM avg %ld count %ld\n", bl_count, bl_avg, bl_page_count);

	printf("Allocating %ldm of initial memory\n", init_mem_size / (1024 * 1024));
	fflush(NULL);
	current_pages += alloc_pages(&pages[current_pages], page_size, init_mem_size);

	cpu_workers_pause = 0;
	sleep(1);

	do {
		if (current_pages + (inc_mem_size/page_size) > max_pages) {
			printf("Too many pages allocated, exiting\n");
			break;
		}
		current_pages += alloc_pages(&pages[current_pages], page_size, inc_mem_size);
		used_mem = calc_used_mem();
		bzero(counters, cpus * sizeof(unsigned long));
		stats.reset = 1;
		sleep(WORK_SLEEP_TIME);
		printf("%03.0f%% mem, %02.0f%% swap, count %02.0f%%, (%06.0f%% avg, count %06.3f%%, err %d)\n",
					 used_mem, calc_used_swap(),
					 pct(calc_counter(counters, cpus), bl_count),
					 pct(stats.avg, bl_avg),
					 pct(stats.count, bl_page_count),
					 stats.err);
		fflush(NULL);
	} while (used_mem < calc_max_mem(MAX_USED_SWAP_PCT));
}


