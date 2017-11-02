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

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/skbuff.h>

#include "mv_dev_audit.h"

#ifdef MV_PP3_AUDIT

/* IF problem found, this audit prints repetitevly every poll-period.
 * Do not save these prints into mv-log but use std printk.h
 * The #define and #include must be first
 */
#define MV_DO_NOT_OVERLOAD_PRINTK
#include "common/mv_printk.h"

#include "platform/mv_pp3.h"
#include "platform/mv_pp3_config.h"
#include "mv_netdev_structs.h"
#include "mv_netdev.h"
#include "mv_dev_vq.h"
#include "fw/mv_fw.h"
#include "vport/mv_pp3_vport.h"
#include "hmac/mv_hmac.h"
#include "bm/mv_bm.h"

/*---------------------------------------------------------------------------*/

#define MV_AUDIT_PREF	"PP3 audit: "
#define MV_AUDIT_DEVS	16
#define MV_AUDIT_POLL_SEC	30
#define MV_AUDIT_POLL_SEC_MIN	10
#define MV_AUDIT_POLL_SEC_MAX	120
#define MV_AUDIT_POLL_SEC_POSTMORTEM	(MV_AUDIT_POLL_SEC * 2)
#define MV_AUDIT_POLL_SEC_POSTMORTEM_MAX	(180)
#define MV_AUDIT_HMAC_SWQ_THRESH (CONFIG_MV_PP3_RXQ_SIZE * MV_PP3_CFH_PKT_DG_SIZE)

/* Alarm / Warning checkers and event masks */
#define MV_AUDIT_EV_NONE	0
#define MV_AUDIT_EV_SW_STOP	BIT(0)
#define MV_AUDIT_EV_FW_STOP	BIT(1)
#define MV_AUDIT_EV_DEV_RX_SWQ_OVERLOAD	BIT(2) /* SW-Queue is CFH-queue */
#define MV_AUDIT_EV_DEV_RX_SW_IRQ_DIS	BIT(3)
#define MV_AUDIT_EV_BM_GPM_FULL	BIT(4)
#define MV_AUDIT_EV_DEV_WARN_IF_TO_WIFI	BIT(16)
#define MV_WARN_IF_TO_WIFI_THRSH	10 /* Min pkt/sec */

/* data "per cpu" is not needed since RX always handled by CPU0 only */
struct mv_per_dev {
	struct net_device *dev;
	bool dev_is_eth;
	u64 pkts_prev[CONFIG_MV_PP3_TXQ_NUM];
	u32 no_traffic_cntr[CONFIG_MV_PP3_TXQ_NUM];
	u32 rx_netif_prev;
	u32 rx_netif_drop_prev;
	u32 rx_netif_drops;

	int occ_dg[CONFIG_MV_PP3_TXQ_NUM];
	bool irq_dis;
};

struct mv_audit {
	struct mv_per_dev info[MV_AUDIT_DEVS];
	u32 poll_cycle_cntr;

	struct task_struct *task;
	bool task_ended;
	int active_dev_cnt;
	int poll_sec;
	int poll_sec_postmortem;

	u32 alarm;
	u64 al_ts_sec;
	char alarm_str[MV_PP3_AUDIT_STR_BUF_SZ];
	int alarm_str_len;
	char warn_str[MV_PP3_AUDIT_STR_BUF_SZ];
	u32 warn_expire_ts;
	u32 rx_netif_drop_thrsh;
	int sw_stop_reason;
};

static struct mv_audit *aud;

static void mv_audit_warning(u32 event, int idx);
static void mv_audit_alarm(u32 event, int idx);

