/*
 * MIT License
 *
 * Copyright (c) 2011-2018 Pedro Henrique Penna <pedrohenriquepenna@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.  THE SOFTWARE IS PROVIDED
 * "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT
 * LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
 * PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <assert.h>
#include <inttypes.h>
#include <string.h>
#include <stdio.h>

#include <mppaipc.h>
#include <mppa/osconfig.h>
#include <HAL/hal/core/mp.h>
#include <HAL/hal/core/diagnostic.h>

#include "../kernel.h"

/**
 * @brief Benchmark parameters.
 */
/**@{*/
static int nclusters = 0;         /**< Number of remotes processes.    */
static int niterations = 0;       /**< Number of benchmark parameters. */
static int bufsize = 0;           /**< Buffer size.                    */
static const char *kernel = NULL; /**< Benchmark kernel.               */
/**@}*/

/**
 * @brief Input portal.
 */
static int inportal;

/**
 * @brief Master sync.
 */
static int sync_master;

/**
 * @brief ID of slave processes.
 */
static int pids[NR_CCLUSTER];

/**
 * @brief Buffer.
 */
static char buffer[NR_CCLUSTER*BUFFER_SIZE_MAX];

/*============================================================================*
 * Utilities                                                                  *
 *============================================================================*/

/**
 * @brief Spawns remote processes.
 */
static void spawn_remotes(void)
{
	char niterations_str[4];
	char bufsize_str[10];
	const char *argv[] = {
		"/mppa256-portal-slave",
		niterations_str,
		bufsize_str,
		kernel,
		NULL
	};

	/* Spawn remotes. */
	sprintf(niterations_str, "%d", niterations);
	sprintf(bufsize_str, "%d", bufsize);
	for (int i = 0; i < nclusters; i++)
		assert((pids[i] = mppa_spawn(i, NULL, argv[0], argv, NULL)) != -1);
}

/**
 * @brief Wait for remote processes.
 */
static void join_remotes(void)
{
	for (int i = 0; i < nclusters; i++)
		assert(mppa_waitpid(pids[i], NULL, 0) != -1);
}

/*============================================================================*
 * Timer                                                                      *
 *============================================================================*/

/**
 * @brief Timer error.
 */
static uint64_t timer_error = 0;

/**
 * @brief Gets the current timer value.
 *
 * @returns The current timer value;
 */
static inline uint64_t timer_get(void)
{
	return (__k1_read_dsu_timestamp());
}

/**
 * @brief Computes the difference between two timer values.
 *
 * @param t1 Start time.
 * @param t2 End time.
 *
 * @returns The difference between the two timers (t2 - t1).
 */
static inline uint64_t timer_diff(uint64_t t1, uint64_t t2)
{
	return (((t2 - t1) <= timer_error) ? timer_error : t2 - t1 - timer_error);
}

/**
 * @brief Calibrates the timer.
 */
static void timer_init(void)
{
	uint64_t start, end;

	start = timer_get();
	end = timer_get();

	timer_error = (end - start);
}

/*============================================================================*
 * Gather Kernel                                                              *
 *============================================================================*/

/**
 * @brief Gather kernel.
 */
static void kernel_gather(void)
{
	int sync_fd;

	/* Open sync. */
	assert((sync_fd = mppa_open(SYNC_SLAVES, O_WRONLY)) != -1);

	/* Benchmark. */
	for (int k = 0; k <= (niterations + 1); k++)
	{
		uint64_t mask;
		mppa_aiocb_t aiocb;
		uint64_t t1, t2, t3, t4;
		double tkernel, tnetwork;

		tnetwork = 0;

		t3 = timer_get();

			for (int i = 0; i < nclusters; i++)
			{
				/* Setup read operation. */
				mppa_aiocb_ctor(&aiocb, inportal, &buffer[i*bufsize], bufsize);
				assert(mppa_aio_read(&aiocb) != -1);

				/* Unblock remotes. */
				mask = 1 << i;
				assert(mppa_ioctl(sync_fd, MPPA_TX_SET_RX_RANK, i) != -1);
				assert(mppa_write(sync_fd, &mask, sizeof(uint64_t)) != -1);

				/* Read data. */
				t1 = timer_get();
					assert(mppa_aio_wait(&aiocb) == bufsize);
				t2 = timer_get();
				tnetwork += timer_diff(t1, t2);
			}

		t4 = timer_get();

		tkernel = timer_diff(t3, t4) - tnetwork;

		/* Warmup. */
		if (((k == 0) || (k == (niterations + 1))))
			continue;

		printf("nodeos;portal;%s;%d;%d;%lf;%lf\n",
			kernel,
			bufsize,
			nclusters,
			tkernel/((double) MPPA256_FREQ),
			tnetwork/((double) MPPA256_FREQ)
		);
	}

	/* House keeping. */
	assert(mppa_close(sync_fd) != -1);
}

