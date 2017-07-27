/*
 * Marvell mini-kmsg logger
 *
 * Copyright (C) 2017 Marvell
 *
 * Yan Markman <ymarkman@marvell.com>
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2. This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

/* includes */
#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/printk.h>
#include <linux/delay.h>
#include <linux/time.h>
#include <asm-generic/uaccess.h>
#include <linux/workqueue.h>

/* The Marvell-logger includes "mv_printk_log.c" and "mv_printk.h" files.
 * The pr_xxx are overloaded by "mv_printk.h" with EXTENSION=mv_log_event()
 * in every .C/.H source having this include.
 * As result, Marvell's printk messages are saved twice:
 *  in the KMSG buffer as "half-binary" kmsg record and
 *  in this logger with some pre-history lines.
 */

/* The mv_printk_log.c uses "original" pr_xxx macros
 * All othre Marvell subsystem sources use extended pr_xxx
 */
#define MV_DO_NOT_OVERLOAD_PRINTK
#include "mv_printk.h"

/*PRINTK: #define __LOG_BUF_LEN (1 << CONFIG_LOG_BUF_SHIFT) */
#define MV_LOG_SZ	(4 * 1024) /* ~30 lines of 120chars */
#define MV_LOG_KMSG_SRC1	"/var/log/kern.log"
#define MV_LOG_KMSG_SRC2	"/var/log/messages"
#define MV_LOG_KMSG_THRSH	100
#define MV_LOG_DIRECTORY	"/data/mv_log"
#define MV_LOG_FILE_DFLT	MV_LOG_DIRECTORY  "/mv_log.txt"
#define NAME_MAX	255

struct mvlog {
	bool ena;
	char *buf;
	int sz;
	char kmsg_src[28];

	struct delayed_work *task;
	struct workqueue_struct *wq;
	unsigned long wq_delay;

	bool save_on_stop;
	int kmsg_src_thresh_cnt;
};

static struct mvlog mvlog;

/* The best buffer is Reboot retentive, but Uboot overwrites it anyway
 * static char mv_log_buf[MV_LOG_SZ] __attribute__ ((section (".noinit")));
 * So use kmalloc for buffer instead of the mv_log_buf[]
*/

static void mv_log_work_handler(struct work_struct *w);
static DECLARE_DELAYED_WORK(mv_log_task, mv_log_work_handler);
static void mv_log_save2file_utl(char *extra_str1, char *extra_str2);

/* -----  Local utilities  -----------------------------------------------*/
static inline bool check_kmsg_src_found(void)
{
	struct file *filp;
	int i;
	char *path[2] = {MV_LOG_KMSG_SRC1, MV_LOG_KMSG_SRC2};

	mvlog.kmsg_src[0] = 0;
	for (i = 0; i < 2; i++) {
		filp = filp_open(path[i], O_RDONLY, 0);
		if (!IS_ERR(filp)) {
			filp_close(filp, NULL);
			strcpy(mvlog.kmsg_src, path[i]);
			return true;
		}
	}
	if (mvlog.kmsg_src_thresh_cnt++ >= MV_LOG_KMSG_THRSH) {
		mv_log_enable(0);
		pr_err("PP3 mv_log: fail to read kernel-log file\n");
	}
	return false;
}

static inline bool check_log_dir_file_log_found(void)
{
	struct file *filp;

	filp = filp_open(MV_LOG_DIRECTORY, O_RDONLY, 0);
	if (!IS_ERR(filp)) {
		filp_close(filp, NULL);
		mvlog.save_on_stop = true;
	} else {
		mvlog.save_on_stop = false;
	}

	if (mvlog.save_on_stop) {
		filp = filp_open(MV_LOG_FILE_DFLT, O_RDONLY, 0);
		if (!IS_ERR(filp)) {
			filp_close(filp, NULL);
			return true;
		}
	}
	return false;
}

/* Log engine: save tail of "/var/log/messages" */
static inline void mv_log_record(void)
{
	const char *path = mvlog.kmsg_src;
	int sz = mvlog.sz - 1;
	struct file *filp;
	mm_segment_t oldfs;
	loff_t offset;
	char *buf;
	int size, len;

	if (!mvlog.ena)
		return;

	if (!path[0] && !check_kmsg_src_found())
		return; /* No Kernel-log file found */

	buf = mvlog.buf;
	oldfs = get_fs();
	set_fs(get_ds());

	filp = filp_open(path, O_RDONLY, 0);
	if (IS_ERR(filp)) {
		set_fs(oldfs);
		return;
	}

	size = vfs_llseek(filp, 0, SEEK_END);
	if (size < 0)
		goto bail;
	if (size > sz) {
		offset = size - sz;
		size = sz;
	} else {
		offset = 0;
		/* size = size */
	}
	len = vfs_llseek(filp, offset, 0);
	len = vfs_read(filp, buf, size, &offset);
	if (len > 0) {
		if (len < (sz - 6)) {
			/* Small len/fileSize occurred on "message" swap to "message.1" */
			/* Above "\n----\n" delimiter is NEW piece of log */
			len += sprintf(buf + len, "\n----\n");
			/* Below "\n----\n" delimiter is OLD piece of log */
		} else {
			buf[len] = 0;
		}
	}
bail:
	set_fs(oldfs);
	filp_close(filp, NULL);
	mvlog.buf[sz] = 0; /* for direct print from buffer instead from file */
}

