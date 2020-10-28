// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
// Copyright (c) 2020 Wenbo Zhang
//
// Based on runqlen(8) from BCC by Brendan Gregg.
// 11-Sep-2020   Wenbo Zhang   Created this.
#include <argp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <linux/perf_event.h>
#include <asm/unistd.h>
#include <bpf/libbpf.h>
#include <bpf/bpf.h>
#include "runqlen.h"
#include "runqlen.skel.h"
#include "trace_helpers.h"

#define FREQ	99

#define max(x, y) ({				 \
	typeof(x) _max1 = (x);			 \
	typeof(y) _max2 = (y);			 \
	(void) (&_max1 == &_max2);		 \
	_max1 > _max2 ? _max1 : _max2; })

struct env {
	bool per_cpu;
	bool runqocc;
	bool timestamp;
	time_t interval;
	int times;
	bool verbose;
} env = {
	.interval = 99999999,
	.times = 99999999,
};

static volatile bool exiting;

const char *argp_program_version = "runqlen 0.1";
const char *argp_program_bug_address = "<bpf@vger.kernel.org>";
const char argp_program_doc[] =
"Summarize scheduler run queue length as a histogram.\n"
"\n"
"USAGE: runqlen [--help] [-C] [-O] [-T] [interval] [count]\n"
"\n"
"EXAMPLES:\n"
"    runqlen         # summarize run queue length as a histogram\n"
"    runqlen 1 10    # print 1 second summaries, 10 times\n"
"    runqlen -T 1    # 1s summaries and timestamps\n"
"    runqlen -O      # report run queue occupancy\n"
"    runqlen -C      # show each CPU separately\n";

static const struct argp_option opts[] = {
	{ "cpus", 'C', NULL, 0, "Print output for each CPU separately" },
	{ "runqocc", 'O', NULL, 0, "Report run queue occupancy" },
	{ "timestamp", 'T', NULL, 0, "Include timestamp on output" },
	{ "verbose", 'v', NULL, 0, "Verbose debug output" },
	{},
};

static error_t parse_arg(int key, char *arg, struct argp_state *state)
{
	static int pos_args;

	switch (key) {
	case 'v':
		env.verbose = true;
		break;
	case 'C':
		env.per_cpu = true;
		break;
	case 'O':
		env.runqocc = true;
		break;
	case 'T':
		env.timestamp = true;
		break;
	case ARGP_KEY_ARG:
		errno = 0;
		if (pos_args == 0) {
			env.interval = strtol(arg, NULL, 10);
			if (errno) {
				fprintf(stderr, "invalid internal\n");
				argp_usage(state);
			}
		} else if (pos_args == 1) {
			env.times = strtol(arg, NULL, 10);
			if (errno) {
				fprintf(stderr, "invalid times\n");
				argp_usage(state);
			}
		} else {
			fprintf(stderr,
				"unrecognized positional argument: %s\n", arg);
			argp_usage(state);
		}
		pos_args++;
		break;
	default:
		return ARGP_ERR_UNKNOWN;
	}
	return 0;
}

static int nr_cpus;

static int open_and_attach_perf_event(int freq, struct bpf_program *prog,
				struct bpf_link *links[])
{
	struct perf_event_attr attr = {
		.type = PERF_TYPE_SOFTWARE,
		.freq = 1,
		.sample_period = freq,
		.config = PERF_COUNT_SW_CPU_CLOCK,
	};
	int i, fd;

	for (i = 0; i < nr_cpus; i++) {
		fd = syscall(__NR_perf_event_open, &attr, -1, i, -1, 0);
		if (fd < 0) {
			fprintf(stderr, "failed to init perf sampling: %s\n",
				strerror(errno));
			return -1;
		}
		links[i] = bpf_program__attach_perf_event(prog, fd);
		if (libbpf_get_error(links[i])) {
			fprintf(stderr, "failed to attach perf event on cpu: "
				"%d\n", i);
			links[i] = NULL;
			close(fd);
			return -1;
		}
	}

