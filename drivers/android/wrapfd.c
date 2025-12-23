// SPDX-License-Identifier: GPL-2.0-only
/* wrapfd.c
 *
 * Wrapfd
 *
 * Copyright (C) 2025 Google, Inc.
 */

#include <linux/anon_inodes.h>
#include <linux/bvec.h>
#include <linux/dma-buf.h>
#include <linux/fdtable.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/hashtable.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/pagemap.h>
#include <linux/scatterlist.h>
#include <linux/seq_file.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/uio.h>
#include <uapi/linux/wrapfd.h>

#include "wrapfd.h"

#define FDINFO_BUF_SIZE	100

struct wrap_ctx;
struct wrap_content;
static const struct file_operations wrap_fops;

struct wrap_content_operations {
	int (*create_wrap)(struct wrap_content *content, struct wrap_ctx *ctx);
	int (*load)(struct wrap_content *content, struct file *file,
		    unsigned long file_offs, unsigned long buf_offs,
		    unsigned long len);
	int (*mmap_prepare)(struct wrap_content *content,
			    struct vm_area_struct *vma);
	int (*mmap)(struct wrap_content *content, struct vm_area_struct *vma);
	vm_fault_t (*fault)(struct wrap_content *content,
			    struct vm_fault *vmf);
	void (*free)(struct wrap_content *content);
	struct wrap_content* (*make_writable)(struct wrap_content* content,
			      bool writable);
	bool (*is_writable)(struct wrap_content* content);
	void (*show_fdinfo)(struct wrap_content *content, char *buf,
			    size_t buf_size);
	struct sg_table *(*get_sgtable)(struct wrap_content *content,
					struct device *dev);
	void (*put_sgtable)(struct wrap_content *content,
			    struct sg_table *sgtbl);
};

/* Abstract wrap content to be embedded in a concrete content object. */
struct wrap_content {
	struct wrap_content_operations *ops;
};

/* Folios content */
struct wrap_content_folios {
	struct wrap_content content;
	struct folio **folios;
	size_t nr_pages;
	bool writable;
};

static int folios_content_create_wrap(struct wrap_content *content,
				      struct wrap_ctx *ctx)
{
	struct wrap_content_folios *folios_content;

	folios_content = container_of(content, struct wrap_content_folios,
				      content);
	return anon_inode_getfd("[wrapfd]", &wrap_fops, ctx,
				folios_content->writable ? O_RDWR : O_RDONLY);
}

static int folios_content_load(struct wrap_content *content, struct file *file,
			       unsigned long file_offs, unsigned long buf_offs,
			       unsigned long len)
{
	return -ENOMEM;
}

static int folios_content_mmap_prepare(struct wrap_content *content,
				       struct vm_area_struct *vma)
{
	if (vma->vm_flags & VM_MAYWRITE) {
		struct wrap_content_folios *folios_content;

		folios_content = container_of(content,
					      struct wrap_content_folios,
					      content);
		if (!folios_content->writable)
			return -EINVAL;
	}

	vm_flags_set(vma, VM_SHARED | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP);

	return 0;
}

static int folios_content_mmap(struct wrap_content *content,
			       struct vm_area_struct *vma)
{
	return 0;
}

static vm_fault_t folios_content_fault(struct wrap_content *content,
				       struct vm_fault *vmf)
{
	struct wrap_content_folios *folios_content;
	struct folio *folio;
	size_t i, page_offs;

	folios_content = container_of(content, struct wrap_content_folios,
				      content);

	if (vmf->pgoff >= folios_content->nr_pages)
		return VM_FAULT_SIGBUS;

	/* Find out the page number within the folio */
	page_offs = 0;
	i = vmf->pgoff;
	folio = folios_content->folios[i];
	while (i > 0) {
		i--;
		if (folio != folios_content->folios[i])
			break;
		page_offs++;
	}

	return vmf_insert_pfn(vmf->vma, vmf->address,
			      page_to_pfn(folio_page(folio, page_offs)));
}

static void folios_content_free(struct wrap_content *content)
{
	struct wrap_content_folios *folios_content;

	folios_content = container_of(content, struct wrap_content_folios,
				      content);
	if (folios_content->folios) {
		for (size_t i = 0; i < folios_content->nr_pages; ) {
			struct folio *folio = folios_content->folios[i];

			i += folio_nr_pages(folio);
			folio_put(folio);
		}
		kfree(folios_content->folios);
	}
	kfree(folios_content);
}

static struct wrap_content*
folios_content_make_writable(struct wrap_content* content, bool writable)
{
	struct wrap_content_folios *folios_content;

	folios_content = container_of(content, struct wrap_content_folios,
				      content);
	folios_content->writable = writable;

	return content;
}

static bool folios_content_is_writable(struct wrap_content* content)
{
	struct wrap_content_folios *folios_content;

	folios_content = container_of(content, struct wrap_content_folios,
				      content);

	return folios_content->writable;
}


static void folios_content_show_fdinfo(struct wrap_content *content,
				       char *buf, size_t buf_size)
{
	struct wrap_content_folios *folios_content;

	folios_content = container_of(content, struct wrap_content_folios,
				      content);
	sprintf(buf, "type:\tanon\nsize:\t%lu",
		folios_content->nr_pages << PAGE_SHIFT);
}

static struct sg_table *folios_content_get_sgtable(struct wrap_content *content,
						   struct device *dev)
{
	struct wrap_content_folios *folios_content;
	struct scatterlist *new_sg;
	struct sg_table *table;
	size_t offset = 0;
	int ret;

	folios_content = container_of(content, struct wrap_content_folios,
				      content);

