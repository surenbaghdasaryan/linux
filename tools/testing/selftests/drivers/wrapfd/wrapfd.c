// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2025 Google LLC.
 * Author: Suren Baghdasaryan <surenb@google.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
/*
 * WrapFD API tests
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <linux/dma-heap.h>

#include "kselftest_harness.h"
#include "wrapfd.h"

#define FILE_SZ_PAGES	100

static void generate_file_content(char *content, size_t size)
{
	srand(time(NULL));
	/* Generate only printable characters in the range of [32, 126] */
	for (size_t i = 0; i < size; i++)
		content[i] = 32 + rand() % 95;
}

static int dmabuf_heap_alloc(int heap_fd, size_t len)
{
	struct dma_heap_allocation_data data = {
		.len = len,
		.fd = 0,
		.fd_flags = O_RDWR | O_CLOEXEC,
		.heap_flags = 0,
	};
	int ret = ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &data);

	return ret < 0 ? ret : data.fd;
}

FIXTURE(wrapfd_tests)
{
	size_t page_size;
	char *content;
	bool verbose;
	size_t size;
	int dev_fd;
	int test_dev_fd;
	int fd;
};

FIXTURE_SETUP(wrapfd_tests)
{
	const char *verbose = getenv("VERBOSE");
	ssize_t total_wr;
	FILE *ftmp;

	self->page_size = (size_t)sysconf(_SC_PAGESIZE);
	self->verbose = verbose && !strncmp(verbose, "1", 1);
	self->size = self->page_size * FILE_SZ_PAGES;

	self->dev_fd = open("/dev/wrapfd", O_RDONLY);
	ASSERT_TRUE(self->dev_fd >= 0);

	self->test_dev_fd = open("/dev/wrapfd_test", O_RDONLY);
	ASSERT_TRUE(self->test_dev_fd >= 0);

	/* Prepare random content buffer */
	self->content = malloc(self->size);
	generate_file_content(self->content, self->size);

	/* Prepare temporary file with the same content */
	ftmp = tmpfile();
	ASSERT_NE(ftmp, NULL);
	self->fd = dup(fileno(ftmp));
	fclose(ftmp);

	total_wr = 0;
	do {
		ssize_t wr = write(self->fd, self->content + total_wr,
				   self->size - total_wr);
		ASSERT_TRUE(wr >= 0);
		total_wr += wr;
	} while (total_wr < self->size);
}

FIXTURE_TEARDOWN(wrapfd_tests)
{
	close(self->fd);
	close(self->test_dev_fd);
	close(self->dev_fd);
}

static void test_wrap(struct __test_metadata *_metadata,
		      FIXTURE_DATA(wrapfd_tests) *self, int fd)
{
	int wrapfd;

	/* Get state of a non-wrapped fd */
	ASSERT_TRUE(wrapfd_get_state(self->dev_fd, fd, NULL) &&
		    errno == EINVAL);
	ASSERT_TRUE(wrapfd_get_state(self->dev_fd, self->dev_fd, NULL) &&
		    errno == EINVAL);

	/* Wrap and fet state of a wrapped fd */
	wrapfd = wrapfd_wrap(self->dev_fd, fd, PROT_READ);
	ASSERT_TRUE(wrapfd >= 0);
	ASSERT_EQ(wrapfd_get_state(self->dev_fd, wrapfd, NULL), 0);
	close(wrapfd);
}

static int cmp_content(struct __test_metadata *_metadata,
		       FIXTURE_DATA(wrapfd_tests) *self, int wrapfd)
{
	char *ptr;
	int ret;

	ptr = mmap(NULL, self->size, PROT_READ, MAP_SHARED, wrapfd, 0);
	ASSERT_NE(ptr, MAP_FAILED);
	ret = memcmp(self->content, ptr, self->size);
	ASSERT_EQ(munmap(ptr, self->size), 0);

	return ret;
}

static void clear_content(struct __test_metadata *_metadata,
			  FIXTURE_DATA(wrapfd_tests) *self, int wrapfd)
{
	char *ptr;

	ptr = mmap(NULL, self->size, PROT_READ | PROT_WRITE, MAP_SHARED,
		   wrapfd, 0);
	ASSERT_NE(ptr, MAP_FAILED);
	memset(ptr, 0, self->size);
	ASSERT_EQ(munmap(ptr, self->size), 0);
}

static void test_load(struct __test_metadata *_metadata,
		      FIXTURE_DATA(wrapfd_tests) *self, int fd)
{
	int wrapfd;

	/* Load the file content first */
	wrapfd = wrapfd_wrap(self->dev_fd, fd, PROT_READ | PROT_WRITE);
	ASSERT_TRUE(wrapfd >= 0);
	ASSERT_EQ(wrapfd_get(wrapfd), 0);

	clear_content(_metadata, self, wrapfd);
	ASSERT_NE(cmp_content(_metadata, self, wrapfd), 0);
	ASSERT_EQ(wrapfd_load(wrapfd, self->fd, 0, 0, self->size), 0);
	ASSERT_EQ(cmp_content(_metadata, self, wrapfd), 0);
	/* TODO: test more load offsets */

	ASSERT_EQ(wrapfd_put(wrapfd), 0);
	close(wrapfd);
}