static void mv_log_work_handler(struct work_struct *work)
{
	mv_log_record();
}

/*------  Global APIs  --------------------------------------------------*/
/* Return log buffer address */
char *mv_log_buf_addr_get(void)
{
	return mvlog.buf;
}

/* Return log buffer size */
u32 mv_log_buf_len_get(void)
{
	return mvlog.sz;
}

/* Get default log-file-name size */
const char *mv_log_fname_dflt_get(void)
{
	return MV_LOG_FILE_DFLT;
}

/* Enable/Disable log-recording. if -1 just returns Current */
static bool mv_log_enable_utl(int delay_ms, char *extra_str1, char *extra_str2)
{
	bool old = mvlog.ena;

	if (!mvlog.buf)
		return false;
	if (delay_ms < 0)
		return old;

	if (!delay_ms && mvlog.ena)
		mv_log_save2file_utl(extra_str1, extra_str2);

	mvlog.ena = !!(delay_ms);
	if (delay_ms > 10) {
		/* Delay recording on startup since there are a lot of prints */
		queue_delayed_work(mvlog.wq, mvlog.task, msecs_to_jiffies(delay_ms));
	}
	return old;
}

bool mv_log_enable(int delay_ms)
{
	return mv_log_enable_utl(delay_ms, NULL, NULL);
}

bool mv_log_disable(char *extra_str1, char *extra_str2)
{
	return mv_log_enable_utl(0, extra_str1, extra_str2);
}


/* An event triggering save to log buffer.
 * Safety to be called on IRQ context!
 * "delayed" to accumulate some frequent MRVL printk
 */
void mv_log_event(void)
{
	if (mvlog.ena)
		queue_delayed_work(mvlog.wq, mvlog.task, msecs_to_jiffies(80));
}

/* Save buffer into file */
static void mv_log_save2file_utl(char *extra_str1, char *extra_str2)
{
	int sz = mvlog.sz, size;
	struct file *filp;
	mm_segment_t oldfs;
	loff_t offset;
	char *buf;

	if (!mvlog.buf) /* save even if disabled */
		return;
	if (!mvlog.save_on_stop)
		return;

	buf = mvlog.buf;
	size = strnlen(buf, sz);

	oldfs = get_fs();
	set_fs(get_ds());
	filp = filp_open(MV_LOG_FILE_DFLT, O_WRONLY | O_CREAT, 0644);
	if (IS_ERR(filp)) {
		set_fs(oldfs);
		return;
	}
	offset = 0;
	sz = vfs_write(filp, buf, size, &offset);
	if (extra_str1) {
		size = strnlen(extra_str1, sz);
		if (size)
			sz = vfs_write(filp, extra_str1, size, &offset);
	}
	if (extra_str2) {
		size = strnlen(extra_str2, sz);
		if (size)
			sz = vfs_write(filp, extra_str2, size, &offset);
	}
	set_fs(oldfs);
	filp_close(filp, NULL);
}

void mv_log_save2file(void)
{
	mv_log_save2file_utl(NULL, NULL);
}

/* Copy "limit" of chars into giben buffer */
int mv_log_cp2buf(char *buf, int limit)
{
	int offs;

	if ((limit < 200) || !mvlog.buf) {
		buf[0] = 0;
		return 0;
	}

	if (limit >= mvlog.sz) {
		limit = mvlog.sz;
		offs = 0;
	} else {
		/* limit = limit */
		offs = mvlog.sz - limit;
	}

	limit = min(limit, mvlog.sz);
	memcpy(buf, mvlog.buf + offs, limit);
	buf[limit - 1] = 0;
	return limit;
}

/* INIT */
void mv_log_init(int delay_ms)
{
	bool old_log_found;
	char str[120];

	if (mvlog.ena)
		return; /* already done */

	old_log_found = check_log_dir_file_log_found();
	mvlog.buf = kzalloc(MV_LOG_SZ, GFP_KERNEL);
	if (!mvlog.buf) {
		pr_err("PP3 mv_log: no memory\n");
		return;
	}
	mvlog.wq = create_singlethread_workqueue("mv_log");
	mvlog.task = &mv_log_task;
	if (!mvlog.wq) {
		kfree(mvlog.buf);
		mvlog.buf = NULL;
		pr_err("PP3 mv_log: cannot create workqueue\n");
		return;
	}
	mvlog.sz = MV_LOG_SZ;
	if (old_log_found)
		sprintf(str, "(old log %s found)", MV_LOG_FILE_DFLT);
	else
		str[0] = 0;
	pr_info("PP3 mv_log initialized %s\n", str);
	if (!delay_ms)
		delay_ms = 1;
	mv_log_enable(delay_ms);
}