/*----  APIs for sysfs  -------------------------------------------*/
int mv_pp3_audit_poll_period_sec_set(int sec)
{
	u32 thrsh_pkt_per_sec;

	if ((sec < MV_AUDIT_POLL_SEC_MIN) || (sec > MV_AUDIT_POLL_SEC_MAX)) {
		pr_info(MV_AUDIT_PREF "valid poll-period is in range %d..%d sec\n",
			MV_AUDIT_POLL_SEC_MIN, MV_AUDIT_POLL_SEC_MAX);
		return -EINVAL;
	}
	thrsh_pkt_per_sec = aud->rx_netif_drop_thrsh / aud->poll_sec;
	aud->poll_sec = sec;
	sec *= 2;
	if (sec > MV_AUDIT_POLL_SEC_POSTMORTEM_MAX)
		sec = MV_AUDIT_POLL_SEC_POSTMORTEM_MAX;
	aud->poll_sec_postmortem = sec;
	aud->rx_netif_drop_thrsh = thrsh_pkt_per_sec * aud->poll_sec;
	return 0;
}

int mv_pp3_audit_drop_thrsh_set(int thrsh_pkt_per_sec)
{
	if (thrsh_pkt_per_sec < 0)
		return -EINVAL;
	aud->rx_netif_drop_thrsh = thrsh_pkt_per_sec * aud->poll_sec;
	return 0;
}

int mv_pp3_audit_alarm_get(char *buf, int buf_len)
{
	if (buf && (buf_len > aud->alarm_str_len))
		strcpy(buf, aud->alarm_str);
	else if (buf_len)
		pr_err("%s", aud->alarm_str); /* sysfs request => pr_err */
	return aud->alarm_str_len;
}

int mv_pp3_audit_warn_get(char *buf, int buf_len)
{
	int warn_len = 0;

	if (!aud->warn_str[0])
		return warn_len;

	warn_len = strlen(aud->warn_str);
	if (buf && (buf_len > warn_len))
		strcpy(buf, aud->warn_str);
	else if (buf_len)
		pr_err("%s", aud->warn_str); /* sysfs request => pr_err */
	return warn_len;
}

void mv_pp3_audit_alarm_emulate(void)
{
	/* DEBUG purpose only */
	int i;

	for (i = 0; i < MV_AUDIT_DEVS; i++)
		if (aud->info[i].dev)
			mv_audit_warning(MV_AUDIT_EV_DEV_WARN_IF_TO_WIFI, i);
	mv_audit_alarm(MV_AUDIT_EV_SW_STOP, -1);
	mv_audit_alarm(MV_AUDIT_EV_FW_STOP, -1);
	i = 0;
	for (i = 0; i < MV_AUDIT_DEVS; i++)
		if (aud->info[i].dev)
			mv_audit_alarm(MV_AUDIT_EV_DEV_RX_SWQ_OVERLOAD, i);
}

/*-----------------------------------------------------------------*/
static inline void mv_audit_bld_alarm_str(struct mv_audit *ad, struct mv_per_dev *info)
{
	char al_str0[24];
	char *al_str1;
	char al_str2[60];
	char *al_str4;

	if (ad->alarm & MV_AUDIT_EV_SW_STOP)
		sprintf(al_str0, "{sw-rx/tx %d}", ad->sw_stop_reason);
	else
		al_str0[0] = 0;

	al_str1 = (ad->alarm & MV_AUDIT_EV_FW_STOP) ? "{fw-ppc}" : "";
	al_str4 = (ad->alarm & MV_AUDIT_EV_BM_GPM_FULL) ? "{bm-gpm-full}" : "";

	if ((ad->alarm & MV_AUDIT_EV_DEV_RX_SWQ_OVERLOAD) && info)
#if (CONFIG_MV_PP3_TXQ_NUM > 1)
		sprintf(al_str2, "{%s occ_dg=%d:%d irq=%s}", info->dev->name,
			info->occ_dg[0], info->occ_dg[1], (info->irq_dis) ? "DIS" : "ENA");
#else
		sprintf(al_str2, "{%s occ_dg=%d irq=%s}", info->dev->name,
			info->occ_dg[0], (info->irq_dis) ? "DIS" : "ENA");
#endif
	else
		al_str2[0] = 0;

	sprintf(ad->alarm_str, MV_AUDIT_PREF "NSS fail at %llu sec; %s %s %s %s\n",
		ad->al_ts_sec, al_str0, al_str1, al_str2, al_str4);
	ad->alarm_str_len = strlen(ad->alarm_str);
}