	table = kmalloc(sizeof(*table), GFP_KERNEL);
	if (!table)
		return ERR_PTR(-ENOMEM);

	ret = sg_alloc_table(table, folios_content->nr_pages, GFP_KERNEL);
	if (ret) {
		kfree(table);
		return ERR_PTR(ret);
	}

	new_sg = table->sgl;
	for (size_t i = 0; i < folios_content->nr_pages; i++) {
		struct folio *folio = folios_content->folios[i];

		folio_get(folio);
		sg_set_folio(new_sg, folio, folio_size(folio), offset);
		offset += folio_size(folio);
		new_sg = sg_next(new_sg);
	}

	return table;
}

static void free_folio_table(struct sg_table *table)
{
	struct scatterlist *sg;
	int i = 0;

	for_each_sgtable_sg(table, sg, i)
		folio_put(page_folio(sg_page(sg)));
	sg_free_table(table);
}

static void folios_content_put_sgtable(struct wrap_content *content,
				       struct sg_table *sgtbl)
{
	struct wrap_content_folios *folios_content;

	folios_content = container_of(content, struct wrap_content_folios,
				      content);
	if (!sgtbl)
		return;

	free_folio_table(sgtbl);
	kfree(sgtbl);
}

static struct wrap_content_operations folios_content_ops = {
	.create_wrap		= folios_content_create_wrap,
	.load			= folios_content_load,
	.mmap_prepare		= folios_content_mmap_prepare,
	.mmap			= folios_content_mmap,
	.fault			= folios_content_fault,
	.make_writable		= folios_content_make_writable,
	.is_writable		= folios_content_is_writable,
	.free			= folios_content_free,
	.show_fdinfo		= folios_content_show_fdinfo,
	.get_sgtable		= folios_content_get_sgtable,
	.put_sgtable		= folios_content_put_sgtable,
};

static struct wrap_content *alloc_folios_content(struct file *file)
{
	struct address_space *mapping = file->f_mapping;
	struct wrap_content_folios *folios_content;
	unsigned long pg_count, nr_pages = 0;
	XA_STATE(xas, &mapping->i_pages, 0);
	unsigned long addr, size;
	struct folio **folios;
	struct folio *folio;

	if (mapping->a_ops->free_folio)
		return NULL;

	inode_lock(file->f_inode);

	/* Fault-in and mlock the content of the file. */
	size = i_size_read(file->f_inode);
	addr = vm_mmap(file, 0, size, PROT_READ, MAP_PRIVATE | MAP_LOCKED, 0);
	if (IS_ERR_VALUE(addr))
		goto out_unlock_inode;

	folios_content = kmalloc(sizeof(*folios_content), GFP_KERNEL);
	if (!folios_content)
		goto out_unmap;

	/* Copy file content into folios_content->folios. */
	pg_count = mapping->nrpages;
	folios = kmalloc(sizeof(struct folio*) * pg_count, GFP_KERNEL);
	if (!folios)
		goto out_free_content;

	filemap_invalidate_lock(mapping);
	if (unlikely(filemap_write_and_wait(mapping)))
		goto out_unlock_filemap;

	rcu_read_lock();
	xas_for_each(&xas, folio, ULONG_MAX) {
		if (xas_retry(&xas, folio))
			continue;
		if (xa_is_value(folio))
			continue;

		folio_get(folio);
		for (size_t i = 0; i < folio_nr_pages(folio); i++)
			folios[nr_pages++] = folio;

		if (nr_pages >= pg_count)
			break;
	}
	rcu_read_unlock();

	BUG_ON(nr_pages < pg_count);

	folios_content->folios = folios;
	folios_content->nr_pages = nr_pages;
	folios_content->writable = true;
	folios_content->content.ops = &folios_content_ops;

	/* We have folio references, we can let go of the file mapping. */
	truncate_inode_pages_range(mapping, 0, ULONG_MAX);
	filemap_invalidate_unlock(mapping);

	vm_munmap(addr, size);

	inode_unlock(file->f_inode);

	return &folios_content->content;

out_unlock_filemap:
	filemap_invalidate_unlock(mapping);
	kfree(folios);
out_free_content:
	kfree(folios_content);
out_unmap:
	vm_munmap(addr, size);
out_unlock_inode:
	inode_unlock(file->f_inode);

	return NULL;
}

/* Read-only file content */
struct wrap_content_rd_file {
	struct wrap_content content;
	struct file *file;
	unsigned long addr;
	unsigned long size;
};

static int rd_file_content_create_wrap(struct wrap_content *content,
				       struct wrap_ctx *ctx)
{
	struct wrap_content_rd_file *rd_file_content;
	struct file *new_file;
	struct file *file;
	int wrapfd;

	rd_file_content = container_of(content, struct wrap_content_rd_file,
				       content);
	file = rd_file_content->file;

	new_file = alloc_file_clone(file, file->f_flags, &wrap_fops);
	if (IS_ERR(new_file))
		return PTR_ERR(new_file);

	wrapfd = get_unused_fd_flags(file->f_flags);
	if (wrapfd < 0) {
		fput(new_file);
		return wrapfd;
	}

	new_file->private_data = ctx;
	fd_install(wrapfd, new_file);

	return wrapfd;
}

static int rd_file_content_load(struct wrap_content *content, struct file *file,
				unsigned long file_offs, unsigned long buf_offs,
				unsigned long len)
{
	return -ENOMEM;
}

static int rd_file_content_mmap_prepare(struct wrap_content *content,
					struct vm_area_struct *vma)
{
	struct wrap_content_rd_file *rd_file_content;

