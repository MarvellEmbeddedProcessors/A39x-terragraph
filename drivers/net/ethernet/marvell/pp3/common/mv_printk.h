#ifndef __MV_KERNEL_PRINTK__
#define __MV_KERNEL_PRINTK__

#ifndef __KERNEL_PRINTK__
#include <linux/kernel.h>
#include <linux/printk.h>
#endif

#ifdef CONFIG_MV_PP3_DEBUG_CODE

#if !defined(MV_DO_NOT_OVERLOAD_PRINTK)
/* Overwrite/extend linux/printk.h::pr_XXX macros */
#define MV_PREF	"MV "
#undef pr_err
#undef pr_warning
#undef pr_warn
#undef pr_notice
#undef pr_info
#define pr_err(fmt, ...) \
	do { printk(KERN_ERR MV_PREF pr_fmt(fmt), ##__VA_ARGS__); mv_log_event(); } while (0)
#define pr_warning(fmt, ...) \
	do { printk(KERN_WARNING MV_PREF pr_fmt(fmt), ##__VA_ARGS__); mv_log_event(); } while (0)
#define pr_warn pr_warning
#define pr_notice(fmt, ...) \
	do { printk(KERN_NOTICE MV_PREF pr_fmt(fmt), ##__VA_ARGS__); mv_log_event(); } while (0)
#define pr_info(fmt, ...) \
	do { printk(KERN_INFO MV_PREF pr_fmt(fmt), ##__VA_ARGS__); mv_log_event(); } while (0)
#endif /* MV_DO_NOT_OVERLOAD_PRINTK */

char *mv_log_buf_addr_get(void);
u32 mv_log_buf_len_get(void);
const char *mv_log_fname_dflt_get(void);
bool mv_log_enable(int ena);
void mv_log_event(void);
void mv_log_save2file(char *file_ext);
int mv_log_cp2buf(char *buf, int limit);
void mv_log_init(int delay_ms);

#else  /* CONFIG_MV_PP3_DEBUG_CODE */
static inline char *mv_log_buf_addr_get(void)	{ return NULL; }
static inline u32 mv_log_buf_len_get(void)	{ return 0; }
static inline const char *mv_log_fname_dflt_get(void)	{ return "none"; }
static inline bool mv_log_enable(bool ena)	{ return false; }
static inline void mv_log_event(void)	{ }
static inline void mv_log_save2file(const char *file_ext)	{ }
static inline int mv_log_cp2buf(char *buf, int limit) { return 0; }
static inline void mv_log_init(void)	{ }
#endif /* CONFIG_MV_PP3_DEBUG_CODE */

#endif /* __MV_KERNEL_PRINTK__ */
