/*
 * Copyright (C) 2005 Jan Kiszka <jan.kiszka@web.de>.
 *
 * Xenomai is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Xenomai is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Xenomai; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/semaphore.h>
#include <linux/ipipe_trace.h>
#include <cobalt/kernel/arith.h>
#include <rtdm/testing.h>
#include <rtdm/driver.h>

struct rt_tmbench_context {
	int mode;
	unsigned long period;
	int freeze_max;
	int warmup_loops;
	int samples_per_sec;
	long *histogram_min;
	long *histogram_max;
	long *histogram_avg;
	int histogram_size;
	int bucketsize;

	rtdm_task_t timer_task;

	rtdm_timer_t timer;
	int warmup;
	uint64_t start_time;
	uint64_t date;
	struct rttst_bench_res curr;

	rtdm_event_t result_event;
	struct rttst_interm_bench_res result;

	struct semaphore nrt_mutex;
};

MODULE_LICENSE("GPL");
MODULE_AUTHOR("jan.kiszka@web.de");

static inline void add_histogram(struct rt_tmbench_context *ctx,
				 long *histogram, long addval)
{
	/* bucketsize steps */
	long inabs = (addval >= 0 ? addval : -addval) / ctx->bucketsize;
	histogram[inabs < ctx->histogram_size ?
		  inabs : ctx->histogram_size - 1]++;
}

static inline long long slldiv(long long s, unsigned d)
{
	return s >= 0 ? xnarch_ulldiv(s, d, NULL) : -xnarch_ulldiv(-s, d, NULL);
}

static void eval_inner_loop(struct rt_tmbench_context *ctx, long dt)
{
	if (dt > ctx->curr.max)
		ctx->curr.max = dt;
	if (dt < ctx->curr.min)
		ctx->curr.min = dt;
	ctx->curr.avg += dt;

#ifdef CONFIG_IPIPE_TRACE
	if (ctx->freeze_max && (dt > ctx->result.overall.max) && !ctx->warmup) {
		ipipe_trace_frozen_reset();
		ipipe_trace_freeze(dt);
		ctx->result.overall.max = dt;
	}
#endif /* CONFIG_IPIPE_TRACE */

	ctx->date += ctx->period;

	if (!ctx->warmup && ctx->histogram_size)
		add_histogram(ctx, ctx->histogram_avg, dt);

	/* Evaluate overruns and adjust next release date.
	   Beware of signedness! */
	while (dt > 0 && (unsigned long)dt > ctx->period) {
		ctx->curr.overruns++;
		ctx->date += ctx->period;
		dt -= ctx->period;
	}
}

static void eval_outer_loop(struct rt_tmbench_context *ctx)
{
	if (!ctx->warmup) {
		if (ctx->histogram_size) {
			add_histogram(ctx, ctx->histogram_max, ctx->curr.max);
			add_histogram(ctx, ctx->histogram_min, ctx->curr.min);
		}

		ctx->result.last.min = ctx->curr.min;
		if (ctx->curr.min < ctx->result.overall.min)
			ctx->result.overall.min = ctx->curr.min;

		ctx->result.last.max = ctx->curr.max;
		if (ctx->curr.max > ctx->result.overall.max)
			ctx->result.overall.max = ctx->curr.max;

		ctx->result.last.avg =
		    slldiv(ctx->curr.avg, ctx->samples_per_sec);
		ctx->result.overall.avg += ctx->result.last.avg;
		ctx->result.overall.overruns += ctx->curr.overruns;
		rtdm_event_pulse(&ctx->result_event);
	}

	if (ctx->warmup &&
	    (ctx->result.overall.test_loops == ctx->warmup_loops)) {
		ctx->result.overall.test_loops = 0;
		ctx->warmup = 0;
	}

	ctx->curr.min = 10000000;
	ctx->curr.max = -10000000;
	ctx->curr.avg = 0;
	ctx->curr.overruns = 0;

	ctx->result.overall.test_loops++;
}

static void timer_task_proc(void *arg)
{
	struct rt_tmbench_context *ctx = arg;
	int count, err;
	spl_t s;

	/* first event: one millisecond from now. */
	ctx->date = rtdm_clock_read_monotonic() + 1000000;

	while (1) {
		for (count = 0; count < ctx->samples_per_sec; count++) {
			cobalt_atomic_enter(s);
			ctx->start_time = rtdm_clock_read_monotonic();
			err = rtdm_task_sleep_abs(ctx->date,
						  RTDM_TIMERMODE_ABSOLUTE);
			cobalt_atomic_leave(s);
			if (err)
				return;

			eval_inner_loop(ctx,
					(long)(rtdm_clock_read_monotonic() -
					       ctx->date));
		}
		eval_outer_loop(ctx);
	}
}

