/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Wrapfd UAPI
 *
 * Copyright (C) 2025 Google, Inc.
 */

#ifndef _UAPI_WRAPFD_H
#define _UAPI_WRAPFD_H

#include <linux/types.h>
#include <stdbool.h>

struct wrapfd_wrap {
	__s64 fd;		/* [in] file to wrap */
	__u64 prot;		/* [in] protection bits */
};

#define WRAPFD_CONTENT_EMPTY	0
#define WRAPFD_CONTENT_RDONLY	1
#define WRAPFD_CONTENT_RDWR	2

struct wrapfd_get_state {
	__s64 fd;		/* [in] wrapfd to get info */
	__u64 state;		/* [out] wrapfd content state */
};

struct wrapfd_load {
	__s64 fd;		/* [in] file to load */
	__u64 file_offs;	/* [in] file offset */
	__u64 buf_offs;		/* [in] buffer offset */
	__u64 len;		/* [in] number of bytes to load */
};

struct wrapfd_rewrap {
	__u64 prot;		/* [in] protection bits */
};

/* ioctls for /dev/wrapfd */
#define WRAPFD_DEV_IOC 0xBC
#define WRAPFD_DEV_IOC_WRAP	_IOW(WRAPFD_DEV_IOC, 0, struct wrapfd_wrap)
#define WRAPFD_DEV_IOC_GET_STATE	_IOWR(WRAPFD_DEV_IOC, 1, \
					      struct wrapfd_get_state)

/* ioctl for wrapfd */
#define WRAPFD_DEV_IOC_GET	_IO(WRAPFD_DEV_IOC, 2)
#define WRAPFD_DEV_IOC_PUT	_IO(WRAPFD_DEV_IOC, 3)
#define WRAPFD_DEV_IOC_LOAD	_IOW(WRAPFD_DEV_IOC, 4, struct wrapfd_load)
#define WRAPFD_DEV_IOC_REWRAP	_IOW(WRAPFD_DEV_IOC, 5, struct wrapfd_rewrap)
#define WRAPFD_DEV_IOC_EMPTY	_IO(WRAPFD_DEV_IOC, 6)
#define WRAPFD_DEV_IOC_ALLOW_GUESTS	_IO(WRAPFD_DEV_IOC, 7)
#define WRAPFD_DEV_IOC_PROHIBIT_GUESTS	_IO(WRAPFD_DEV_IOC, 8)

static inline int wrapfd_wrap(int dev_fd, int fd, unsigned int prot)
{
	struct wrapfd_wrap wrap = {
		.fd = fd,
		.prot = prot,
	};

	return ioctl(dev_fd, WRAPFD_DEV_IOC_WRAP, &wrap);
}

static inline int wrapfd_get_state(int dev_fd, int fd, unsigned long *state)
{
	struct wrapfd_get_state wrap_state = {
		.fd = fd,
	};
	int ret;

	ret = ioctl(dev_fd, WRAPFD_DEV_IOC_GET_STATE, &wrap_state);
	if (!ret && state)
		*state = wrap_state.state;

	return ret;
}

static inline int wrapfd_get(int wrapfd)
{
	return ioctl(wrapfd, WRAPFD_DEV_IOC_GET, NULL);
}

static inline int wrapfd_put(int wrapfd)
{
	return ioctl(wrapfd, WRAPFD_DEV_IOC_PUT, NULL);
}

static inline int wrapfd_load(int wrapfd, int fd, unsigned long file_offs,
			      unsigned long buf_offs, unsigned long len)
{
	struct wrapfd_load load = {
		.fd = fd,
		.file_offs = file_offs,
		.buf_offs = buf_offs,
		.len = len,
	};

	return ioctl(wrapfd, WRAPFD_DEV_IOC_LOAD, &load);
}

static inline int wrapfd_rewrap(int wrapfd, unsigned int prot)
{
	struct wrapfd_rewrap rewrap = {
		.prot = prot,
	};

	return ioctl(wrapfd, WRAPFD_DEV_IOC_REWRAP, &rewrap);
}

static inline int wrapfd_empty(int wrapfd)
{
	return ioctl(wrapfd, WRAPFD_DEV_IOC_EMPTY, NULL);
}

static inline int wrapfd_allow_guests(int wrapfd)
{
	return ioctl(wrapfd, WRAPFD_DEV_IOC_ALLOW_GUESTS, NULL);
}

static inline int wrapfd_prohibit_guests(int wrapfd)
{
	return ioctl(wrapfd, WRAPFD_DEV_IOC_PROHIBIT_GUESTS, NULL);
}

struct wrapfd_test_get {
	__s64 wrapfd;	/* [in] wrap file to get */
	__u64 prot;	/* [in] protection bits */
};

/* ioctls for /dev/wrapfd_test */
#define WRAPFD_TEST_DEV_IOC	0xBE
#define WRAPFD_TEST_DEV_GET	_IOW(WRAPFD_TEST_DEV_IOC, 0, struct wrapfd_test_get)

static inline int wrapfd_test_get(int test_dev_fd, int wrapfd, unsigned int prot)
{
	struct wrapfd_test_get get = {
		.wrapfd = wrapfd,
		.prot = prot,
	};

	return ioctl(test_dev_fd, WRAPFD_TEST_DEV_GET, &get);
}

#endif /* _UAPI_WRAPFD_H */
