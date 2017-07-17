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
#define MV_LOG_KMSG_SRC	"/var/log/messages"
#define MV_LOG_DIRECTORY	"/data/"
#define MV_LOG_FILE_PATH	MV_LOG_DIRECTORY  "mv_log"
#define MV_LOG_FILE_DFLT	MV_LOG_FILE_PATH  ".txt"
#define NAME_MAX	255

struct mvlog {
	bool ena;
	char *buf;
	int sz;

	struct delayed_work *task;
	struct workqueue_struct *wq;
	unsigned long wq_delay;
};

static struct mvlog mvlog;

/* The best buffer is Reboot retentive, but Uboot overwrites it anyway
 * static char mv_log_buf[MV_LOG_SZ] __attribute__ ((section (".noinit")));
 * So use kmalloc for buffer instead of the mv_log_buf[]
*/

static void mv_log_work_handler(struct work_struct *w);
static DECLARE_DELAYED_WORK(mv_log_task, mv_log_work_handler);

/* -----  Local utilities  -----------------------------------------------*/
static inline void create_log_dir(void)
{
	struct file *filp;

	filp = filp_open(MV_LOG_DIRECTORY, O_DIRECTORY | O_CREAT | O_WRONLY,
			 S_IRWXU | S_IRWXG | S_IFDIR);
	if (!IS_ERR(filp))
		filp_close(filp, NULL);
}

static inline void log_name_extension(char *buf)
{
/* Extend file-name with xxxx_date
 * { time_t rawtime; time(&rawtime); "_%s", ctime(&rawtime) }
 * or with enumerator xxxx_0, xxxx_1, xxxx_9
 */
}

/* Log engine: save tail of "/var/log/messages" */
static inline void mv_log_record(void)
{
	const char *path = MV_LOG_KMSG_SRC;
	int sz = mvlog.sz - 1;
	struct file *filp;
	mm_segment_t oldfs;
	loff_t offset;
	char *buf;
	int size, len;

	if (!mvlog.ena)
		return;

	buf = mvlog.buf;
	oldfs = get_fs();
	set_fs(get_ds());
	filp = filp_open(path, O_RDONLY, 0);
	if (IS_ERR(filp)) {
		set_fs(oldfs);
		mv_log_enable(0);
		pr_err("PP3 mv_log: fail to read <%s>\n", path);
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
	if (len > 0)
		buf[len] = 0;
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
bool mv_log_enable(int delay_ms)
{
	bool old = mvlog.ena;

	if (!mvlog.buf)
		return false;
	if (delay_ms < 0)
		return old;

	mvlog.ena = !!(delay_ms);
	if (delay_ms > 10) {
		/* Delay recording on startup since there are a lot of prints */
		queue_delayed_work(mvlog.wq, mvlog.task, msecs_to_jiffies(delay_ms));
	}
	return old;
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
void mv_log_save2file(char *file_ext)
{
	int sz = mvlog.sz, size;
	struct file *filp;
	mm_segment_t oldfs;
	loff_t offset;
	char *buf;
	char path[NAME_MAX];
	char *ext;

	if (!mvlog.buf) /* save even if disabled */
		return;

	ext = (file_ext) ? file_ext : "txt";
	snprintf(path, NAME_MAX, "%s.%s", MV_LOG_FILE_PATH, ext);
	path[NAME_MAX - 1] = 0;
	log_name_extension(path);

	buf = mvlog.buf;
	size = strnlen(buf, sz);

	oldfs = get_fs();
	set_fs(get_ds());
	filp = filp_open(path, O_WRONLY | O_CREAT, 0644);
	if (IS_ERR(filp)) {
		set_fs(oldfs);
		return;
	}
	offset = 0;
	sz = vfs_write(filp, buf, size, &offset);
	set_fs(oldfs);
	filp_close(filp, NULL);
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
	if (mvlog.ena)
		return; /* already done */

	create_log_dir();
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
	pr_info("PP3 mv_log initialized\n");
	if (!delay_ms)
		delay_ms = 1;
	mv_log_enable(delay_ms);
}