	/* Read-only mappings only */
	if (vma->vm_flags & VM_MAYWRITE)
		return -EINVAL;

	rd_file_content = container_of(content, struct wrap_content_rd_file,
				       content);
	/* Replace vm_file with the underlying one. */
	fput(vma->vm_file);
	vma->vm_file = get_file(rd_file_content->file);

	return 0;
}

static int rd_file_content_mmap(struct wrap_content *content,
				struct vm_area_struct *vma)
{
	struct wrap_content_rd_file *rd_file_content;


	rd_file_content = container_of(content, struct wrap_content_rd_file,
				       content);
	if (!vma->vm_file->f_op->mmap)
		return 0;

	return vma->vm_file->f_op->mmap(vma->vm_file, vma);
}

static vm_fault_t rd_file_content_fault(struct wrap_content *content,
					struct vm_fault *vmf)
{
	return filemap_fault(vmf);
}

static void rd_file_content_free(struct wrap_content *content)
{
	struct wrap_content_rd_file *rd_file_content;

	rd_file_content = container_of(content, struct wrap_content_rd_file,
				       content);
	/* If exit_mm() already happened all the areas are already freed. */
	if (current->mm)
		vm_munmap(rd_file_content->addr, rd_file_content->size);
	fput(rd_file_content->file);
	kfree(rd_file_content);
}

static struct wrap_content*
rd_file_content_make_writable(struct wrap_content* content, bool writable)
{
	struct wrap_content_rd_file *rd_file_content;

	if (!writable)
		return content; /* The content is already read-only. */

	rd_file_content = container_of(content, struct wrap_content_rd_file,
				       content);

	return alloc_folios_content(rd_file_content->file);
}

static bool rd_file_content_is_writable(struct wrap_content* content)
{
	return false;
}

static void rd_file_content_show_fdinfo(struct wrap_content *content,
					char *buf, size_t buf_size)
{
	struct wrap_content_rd_file *rd_file_content;

	rd_file_content = container_of(content, struct wrap_content_rd_file,
				       content);
	sprintf(buf, "type:\tfile\nsrc:\t%ld\nsize:\t%lu",
		rd_file_content->file->f_inode->i_ino, rd_file_content->size);
}

static struct sg_table *rd_file_content_get_sgtable(struct wrap_content *content,
						    struct device *dev)
{
	struct wrap_content_rd_file *rd_file_content;
	struct address_space *mapping;
	struct sg_table *table;
	int ret;

	rd_file_content = container_of(content, struct wrap_content_rd_file,
				       content);
	table = kmalloc(sizeof(*table), GFP_KERNEL);
	if (!table)
		return ERR_PTR(-ENOMEM);

	mapping = rd_file_content->file->f_mapping;
	ret = sg_alloc_table(table, mapping->nrpages, GFP_KERNEL);
	if (!ret) {
		XA_STATE(xas, &mapping->i_pages, 0);
		struct folio *folio;

		rcu_read_lock();
		xas_for_each(&xas, folio, ULONG_MAX) {
			if (xas_retry(&xas, folio))
				continue;
			if (xa_is_value(folio))
				continue;

			folio_get(folio);
			sg_set_folio(table->sgl, folio, folio_size(folio),
				     folio->index * PAGE_SIZE);
		}
		rcu_read_unlock();
	}

	return table;
}

static void rd_file_content_put_sgtable(struct wrap_content *content,
					struct sg_table *sgtbl)
{
	struct wrap_content_rd_file *rd_file_content;

	rd_file_content = container_of(content, struct wrap_content_rd_file,
				       content);
	if (!sgtbl)
		return;

	free_folio_table(sgtbl);
	kfree(sgtbl);
}

static struct wrap_content_operations rd_file_content_ops = {
	.create_wrap		= rd_file_content_create_wrap,
	.load			= rd_file_content_load,
	.mmap_prepare		= rd_file_content_mmap_prepare,
	.mmap			= rd_file_content_mmap,
	.fault			= rd_file_content_fault,
	.free			= rd_file_content_free,
	.make_writable		= rd_file_content_make_writable,
	.is_writable		= rd_file_content_is_writable,
	.show_fdinfo		= rd_file_content_show_fdinfo,
	.get_sgtable		= rd_file_content_get_sgtable,
	.put_sgtable		= rd_file_content_put_sgtable,
};

static struct wrap_content *alloc_rd_file_content(struct file *file)
{
	struct wrap_content_rd_file *rd_file_content;
	unsigned long addr;
	unsigned long size;

	rd_file_content = kmalloc(sizeof(*rd_file_content), GFP_KERNEL);
	if (!rd_file_content)
		return NULL;

	/* Fault in and mlock the content of the file */
	size = i_size_read(file->f_inode);
	addr = vm_mmap(file, 0, size, PROT_READ, MAP_PRIVATE | MAP_LOCKED, 0);
	if (IS_ERR_VALUE(addr)) {
		kfree(rd_file_content);
		return NULL;
	}

	rd_file_content->content.ops = &rd_file_content_ops;
	rd_file_content->file = get_file(file);
	rd_file_content->addr = addr;
	rd_file_content->size = size;

	return &rd_file_content->content;
}

/* dmabuf content */
struct wrap_content_dmabuf {
	struct wrap_content content;
	struct dma_buf *dmabuf;
	struct dma_buf_attachment *attachment;
	const struct vm_operations_struct *vm_ops;
	void *vm_private_data;
	bool writable;
};

static int dmabuf_content_create_wrap(struct wrap_content *content,
				      struct wrap_ctx *ctx)
{
	struct wrap_content_dmabuf *dmabuf_content;