/*============================================================================*
 * Broadcast Kernel                                                           *
 *============================================================================*/

/**
 * @brief Broadcast kernel.
 */
static void kernel_broadcast(void)
{
	int outportal;

	assert((outportal = mppa_open(PORTAL_SLAVES, O_WRONLY)) != -1);

	/* Benchmark. */
	for (int k = 0; k <= (niterations + 1); k++)
	{
		uint64_t mask;
		double tkernel, tnetwork;
		uint64_t t1, t2, t3, t4;

		tnetwork = 0;

		t3 = timer_get();

			/* Wait for slaves. */
			assert(mppa_read(sync_master, &mask, sizeof(uint64_t)) != -1);

			/* Send data. */
			for (int i = 0; i < nclusters; i++)
			{
				assert(mppa_ioctl(outportal, MPPA_TX_SET_RX_RANK, i) != -1);

				t1 = timer_get();
					assert(mppa_pwrite(outportal, &buffer[i*bufsize], bufsize, 0) == bufsize);
				t2 = timer_get();
				tnetwork += timer_diff(t1, t2);
			}

		t4 = timer_get();

		tkernel = timer_diff(t3, t4) - tnetwork;

		/* Warmup. */
		if (((k == 0) || (k == (niterations + 1))))
			continue;

		printf("nodeos;portal;%s;%d;%d;%lf;%lf\n",
			kernel,
			bufsize,
			nclusters,
			tkernel/((double) MPPA256_FREQ),
			tnetwork/((double) MPPA256_FREQ)
		);
	}

	/* House keeping. */
	assert(mppa_close(outportal) != -1);
}

/*============================================================================*
 * MPPA-256 Portal Microbenchmark Driver                                      *
 *============================================================================*/

/**
 * @brief Portal microbenchmark.
 */
static void benchmark(void)
{
	uint64_t mask;
	
	mask = ~((1 << nclusters) - 1);

	/* Initialization. */
	assert((inportal = mppa_open(PORTAL_MASTER, O_RDONLY)) != -1);
	assert((sync_master = mppa_open(SYNC_MASTER, O_RDONLY)) != -1);
	assert(mppa_ioctl(sync_master, MPPA_RX_SET_MATCH, mask) != -1);
	spawn_remotes();

	timer_init();

	/* Run kernel. */
	if (!strcmp(kernel, "gather"))
		kernel_gather();
	else if (!strcmp(kernel, "broadcast"))
		kernel_broadcast();
	
	/* House keeping. */
	join_remotes();
	assert(mppa_close(sync_master) != -1);
	assert(mppa_close(inportal) != -1);
}

/**
 * @brief Portal Microbenchmark Driver
 */
int main(int argc, const char **argv)
{
	assert(argc == 5);

	/* Retrieve kernel parameters. */
	nclusters = atoi(argv[1]);
	niterations = atoi(argv[2]);
	bufsize = atoi(argv[3]);
	kernel = argv[4];

	/* Parameter checking. */
	assert((nclusters > 0) && (nclusters <= NR_CCLUSTER));
	assert(niterations > 0);
	assert((bufsize > 0) && (bufsize <= (BUFFER_SIZE_MAX)));
	assert((bufsize%2) == 0);

	benchmark();

	return (EXIT_SUCCESS);
}
