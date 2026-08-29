/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SCHED_TASK_CRITICAL_H
#define _LINUX_SCHED_TASK_CRITICAL_H

#include <linux/compiler.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>

#define CRITICAL_OOM_SCORE_ADJ	(-900)

/*
 * Return true if @p is a performance-sensitive (foreground / top-app / UI)
 * task.  Used to keep eager preemption for critical work while allowing
 * background wakeups to be lazily deferred.
 */
static inline bool sched_task_critical(const struct task_struct *p)
{
	if (!p || (p->flags & PF_KTHREAD) || unlikely(!READ_ONCE(p->signal)))
		return false;

	return READ_ONCE(p->signal->oom_score_adj) <= CRITICAL_OOM_SCORE_ADJ;
}

#endif /* _LINUX_SCHED_TASK_CRITICAL_H */
