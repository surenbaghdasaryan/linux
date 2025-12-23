// SPDX-License-Identifier: GPL-2.0-only
/* wrapfd.c
 *
 * Wrapfd
 *
 * Copyright (C) 2025 Google, Inc.
 */

#include <linux/anon_inodes.h>
#include <linux/dma-buf.h>
#include <linux/fdtable.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/hashtable.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/scatterlist.h>
#include <linux/seq_file.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <uapi/linux/wrapfd_test.h>

#include "wrapfd.h"

static struct miscdevice wrapfd_test_misc;

struct wrap_test_ctx {
	struct sg_table *sgtbl;
	struct file *file;
};

/*
static int wrap_test_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct wrap_test_ctx *ctx = file->private_data;
	unsigned long addr = vma->vm_start;
	struct sg_page_iter piter;
	int ret;

	if (!ctx || !ctx->sgtbl)
		return -EINVAL;

	for_each_sgtable_page(ctx->sgtbl, &piter, vma->vm_pgoff) {
		struct folio *folio = page_folio(sg_page_iter_page(&piter));
		unsigned long size = folio_nr_pages(folio) * PAGE_SIZE;

		ret = remap_pfn_range(vma, addr, page_to_pfn(folio_page(folio, 0)),
				      size, vma->vm_page_prot);
		if (ret)
			return ret;

		addr += size;
		if (addr >= vma->vm_end)
			return 0;
	}

	return 0;
}
*/
static int wrap_test_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct wrap_test_ctx *ctx = file->private_data;
	struct sg_table *table = ctx->sgtbl;
	unsigned long addr = vma->vm_start;
	unsigned long pgoff = vma->vm_pgoff;
	struct scatterlist *sg;
	int i, ret;

	for_each_sgtable_sg(table, sg, i) {
		unsigned long n = sg->length >> PAGE_SHIFT;

		if (pgoff < n)
			break;
		pgoff -= n;
	}

	for (; sg && addr < vma->vm_end; sg = sg_next(sg)) {
		unsigned long n = (sg->length >> PAGE_SHIFT) - pgoff;
		struct page *page = sg_page(sg) + pgoff;
		unsigned long size = n << PAGE_SHIFT;

		if (addr + size > vma->vm_end)
			size = vma->vm_end - addr;

		ret = remap_pfn_range(vma, addr, page_to_pfn(page),
				size, vma->vm_page_prot);
		if (ret)
			return ret;

		addr += size;
		pgoff = 0;
	}

	return 0;
}

static int wrap_test_release(struct inode *ignored, struct file *file)
{
	struct wrap_test_ctx *ctx = file->private_data;
	int ret = 0;

	if (!ctx)
		return -ENOENT;

	if (ctx->sgtbl) {
		ret = wrapfd_put(ctx->file, wrapfd_test_misc.this_device,
				 ctx->sgtbl);
		fput(ctx->file);
	}

	kfree(ctx);

	return ret;
}

static const struct file_operations wrap_test_fops = {
	.owner		= THIS_MODULE,
	.mmap		= wrap_test_mmap,
	.release	= wrap_test_release,
};

static int get_wrapfd(unsigned long arg)
{
	struct wrapfd_test_get __user *user_wrapfd_test_get;
	struct wrapfd_test_get wrapfd_test_get;
	struct wrap_test_ctx *ctx;
	struct sg_table *sgtbl;
	struct file *file;
	int ret;

	user_wrapfd_test_get = (struct wrapfd_test_get __user *)arg;
	if (copy_from_user(&wrapfd_test_get, user_wrapfd_test_get,
			   sizeof(wrapfd_test_get)))
		return -EFAULT;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	file = fget(wrapfd_test_get.wrapfd);
	if (!file) {
		kfree(ctx);
		return -EBADF;
	}

	sgtbl = wrapfd_get(file, wrapfd_test_misc.this_device);
	if (IS_ERR(sgtbl)) {
		fput(file);
		kfree(ctx);
		return PTR_ERR(sgtbl);
	}
	ctx->sgtbl = sgtbl;

	ret = anon_inode_getfd("[wrapfd_test]", &wrap_test_fops, ctx,
			       wrapfd_test_get.prot);
	if (ret < 0) {
		wrapfd_put(file, wrapfd_test_misc.this_device, ctx->sgtbl);
		fput(file);
		kfree(ctx);
		return ret;
	}
	ctx->file = file;

	return ret;
}

static long wrapfd_test_dev_ioctl(struct file *file, unsigned int cmd,
				  unsigned long arg)
{
	int ret;

	VM_WARN_ON_ONCE(!current->mm);

	switch (cmd) {
	case WRAPFD_TEST_DEV_GET:
		ret = get_wrapfd(arg);
		break;
	default:
		return -ENOTTY;
	}

	return ret;
}

static const struct file_operations wrapfd_test_dev_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = wrapfd_test_dev_ioctl,
	.compat_ioctl = wrapfd_test_dev_ioctl,
	.llseek = noop_llseek,
};

static struct miscdevice wrapfd_test_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "wrapfd_test",
	.fops = &wrapfd_test_dev_fops,
};

static int __init wrapfd_test_init(void)
{
	int ret;

	ret = misc_register(&wrapfd_test_misc);
	if (ret) {
		pr_err("failed to register misc device!\n");
		return ret;
	}
	wrapfd_test_misc.this_device->bus_dma_limit =
		wrapfd_test_misc.this_device->coherent_dma_mask = 0xFFFFFFFFFFFFFFFF;
	wrapfd_test_misc.this_device->dma_mask =
		&wrapfd_test_misc.this_device->coherent_dma_mask;

	pr_info("wrapfd_test initialized\n");

	return 0;
}
device_initcall(wrapfd_test_init);