	dmabuf_content = container_of(content, struct wrap_content_dmabuf,
				      content);
	return anon_inode_getfd("[wrapfd]", &wrap_fops, ctx,
				dmabuf_content->writable ? O_RDWR : O_RDONLY);
}

static struct miscdevice wrapfd_misc;

static unsigned int init_bio_data(struct sg_table *sgtbl,
				  size_t offset, size_t len,
				  struct bio_vec *bvec)
{
	struct scatterlist *sg;
	unsigned int count = 0;
	size_t end_offs = 0;
	unsigned int i;
	size_t sg_len;

	for_each_sg(sgtbl->sgl, sg, sgtbl->nents, i) {
		end_offs += sg->length;
		if (end_offs <= offset)
			continue;

		sg_len = end_offs - offset;
		bvec[count].bv_page = sg_page(sg);
		bvec[count].bv_offset = sg->offset + sg->length - sg_len;
		if (sg_len >= len) {
			bvec[count++].bv_len = len;
			break;
		}
		bvec[count++].bv_len = sg_len;
		offset += sg_len;
		len -= sg_len;
	}

	return count;
}

static int dmabuf_content_load(struct wrap_content *content, struct file *file,
			       unsigned long file_offs, unsigned long buf_offs,
			       unsigned long len)
{
	struct wrap_content_dmabuf *dmabuf_content;
	struct dma_buf_attachment *attachment;
	unsigned int bvec_size;
	struct sg_table *sgtbl;
	struct bio_vec *bvec;
	struct iov_iter iter;
	struct kiocb kiocb;
	int ret;

	dmabuf_content = container_of(content, struct wrap_content_dmabuf,
				      content);

	if (file_offs + len > dmabuf_content->dmabuf->size - buf_offs)
		return -EINVAL;

	attachment = dma_buf_attach(dmabuf_content->dmabuf,
				    wrapfd_misc.this_device);
	if (IS_ERR(attachment))
		return PTR_ERR(attachment);

	sgtbl = dma_buf_map_attachment(attachment, DMA_FROM_DEVICE);
	if (IS_ERR(sgtbl)) {
		dma_buf_detach(dmabuf_content->dmabuf, attachment);
		return PTR_ERR(sgtbl);
	}

	dma_buf_mangle_sg_table(sgtbl);

	bvec = kvcalloc(sgtbl->nents, sizeof(*bvec), GFP_KERNEL);
	if (!bvec) {
		dma_buf_unmap_attachment(attachment, sgtbl, DMA_FROM_DEVICE);
		dma_buf_detach(dmabuf_content->dmabuf, attachment);
		return -ENOMEM;
	}

	bvec_size = init_bio_data(sgtbl, buf_offs, len, bvec);
	iov_iter_bvec(&iter, ITER_DEST, bvec, bvec_size, len);
	init_sync_kiocb(&kiocb, file);
	kiocb.ki_pos = file_offs;
	kiocb.ki_flags |= IOCB_DIRECT;

	while (kiocb.ki_pos < file_offs + len) {
		ret = vfs_iocb_iter_read(file, &kiocb, &iter);
		if (ret <= 0)
			break;
	}

	kvfree(bvec);
	dma_buf_unmap_attachment(attachment, sgtbl, DMA_FROM_DEVICE);
	dma_buf_detach(dmabuf_content->dmabuf, attachment);

	return ret < 0 ? ret : 0;
}

static int dmabuf_content_mmap_prepare(struct wrap_content *content,
				       struct vm_area_struct *vma)
{
	struct wrap_content_dmabuf *dmabuf_content;

	dmabuf_content = container_of(content, struct wrap_content_dmabuf,
				      content);
	if (vma->vm_flags & VM_MAYWRITE) {
		if (!dmabuf_content->writable)
			return -EINVAL;
	}

	vm_flags_set(vma, VM_SHARED | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP);

	return 0;
}

static int dmabuf_content_mmap(struct wrap_content *content,
			       struct vm_area_struct *vma)
{
	struct wrap_content_dmabuf *dmabuf_content;
	const struct vm_operations_struct *orig_ops;
	void *orig_priv;
	int ret;

	dmabuf_content = container_of(content, struct wrap_content_dmabuf,
				      content);

	orig_ops = vma->vm_ops;
	orig_priv = vma->vm_private_data;
	ret = dma_buf_mmap(dmabuf_content->dmabuf, vma, 0);
	if (ret)
		return ret;

	/*
	 * dmabuf mapping might replace the original vm_ops and vm_private_data.
	 * Store the new ones and restore the original ones.
	 */
	dmabuf_content->vm_ops = vma->vm_ops;
	dmabuf_content->vm_private_data = vma->vm_private_data;
	vma->vm_ops = orig_ops;
	vma->vm_private_data = orig_priv;

	return 0;
}

static vm_fault_t dmabuf_content_fault(struct wrap_content *content,
				       struct vm_fault *vmf)
{
	struct wrap_content_dmabuf *dmabuf_content;
	void *orig_priv;
	vm_fault_t ret;

	dmabuf_content = container_of(content, struct wrap_content_dmabuf,
				      content);
	if (!dmabuf_content->vm_ops || !dmabuf_content->vm_ops->fault)
		return VM_FAULT_SIGBUS;

	orig_priv = vmf->vma->vm_private_data;
	vmf->vma->vm_private_data = dmabuf_content->vm_private_data;
	ret = dmabuf_content->vm_ops->fault(vmf);
	vmf->vma->vm_private_data = orig_priv;

