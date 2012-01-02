/*
 * Copyright (C) 2005 Philippe Gerum <rpm@xenomai.org>.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.

 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA.
 */

#include <errno.h>
#include <nucleus/synch.h>
#include <cobalt/syscall.h>
#include <kernel/cobalt/mutex.h>
#include <kernel/cobalt/cond.h>
#include <asm-generic/bits/current.h>
#include "internal.h"

extern unsigned long xeno_sem_heap[2];

static inline unsigned long *cond_get_signalsp(struct __shadow_cond *shadow)
{
	if (likely(!shadow->attr.pshared))
		return shadow->pending_signals;

	return (unsigned long *)(xeno_sem_heap[1]
				 + shadow->pending_signals_offset);
}

static inline struct mutex_dat *
cond_get_mutex_datp(struct __shadow_cond *shadow)
{
	if (shadow->mutex_datp == (struct mutex_dat *)~0UL)
		return NULL;

	if (likely(!shadow->attr.pshared))
		return shadow->mutex_datp;

	return (struct mutex_dat *)(xeno_sem_heap[1]
				    + shadow->mutex_datp_offset);
}

int __wrap_pthread_condattr_init(pthread_condattr_t *attr)
{
	return -XENOMAI_SKINCALL1(__cobalt_muxid, sc_cobalt_condattr_init, attr);
}

int __wrap_pthread_condattr_destroy(pthread_condattr_t *attr)
{
	return -XENOMAI_SKINCALL1(__cobalt_muxid,sc_cobalt_condattr_destroy,attr);
}

int __wrap_pthread_condattr_getclock(const pthread_condattr_t *attr,
				     clockid_t *clk_id)
{
	return -XENOMAI_SKINCALL2(__cobalt_muxid,
				  sc_cobalt_condattr_getclock, attr, clk_id);
}

int __wrap_pthread_condattr_setclock(pthread_condattr_t *attr,
				     clockid_t clk_id)
{
	return -XENOMAI_SKINCALL2(__cobalt_muxid,
				  sc_cobalt_condattr_setclock, attr, clk_id);
}

int __wrap_pthread_condattr_getpshared(const pthread_condattr_t *attr,
				       int *pshared)
{
	return -XENOMAI_SKINCALL2(__cobalt_muxid,
				  sc_cobalt_condattr_getpshared, attr, pshared);
}

int __wrap_pthread_condattr_setpshared(pthread_condattr_t *attr, int pshared)
{
	return -XENOMAI_SKINCALL2(__cobalt_muxid,
				  sc_cobalt_condattr_setpshared, attr, pshared);
}

int __wrap_pthread_cond_init(pthread_cond_t *cond,
			     const pthread_condattr_t * attr)
{
	struct __shadow_cond *_cnd = &((union __xeno_cond *)cond)->shadow_cond;
	int err;

	err = XENOMAI_SKINCALL2(__cobalt_muxid, sc_cobalt_cond_init, _cnd, attr);
	if (!err && !_cnd->attr.pshared) {
		_cnd->pending_signals = (unsigned long *)
			(xeno_sem_heap[0] + _cnd->pending_signals_offset);
	}

	return -err;
}

int __wrap_pthread_cond_destroy(pthread_cond_t *cond)
{
	struct __shadow_cond *_cond = &((union __xeno_cond *)cond)->shadow_cond;

	return -XENOMAI_SKINCALL1(__cobalt_muxid, sc_cobalt_cond_destroy, _cond);
}

struct cobalt_cond_cleanup_t {
	struct __shadow_cond *cond;
	struct __shadow_mutex *mutex;
	unsigned count;
	int err;
};

static void __pthread_cond_cleanup(void *data)
{
	struct cobalt_cond_cleanup_t *c = (struct cobalt_cond_cleanup_t *)data;
	int err;

	do {
		err = XENOMAI_SKINCALL2(__cobalt_muxid,
					sc_cobalt_cond_wait_epilogue,
					c->cond, c->mutex);
	} while (err == -EINTR);

	c->mutex->lockcnt = c->count;
}

