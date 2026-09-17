/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Minimale Kernel-Attrappe, damit sich pixy-mvblli.c im Userspace
 * uebersetzen laesst. Nur so viel, wie der Treiber tatsaechlich benutzt.
 * Der Traffic Memory wird durch ein gewoehnliches Byte-Array ersetzt.
 */
#ifndef FAKE_LINUX_KERNEL_H
#define FAKE_LINUX_KERNEL_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stddef.h>
#include <errno.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int16_t  s16;
typedef int32_t  s32;
typedef uint32_t __poll_t;
typedef uint16_t __u16;
typedef uint32_t __u32;

#define __iomem
#define __user
#define __init
#define __exit
#define THIS_MODULE ((void *)0)

#define ALIGN(x, a)		(((x) + (a) - 1) & ~((typeof(x))(a) - 1))
#define ARRAY_SIZE(a)		(sizeof(a) / sizeof((a)[0]))
#define MKDEV(ma, mi)		(((ma) << 20) | (mi))
#define MAJOR(d)		((d) >> 20)
#define MINOR(d)		((d) & 0xfffff)
#define iminor(i)		0
#define cpu_relax()		do { } while (0)

#define pr_info(fmt, ...)	fprintf(stderr, "[info ] " fmt, ##__VA_ARGS__)
#define pr_warn(fmt, ...)	fprintf(stderr, "[warn ] " fmt, ##__VA_ARGS__)
#define pr_err(fmt, ...)	fprintf(stderr, "[err  ] " fmt, ##__VA_ARGS__)
#define pr_warn_once(fmt, ...)	fprintf(stderr, "[warn ] " fmt, ##__VA_ARGS__)

#define scnprintf		snprintf
#define strscpy(d, s, n)	(strncpy((d), (s), (n) - 1), (d)[(n) - 1] = 0, 0)

/*
 * Der Traffic Memory als flaches Array. ioread/iowrite rechnen die
 * Zeigerdifferenz in einen Index um, so dass der Treibercode
 * unveraendert bleibt.
 */
#define FAKE_TM_SIZE 0x40000UL
extern u8 fake_tm[FAKE_TM_SIZE];
#define fake_tm_size FAKE_TM_SIZE
extern unsigned long fake_io_writes;

static inline u16 ioread16(const void *p)
{
	unsigned long off = (const u8 *)p - fake_tm;

	if (off + 2 > fake_tm_size) {
		fprintf(stderr, "!! ioread16 ausserhalb: 0x%lx\n", off);
		abort();
	}
	return (u16)(fake_tm[off] | (fake_tm[off + 1] << 8));
}

static inline void iowrite16(u16 v, void *p)
{
	unsigned long off = (u8 *)p - fake_tm;

	if (off + 2 > fake_tm_size) {
		fprintf(stderr, "!! iowrite16 ausserhalb: 0x%lx\n", off);
		abort();
	}
	fake_tm[off] = v & 0xff;
	fake_tm[off + 1] = v >> 8;
	fake_io_writes++;
}

static inline u32 ioread32(const void *p)
{
	return ioread16(p) | ((u32)ioread16((const u8 *)p + 2) << 16);
}

static inline void iowrite32(u32 v, void *p)
{
	iowrite16(v & 0xffff, p);
	iowrite16(v >> 16, (u8 *)p + 2);
}

#define kcalloc(n, s, f)	calloc((n), (s))
#define kfree(p)		free((void *)(p))
#define GFP_KERNEL		0

#define usleep_range(a, b)	do { } while (0)

#define EPOLLIN 1
#define EPOLLOUT 4
#define EPOLLRDNORM 0x40
#define EPOLLWRNORM 0x100
#define EPOLLERR 8

#define IS_ERR(p)		((unsigned long)(void *)(p) > (unsigned long)-4096)
#define PTR_ERR(p)		((long)(p))
#define ERR_PTR(e)		((void *)(long)(e))

#define O_RDONLY 0
#define O_RDWR   2

#define module_param(a, b, c)
#define MODULE_PARM_DESC(a, b)
#define module_init(f)
#define module_exit(f)
#define MODULE_LICENSE(s)
#define MODULE_DESCRIPTION(s)
#define MODULE_VERSION(s)

#define _IOC_TYPE(c)		(((c) >> 8) & 0xff)
#define _IOC_NR(c)		((c) & 0xff)

#define offsetof_mvb(t, m)	offsetof(t, m)

typedef struct { int dummy; } spinlock_t;
typedef struct { int dummy; } wait_queue_head_t;
typedef struct { int dummy; } poll_table;
struct mutex { int dummy; };
struct cdev { void *owner; const void *ops; };
struct class { const void *dev_groups; };
struct device { void *p; };
struct inode { dev_t i_rdev; };
struct file { void *private_data; const struct file_operations *f_op; };
struct file_operations {
	void *owner;
	int (*open)(struct inode *, struct file *);
	int (*release)(struct inode *, struct file *);
	long (*read)(struct file *, char __user *, size_t, loff_t *);
	long (*write)(struct file *, const char __user *, size_t, loff_t *);
	__poll_t (*poll)(struct file *, poll_table *);
	long (*unlocked_ioctl)(struct file *, unsigned int, unsigned long);
	long (*compat_ioctl)(struct file *, unsigned int, unsigned long);
	int llseek;
};

#define spin_lock_init(l)		do { (void)(l); } while (0)
#define spin_lock_irqsave(l, f)		do { (void)(l); (f) = 0; } while (0)
#define spin_unlock_irqrestore(l, f)	do { (void)(l); (void)(f); } while (0)
#define mutex_init(m)			do { (void)(m); } while (0)
#define mutex_lock(m)			do { (void)(m); } while (0)
#define mutex_unlock(m)			do { (void)(m); } while (0)
#define init_waitqueue_head(q)		do { (void)(q); } while (0)
#define wake_up_interruptible(q)	do { (void)(q); } while (0)
#define poll_wait(f, q, w)		do { } while (0)
#define test_and_set_bit(b, p)		(0)
#define clear_bit(b, p)			do { } while (0)
#define no_llseek			0

#define copy_to_user(d, s, n)		(memcpy((d), (s), (n)), 0)
#define copy_from_user(d, s, n)		(memcpy((d), (s), (n)), 0)
#define get_user(v, p)			((v) = *(p), 0)
#define put_user(v, p)			(*(p) = (v), 0)

#define cdev_init(c, f)			do { } while (0)
#define cdev_add(c, d, n)		(0)
#define cdev_del(c)			do { } while (0)
#define device_create(c, p, d, x, f, ...) ((struct device *)1)
#define device_destroy(c, d)		do { } while (0)
#define class_create(o, n)		((struct class *)1)
#define class_destroy(c)		do { } while (0)
#define alloc_chrdev_region(d, f, c, n)	(*(d) = MKDEV(250, 0), 0)
#define unregister_chrdev_region(d, c)	do { } while (0)
#define filp_open(p, f, m)		((struct file *)ERR_PTR(-ENODEV))
#define filp_close(f, i)		do { } while (0)

#endif /* FAKE_LINUX_KERNEL_H */