	return ret;
}

static void dmabuf_content_free(struct wrap_content *content)
{
	struct wrap_content_dmabuf *dmabuf_content;

	dmabuf_content = container_of(content, struct wrap_content_dmabuf,
				      content);
	if (dmabuf_content->dmabuf)
		dma_buf_put(dmabuf_content->dmabuf);
	kfree(dmabuf_content);
}

static struct wrap_content*
dmabuf_content_make_writable(struct wrap_content* content, bool writable)
{
	struct wrap_content_dmabuf *dmabuf_content;

	dmabuf_content = container_of(content, struct wrap_content_dmabuf,
				      content);
	dmabuf_content->writable = writable;

	return content;
}

static bool dmabuf_content_is_writable(struct wrap_content* content)
{
	struct wrap_content_dmabuf *dmabuf_content;

	dmabuf_content = container_of(content, struct wrap_content_dmabuf,
				      content);

	return dmabuf_content->writable;
}


static void dmabuf_content_show_fdinfo(struct wrap_content *content,
				       char *buf, size_t buf_size)
{
	struct wrap_content_dmabuf *dmabuf_content;

	dmabuf_content = container_of(content, struct wrap_content_dmabuf,
				      content);
	sprintf(buf, "type:\tdmabuf");
}

static struct sg_table *dmabuf_content_get_sgtable(struct wrap_content *content,
						   struct device *dev)
{
	struct wrap_content_dmabuf *dmabuf_content;
	struct dma_buf_attachment *attachment;
	struct sg_table *sgtbl;

	dmabuf_content = container_of(content, struct wrap_content_dmabuf,
				      content);
	if (dmabuf_content->attachment)
		return ERR_PTR(-EBUSY);

	attachment = dma_buf_attach(dmabuf_content->dmabuf, dev);
	if (IS_ERR(attachment))
		return ERR_PTR(PTR_ERR(attachment));

	sgtbl = dma_buf_map_attachment(attachment, DMA_BIDIRECTIONAL);
	if (IS_ERR(sgtbl)) {
		dma_buf_detach(dmabuf_content->dmabuf, attachment);
		return sgtbl;
	}
	dma_buf_mangle_sg_table(sgtbl);
	dmabuf_content->attachment = attachment;

	return sgtbl;
}

static void dmabuf_content_put_sgtable(struct wrap_content *content,
				       struct sg_table *sgtbl)
{
	struct wrap_content_dmabuf *dmabuf_content;

	dmabuf_content = container_of(content, struct wrap_content_dmabuf,
				      content);
	if (!dmabuf_content->attachment)
		return;

	dma_buf_unmap_attachment(dmabuf_content->attachment, sgtbl,
				 DMA_BIDIRECTIONAL);
	dma_buf_detach(dmabuf_content->dmabuf, dmabuf_content->attachment);
	dmabuf_content->attachment = NULL;
}

static struct wrap_content_operations dmabuf_content_ops = {
	.create_wrap		= dmabuf_content_create_wrap,
	.load			= dmabuf_content_load,
	.mmap_prepare		= dmabuf_content_mmap_prepare,
	.mmap			= dmabuf_content_mmap,
	.fault			= dmabuf_content_fault,
	.make_writable		= dmabuf_content_make_writable,
	.is_writable		= dmabuf_content_is_writable,
	.free			= dmabuf_content_free,
	.show_fdinfo		= dmabuf_content_show_fdinfo,
	.get_sgtable		= dmabuf_content_get_sgtable,
	.put_sgtable		= dmabuf_content_put_sgtable,
};

static struct wrap_content *alloc_dmabuf_content(struct dma_buf *dmabuf,
						 bool writable)
{
	struct wrap_content_dmabuf *dmabuf_content;

	dmabuf_content = kmalloc(sizeof(*dmabuf_content), GFP_KERNEL);
	if (!dmabuf_content)
		return NULL;

	get_dma_buf(dmabuf);
	dmabuf_content->dmabuf = dmabuf;
	dmabuf_content->attachment = NULL;
	dmabuf_content->vm_ops = NULL;
	dmabuf_content->vm_private_data = NULL;
	dmabuf_content->writable = writable;
	dmabuf_content->content.ops = &dmabuf_content_ops;

	return &dmabuf_content->content;
}

/* Generic wrapfd */
struct wrap_owner
{
	struct task_struct *task;
	struct device *dev;
};

struct wrap_ctx {
	struct wrap_content *content;
	spinlock_t lock; /* protects all fields below */
	struct wrap_owner owner;
	bool allow_guests;
	int map_count;
};

static struct wrap_ctx *create_wrap_ctx(void)
{
	struct wrap_ctx *ctx;

	ctx = kmalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return NULL;

	spin_lock_init(&ctx->lock);
	ctx->content = NULL;
	ctx->owner.task = NULL;
	ctx->owner.dev = NULL;
	ctx->map_count = 0;
	ctx->allow_guests = false;

	return ctx;
}

static inline bool is_owner(struct wrap_ctx *ctx)
{
	assert_spin_locked(&ctx->lock);
	return ctx->owner.task || ctx->owner.dev;
}

static inline bool is_owner_task(struct wrap_ctx *ctx,
				 struct task_struct *task)
{
	assert_spin_locked(&ctx->lock);
	return ctx->owner.task && ctx->owner.task->mm == task->mm;
}

static inline bool is_owner_dev(struct wrap_ctx *ctx,
				struct device *dev)
{
	assert_spin_locked(&ctx->lock);
	return ctx->owner.dev == dev;
}

