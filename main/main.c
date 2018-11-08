/*
 * Gatekeeper - DoS protection system.
 * Copyright (C) 2016 Digirati LTDA.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdbool.h>
#include <signal.h>
#include <time.h>
#include <inttypes.h>

#include <rte_eal.h>
#include <rte_log.h>
#include <rte_common.h>
#include <rte_launch.h>
#include <rte_cycles.h>
#include <rte_timer.h>

#include "gatekeeper_main.h"
#include "gatekeeper_config.h"
#include "gatekeeper_net.h"
#include "gatekeeper_launch.h"
#include "gatekeeper_ratelimit.h"

/* Indicates whether the program needs to exit or not. */
volatile int exiting = false;

/*
 * These metrics are system dependent, and 
 * initialized via time_resolution_init() function.
 */
uint64_t cycles_per_sec;
uint64_t cycles_per_ms;
uint64_t picosec_per_cycle;

/* Obtain the system time resolution. */
static int
time_resolution_init(void)
{
	int ret;
	uint64_t diff_ns;
	uint64_t cycles;
	uint64_t tsc_start;
	struct timespec tp_start;

	tsc_start = rte_rdtsc();
	ret = clock_gettime(CLOCK_MONOTONIC_RAW, &tp_start);
	if (ret < 0)
		return ret;

	while (1) {
		uint64_t tsc_now;
		struct timespec tp_now;

		ret = clock_gettime(CLOCK_MONOTONIC_RAW, &tp_now);
		tsc_now = rte_rdtsc();
		if (ret < 0)
			return ret;

		diff_ns = (uint64_t)(tp_now.tv_sec - tp_start.tv_sec) * 1000000000UL 
				+ (uint64_t)(tp_now.tv_nsec - tp_start.tv_nsec);

		if (diff_ns >= 1000000000UL) {
			cycles = tsc_now - tsc_start;
			break;
		}
	}

	cycles_per_sec = cycles * 1000000000UL / diff_ns;
	cycles_per_ms = cycles_per_sec / 1000UL;
	picosec_per_cycle = 1000UL * diff_ns / cycles;

	RTE_LOG(NOTICE, TIMER,
		"cycles/second = %" PRIu64 ", cycles/millisecond = %" PRIu64 ", picosec/cycle = %" PRIu64 "!\n",
		cycles_per_sec, cycles_per_ms, picosec_per_cycle);

	return 0;
}

static void
signal_handler(int signum)
{
	if (signum == SIGINT)
		fprintf(stderr, "caught SIGINT\n");
	else if (signum == SIGTERM)
		fprintf(stderr, "caught SIGTERM\n");
	else
		fprintf(stderr, "caught unknown signal (%d)\n", signum);
	exiting = true;
}

static int
run_signal_handler(void)
{
	int ret = -1;
	sig_t pipe_handler;
	struct sigaction new_action;
	struct sigaction old_int_action;
	struct sigaction old_term_action;

	new_action.sa_handler = signal_handler;
	sigemptyset(&new_action.sa_mask);
	new_action.sa_flags = 0;
	
	ret = sigaction(SIGINT, &new_action, &old_int_action);
	if (ret < 0)
		goto out;

	ret = sigaction(SIGTERM, &new_action, &old_term_action);
	if (ret < 0)
		goto int_action;

	pipe_handler = signal(SIGPIPE, SIG_IGN);
	if (pipe_handler == SIG_ERR) {
		fprintf(stderr, "Error: failed to ignore SIGPIPE - %s\n",
			strerror(errno));
		goto term_action;
	}

	goto out;

term_action:
	sigaction(SIGTERM, &old_term_action, NULL);
int_action:
	sigaction(SIGINT, &old_int_action, NULL);
out:
	return ret;
}

int
main(int argc, char **argv)
{
	int ret = rte_eal_init(argc, argv);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Error with EAL initialization!\n");

	log_ratelimit_init();

	/* XXX Set the global log level. Change it as needed. */
	rte_log_set_global_level(RTE_LOG_DEBUG);

	/* Used by the LLS block. */
	rte_timer_subsystem_init();

	/* Given the nature of signal, it's okay to not have a cleanup for them. */
	ret = run_signal_handler();
	if (ret < 0)
		goto out;

	/*
	 * Given the nature of 'clock_gettime()' call, it's okay to not have a 
	 * cleanup for them.
	 */
	ret = time_resolution_init();
	if (ret < 0)
		goto out;

	ret = config_gatekeeper();
	if (ret < 0) {
		RTE_LOG(ERR, GATEKEEPER, "Failed to configure Gatekeeper!\n");
		goto net;
	}

	/*
	 * Finalize any network configuration, such as building ACL tries,
	 * after blocks have had a chance to make use of network state
	 * during stage 2. This is needed because there is no stage 3 for
	 * the network configuration.
	 */
	ret = launch_at_stage2(finalize_stage2, NULL);
	if (ret < 0)
		goto net;

	ret = launch_gatekeeper();
	if (ret < 0)
		exiting = true;

	rte_eal_mp_wait_lcore();
net:
	gatekeeper_free_network();
out:
	return ret;
}