int __wrap_pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex)
{
	struct __shadow_cond *_cnd = &((union __xeno_cond *)cond)->shadow_cond;
	struct __shadow_mutex *_mx =
		&((union __xeno_mutex *)mutex)->shadow_mutex;
	struct cobalt_cond_cleanup_t c = {
		.cond = _cnd,
		.mutex = _mx,
		.err = 0,
	};
	int err, oldtype;
	unsigned count;

	if (_mx->magic != COBALT_MUTEX_MAGIC
	    || _cnd->magic != COBALT_COND_MAGIC)
		return EINVAL;

	if (_mx->attr.type == PTHREAD_MUTEX_ERRORCHECK) {
		xnhandle_t cur = xeno_get_current();

		if (cur == XN_NO_HANDLE)
			return EPERM;

		if (xnsynch_fast_owner_check(mutex_get_ownerp(_mx), cur))
			return EPERM;
	}

	pthread_cleanup_push(&__pthread_cond_cleanup, &c);

	count = _mx->lockcnt;

	pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, &oldtype);

	err = XENOMAI_SKINCALL5(__cobalt_muxid,
				 sc_cobalt_cond_wait_prologue,
				 _cnd, _mx, &c.err, 0, NULL);

	pthread_setcanceltype(oldtype, NULL);

	pthread_cleanup_pop(0);

	while (err == -EINTR)
		err = XENOMAI_SKINCALL2(__cobalt_muxid,
					sc_cobalt_cond_wait_epilogue, _cnd, _mx);

	_mx->lockcnt = count;

	pthread_testcancel();

	return -err ?: -c.err;
}

int __wrap_pthread_cond_timedwait(pthread_cond_t *cond,
				  pthread_mutex_t *mutex,
				  const struct timespec *abstime)
{
	struct __shadow_cond *_cnd = &((union __xeno_cond *)cond)->shadow_cond;
	struct __shadow_mutex *_mx =
		&((union __xeno_mutex *)mutex)->shadow_mutex;
	struct cobalt_cond_cleanup_t c = {
		.cond = _cnd,
		.mutex = _mx,
	};
	int err, oldtype;
	unsigned count;

	if (_mx->magic != COBALT_MUTEX_MAGIC
	    || _cnd->magic != COBALT_COND_MAGIC)
		return EINVAL;

	if (_mx->attr.type == PTHREAD_MUTEX_ERRORCHECK) {
		xnhandle_t cur = xeno_get_current();

		if (cur == XN_NO_HANDLE)
			return EPERM;

		if (xnsynch_fast_owner_check(mutex_get_ownerp(_mx), cur))
			return EPERM;
	}

	pthread_cleanup_push(&__pthread_cond_cleanup, &c);

	count = _mx->lockcnt;

	pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, &oldtype);

	err = XENOMAI_SKINCALL5(__cobalt_muxid,
				sc_cobalt_cond_wait_prologue,
				_cnd, _mx, &c.err, 1, abstime);
	pthread_setcanceltype(oldtype, NULL);

	pthread_cleanup_pop(0);

	while (err == -EINTR)
		err = XENOMAI_SKINCALL2(__cobalt_muxid,
					sc_cobalt_cond_wait_epilogue, _cnd, _mx);

	_mx->lockcnt = count;

	pthread_testcancel();

	return -err ?: -c.err;
}

int __wrap_pthread_cond_signal(pthread_cond_t *cond)
{
	struct __shadow_cond *_cnd = &((union __xeno_cond *)cond)->shadow_cond;
	unsigned long pending_signals, *pending_signalsp;
	struct mutex_dat *mutex_datp;

	if (_cnd->magic != COBALT_COND_MAGIC)
		return EINVAL;

	mutex_datp = cond_get_mutex_datp(_cnd);
	if (mutex_datp) {
		unsigned long flags = mutex_datp->flags ;

		if (unlikely(flags & COBALT_MUTEX_ERRORCHECK)) {
			xnhandle_t cur = xeno_get_current();

			if (cur == XN_NO_HANDLE)
				return EPERM;

			if (xnsynch_fast_owner_check(&mutex_datp->owner, cur) < 0)
				return EPERM;
		}

		mutex_datp->flags = flags | COBALT_MUTEX_COND_SIGNAL;

		pending_signalsp = cond_get_signalsp(_cnd);
		pending_signals = *pending_signalsp;
		if (pending_signals != ~0UL)
			*pending_signalsp = pending_signals + 1;
	}

	return 0;
}

int __wrap_pthread_cond_broadcast(pthread_cond_t *cond)
{
	struct __shadow_cond *_cnd = &((union __xeno_cond *)cond)->shadow_cond;
	struct mutex_dat *mutex_datp;

	if (_cnd->magic != COBALT_COND_MAGIC)
		return EINVAL;

	mutex_datp = cond_get_mutex_datp(_cnd);
	if (mutex_datp) {
		unsigned long flags = mutex_datp->flags ;

		if (unlikely(flags & COBALT_MUTEX_ERRORCHECK)) {
			xnhandle_t cur = xeno_get_current();

			if (cur == XN_NO_HANDLE)
				return EPERM;

			if (xnsynch_fast_owner_check(&mutex_datp->owner, cur) < 0)
				return EPERM;
		}

		mutex_datp->flags = flags | COBALT_MUTEX_COND_SIGNAL;

		*cond_get_signalsp(_cnd) = ~0UL;
	}

	return 0;
}
