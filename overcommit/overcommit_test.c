
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <pthread.h>

#define VERSION 1

#define MAX_USED_MEM 120
#define MEM_INC_PCT 1

static int work_sleep_time = 5;
static int no_cpu = 0, no_random = 0, no_recent = 0;
static int cpu_workers_exit = 0, mem_random_exit = 0, mem_recent_exit = 0;
static int cpu_workers_pause = 0, mem_random_pause = 0, mem_recent_pause = 0;
static void **pages = NULL;
static unsigned long page_count = 0;
static long frontswap_loads = 0;
static unsigned long last_user = 0, last_system = 0, last_idle = 0, last_iowait = 0, last_other = 0;
static unsigned long stat_user = 0, stat_system = 0, stat_idle = 0, stat_iowait = 0, stat_other = 0;
static int random_fill_pages = 1;
static void *base_page = NULL;
static int recent_pages = 64;

void *cpu_worker(void *arg)
{
	unsigned long *counter = (unsigned long*)arg;
	int outer_counter = 1024;
	while (!cpu_workers_exit) {
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
	while (!mem_random_exit) {
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
	int i = page_count;
	while (!mem_recent_exit) {
		if (mem_recent_pause)
			sleep(1);
		else {
			if (i >= page_count)
				i = page_count - (recent_pages * 1024);
			if (i < 0)
				i = 0;
			memset(pages[i], (*counter) % 0xff, 1);
			(*counter)++;
			i++;
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

void create_base_page(size_t page_size)
{
	base_page = malloc(page_size);
	int i = 0, fillsize = 0;
	char *pg = (char *)base_page;
	switch (random_fill_pages) {
	case 1: fillsize = (page_size/2) - 64; break;
	case 2: fillsize = page_size; break;
	default: fillsize = 1; /* force allocation of phys page */ break;
	}
	for (; i<fillsize; i++)
		pg[i] = rand() % 0xff;
}

unsigned long alloc_pages(void *pages[], size_t page_size, unsigned long n)
{
	unsigned long p = 0, total = (n / page_size);
	for (; p<total; p++) {
		pages[p] = malloc(page_size);
		if (pages[p])
			memcpy(pages[p], base_page, page_size); /* force allocation of phys page */
		else
			break; /* ENOMEM */
	}
	return p;
}

long get_frontswap_loads()
{
	FILE *fp = fopen("/sys/kernel/debug/frontswap/loads", "r");
	char *line = NULL;
	size_t len = 0;
	ssize_t read;
	long last_loads = frontswap_loads;

	if ((read = getline(&line, &len, fp)) >= 0) {
		frontswap_loads = strtol(line, NULL, 10);
		free(line);
	}
	fclose(fp);
	return frontswap_loads - last_loads;
}

void get_stat_data()
{
	FILE *fp = fopen("/proc/stat", "r");
	char *line = NULL, *tmp;
	size_t len = 0;
	ssize_t read;
	unsigned long tmp_user, tmp_system, tmp_idle, tmp_iowait;

	while ((read = getline(&line, &len, fp)) >= 0) {
		if (!strncmp("cpu ", line, 4))
			break;
		free(line);
		line = NULL;
		len = 0;
	}
	if (read <= 0)
		return;
	tmp = line + 3;
	tmp_user = strtol(tmp, &tmp, 10);
	tmp_user += strtol(tmp, &tmp, 10);
	tmp_system = strtol(tmp, &tmp, 10);
	tmp_idle = strtol(tmp, &tmp, 10);
	tmp_iowait = strtol(tmp, &tmp, 10);
	tmp_system += strtol(tmp, &tmp, 10);
	tmp_system += strtol(tmp, &tmp, 10);
	free(line);
	fclose(fp);
	stat_user = tmp_user - last_user;
	stat_system = tmp_system - last_system;
	stat_idle = tmp_idle - last_idle;
	stat_iowait = tmp_iowait - last_iowait;
	last_user = tmp_user;
	last_system = tmp_system;
	last_idle = tmp_idle;
	last_iowait = tmp_iowait;
}

int get_zswap_max_pool()
{
	FILE *fp = fopen("/sys/module/zswap/parameters/max_pool_percent", "r");
	char *line = NULL, *tmp;
	size_t len = 0;
	ssize_t read;
	int max = 0;

	if (!fp)
		return 0;

	if ((read = getline(&line, &len, fp)) >= 0) {
		max = atoi(line);
		free(line);
	}

	fclose(fp);

	return max;
}

void set_zswap_max_pool(int max)
{
	FILE *fp = fopen("/sys/module/zswap/parameters/max_pool_percent", "w");
	char s[64];

	if (!fp)
		return;

	snprintf(s, 64, "%d", max);
	fputs(s, fp);

	fclose(fp);
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
	if (d == 0)
		return 0;
	return (double)(n * 100) / (double)d;
}

double inline adj_counter_pct(unsigned long counter, unsigned long baseline, unsigned long ms) {
	double adj = (double)ms / (double)(work_sleep_time * 1000);
	double bl_new = (double)baseline * adj;
	return (double)(counter * 100) / bl_new;
}

void show_help(char *argv[])
{
	printf("Usage: %s [opts]\n", argv[0]);
	printf("  -nocpu          Do not test the CPU use\n");
	printf("  -norandom       Do not test random page accesses\n");
	printf("  -norecent       Do not test recently allocated page accesses\n");
	printf("  -nofillpages    Zero-fill allocated pages\n");
	printf("  -halffillpages  Almost half fill allocated pages with random data\n");
	printf("  -fillpages      Completely fill allocated pages with random data\n");
	printf("  -worktime SECS  Sleep for SECS seconds between each new allocation round [default %d]\n",work_sleep_time);
	printf("  -recentpages N  Touch Nk most recent pages with the recent page test thread [default %dk]\n",recent_pages);
	printf("  -help           This help\n");
}

void main(int argc, char *argv[])
{
	int cpus = (int)sysconf(_SC_NPROCESSORS_CONF);
	size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
	unsigned long phys_pages = sysconf(_SC_PHYS_PAGES);
	unsigned long max_pages = (phys_pages * 2); /* 2x is arbitrary */
	unsigned long cpu_counters[cpus];
	unsigned long mem_random_counter, mem_recent_counter;
	pthread_t mem_random_worker, mem_recent_worker;
	unsigned long init_mem_size = calc_init_mem_size();
	unsigned long inc_mem_size = calc_inc_mem_size();
	unsigned long bl_cpu_count = 0, bl_random_count = 0, bl_recent_count = 0;
	unsigned long alloc_cpu_count, alloc_random_count, alloc_recent_count, alloc_ms;
	long alloc_loads;
	double used_mem;
	struct timespec before, after;
	int a;

	for (a=1; a<argc; a++) {
		if (!strcmp("-nocpu", argv[a]))
			no_cpu = 1;
		else if (!strcmp("-norandom", argv[a]))
			no_random = 1;
		else if (!strcmp("-norecent", argv[a]))
			no_recent = 1;
		else if (!strcmp("-nofillpages", argv[a]))
			random_fill_pages = 0;
		else if (!strcmp("-halffillpages", argv[a]))
			random_fill_pages = 1;
		else if (!strcmp("-fillpages", argv[a]))
			random_fill_pages = 2;
		else if (!strcmp("-worktime", argv[a]))
			work_sleep_time = atoi(argv[a++]);
		else if (!strcmp("-recentpages", argv[a]))
			recent_pages = atoi(argv[a++]);
		else if (!strcmp("-help", argv[a]) || !strcmp("-h", argv[a])) {
			show_help(argv);
			return;
		} else {
			printf("Invalid opt : %s\n",argv[a]);
			show_help(argv);
			return;
		}
	}

	pages = calloc(max_pages, sizeof(void*));

	create_base_page(page_size);

	printf("Version %d\n",VERSION);

	printf("zswap max pool pct %d\n", get_zswap_max_pool());

	printf("Getting baseline numbers\n");
	fflush(NULL);

	if (!no_cpu) {
		start_cpu_workers(cpus, cpu_counters);
		bzero(cpu_counters, cpus * sizeof(unsigned long));
		sleep(work_sleep_time);
		bl_cpu_count = calc_counter(cpu_counters, cpus);

		cpu_workers_pause = 1;
	}

	page_count += alloc_pages(&pages[page_count], page_size, page_size * recent_pages * 1024);

	if (!no_random) {
		pthread_create(&mem_random_worker, NULL, &random_page_worker, &mem_random_counter);
		mem_random_counter = 0;
		sleep(work_sleep_time);
		bl_random_count = mem_random_counter;

		mem_random_pause = 1;
	}

	if (!no_recent) {
		pthread_create(&mem_recent_worker, NULL, &recent_page_worker, &mem_recent_counter);
		mem_recent_counter = 0;
		sleep(work_sleep_time);
		bl_recent_count = mem_recent_counter;
	}

	printf("Baseline CPU %ld MEM random %ld recent %ld\n", bl_cpu_count, bl_random_count, bl_recent_count);

	printf("Allocating %ldm of initial memory\n", init_mem_size / (1024 * 1024));
	fflush(NULL);
	page_count += alloc_pages(&pages[page_count], page_size, init_mem_size);

	cpu_workers_pause = 0;
	mem_random_pause = 0;
	sleep(1);

	switch (random_fill_pages) {
	case 1: printf("Allocated pages half-filled with random numbers\n"); break;
	case 2: printf("Allocated pages filled with random numbers\n"); break;
	default: printf("Allocated pages zero-filled (except 1 byte to force physical page allocation)\n"); break;
	}
	printf("All mem units %% of total physical mem\n");
	printf("CPU and MEMORY units %% of baseline measurement\n");
	printf("Allocation time units ms\n");
	printf("Alloc period is when new memory is being allocated\n");
	printf("Recent page testing %dk most recently allocated pages\n",recent_pages);
	printf("Measure period is %d secs sleep delay to measure counters\n", work_sleep_time);
	printf("\n");
	printf("               |                     Measure Period                        |              Alloc Period                 |\n");
	printf("total|used|swap| CPU |  MEMORY |  MEMORY |  zswap  | user| sys | idle| iowt| alloc | CPU |  MEMORY |  MEMORY |  zswap  |\n");
	printf(" mem | mem| mem|     |  random |  recent |  loads  |     |     |     |     |  time |     |  random |  recent |  loads  |\n");
	printf("------------------------------------------------------------------------------------------------------------------------\n");
	fflush(NULL);

	do {
		if (page_count + (inc_mem_size/page_size) > max_pages) {
			printf("Too many pages allocated, exiting\n");
			break;
		}

		bzero(cpu_counters, cpus * sizeof(unsigned long));
		mem_random_counter = 0;
		mem_recent_counter = 0;
		get_frontswap_loads();
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
		alloc_loads = get_frontswap_loads();

		used_mem = calc_used_mem();
		bzero(cpu_counters, cpus * sizeof(unsigned long));
		mem_random_counter = 0;
		mem_recent_counter = 0;
		get_stat_data();
		sleep(work_sleep_time);
		get_stat_data();
		printf(" %3.0f | %2.0f | %2.0f | %3.0f | %7.3f | %7.3f | %7ld |%4ld |%4ld |%4ld |%4ld | %5ld | %3.0f | %7.3f | %7.3f | %7ld |\n",
					 used_mem,
					 calc_used_mem_noswap(),
					 calc_used_swap(),
					 pct(calc_counter(cpu_counters, cpus), bl_cpu_count),
					 pct(mem_random_counter, bl_random_count),
					 pct(mem_recent_counter, bl_recent_count),
					 get_frontswap_loads(),
					 stat_user,
					 stat_system,
					 stat_idle,
					 stat_iowait,
					 alloc_ms,
					 adj_counter_pct(alloc_cpu_count, bl_cpu_count, alloc_ms),
					 adj_counter_pct(alloc_random_count, bl_random_count, alloc_ms),
					 adj_counter_pct(alloc_recent_count, bl_recent_count, alloc_ms),
					 alloc_loads);
		fflush(NULL);
	} while (used_mem < MAX_USED_MEM);

	cpu_workers_exit = mem_random_exit = mem_recent_exit = 1;

}