static void test_owner(struct __test_metadata *_metadata,
		       FIXTURE_DATA(wrapfd_tests) *self, int fd)
{
	int wrapfd;
	char *ptr;

	wrapfd = wrapfd_wrap(self->dev_fd, fd, PROT_READ);
	ASSERT_TRUE(wrapfd >= 0);

	/* Try taking ownership of a mapped wrapfd */
	ptr = mmap(NULL, self->size, PROT_READ, MAP_SHARED | MAP_POPULATE,
		   wrapfd, 0);
	ASSERT_NE(ptr, MAP_FAILED);
	ASSERT_TRUE(wrapfd_get(wrapfd) && errno == EINVAL);
	ASSERT_EQ(munmap(ptr, self->size), 0);

	/* Take ownership of an unmapped wrapfd */
	ASSERT_EQ(wrapfd_get(wrapfd), 0);

	/* Map owner wrapfd */
	ptr = mmap(NULL, self->size, PROT_READ, MAP_SHARED | MAP_POPULATE,
		   wrapfd, 0);
	ASSERT_NE(ptr, MAP_FAILED);

	/* Try releasing ownrtship while still mapped */
	ASSERT_TRUE(wrapfd_put(wrapfd) && errno == EINVAL);
	ASSERT_EQ(munmap(ptr, self->size), 0);

	/* Release ownership of an unmapped wrapfd */
	ASSERT_EQ(wrapfd_put(wrapfd), 0);
	close(wrapfd);
}

static void test_rewrap(struct __test_metadata *_metadata,
			FIXTURE_DATA(wrapfd_tests) *self, int fd)
{
	/* TODO */
}

static void test_empty(struct __test_metadata *_metadata,
		       FIXTURE_DATA(wrapfd_tests) *self, int fd)
{
	/* TODO */
}

static void test_guests(struct __test_metadata *_metadata,
			FIXTURE_DATA(wrapfd_tests) *self, int fd)
{
	/* TODO */
}

static void test_dup(struct __test_metadata *_metadata,
		     FIXTURE_DATA(wrapfd_tests) *self, int fd)
{
	/* TODO */
}

static void test_wrap_rdonly(struct __test_metadata *_metadata,
			     FIXTURE_DATA(wrapfd_tests) *self, int fd)
{
	/* TODO */
}

static void test_wrap_rdwr(struct __test_metadata *_metadata,
			   FIXTURE_DATA(wrapfd_tests) *self, int fd)
{
	/* TODO */
}

static void test_kernel_api(struct __test_metadata *_metadata,
			    FIXTURE_DATA(wrapfd_tests) *self, int fd)
{
	int wrapfd;
	int testfd;

	wrapfd = wrapfd_wrap(self->dev_fd, fd, PROT_READ | PROT_WRITE);
	ASSERT_TRUE(wrapfd >= 0);

	/* Clear buffer content */
	ASSERT_EQ(wrapfd_get(wrapfd), 0);
	clear_content(_metadata, self, wrapfd);
	ASSERT_EQ(wrapfd_put(wrapfd), 0);

	/* Map via the test driver and check the content */
	testfd = wrapfd_test_get(self->test_dev_fd, wrapfd, O_RDWR);
	ASSERT_TRUE(testfd >= 0);
	ASSERT_NE(cmp_content(_metadata, self, testfd), 0);
	close(testfd);

	/* Load buffer content from the file */
	ASSERT_EQ(wrapfd_get(wrapfd), 0);
	ASSERT_EQ(wrapfd_load(wrapfd, self->fd, 0, 0, self->size), 0);
	ASSERT_EQ(wrapfd_put(wrapfd), 0);

	/* Map via the test driver and check the content */
	testfd = wrapfd_test_get(self->test_dev_fd, wrapfd, O_RDWR);
	ASSERT_TRUE(testfd >= 0);
	ASSERT_EQ(cmp_content(_metadata, self, testfd), 0);
	close(testfd);

	close(wrapfd);
}

static void run_tests(struct __test_metadata *_metadata,
		      FIXTURE_DATA(wrapfd_tests) *self, int fd)
{
	test_wrap(_metadata, self, fd);
	test_load(_metadata, self, fd);
	test_owner(_metadata, self, fd);
	test_rewrap(_metadata, self, fd);
	test_empty(_metadata, self, fd);
	test_guests(_metadata, self, fd);
	test_dup(_metadata, self, fd);
	test_wrap_rdonly(_metadata, self, fd);
	test_wrap_rdwr(_metadata, self, fd);
	test_kernel_api(_metadata, self, fd);
}

TEST_F(wrapfd_tests, wrapfd_test_dmabuf_system_heap)
{
	int dmabuf_fd;
	int heap_fd;

	/* Prepare system dmabuf */
	heap_fd = open("/dev/dma_heap/system", O_RDONLY);
	ASSERT_TRUE(heap_fd >= 0);
	dmabuf_fd = dmabuf_heap_alloc(heap_fd, self->size);
	ASSERT_TRUE(dmabuf_fd >= 0);
	close(heap_fd);
	run_tests(_metadata, self, dmabuf_fd);
	close(dmabuf_fd);
}

TEST_F(wrapfd_tests, wrapfd_test_dmabuf_cma_heap)
{
	int dmabuf_fd;
	int heap_fd;

	/* Prepare system dmabuf */
	heap_fd = open("/dev/dma_heap/default_cma_region", O_RDONLY);
	ASSERT_TRUE(heap_fd >= 0);
	dmabuf_fd = dmabuf_heap_alloc(heap_fd, self->size);
	ASSERT_TRUE(dmabuf_fd >= 0);
	close(heap_fd);
	run_tests(_metadata, self, dmabuf_fd);
	close(dmabuf_fd);
}

TEST_HARNESS_MAIN
