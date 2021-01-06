/*
 * SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2001,2002,2003,2007,2012 Philippe Gerum <rpm@xenomai.org>.
 * Copyright (C) 2004 Gilles Chanteperdrix <gilles.chanteperdrix@xenomai.org>
 */

#include <linux/tick.h>
#include <linux/clockchips.h>
#include <cobalt/kernel/intr.h>
#include <pipeline/tick.h>
#include <cobalt/kernel/sched.h>
#include <cobalt/kernel/timer.h>

extern struct xnintr nktimer;

static DEFINE_PER_CPU(struct clock_proxy_device *, proxy_device);

inline void xnproxy_timer_set(unsigned long delta)
{
	struct clock_proxy_device *dev = __this_cpu_read(proxy_device);
	struct clock_event_device *real_dev = dev->real_device;
	int ret;
	u64 cycles;

	if (delta <= 0)
		delta = real_dev->min_delta_ns;
	else {
		delta = min_t(int64_t, delta,
				(int64_t)real_dev->max_delta_ns);
		delta = max_t(int64_t, delta,
					(int64_t)real_dev->min_delta_ns);
	}
	cycles = ((u64)delta * real_dev->mult) >> real_dev->shift;

	ret = real_dev->set_next_event(cycles, real_dev);
	if (ret) {
		ret = real_dev->set_next_event(real_dev->min_delta_ticks,
					real_dev);
	}
}

static int proxy_set_next_ktime(ktime_t expires,
				struct clock_event_device *proxy_dev)
{
	struct xnsched *sched;
	ktime_t delta;
	unsigned long flags;
	int ret;

	/*
	 * When Negative delta have been observed, we set delta zero.
	 * Or else exntimer_start() will return -ETIMEDOUT and do not
	 * trigger shot
	 */
	delta = ktime_sub(expires, ktime_get_mono_fast_ns());
	if (delta < 0)
		delta = 0;

	flags = hard_local_irq_save(); /* Prevent CPU migration. */
	sched = xnsched_current();
	ret = xntimer_start(&sched->htimer, delta, XN_INFINITE, XN_RELATIVE);
	hard_local_irq_restore(flags);

	return ret ? -ETIME : 0;
}


void xn_core_tick(struct clock_event_device *dummy) /* hard irqs off */
{
	xnintr_core_clock_handler();
}

static int proxy_set_oneshot_stopped(struct clock_event_device *proxy_dev)
{
	struct clock_event_device *real_dev;
	struct clock_proxy_device *dev;
	struct xnsched *sched;
	spl_t s;

	dev = container_of(proxy_dev, struct clock_proxy_device, proxy_device);

	/*
	 * In-band wants to disable the clock hardware on entering a
	 * tickless state, so we have to stop our in-band tick
	 * emulation. Propagate the request for shutting down the
	 * hardware to the real device only if we have no outstanding
	 * OOB timers. CAUTION: the in-band timer is counted when
	 * assessing the RQ_IDLE condition, so we need to stop it
	 * prior to testing the latter.
	 */
	xnlock_get_irqsave(&nklock, s);
	sched = xnsched_current();
	xntimer_stop(&sched->htimer);
	//sched->lflags |= XNTSTOP;

	if (sched->lflags & XNIDLE) {
		real_dev = dev->real_device;
		real_dev->set_state_oneshot_stopped(real_dev);
	}

	xnlock_put_irqrestore(&nklock, s);

	return 0;
}

static void setup_proxy(struct clock_proxy_device *dev)
{
	struct clock_event_device *proxy_dev = &dev->proxy_device;

	dev->handle_oob_event = xn_core_tick;
	proxy_dev->features |= CLOCK_EVT_FEAT_KTIME;
	proxy_dev->set_next_ktime = proxy_set_next_ktime;
	if (proxy_dev->set_state_oneshot_stopped)
		proxy_dev->set_state_oneshot_stopped = proxy_set_oneshot_stopped;
	__this_cpu_write(proxy_device, dev);
}

int pipeline_install_tick_proxy(void)
{
	int ret;

	ret = pipeline_request_timer_ipi(xnintr_core_clock_handler);
	if (ret)
		return ret;

	/* Install the proxy tick device */
	ret = tick_install_proxy(setup_proxy, &xnsched_realtime_cpus);
	if (ret)
		goto fail_proxy;

	return 0;

fail_proxy:
	pipeline_free_timer_ipi();

	return ret;
}

void pipeline_uninstall_tick_proxy(void)
{
	/* Uninstall the proxy tick device. */
	tick_uninstall_proxy(&xnsched_realtime_cpus);

	pipeline_free_timer_ipi();

#ifdef CONFIG_XENO_OPT_STATS_IRQS
	xnintr_destroy(&nktimer);
#endif /* CONFIG_XENO_OPT_STATS_IRQS */
}