static void timer_proc(rtdm_timer_t *timer)
{
	struct rt_tmbench_context *ctx =
	    container_of(timer, struct rt_tmbench_context, timer);
	int err;

	do {
		eval_inner_loop(ctx, (long)(rtdm_clock_read_monotonic() -
					    ctx->date));

		ctx->start_time = rtdm_clock_read_monotonic();
		err = rtdm_timer_start_in_handler(&ctx->timer, ctx->date, 0,
						  RTDM_TIMERMODE_ABSOLUTE);

		if (++ctx->curr.test_loops >= ctx->samples_per_sec) {
			ctx->curr.test_loops = 0;
			eval_outer_loop(ctx);
		}
	} while (err);
}

static int rt_tmbench_open(struct rtdm_fd *fd, int oflags)
{
	struct rt_tmbench_context *ctx;

	ctx = rtdm_fd_to_private(fd);

	ctx->mode = RTTST_TMBENCH_INVALID;
	sema_init(&ctx->nrt_mutex, 1);

	return 0;
}

static void rt_tmbench_close(struct rtdm_fd *fd)
{
	struct rt_tmbench_context *ctx;

	ctx = rtdm_fd_to_private(fd);

	down(&ctx->nrt_mutex);

	if (ctx->mode >= 0) {
		if (ctx->mode == RTTST_TMBENCH_TASK)
			rtdm_task_destroy(&ctx->timer_task);
		else if (ctx->mode == RTTST_TMBENCH_HANDLER)
			rtdm_timer_destroy(&ctx->timer);

		rtdm_event_destroy(&ctx->result_event);

		if (ctx->histogram_size)
			kfree(ctx->histogram_min);

		ctx->mode = RTTST_TMBENCH_INVALID;
		ctx->histogram_size = 0;
	}

	up(&ctx->nrt_mutex);
}

static int rt_tmbench_start(struct rtdm_fd *fd,
			    struct rt_tmbench_context *ctx,
			    struct rttst_tmbench_config __user *user_config)
{
	int err = 0;
	spl_t s;

	struct rttst_tmbench_config config_buf;
	struct rttst_tmbench_config *config =
		(struct rttst_tmbench_config *)user_config;

	if (rtdm_fd_is_user(fd)) {
		if (rtdm_safe_copy_from_user
		    (fd, &config_buf,user_config,
		     sizeof(struct rttst_tmbench_config)) < 0)
			return -EFAULT;

		config = &config_buf;
	}

	down(&ctx->nrt_mutex);

	ctx->period = config->period;
	ctx->warmup_loops = config->warmup_loops;
	ctx->samples_per_sec = 1000000000 / ctx->period;
	ctx->histogram_size = config->histogram_size;
	ctx->freeze_max = config->freeze_max;

	if (ctx->histogram_size > 0) {
		ctx->histogram_min =
		    kmalloc(3 * ctx->histogram_size * sizeof(long),
			    GFP_KERNEL);
		ctx->histogram_max =
		    ctx->histogram_min + config->histogram_size;
		ctx->histogram_avg =
		    ctx->histogram_max + config->histogram_size;

		if (!ctx->histogram_min) {
			up(&ctx->nrt_mutex);
			return -ENOMEM;
		}

		memset(ctx->histogram_min, 0,
		       3 * ctx->histogram_size * sizeof(long));
		ctx->bucketsize = config->histogram_bucketsize;
	}

	ctx->result.overall.min = 10000000;
	ctx->result.overall.max = -10000000;
	ctx->result.overall.avg = 0;
	ctx->result.overall.test_loops = 1;
	ctx->result.overall.overruns = 0;

	ctx->warmup = 1;

	ctx->curr.min = 10000000;
	ctx->curr.max = -10000000;
	ctx->curr.avg = 0;
	ctx->curr.overruns = 0;
	ctx->mode = RTTST_TMBENCH_INVALID;

	rtdm_event_init(&ctx->result_event, 0);

	if (config->mode == RTTST_TMBENCH_TASK) {
		err = rtdm_task_init(&ctx->timer_task, "timerbench",
				timer_task_proc, ctx,
				config->priority, 0);
		if (!err)
			ctx->mode = RTTST_TMBENCH_TASK;
	} else {
		rtdm_timer_init(&ctx->timer, timer_proc,
				rtdm_fd_device(fd)->name);

		ctx->curr.test_loops = 0;

		ctx->mode = RTTST_TMBENCH_HANDLER;

		cobalt_atomic_enter(s);
		ctx->start_time = rtdm_clock_read_monotonic();

		/* first event: one millisecond from now. */
		ctx->date = ctx->start_time + 1000000;

		err = rtdm_timer_start(&ctx->timer, ctx->date, 0,
				RTDM_TIMERMODE_ABSOLUTE);
		cobalt_atomic_leave(s);
	}

	up(&ctx->nrt_mutex);

	return err;
}

static int rt_tmbench_stop(struct rt_tmbench_context *ctx,
			   struct rttst_overall_bench_res __user *user_res)
{
	struct rtdm_fd *fd = rtdm_private_to_fd(ctx);
	int err = 0;

	down(&ctx->nrt_mutex);

	if (ctx->mode < 0) {
		up(&ctx->nrt_mutex);
		return -EINVAL;
	}

