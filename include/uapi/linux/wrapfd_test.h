/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Wrapfd UAPI
 *
 * Copyright (C) 2025 Google, Inc.
 */

#ifndef _UAPI_WRAPFD_TEST_H
#define _UAPI_WRAPFD_TEST_H

#include <linux/types.h>

struct wrapfd_test_get {
	__s64 wrapfd;	/* [in] file to get */
	__u64 prot;	/* [in] protection bits */
};

/* ioctls for /dev/wrapfd_test */
#define WRAPFD_TEST_DEV_IOC	0xBE
#define WRAPFD_TEST_DEV_GET	_IOW(WRAPFD_TEST_DEV_IOC, 0, struct wrapfd_test_get)

#endif /* _UAPI_WRAPFD_TEST_H */