static inline int publish_wrap(struct wrap_ctx *ctx,
			       struct wrap_content *content)
{
	ctx->content = content;
	return content->ops->create_wrap(content, ctx);
}

static int can_access(struct wrap_ctx *ctx, struct task_struct *task,
		      bool check_content)
{
	assert_spin_locked(&ctx->lock);

	if (!is_owner_task(ctx, task))
		return -EBUSY;

	if (ctx->map_count > 0)
		return -EINVAL;

	if (check_content && !ctx->content)
		return -ENOENT;

	return 0;
}

static void wrap_vm_close(struct vm_area_struct *area)
{
	struct wrap_ctx *ctx = area->vm_private_data;

	spin_lock(&ctx->lock);
	ctx->map_count--;
	spin_unlock(&ctx->lock);
}

static vm_fault_t wrap_vm_fault(struct vm_fault *vmf)
{
	struct wrap_ctx *ctx = vmf->vma->vm_private_data;

	return ctx->content->ops->fault(ctx->content, vmf);
}

static const struct vm_operations_struct wrap_vm_ops = {
	.close		= wrap_vm_close,
	.fault		= wrap_vm_fault,
};

static int wrap_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct wrap_ctx *ctx = file->private_data;
	struct wrap_content *content;
	int ret = 0;

	spin_lock(&ctx->lock);
	if (!ctx->allow_guests && is_owner(ctx) &&
	    !is_owner_task(ctx, current)) {
		ret = -EBUSY;
		goto unlock;
	}

	content = ctx->content;
	if (!content) {
		ret = -ENOENT;
		goto unlock;
	}

	ret = content->ops->mmap_prepare(content, vma);
	if (!ret) {
		/*
		 * Increased map_count prevents changes in the ownership,
		 * rewrapping or emptying the content. Therefore content
		 * is stable.
		 */
		ctx->map_count++;
		vma->vm_ops = &wrap_vm_ops;
		vma->vm_private_data = ctx;
	}
unlock:
	spin_unlock(&ctx->lock);

	if (!ret) {
		ret = content->ops->mmap(content, vma);
		if (ret) {
			spin_lock(&ctx->lock);
			ctx->map_count--;
			spin_unlock(&ctx->lock);
		}
	}

	return ret;
}

static int wrap_release(struct inode *ignored, struct file *file)
{
	struct wrap_ctx *ctx = file->private_data;

	if (ctx->content)
		ctx->content->ops->free(ctx->content);
	kfree(ctx);

	return 0;
}

static int wrap_file_get(struct wrap_ctx *ctx)
{
	int ret = 0;

	spin_lock(&ctx->lock);

	if (is_owner_task(ctx, current))
		goto unlock;

	if (is_owner(ctx)) {
		ret = -EBUSY;
		goto unlock;
	}

	if (ctx->map_count > 0) {
		ret = -EINVAL;
		goto unlock;
	}

	if (!ctx->content) {
		ret = -ENOENT;
		goto unlock;
	}

	ctx->owner.task = current;
unlock:
	spin_unlock(&ctx->lock);

	return ret;
}

static int wrap_file_put(struct wrap_ctx *ctx)
{
	int ret = 0;

	spin_lock(&ctx->lock);

	ret = can_access(ctx, current, false);
	if (ret)
		goto unlock;

	ctx->owner.task = NULL;
	ctx->allow_guests = false;
unlock:
	spin_unlock(&ctx->lock);

	return ret;
}

static int wrap_file_load(struct wrap_ctx *ctx,
			  struct wrapfd_load __user *user_wrapfd_load)
{
	struct wrapfd_load wrapfd_load;
	struct file *file;
	int ret = 0;

	if (copy_from_user(&wrapfd_load, user_wrapfd_load,
			   sizeof(wrapfd_load)))
		return -EFAULT;

	file = fget(wrapfd_load.fd);
	if (!file)
		return -EBADF;

	if (!(file->f_mode & FMODE_READ))
		return -EBADF;

	if (!file->f_op->read_iter)
		return -EINVAL;

	if (!(file->f_mode & FMODE_CAN_READ))
		return -EINVAL;

	if (!(file->f_mode & FMODE_CAN_ODIRECT))
		return -EINVAL;

	if (!PAGE_ALIGNED(wrapfd_load.file_offs))
		return -EINVAL;

	if (!PAGE_ALIGNED(wrapfd_load.buf_offs))
		return -EINVAL;

	if (wrapfd_load.file_offs + wrapfd_load.len >
	    i_size_read(file_inode(file)))
		return -EINVAL;

	/* Align the size to the page boundary */
	wrapfd_load.len = PAGE_ALIGN(wrapfd_load.len);

	spin_lock(&ctx->lock);
	ret = can_access(ctx, current, true);
	spin_unlock(&ctx->lock);

	if (!ret)
		ret = ctx->content->ops->load(ctx->content, file,
					      wrapfd_load.file_offs,
					      wrapfd_load.buf_offs,
					      wrapfd_load.len);
	fput(file);

	return ret;
}

static int wrap_file_rewrap(struct wrap_ctx *ctx,
			    struct wrapfd_rewrap __user *user_wrapfd_rewrap)
{
	struct wrapfd_rewrap wrapfd_rewrap;
	struct wrap_content *new_content;
	struct wrap_content *content;
	struct wrap_ctx *new_ctx;
	int ret = 0;

	if (copy_from_user(&wrapfd_rewrap, user_wrapfd_rewrap,
			   sizeof(wrapfd_rewrap)))
		return -EFAULT;

	if (wrapfd_rewrap.prot & ~(PROT_WRITE | PROT_READ))
		return -EINVAL;