static void mv_audit_warning(u32 event, int idx)
{
	struct mv_per_dev *info;
	char str[60];
	char *to_str;
	u64 sec;
	u32 w_ts;

	/* if(aud->alarm)=>return -- not needed */
	if (idx < 0)
		return;
	info = &aud->info[idx];
	if (event & MV_AUDIT_EV_DEV_WARN_IF_TO_WIFI) {
		to_str = (info->dev_is_eth) ? "net-LINUX" : "WiFi-tx";
		sprintf(str, "%s rx to %s drop all %u pkts",
			info->dev->name, to_str, info->rx_netif_drops);
	}
	sec = local_clock(); /* nano-sec */
	do_div(sec, 1000000000);
	sprintf(aud->warn_str, MV_AUDIT_PREF "warning %llu sec: %s\n", sec, str);
	pr_info("%s", aud->warn_str);

	/* Warn-Expiring TimeStamp to clear Warn status and string */
	w_ts = 120; /* sec till expire */
	w_ts /= aud->poll_sec; /* in Poll-Cycles */
	if (w_ts < 2)
		w_ts = 2;
	aud->warn_expire_ts = w_ts + aud->poll_cycle_cntr;
}

static inline void mv_audit_warning_clear(struct mv_audit *ad)
{
	if (ad->alarm) /* Keep last warning before alarm */
		return;
	if (ad->poll_cycle_cntr == aud->warn_expire_ts)
		ad->warn_str[0] = 0;
}

static void mv_audit_alarm(u32 event, int idx)
{
	u64 sec;
	struct mv_per_dev *info;
	u32 prev_alarm = aud->alarm;

	aud->alarm |= event;

	if (!prev_alarm) { /* Take timestamp on First alarm only */
		sec = local_clock(); /* nano-sec */
		do_div(sec, 1000000000);
		aud->al_ts_sec = sec;
		aud->poll_sec = aud->poll_sec_postmortem;
	}
	if (aud->alarm != prev_alarm) {
		info = (idx >= 0) ? &aud->info[idx] : NULL;
		mv_audit_bld_alarm_str(aud, info);
	}
#ifdef MV_PRINTK_LOG
	if (!prev_alarm) /* Stop LOG upon First alarm, add warn/alarm into log */
		mv_log_disable(aud->warn_str, aud->alarm_str);
#endif
}

/*-----------------------------------------------------------------*/
void mv_pp3_audit_open(struct net_device *dev)
{
	int i, free_idx;

	if (aud->active_dev_cnt == MV_AUDIT_DEVS) {
		pr_err(MV_AUDIT_PREF "cannot register <%s>\n", dev->name);
		return;
	}
	free_idx = 0;
	i = 0;
	while (i < MV_AUDIT_DEVS) {
		if (aud->info[i].dev == dev)
			return; /* already open */
		if (!aud->info[i].dev)
			free_idx = i; /* continue for checking "already open" */
		i++;
	}
	memset(&aud->info[free_idx], 0, sizeof(struct mv_per_dev));
	aud->active_dev_cnt++;
	aud->info[free_idx].dev = dev;
	aud->info[free_idx].dev_is_eth =
		(mv_pp3_dev_priv_ready_get(dev)->vport->type == MV_PP3_NSS_PORT_ETH);
}

void mv_pp3_audit_stop(struct net_device *dev)
{
	int i;

	if (!aud->active_dev_cnt)
		return;
	i = 0;
	while (i < MV_AUDIT_DEVS) {
		if (aud->info[i].dev == dev) {
			memset(&aud->info[i], 0, sizeof(struct mv_per_dev));
			aud->active_dev_cnt--;
			return;
		}
		i++;
	}
}

