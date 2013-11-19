
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <pthread.h>

#define WORK_SLEEP_TIME 3
#define MAX_USED_MEM 150
#define RECENT_PAGES 1024
#define MEM_INC_PCT 1

static int cpu_workers_pause = 0, mem_random_pause = 0, mem_recent_pause = 0;
static void **pages = NULL;
static unsigned long page_count = 0;

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

void *random_page_worker(void *arg)
{
	unsigned long *counter = (unsigned long *)arg;
	int r, v;
	while (1) {
		if (mem_random_pause)
			sleep(1);
		else {
			r = rand() % page_count;
			v = (*counter) % 0xff;
			memset(pages[r], v, 1);
			(*counter)++;
		}
	}
}

void *recent_page_worker(void *arg)
{
	unsigned long *counter = (unsigned long *)arg;
	int r, v;
	while (1) {
		if (mem_recent_pause)
			sleep(1);
		else {
			r = rand() % RECENT_PAGES;
			r = page_count - 1 - r;
			if (r < 0)
				r = 0;
			v = (*counter) % 0xff;
			memset(pages[r], v, 1);
			(*counter)++;
		}
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

double calc_used_mem_noswap()
{
	char *keys[] = { "MemTotal", "AnonPages", NULL };
	unsigned long vals[2] = { 0 };

	get_meminfo_vals(keys, vals);

	return ((double)(vals[1] * 100) / (double)vals[0]);
}

double calc_used_swap()
{
	char *keys[] = { "MemTotal", "SwapTotal", "SwapFree", NULL };
	unsigned long vals[3] = { 0 };
	unsigned long used;

	get_meminfo_vals(keys, vals);

	return (double)((vals[1] - vals[2]) * 100) / (double)vals[0];
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

	return ((vals[0] * MEM_INC_PCT) / 100) * 1024;
}

unsigned long inline calc_counter(unsigned long counters[], int cpus)
{
	unsigned long total = 0;
	int i = 0;
	for (; i<cpus; i++)
		total += counters[i];
	return total;
}

unsigned long inline calc_time_diff_ms(struct timespec before, struct timespec after)
{
	time_t secs = after.tv_sec - before.tv_sec;
	long nsecs = after.tv_nsec - before.tv_nsec;
	return (secs * 1000) + (nsecs / 1000000);
}

double inline pct(unsigned long n, unsigned long d)
{
	return (double)(n * 100) / (double)d;
}

double inline adj_counter_pct(unsigned long counter, unsigned long baseline, unsigned long ms) {
	double adj = (double)ms / (double)(WORK_SLEEP_TIME * 1000);
	double bl_new = (double)baseline * adj;
	return (double)(counter * 100) / bl_new;
}

void main(int argc, char *argv[])
{
	int cpus = (int)sysconf(_SC_NPROCESSORS_CONF);
	size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
	unsigned long phys_pages = sysconf(_SC_PHYS_PAGES);
	unsigned long max_pages = (phys_pages * 2); /* 2x is arbitrary */
	pages = calloc(max_pages, sizeof(void*));
	unsigned long cpu_counters[cpus];
	unsigned long mem_random_counter, mem_recent_counter;
	pthread_t mem_random_worker, mem_recent_worker;
	unsigned long init_mem_size = calc_init_mem_size();
	unsigned long inc_mem_size = calc_inc_mem_size();
	unsigned long bl_cpu_count, bl_random_count, bl_recent_count;
	unsigned long alloc_cpu_count, alloc_random_count, alloc_recent_count, alloc_ms;
	double used_mem;
	struct timespec before, after;

	printf("Getting baseline numbers\n");
	fflush(NULL);
	start_cpu_workers(cpus, cpu_counters);
	bzero(cpu_counters, cpus * sizeof(unsigned long));
	sleep(WORK_SLEEP_TIME);
	bl_cpu_count = calc_counter(cpu_counters, cpus);

	cpu_workers_pause = 1;

	page_count += alloc_pages(&pages[page_count], page_size, page_size * RECENT_PAGES);

	pthread_create(&mem_random_worker, NULL, &random_page_worker, &mem_random_counter);
	mem_random_counter = 0;
	sleep(WORK_SLEEP_TIME);
	bl_random_count = mem_random_counter;

	mem_random_pause = 1;

	pthread_create(&mem_recent_worker, NULL, &recent_page_worker, &mem_recent_counter);
	mem_recent_counter = 0;
	sleep(WORK_SLEEP_TIME);
	bl_recent_count = mem_recent_counter;

	printf("Baseline CPU %ld MEM random %ld recent %ld\n", bl_cpu_count, bl_random_count, bl_recent_count);

	printf("Allocating %ldm of initial memory\n", init_mem_size / (1024 * 1024));
	fflush(NULL);
	page_count += alloc_pages(&pages[page_count], page_size, init_mem_size);

	cpu_workers_pause = 0;
	mem_random_pause = 0;
	sleep(1);

	printf("All mem units %% of total physical mem\n");
	printf("CPU and MEMORY units %% of baseline measurement\n");
	printf("Allocation time units ms\n");
	printf("Alloc period is when new memory is being allocated\n");
	printf("Measure period is %d sleep delay to measure counters\n", WORK_SLEEP_TIME);
	printf("\n");
	printf("               |    Measure Period       |        Alloc Period             |\n");
	printf("total|used|swap| CPU |  MEMORY |  MEMORY | alloc | CPU |  MEMORY |  MEMORY |\n");
	printf(" mem | mem| mem|     |  random |  recent |  time |     |  random |  recent |\n");
	printf("----------------------------------------------------------------------------\n");
	fflush(NULL);

	do {
		if (page_count + (inc_mem_size/page_size) > max_pages) {
			printf("Too many pages allocated, exiting\n");
			break;
		}

		bzero(cpu_counters, cpus * sizeof(unsigned long));
		mem_random_counter = 0;
		mem_recent_counter = 0;
		if (clock_gettime(CLOCK_MONOTONIC_RAW, &before)) {
			printf("Error getting timestamp\n");
			break;
		}
	  page_count += alloc_pages(&pages[page_count], page_size, inc_mem_size);
		if (clock_gettime(CLOCK_MONOTONIC_RAW, &after)) {
			printf("Error getting timestamp\n");
			break;
		}
		alloc_cpu_count = calc_counter(cpu_counters, cpus);
		alloc_random_count = mem_random_counter;
		alloc_recent_count = mem_recent_counter;
		alloc_ms = calc_time_diff_ms(before, after);

		used_mem = calc_used_mem();
		bzero(cpu_counters, cpus * sizeof(unsigned long));
		mem_random_counter = 0;
		mem_recent_counter = 0;
		sleep(WORK_SLEEP_TIME);
		printf(" %3.0f | %2.0f | %2.0f | %3.0f | %7.3f | %7.3f | %5ld | %3.0f | %7.3f | %7.3f |\n",
					 used_mem,
					 calc_used_mem_noswap(),
					 calc_used_swap(),
					 pct(calc_counter(cpu_counters, cpus), bl_cpu_count),
					 pct(mem_random_counter, bl_random_count),
					 pct(mem_recent_counter, bl_recent_count),
					 alloc_ms,
					 adj_counter_pct(alloc_cpu_count, bl_cpu_count, alloc_ms),
					 adj_counter_pct(alloc_random_count, bl_random_count, alloc_ms),
					 adj_counter_pct(alloc_recent_count, bl_recent_count, alloc_ms));
		fflush(NULL);
	} while (used_mem < MAX_USED_MEM);
}


