// SPDX-License-Identifier: GPL-2.0-only
/*
 * WrapFD kernel API
 *
 * Copyright (C) 2025 Google, Inc.
 */

#ifndef _LINUX_WRAPFD_H
#define _LINUX_WRAPFD_H

/*
 * Get sgtable of pages. Caller also gets buffer ownership.
 *
 * file: wrap file to get the folios from.
 * dev: device requesting the folios and the ownership.
 *
 * On success returns sgtable pointer. On error returns:
 * -EBADF: wrapfd is not a valid file descriptor.
 * -EBUSY: buffer is owned by someone else.
 * -ENOENT: wrap is empty (buffer got freed or moved)
 */
struct sg_table *wrapfd_get(struct file *file, struct device *dev);

/*
 * Release buffer ownership. Caller should own the buffer. Note that the
 * caller might still keep page mappings but in that case it has to raise
 * refcounts of the folios to keep them from being freed. If the buffer
 * is reused or freed then the next wrapfd_get() call will fail with -ENOENT
 * and mapped pages will have to be unmapped and refcounts of the folios to
 * be dropped. If wrapfd_get() succeeds then previous mappings are still
 * valid and can be reused.
 *
 * file: wrap file to release ownership for.
 * dev: device requesting the operation.
 * sgtbl: sgtable of pages returned by wrapfd_get().
 *
 * On success returns 0. On error returns:
 * -EBADF: wrapfd is not a valid file descriptor.
 * -EBUSY: buffer is not owned by the caller.
 */
int wrapfd_put(struct file *file, struct device *dev, struct sg_table *sgtbl);

#endif /* _LINUX_WRAPFD_H */