/*-----------------------------------------------------------------*/
static inline void check_rx_swq_dev(int idx, int cpu)
{
	/* Check RX-SW-Queue (CFH) overloading, BUT ONLY
	 * for no-traffic (rx-pkt counter doesn't increment).
	 */
	struct mv_per_dev *info = &aud->info[idx];
	struct pp3_dev_priv *dev_priv = MV_PP3_PRIV(info->dev);
	struct pp3_vq *rx_vq;
	int q;
	struct pp3_swq *rx_swq;
	struct pp3_vport *cpu_vp = dev_priv->cpu_vp[cpu];

	/* MV_AUDIT_EV_DEV_RX_SWQ_OVERLOAD - alarm: traffic stopped, SWQ/CFH huge
	 * MV_AUDIT_EV_DEV_RX_SW_IRQ_DIS   - additional dbg-info for SWQ_OVERLOAD
	 */
	for (q = cpu_vp->port.cpu.napi_q_num - 1; q >= 0; q--) {
		rx_vq = cpu_vp->rx_vqs[MV_PP3_PROC_RXQ_INDEX_GET(cpu_vp->port.cpu, q)];
		rx_swq = rx_vq->swq;

		if ((rx_swq->stats.pkts != info->pkts_prev[q]) || !info->pkts_prev[q]) {
			/* under traffic or just Open */
			info->no_traffic_cntr[q] = 0;
			info->pkts_prev[q] = rx_swq->stats.pkts;
		} else {
			/* no traffic */
			if (info->no_traffic_cntr[q] < (u32)(-2))
				info->no_traffic_cntr[q]++;
			if (info->no_traffic_cntr[q] <= 2) {
				/* RX just stopped, check not by abnormal */
				if (cpu_vp->rx_vqs[q]->valid) {
					/* (!valid) is under pause/suspend. OCC-full is ok in that case */
					info->occ_dg[q] = mv_pp3_hmac_rxq_occ_get(rx_swq->frame_num, rx_swq->swq);
					if (info->occ_dg[q] >= MV_AUDIT_HMAC_SWQ_THRESH) {
						info->irq_dis = mv_pp3_vp_rx_int_is_masked(cpu_vp);
						mv_audit_alarm(MV_AUDIT_EV_DEV_RX_SWQ_OVERLOAD, idx);
					}
				}
			}
		}
	}
}

void check_rx_swq(int cpu)
{
	int i;

	for (i = 0; i < MV_AUDIT_DEVS; i++)
		if (aud->info[i].dev)
			check_rx_swq_dev(i, cpu);
}

static inline void check_rx_to_netif_dev(int idx, int cpu)
{
	/*Optional warn: Nss-RX sent to WIFI but all dropped (SWQ may be overloaded) */
	struct mv_per_dev *info = &aud->info[idx];
	struct pp3_dev_priv *dev_priv = MV_PP3_PRIV(info->dev);
	struct pp3_vport *cpu_vp = dev_priv->cpu_vp[cpu];
	u32 d_drops, d_netif;

	d_netif = cpu_vp->port.cpu.stats.rx_netif - info->rx_netif_prev;

	/* No Warning if no-traffic or only few pack/sec */
	if (d_netif < aud->rx_netif_drop_thrsh)
		return;

	/* (d_drops > d_netif) possible if at this point Audit-thread intercepted by RX-SOFTIRQ */
	d_drops = cpu_vp->port.cpu.stats.rx_netif_drop - info->rx_netif_drop_prev;

	if (d_drops && (d_drops >= d_netif)) {
		info->rx_netif_drops = d_drops;
		mv_audit_warning(MV_AUDIT_EV_DEV_WARN_IF_TO_WIFI, idx);
	}

	info->rx_netif_prev = cpu_vp->port.cpu.stats.rx_netif;
	if (d_drops)
		info->rx_netif_drop_prev = cpu_vp->port.cpu.stats.rx_netif_drop;
}