	spin_lock(&ctx->lock);
	ret = can_access(ctx, current, true);
	if (!ret) {
		content = ctx->content;
		ctx->content = NULL;
	}
	spin_unlock(&ctx->lock);

	if (ret)
		goto out;

	new_content = content->ops->make_writable(content,
				(wrapfd_rewrap.prot & PROT_WRITE) != 0);
	if (!new_content) {
		ret = -ENOMEM;
		goto restore_content;
	}

	new_ctx = create_wrap_ctx();
	if (!new_ctx) {
		ret = -ENOMEM;
		goto free_new_content;
	}

	ret = publish_wrap(new_ctx, new_content);
	if (ret < 0)
		goto free_new_ctx;

	if (new_content != content)
		content->ops->free(content);

	return ret;

free_new_ctx:
	kfree(new_ctx);
free_new_content:
	if (new_content != content)
		new_content->ops->free(new_content);
restore_content:
	/*
	 * Restore original wrap. We are the owner and the wrap
	 * is empty, so it could not have changed from under us.
	 */
	spin_lock(&ctx->lock);
	ctx->content = content;
	spin_unlock(&ctx->lock);
out:
	return ret;
}


static int wrap_file_empty(struct wrap_ctx *ctx)
{
	struct wrap_content *content;
	int ret = 0;

	spin_lock(&ctx->lock);

	ret = can_access(ctx, current, true);
	if (ret)
		goto unlock;

	content = ctx->content;
	ctx->content = NULL;
unlock:
	spin_unlock(&ctx->lock);

	if (!ret)
		content->ops->free(content);

	return ret;
}

static int wrap_file_allow_guests(struct wrap_ctx *ctx, bool allow)
{
	int ret = 0;

	spin_lock(&ctx->lock);

	ret = can_access(ctx, current, true);
	if (ret)
		goto unlock;

	ctx->allow_guests = allow;
unlock:
	spin_unlock(&ctx->lock);

	return ret;
}

static long wrap_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct wrap_ctx *ctx = file->private_data;
	long ret;

	switch (cmd) {
	case WRAPFD_DEV_IOC_GET:
		ret = wrap_file_get(ctx);
		break;
	case WRAPFD_DEV_IOC_PUT:
		ret = wrap_file_put(ctx);
		break;
	case WRAPFD_DEV_IOC_LOAD:
		ret = wrap_file_load(ctx, (struct wrapfd_load __user *)arg);
		break;
	case WRAPFD_DEV_IOC_REWRAP:
		ret = wrap_file_rewrap(ctx,
				       (struct wrapfd_rewrap __user *)arg);
		break;
	case WRAPFD_DEV_IOC_EMPTY:
		ret = wrap_file_empty(ctx);
		break;
	case WRAPFD_DEV_IOC_ALLOW_GUESTS:
		ret = wrap_file_allow_guests(ctx, true);
		break;
	case WRAPFD_DEV_IOC_PROHIBIT_GUESTS:
		ret = wrap_file_allow_guests(ctx, false);
		break;
	default:
		return -ENOTTY;
	}

	return ret;
}

#ifdef CONFIG_PROC_FS
static void wrap_show_fdinfo(struct seq_file *m, struct file *file)
{
	struct wrap_ctx *ctx = file->private_data;
	char buf[FDINFO_BUF_SIZE];
	char *pos = buf;

	spin_lock(&ctx->lock);
	if (ctx->owner.task) {
		pos += sprintf(pos, "owner:\t%d\n", ctx->owner.task->pid);
	} else {
		if (ctx->owner.dev)
			pos += sprintf(pos, "owner:\t<device>\n");
		else
			pos += sprintf(pos, "owner:\t<none>\n");
	}
	pos += sprintf(pos, "guests:\t%s\n", ctx->allow_guests ? "yes" : "no");
	pos += sprintf(pos, "maps:\t%d\n", ctx->map_count);
	pos += sprintf(pos, "empty:\t%s\n", ctx->content ? "no" : "yes");
	if (ctx->content) {
		struct wrap_content *content = ctx->content;

		pos += sprintf(pos, "rdonly:\t%s\n",
			       content->ops->is_writable(content) ?
					"no" : "yes");
		content->ops->show_fdinfo(content, pos,
					  FDINFO_BUF_SIZE - (pos - buf));
	}
	spin_unlock(&ctx->lock);
}
#endif

struct sg_table *wrapfd_get(struct file *file, struct device *dev)
{
	struct sg_table *sgtbl;
	struct wrap_ctx *ctx;
	int ret;

	if (file->f_op != &wrap_fops)
		return ERR_PTR(-EBADF);

	ctx = file->private_data;

	spin_lock(&ctx->lock);

	if (is_owner(ctx) && !is_owner_dev(ctx, dev)) {
		ret = -EBUSY;
		goto unlock;
	}

	if (ctx->map_count > 0) {
		ret = -EINVAL;
		goto unlock;
	}

	if (!ctx->content) {
		ret = -ENOENT;
		goto unlock;
	}

	ctx->owner.dev = dev;
	/* Device is the owner, context can't change from under us. */
	ret = 0;
unlock:
	spin_unlock(&ctx->lock);

	if (ret)
		return ERR_PTR(ret);

	sgtbl = ctx->content->ops->get_sgtable(ctx->content, dev);
	if (IS_ERR(sgtbl)) {
		spin_lock(&ctx->lock);
		ctx->owner.dev = NULL;
		spin_unlock(&ctx->lock);
	}

	return sgtbl;
}

