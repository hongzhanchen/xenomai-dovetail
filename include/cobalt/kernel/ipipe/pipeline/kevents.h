/*
 * SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2019 Philippe Gerum  <rpm@xenomai.org>
 */

#ifndef _COBALT_KERNEL_IPIPE_KEVENTS_H
#define _COBALT_KERNEL_IPIPE_KEVENTS_H

struct cobalt_process;
struct cobalt_thread;

static inline
int pipeline_attach_process(struct cobalt_process *process)
{
	return 0;
}

static inline
void pipeline_detach_process(struct cobalt_process *process)
{ }

int pipeline_prepare_current(void);

void pipeline_attach_current(struct xnthread *thread);

int pipeline_trap_kevents(void);

void pipeline_enable_kevents(void);

#endif /* !_COBALT_KERNEL_IPIPE_KEVENTS_H */