	if (ctx->mode == RTTST_TMBENCH_TASK)
		rtdm_task_destroy(&ctx->timer_task);
	else if (ctx->mode == RTTST_TMBENCH_HANDLER)
		rtdm_timer_destroy(&ctx->timer);

	rtdm_event_destroy(&ctx->result_event);

	ctx->mode = RTTST_TMBENCH_INVALID;

	ctx->result.overall.avg =
	    slldiv(ctx->result.overall.avg,
		   ((ctx->result.overall.test_loops) > 1 ?
		    ctx->result.overall.test_loops : 2) - 1);

	if (rtdm_fd_is_user(fd))
		err = rtdm_safe_copy_to_user(fd, &user_res->result,
					     &ctx->result.overall,
					     sizeof(struct rttst_bench_res));
		/* Do not break on error here - we may have to free a
		   histogram buffer first. */
	else {
		struct rttst_overall_bench_res *res =
			(struct rttst_overall_bench_res *)user_res;

		memcpy(&res->result, &ctx->result.overall,
		       sizeof(struct rttst_bench_res));
	}

	if (ctx->histogram_size > 0) {
		int size = ctx->histogram_size * sizeof(long);

		if (rtdm_fd_is_user(fd)) {
			struct rttst_overall_bench_res res_buf;

			if (rtdm_safe_copy_from_user(fd,
						     &res_buf, user_res,
						     sizeof(res_buf)) < 0 ||
			    rtdm_safe_copy_to_user(fd,
				    (void __user *)res_buf.histogram_min,
				    ctx->histogram_min, size) < 0 ||
			    rtdm_safe_copy_to_user(fd,
				    (void __user *)res_buf.histogram_max,
				    ctx->histogram_max, size) < 0 ||
			    rtdm_safe_copy_to_user(fd,
				    (void __user *)res_buf.histogram_avg,
				    ctx->histogram_avg, size) < 0)
				err = -EFAULT;
		} else {
			struct rttst_overall_bench_res *res =
				(struct rttst_overall_bench_res *)user_res;

			memcpy(res->histogram_min, ctx->histogram_min, size);
			memcpy(res->histogram_max, ctx->histogram_max, size);
			memcpy(res->histogram_avg, ctx->histogram_avg, size);
		}

		kfree(ctx->histogram_min);
	}

	up(&ctx->nrt_mutex);

	return err;
}

static int rt_tmbench_ioctl_nrt(struct rtdm_fd *fd,
				unsigned int request, void __user *arg)
{
	struct rt_tmbench_context *ctx;
	int err = 0;

	ctx = rtdm_fd_to_private(fd);

	switch (request) {
	case RTTST_RTIOC_TMBENCH_START:
		err = rt_tmbench_start(fd, ctx, arg);
		break;

	case RTTST_RTIOC_TMBENCH_STOP:
		err = rt_tmbench_stop(ctx, arg);
		break;
	default:
		err = -ENOTTY;
	}

	return err;
}

static int rt_tmbench_ioctl_rt(struct rtdm_fd *fd,
			       unsigned int request, void __user *arg)
{
	struct rt_tmbench_context *ctx;
	int err = 0;

	ctx = rtdm_fd_to_private(fd);

	switch (request) {
	case RTTST_RTIOC_INTERM_BENCH_RES:
		err = rtdm_event_wait(&ctx->result_event);
		if (err)
			return err;

		if (rtdm_fd_is_user(fd)) {
			struct rttst_interm_bench_res __user *user_res = arg;

			err = rtdm_safe_copy_to_user(fd, user_res,
						     &ctx->result,
						     sizeof(*user_res));
		} else {
			struct rttst_interm_bench_res *res = (void *)arg;

			memcpy(res, &ctx->result, sizeof(*res));
		}

		break;

	default:
		err = -ENOSYS;
	}

	return err;
}

static struct rtdm_device_class timerbench = {
	.profile_info		= RTDM_PROFILE_INFO(timerbench,
						    RTDM_CLASS_TESTING,
						    RTDM_SUBCLASS_TIMERBENCH,
						    RTTST_PROFILE_VER),
	.device_flags		= RTDM_NAMED_DEVICE,
	.device_count		= 1,
	.context_size		= sizeof(struct rt_tmbench_context),
	.ops = {
		.open		= rt_tmbench_open,
		.close		= rt_tmbench_close,
		.ioctl_rt	= rt_tmbench_ioctl_rt,
		.ioctl_nrt	= rt_tmbench_ioctl_nrt,
	},
	.driver_name		= "timerbench",
	.driver_version		= RTDM_DRIVER_VER(0, 2, 1),
	.peripheral_name	= "Timer Latency Benchmark",
	.provider_name		= "Jan Kiszka",
};

static struct rtdm_device device = {
	.class = &timerbench,
	.label = "timerbench",
};

static int __init __timerbench_init(void)
{
	if (!realtime_core_enabled())
		return 0;

	return rtdm_dev_register(&device);
}

static void __timerbench_exit(void)
{
	if (!realtime_core_enabled())
		return;

	rtdm_dev_unregister(&device, 1000);
}

module_init(__timerbench_init);
module_exit(__timerbench_exit);
