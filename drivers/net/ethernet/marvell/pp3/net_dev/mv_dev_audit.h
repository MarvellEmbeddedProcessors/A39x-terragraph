/*
 * Marvell PP3 audit - periodic helth checker
 *
 * Copyright (C) 2017 Marvell
 *
 * Yan Markman <ymarkman@marvell.com>
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2. This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#ifndef _MV_DEV_AUDIT_H_
#define _MV_DEV_AUDIT_H_

#define MV_PP3_AUDIT_STR_BUF_SZ	120

#ifdef CONFIG_MV_PP3_DEBUG_CODE
#define MV_PP3_AUDIT	/* could be disabled if not needed */
#else
/* #define MV_PP3_AUDIT	/@ could be enabled also without DEBUG_CODE */
#endif

#ifdef MV_PP3_AUDIT
int mv_pp3_audit_init(void);
void mv_pp3_audit_open(struct net_device *dev);
void mv_pp3_audit_stop(struct net_device *dev);
int mv_pp3_audit_poll_period_sec_set(int sec);
int mv_pp3_audit_drop_thrsh_set(int thrsh_pkt_per_sec);
int mv_pp3_audit_alarm_get(char *buf, int buf_len);
int mv_pp3_audit_warn_get(char *buf, int buf_len);
void mv_pp3_audit_alarm_emulate(void);
#else
static inline int mv_pp3_audit_init(void)	{ return 0; }
static inline void mv_pp3_audit_open(struct net_device *dev) { }
static inline void mv_pp3_audit_stop(struct net_device *dev) { }
static inline int mv_pp3_audit_poll_period_sec_set(int sec) { return 0; }
static inline int mv_pp3_audit_drop_thrsh_set(int thrsh_pkt_per_sec) { return 0; }
static inline int mv_pp3_audit_alarm_get(char *buf, int buf_len) { return 0; }
static inline int mv_pp3_audit_warn_get(char *buf, int buf_len) { return 0; }
static inline void mv_pp3_audit_alarm_emulate(void) { }
#endif

#endif /*_MV_DEV_AUDIT_H_*/