	return 0;
}

int libbpf_print_fn(enum libbpf_print_level level,
		    const char *format, va_list args)
{
	if (level == LIBBPF_DEBUG && !env.verbose)
		return 0;
	return vfprintf(stderr, format, args);
}

static void sig_handler(int sig)
{
	exiting = true;
}

static struct hist zero;

static void print_runq_occupancy(struct runqlen_bpf__bss *bss)
{
	__u64 samples, idle = 0, queued = 0;
	struct hist hist;
	int slot, i = 0;
	float runqocc;

	do {
		hist = bss->hists[i];
		bss->hists[i] = zero;
		for (slot = 0; slot < MAX_SLOTS; slot++) {
			__u64 val = hist.slots[slot];

			if (slot == 0)
				idle += val;
			else
				queued += val;
		}
		samples = idle + queued;
		runqocc = queued * 1.0 / max(1ULL, samples);
		if (env.per_cpu)
			printf("runqocc, CPU %-3d %6.2f%%\n", i,
				100 * runqocc);
		else
			printf("runqocc: %0.2f%%\n", 100 * runqocc);
	} while (env.per_cpu && ++i < nr_cpus);
}

static void print_linear_hists(struct runqlen_bpf__bss *bss)
{
	struct hist hist;
	int i = 0;

	do {
		hist = bss->hists[i];
		bss->hists[i] = zero;
		if (env.per_cpu)
			printf("cpu = %d\n", i);
		print_linear_hist(hist.slots, MAX_SLOTS, "runqlen");
	} while (env.per_cpu && ++i < nr_cpus);
}

int main(int argc, char **argv)
{
	static const struct argp argp = {
		.options = opts,
		.parser = parse_arg,
		.doc = argp_program_doc,
	};
	struct bpf_link **links = NULL;
	struct runqlen_bpf *obj;
	struct tm *tm;
	char ts[32];
	int err, i;
	time_t t;

	err = argp_parse(&argp, argc, argv, 0, NULL, NULL);
	if (err)
		return err;

	libbpf_set_print(libbpf_print_fn);

	err = bump_memlock_rlimit();
	if (err) {
		fprintf(stderr, "failed to increase rlimit: %d\n", err);
		return 1;
	}

	obj = runqlen_bpf__open();
	if (!obj) {
		fprintf(stderr, "failed to open and/or load BPF object\n");
		return 1;
	}

	nr_cpus = libbpf_num_possible_cpus();
	if (nr_cpus > MAX_CPU_NR) {
		fprintf(stderr, "The number of cpu cores is too much, please "
			"increase MAX_CPU_NR's value and recompile");
		return 1;
	}
	links = calloc(nr_cpus, sizeof(*links));
	if (!links) {
		fprintf(stderr, "failed to alloc links\n");
		goto cleanup;
	}

	/* initialize global data (filtering options) */
	obj->rodata->targ_per_cpu = env.per_cpu;

	err = runqlen_bpf__load(obj);
	if (err) {
		fprintf(stderr, "failed to load BPF object: %d\n", err);
		goto cleanup;
	}

	if (open_and_attach_perf_event(FREQ, obj->progs.do_sample, links))
		goto cleanup;

	printf("Sampling run queue length... Hit Ctrl-C to end.\n");

	signal(SIGINT, sig_handler);

	while (1) {
		sleep(env.interval);
		printf("\n");

		if (env.timestamp) {
			time(&t);
			tm = localtime(&t);
			strftime(ts, sizeof(ts), "%H:%M:%S", tm);
			printf("%-8s\n", ts);
		}

		if (env.runqocc)
			print_runq_occupancy(obj->bss);
		else
			print_linear_hists(obj->bss);

		if (exiting || --env.times == 0)
			break;
	}

cleanup:
	for (i = 0; i < nr_cpus; i++)
		bpf_link__destroy(links[i]);
	free(links);
	runqlen_bpf__destroy(obj);

	return err != 0;
}
