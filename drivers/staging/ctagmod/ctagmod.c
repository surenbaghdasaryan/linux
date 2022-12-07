#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

MODULE_LICENSE("GPL");

static struct page *pg_data;
static void *slab_data;
static void *v_data;

static int __init ctagmod_start(void)
{
	struct page *pg_tmp;
	void *slab_tmp;
	void *v_tmp;

	printk(KERN_INFO "Loading ctagmod module...\n");

#ifdef CONFIG_ALLOC_TAGGING
	pg_data = alloc_pages(GFP_KERNEL, 0);
	if (unlikely(!pg_data)) {
		printk(KERN_ERR "Failed to allocate a page!\n");
		return -ENOMEM;
	}
	pg_tmp = alloc_pages(GFP_KERNEL, 0);
	if (unlikely(!pg_tmp)) {
		printk(KERN_ERR "Failed to allocate a page!\n");
		return -ENOMEM;
	}
	free_pages((unsigned long)page_address(pg_tmp), 0);
	printk(KERN_INFO "Page is allocated\n");

	slab_data = kmalloc(10, GFP_KERNEL);
	if (unlikely(!slab_data)) {
		printk(KERN_ERR "Failed to allocate a slab object!\n");
		return -ENOMEM;
	}
	slab_tmp = kmalloc(10, GFP_KERNEL);
	if (unlikely(!slab_tmp)) {
		printk(KERN_ERR "Failed to allocate a slab object!\n");
		return -ENOMEM;
	}
	kfree(slab_tmp);
	printk(KERN_INFO "Slab object is allocated\n");

	v_data = vmalloc(100);
	if (unlikely(!v_data)) {
		printk(KERN_ERR "Failed to allocate virtual memory!\n");
		return -ENOMEM;
	}
	v_tmp = vmalloc(100);
	if (unlikely(!v_tmp)) {
		printk(KERN_ERR "Failed to allocate virtual memory!\n");
		return -ENOMEM;
	}
	vfree(v_tmp);
	printk(KERN_INFO "Virtual memory is allocated\n");
#else
	printk(KERN_INFO "CONFIG_ALLOC_TAGGING is undefined\n");
#endif
	return 0;
}

static void __exit ctagmod_end(void)
{
	if (v_data)
		vfree(v_data);
	if (slab_data)
		kfree(slab_data);
	if (pg_data)
		free_pages((unsigned long)page_address(pg_data), 0);
	printk(KERN_INFO "Unloading ctagmod\n");
}

module_init(ctagmod_start);
module_exit(ctagmod_end);