int wrapfd_put(struct file *file, struct device *dev, struct sg_table *sgtbl)
{
	struct wrap_ctx *ctx;
	int ret;

	if (file->f_op != &wrap_fops)
		return -EBADF;

	ctx = file->private_data;

	spin_lock(&ctx->lock);

	if (!is_owner_dev(ctx, dev)) {
		ret = -EBUSY;
		goto unlock;
	}

	ctx->owner.dev = NULL;
	ret = 0;
unlock:
	spin_unlock(&ctx->lock);

	if (!ret)
		ctx->content->ops->put_sgtable(ctx->content, sgtbl);

	return ret;
}

static const struct file_operations wrap_fops = {
	.owner		= THIS_MODULE,
	.mmap		= wrap_mmap,
	.release	= wrap_release,
	.unlocked_ioctl	= wrap_ioctl,
	.compat_ioctl	= wrap_ioctl,
#ifdef CONFIG_PROC_FS
	.show_fdinfo	= wrap_show_fdinfo,
#endif
};

static struct wrap_content *create_content_for(int fd, unsigned long prot)
{
	struct wrap_content *content = ERR_PTR(-EINVAL);
	struct dma_buf *dmabuf;

	dmabuf = dma_buf_get(fd);
	if (!IS_ERR(dmabuf)) {
		bool writable = !!(prot & PROT_WRITE);

		content = alloc_dmabuf_content(dmabuf, writable);
		dma_buf_put(dmabuf);
	} else {
		struct file *file;

		if (PTR_ERR(dmabuf) != -EINVAL)
			return ERR_PTR(PTR_ERR(dmabuf));

		file = fget(fd);
		if (!file)
			return ERR_PTR(-EBADF);

		/* File should be read-only */
		if ((file->f_flags & O_ACCMODE) != O_RDONLY) {
			fput(file);
			return ERR_PTR(-EPERM);
		}

		content = (prot & PROT_WRITE) ?
				alloc_folios_content(file) :
				alloc_rd_file_content(file);
		fput(file);
	}

	return content;
}

static int wrap_file(struct wrap_ctx *ctx,
		     struct wrapfd_wrap __user *user_wrapfd_wrap)
{
	struct wrapfd_wrap wrapfd_wrap;
	struct wrap_content *content;
	int wrapfd;

	if (copy_from_user(&wrapfd_wrap, user_wrapfd_wrap,
			   sizeof(wrapfd_wrap)))
		return -EFAULT;

	if (wrapfd_wrap.prot & ~(PROT_WRITE | PROT_READ))
		return -EINVAL;

	content = create_content_for(wrapfd_wrap.fd, wrapfd_wrap.prot);
	if (IS_ERR(content))
		return PTR_ERR(content);

	wrapfd = publish_wrap(ctx, content);
	if (wrapfd < 0) {
		ctx->content = NULL;
		content->ops->free(content);
	}

	return wrapfd;
}

static int get_wrap_state(struct wrapfd_get_state __user *user_wrapfd_get_state)
{
	struct wrapfd_get_state wrapfd_get_state;
	struct wrap_ctx *ctx;
	struct file *file;

	if (copy_from_user(&wrapfd_get_state, user_wrapfd_get_state,
			   sizeof(wrapfd_get_state)))
		return -EFAULT;

	file = fget(wrapfd_get_state.fd);
	if (!file)
		return -EBADF;

	if (file->f_op != &wrap_fops) {
		fput(file);
		return -EINVAL;
	}

	ctx = file->private_data;
	if (ctx->content) {
		if (ctx->content->ops->is_writable(ctx->content))
			wrapfd_get_state.state = WRAPFD_CONTENT_RDWR;
		else
			wrapfd_get_state.state = WRAPFD_CONTENT_RDONLY;
	} else {
		wrapfd_get_state.state = WRAPFD_CONTENT_EMPTY;
	}

	fput(file);

	if (copy_to_user(user_wrapfd_get_state, &wrapfd_get_state,
			 sizeof(wrapfd_get_state)))
		return -EFAULT;

	return 0;
}

static long wrapfd_dev_ioctl(struct file *file, unsigned int cmd,
			     unsigned long arg)
{
	struct wrap_ctx *ctx;
	int ret;

	VM_WARN_ON_ONCE(!current->mm);

	switch (cmd) {
	case WRAPFD_DEV_IOC_WRAP:
		ctx = create_wrap_ctx();
		if (!ctx)
			return -ENOMEM;

		ret = wrap_file(ctx, (struct wrapfd_wrap __user *)arg);
		if (ret < 0)
			kfree(ctx);

		break;
	case WRAPFD_DEV_IOC_GET_STATE:
		ret = get_wrap_state((struct wrapfd_get_state __user *)arg);
		break;
	default:
		return -ENOTTY;
	}

	return ret;
}

static const struct file_operations wrapfd_dev_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = wrapfd_dev_ioctl,
	.compat_ioctl = wrapfd_dev_ioctl,
	.llseek = noop_llseek,
};

static struct miscdevice wrapfd_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "wrapfd",
	.fops = &wrapfd_dev_fops,
};

static int __init wrapfd_init(void)
{
	int ret;

	ret = misc_register(&wrapfd_misc);
	if (ret) {
		pr_err("failed to register misc device!\n");
		return ret;
	}
	wrapfd_misc.this_device->bus_dma_limit =
		wrapfd_misc.this_device->coherent_dma_mask = 0xFFFFFFFFFFFFFFFF;
	wrapfd_misc.this_device->dma_mask =
		&wrapfd_misc.this_device->coherent_dma_mask;

	return 0;
}
device_initcall(wrapfd_init);