static void check_rx_to_netif(int cpu)
{
	int i;

	for (i = 0; i < MV_AUDIT_DEVS; i++)
		if (aud->info[i].dev)
			check_rx_to_netif_dev(i, cpu);
}

static void mv_audit_check(void)
{
	/* This is Global function to be called by Audit-polling,
	 * or by Sysfs debug command (so no Input-Parameters)
	 */
	int cpu, ppc, sw_stop;

	if (aud->alarm)
		goto report;

	/* MV_AUDIT_EV_SW_STOP */
	sw_stop = mv_get_debug_stop_status();
	if (sw_stop) {
		aud->sw_stop_reason = sw_stop;
		mv_audit_alarm(MV_AUDIT_EV_SW_STOP, -1);
	}

	/* MV_AUDIT_EV_FW_STOP */
	for (ppc = 0; ppc < MV_PP3_PPC_MAX_NUM; ppc++) {
		if (mv_fw_keep_alive_get_str2buf(ppc, NULL, 1) != true) {
			mv_audit_alarm(MV_AUDIT_EV_FW_STOP, -1);
			break;
		}
	}

	/* MV_AUDIT_EV_BM_GPM_FULL */
	if (!bm_gpm_pool_fill_level_get(0) || !bm_gpm_pool_fill_level_get(1))
		mv_audit_alarm(MV_AUDIT_EV_BM_GPM_FULL, -1);

	cpu = 0; /* check for CPU_0 only */

	/* MV_AUDIT_EV_DEV_WARN_IF_TO_WIFI    - warning: WIFI is in down (SWQ high loaded) */
	check_rx_to_netif(cpu);

	/* MV_AUDIT_EV_DEV_RX_SWQ_OVERLOAD - alarm: traffic stopped, SWQ/CFH huge
	 * MV_AUDIT_EV_DEV_RX_SW_IRQ_DIS   - additional dbg-info for SWQ_OVERLOAD
	 */
	check_rx_swq(cpu);
report:
	if (aud->alarm) {
		 /* periodic auto-print => pr_info */
		if (aud->warn_str[0])
			pr_info("%s", aud->warn_str);
		pr_info("%s", aud->alarm_str);
	}
}

static int mv_audit_task(void *data)
{
	struct mv_audit *ad = (struct mv_audit *)data; /* Audit Descriptor */

	while (!kthread_should_stop()) {
		schedule();
		mv_audit_check();
		mv_audit_warning_clear(ad);
		ad->poll_cycle_cntr++;
		ssleep(ad->poll_sec);
	}
	/*do_exit(1);*/
	ad->task_ended = true;
	return 0;
}

/*-----------------------------------------------------------------*/
int mv_pp3_audit_init(void)
{
	int cpu = 0;

	aud = kzalloc(sizeof(*aud), GFP_KERNEL);
	if (!aud)
		return -ENOMEM;
	aud->poll_sec = MV_AUDIT_POLL_SEC;
	aud->poll_sec_postmortem = MV_AUDIT_POLL_SEC_POSTMORTEM;
	aud->rx_netif_drop_thrsh = MV_WARN_IF_TO_WIFI_THRSH * aud->poll_sec;
	aud->task = kthread_create(&mv_audit_task, (void *)aud, "mv_audit_rx");
	kthread_bind(aud->task, cpu);
	wake_up_process(aud->task);
	return 0;
}

void pp3_dbg_audit_exit(void)
{
	int msec = 100;
	int i = MV_AUDIT_POLL_SEC_POSTMORTEM_MAX / msec;

	kthread_stop(aud->task);
	while (aud->task_ended && i--)
		msleep(msec); /*wait thread finish */
	if (!i)
		pr_err(MV_AUDIT_PREF "cannot stop task\n");
	else
		kfree(aud);
}

#endif /* MV_PP3_AUDIT */
