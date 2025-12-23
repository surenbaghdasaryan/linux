# SPDX-License-Identifier: GPL-2.0-only
ccflags-y += -I$(src)			# needed for trace events

obj-$(CONFIG_ANDROID_BINDERFS)		+= binderfs.o
obj-$(CONFIG_ANDROID_BINDER_IPC)	+= binder.o binder_alloc.o binder_netlink.o
obj-$(CONFIG_ANDROID_BINDER_ALLOC_KUNIT_TEST)	+= tests/
obj-$(CONFIG_ANDROID_BINDER_IPC_RUST)	+= binder/
obj-$(CONFIG_ANDROID_WRAPFD)			+= wrapfd.o
obj-$(CONFIG_ANDROID_WRAPFD)			+= wrapfd_test.o
