// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023-2025 Sultan Alsawaf <sultan@kerneltoast.com>.
 */

/**
 * DOC: Capacity Aware Superset Scheduler (CASS) description
 *
 * The Capacity Aware Superset Scheduler (CASS) optimizes runqueue selection of
 * CFS tasks. By using CPU capacity as a basis for comparing the relative
 * utilization between different CPUs, CASS fairly balances load across CPUs of
 * varying capacities. This results in improved multi-core performance,
 * especially when CPUs are overutilized because CASS doesn't clip a CPU's
 * utilization when it eclipses the CPU's capacity.
 *
 * As a superset of capacity aware scheduling, CASS implements a hierarchy of
 * criteria to determine the better CPU to wake a task upon between CPUs that
 * have the same relative utilization. This way, single-core performance,
 * latency, and cache affinity are all optimized where possible.
 *
 * CASS doesn't feature explicit energy awareness but its basic load balancing
 * principle results in decreased overall energy, often better than what is
 * possible with explicit energy awareness. By fairly balancing load based on
 * relative utilization, all CPUs are kept at their lowest P-state necessary to
 * satisfy the overall load at any given moment.
 */

struct cass_cpu_cand {
	int cpu;
	unsigned int exit_lat;
	unsigned int highest_prio;
	unsigned int rt_throttled;
	unsigned int cannot_preempt;
	unsigned long cap;
	unsigned long cap_max;
	unsigned long cap_no_therm;
	unsigned long cap_orig;
	unsigned long therm;
	unsigned long eff_util;
	unsigned long hard_util;
	unsigned long util;
};

static __always_inline
void cass_cpu_util(struct cass_cpu_cand *c, int this_cpu, bool sync)
{
	struct rq *rq = cpu_rq(c->cpu);
	struct cfs_rq *cfs_rq = &rq->cfs;
	unsigned long hard_util;
	unsigned long est;

	/* Get this CPU's utilization from CFS tasks */
	c->util = READ_ONCE(cfs_rq->avg.util_avg);
	if (sched_feat(UTIL_EST)) {
		est = READ_ONCE(cfs_rq->avg.util_est);
		if (est > c->util) {
			/* Don't deduct @current's util from estimated util */
			sync = false;
			c->util = est;
		}
	}

	/*
	 * Deduct @current's util from this CPU if this is a sync wake, unless
	 * @current is an RT task; RT tasks don't have per-entity load tracking.
	 */
	if (sync && c->cpu == this_cpu && !rt_task(current))
		c->util -= min(c->util, task_util(current));

	/* Get the utilization of everything other than CFS tasks */
	hard_util = cpu_util_rt(rq) + cpu_util_dl(rq) + cpu_util_irq(rq);
	c->hard_util = hard_util;

	/*
	 * Account for lost capacity due to time spent in RT/DL tasks and IRQs.
	 * Capacity is considered lost to RT tasks even when @p is an RT task in
	 * order to produce consistently balanced task placement results between
	 * CFS and RT tasks when CASS selects a CPU for them.
	 */
	c->cap = c->cap_max - min(hard_util, c->cap_max - 1);

	/* Get the current capacity with thermal pressure excluded */
	c->cap_no_therm = c->cap_orig - min(hard_util, c->cap_orig - 1);
}

/*
 * Returns the CPU id of the single prime CPU, or -1 if there isn't one.
 */
static __always_inline
int cass_prime_cpu_id(void)
{
	int prime = -1, prev = -1;
	int cpu;

	for_each_cpu(cpu, cpu_possible_mask) {
		prev = prime;
		prime = cpu;
	}

	if (prime < 0 || prev < 0)
		return -1;

	if (arch_scale_cpu_capacity(prev) >= arch_scale_cpu_capacity(prime))
		return -1;

	return prime;
}

static __always_inline
bool cass_prime_cpu(const struct cass_cpu_cand *c, int prime_cpu)
{
	return c->cpu == prime_cpu;
}

/* Returns true if @a is a better CPU than @b */
static __always_inline
bool cass_cpu_better(const struct cass_cpu_cand *a,
		     const struct cass_cpu_cand *b, unsigned long p_util,
		     unsigned long uc_min, int this_cpu, int prev_cpu, bool sync,
		     bool rt, bool prefer_idle, bool allow_prime_avoidance,
		     s64 eevdf_lag_margin, int prime_cpu)
{
#define cass_cmp(a, b) ({ res = (a) - (b); })
#define cass_eq(a, b) ({ res = (a) == (b); })
	long res;
	bool a_prime = cass_prime_cpu(a, prime_cpu);
	bool b_prime = cass_prime_cpu(b, prime_cpu);
	const struct cass_cpu_cand *non_prime;
	unsigned long margin, hyst;

	/* Prefer the CPU that's not overloaded */
	if (cass_cmp(b->eff_util * a->cap_max, a->eff_util * b->cap_max))
		goto done;

	/* Prefer the CPU that fits the task */
	if (cass_cmp(fits_capacity(p_util, a->cap_max),
		     fits_capacity(p_util, b->cap_max)))
		goto done;

	/*
	 * Prefer packing small, non-sync work on an active cpu over waking an idle
	 * CPU, unless the active CPU is much worse.
	 */
	if (!prefer_idle && !!a->exit_lat != !!b->exit_lat) {
		if (!a->exit_lat && b->exit_lat) {
			if (a->eff_util <= a->cap_max &&
			    a->util <= b->util + eevdf_lag_margin) {
				res = 1;
				goto done;
			}
		} else if (a->exit_lat && !b->exit_lat) {
			if (b->eff_util <= b->cap_max &&
			    b->util <= a->util + eevdf_lag_margin) {
				res = -1;
				goto done;
			}
		}
	}

	/* RT specific preferences */
	if (rt) {
		/* Prefer CPU that is not marked by cannot_preempt */
		if (cass_cmp(b->cannot_preempt, a->cannot_preempt))
			goto done;

		/* Prefer CPU that does not have throttled runqueues */
		if (cass_cmp(b->rt_throttled, a->rt_throttled))
			goto done;

		/* Prefer CPUs with lower priority top tasks */
		if (cass_cmp(a->highest_prio, b->highest_prio))
			goto done;
	}

	/*
	 * Prefer the CPU that isn't the single fastest one in the system,
	 * but only for low-util, non-sync, non-urgent tasks.
	 */
	if (a_prime != b_prime) {
		non_prime = a_prime ? b : a;

		if (allow_prime_avoidance &&
		    non_prime->cap_max >= uc_min &&
		    fits_capacity(p_util, non_prime->cap_max) &&
		    cass_cmp(b_prime, a_prime))
			goto done;
	}

	/* Prefer lower relative utilization */
	if (cass_cmp(b->util, a->util))
		goto done;

	/*
	 * On shared llc systems (dsu/dynamiq) we can't reliably differentiate shared cache
	 * with prev_cpu. Retain locality by making prev_cpu stickier when close.
	 */
	if (a->cpu != prev_cpu && b->cpu == prev_cpu) {
		hyst = SCHED_CAPACITY_SCALE / 32; /* ~3% */

		/*
		 * If prev_cpu fits and isn't overloaded, require enough util advantage
		 * to move away from it.
		 */
		if (fits_capacity(p_util, b->cap_max) &&
			b->eff_util <= b->cap_max &&
			a->util <= b->util + hyst) {
			res = -1;
			goto done;
		}
	}

	/*
	 * Prefer the current CPU for sync wakes, but only if it isn't
	 * substantially more overloaded than the alternative.
	 */
	if (sync) {
		margin = SCHED_CAPACITY_SCALE / 20; /* 5% */

		if (a->cpu == this_cpu) {
			if (a->eff_util <= a->cap_max &&
			    a->util <= b->util + margin &&
			    cass_eq(a->cpu, this_cpu))
				goto done;
		} else if (b->cpu == this_cpu) {
			if (b->eff_util <= b->cap_max &&
			    b->util <= a->util + margin &&
			    !cass_cmp(b->cpu, this_cpu))
				goto done;
		}
	}

	/* Prefer the CPU with higher capacity */
	if (cass_cmp(a->cap, b->cap))
		goto done;

	/* Prefer the CPU that is idle */
	if (cass_cmp(!!a->exit_lat, !!b->exit_lat))
		goto done;

	/* Prefer the CPU with lower idle exit latency */
	if (cass_cmp(b->exit_lat, a->exit_lat))
		goto done;

	/* Prefer the CPU with less thermal pressure */
	if (cass_cmp(b->therm, a->therm))
		goto done;

	/* Prefer the previous CPU */
	if (cass_eq(a->cpu, prev_cpu) || !cass_cmp(b->cpu, prev_cpu))
		goto done;

	/* @a isn't a better CPU than @b. @res must be <=0 to indicate such. */
done:
	/* @a is a better CPU than @b if @res is positive */
	return res > 0;
}

static __always_inline
bool cass_allow_prime_avoidance(unsigned long p_util, unsigned long uc_min, bool sync)
{
	/* Never avoid prime CPU for sync wakes or uclamp-min constrained tasks */
	if (sync || uc_min)
		return false;
	/* Only avoid for tasks using < 12.5% of a single CPU */
	return p_util < (SCHED_CAPACITY_SCALE / 8);
}

static __always_inline
bool cass_prev_cpu_fastpath(struct task_struct *p, int prev_cpu, int this_cpu,
			    unsigned long p_util, unsigned long uc_min,
			    bool sync, bool rt)
{
	struct cass_cpu_cand c = { .cpu = prev_cpu };
	struct rq *rq;
	unsigned long therm;

	if (!cpumask_test_cpu(prev_cpu, p->cpus_ptr) || !cpu_active(prev_cpu))
		return false;

	rq = cpu_rq(prev_cpu);

	if (!((sync && prev_cpu == this_cpu && rq->nr_running == 1) ||
	      choose_idle_cpu(prev_cpu, p)))
		return false;

	c.cap_orig = max_t(unsigned long, 1, arch_scale_cpu_capacity(prev_cpu));
	therm = min(thermal_load_avg(rq), c.cap_orig - 1);
	c.cap_max = c.cap_orig - therm;
	c.therm = therm;

	if (c.cap_max < uc_min)
		return false;

	cass_cpu_util(&c, this_cpu, sync);

	if (prev_cpu != task_cpu(p))
		c.util += p_util;

	c.eff_util = max(c.util + c.hard_util, uc_min);

	if (!fits_capacity(p_util, c.cap_max))
		return false;

	return c.eff_util <= c.cap_max;
}

static int cass_best_cpu(struct task_struct *p, int prev_cpu, bool sync, bool rt)
{
	/* Initialize @best such that @best always has a valid CPU at the end */
	struct cass_cpu_cand cands[2], *best = cands;
	struct cfs_rq *cfs_rq;
	int p_cpu = task_cpu(p);
	int this_cpu = raw_smp_processor_id();
	unsigned long p_util, uc_min, eevdf_lag_margin;
	bool has_idle = false, best_valid = false;
	u64 p_vruntime = 0;
	bool allow_prime_avoidance;
	bool prefer_idle;
	int prime_cpu;
	int cidx = 0, cpu;

	/*
	 * Get the utilization and uclamp minimum threshold for this task. Note
	 * that RT tasks don't have per-entity load tracking.
	 */
	p_util = rt ? 0 : task_util_est(p);
	uc_min = uclamp_eff_value(p, UCLAMP_MIN);

	allow_prime_avoidance = cass_allow_prime_avoidance(p_util, uc_min, sync);
	prime_cpu = cass_prime_cpu_id();

	/*
	 * Prefer idle CPUs for sync wakes and for "heavy enough" work; otherwise,
	 * prefer packing onto an already-active CPU.
	 */
	prefer_idle = sync || rt || uc_min || p_util >= (SCHED_CAPACITY_SCALE / 8);

	if (cass_prev_cpu_fastpath(p, prev_cpu, this_cpu, p_util, uc_min, sync, rt))
		return prev_cpu;

	if (!rt)
		p_vruntime = READ_ONCE(p->se.vruntime);

	eevdf_lag_margin = SCHED_CAPACITY_SCALE / 16;
	if (!rt && !sync && !uc_min && p_util < (SCHED_CAPACITY_SCALE / 8)) {
		cfs_rq = &cpu_rq(this_cpu)->cfs;
		s64 this_lag = READ_ONCE(cfs_rq->zero_vruntime) - (s64)p_vruntime;

		if (this_lag < 0) {
			eevdf_lag_margin = SCHED_CAPACITY_SCALE / 8;
		} else if (this_lag > 0) {
			eevdf_lag_margin = SCHED_CAPACITY_SCALE / 32;
		}
	}

	/*
	 * Find the best CPU to wake @p on. Although idle_get_state() requires
	 * an RCU read lock, an RCU read lock isn't needed because we're not
	 * preemptible and RCU-sched is unified with normal RCU. Therefore,
	 * non-preemptible contexts are implicitly RCU-safe.
	 *
	 * Note: @curr->cpu must be initialized before this loop ends. This is
	 * necessary to ensure @best->cpu contains a valid CPU upon returning;
	 * otherwise, if only one CPU is allowed and it is skipped before
	 * @curr->cpu is set, then @best->cpu will be garbage.
	 */
	for_each_cpu_and(cpu, p->cpus_ptr, cpu_active_mask) {
		/* Use the free candidate slot for @curr */
		struct cass_cpu_cand *curr = &cands[cidx];
		struct cpuidle_state *idle_state;
		struct rq *rq = cpu_rq(cpu);
		unsigned long therm;

		/* Initialize early so @best->cpu is never garbage */
		curr->cpu = cpu;

		/* Bias RT placement */
		if (rt) {
			curr->highest_prio = rq->rt.highest_prio.curr;
			curr->rt_throttled = rt_rq_throttled(&rq->rt) ? 1 : 0;
			/* If @p cannot preempt the top RT task, mark that. */
			curr->cannot_preempt = (p->prio > rq->rt.highest_prio.curr) ? 1 : 0;
		}

		/* Get the original, maximum _possible_ capacity of this CPU */
		curr->cap_orig = max_t(unsigned long, 1,
				       arch_scale_cpu_capacity(cpu));

		/* Get the _current_, throttled maximum capacity of this CPU */
		therm = min(thermal_load_avg(rq), curr->cap_orig - 1);
		curr->cap_max = curr->cap_orig - therm;
		curr->therm = therm;

		/* Prefer the CPU that more closely meets the uclamp minimum */
		if (best_valid && curr->cap_max < uc_min && best->cap_max >= uc_min)
			continue;

		/*
		 * Check if this CPU is idle or only has SCHED_IDLE tasks. For
		 * sync wakes, treat the current CPU as idle if @current is the
		 * only running task.
		 */
		if ((sync && cpu == this_cpu && rq->nr_running == 1) ||
		    choose_idle_cpu(cpu, p)) {
			/*
			 * A non-idle candidate may be better for energy
			 * efficiency when @p is uclamp boosted above @curr's
			 * minimum capacity, or when the only idle candidate
			 * found so far is the prime CPU. Otherwise, prefer idle
			 * candidates.
			 */
			if (!has_idle && prefer_idle &&
			    uc_min <= arch_scale_min_freq_capacity(cpu) &&
			    !cass_prime_cpu(curr, prime_cpu)) {
				/* Discard any previous non-idle candidate */
				best = curr;
				has_idle = true;
			}

			/* Nonzero exit latency indicates this CPU is idle */
			curr->exit_lat = 1;

			/* Add on the actual idle exit latency, if any */
			idle_state = idle_get_state(rq);
			if (idle_state)
				curr->exit_lat += idle_state->exit_latency;
		} else {
			/* Skip non-idle CPUs if there's an idle candidate */
			if (has_idle && prefer_idle)
				continue;

			/* Zero exit latency indicates this CPU isn't idle */
			curr->exit_lat = 0;
		}

		/* Get this CPU's capacity and utilization */
		cass_cpu_util(curr, this_cpu, sync);

		/*
		 * Add @p's utilization to this CPU if it's not @p's CPU, to
		 * find what this CPU's relative utilization would look like if
		 * @p were on it.
		 */
		if (cpu != p_cpu)
			curr->util += p_util;

		/*
		 * Calculate the effective utilization for this CPU candidate;
		 * i.e., the utilization calculated by the CPU governor. This is
		 * needed to evaluate whether or not a throttled CPU is
		 * overloaded, since the relative utilization calculation
		 * disregards thermal pressure.
		 */
		curr->eff_util = max(curr->util + curr->hard_util, uc_min);

		/* Clamp the utilization to the minimum performance threshold */
		if (curr->util < uc_min)
			curr->util = uc_min;

		/*
		 * Calculate the relative utilization for this CPU candidate
		 * without thermal pressure included. Thermal pressure needs to
		 * be disregarded in order to fairly distribute load such that
		 * higher P-states aren't pushed on CPUs that are throttled to a
		 * lesser degree. For example, if CPU A were throttled to 50% of
		 * its maximum possible capacity, and CASS targeted 20% relative
		 * load on all CPUs, CPU A would receive (20% * 50%) = 10% load
		 * relative to its maximum possible P-state. This burden would
		 * then be redistributed to other CPUs, causing a load imbalance
		 * that would reduce CASS's energy efficiency due to
		 * disproportionate P-states.
		 */
		curr->util =
			curr->util * SCHED_CAPACITY_SCALE / curr->cap_no_therm;

		/*
		 * Check if this CPU is better than the best CPU found so far.
		 * If @best == @curr then there's no need to compare them, but
		 * cidx still needs to be changed to the other candidate slot.
		 */
		if (!best_valid ||
			cass_cpu_better(curr, best, p_util, uc_min, this_cpu, prev_cpu,
				    sync, rt, prefer_idle, allow_prime_avoidance,
				    eevdf_lag_margin, prime_cpu)) {
			best = curr;
			best_valid = true;
			cidx ^= 1;
			/*
			 * Early-exit when we land on an idle, cache-affined CPU that
			 * cleanly fits the task and isn't overloaded.
			 */
			if (best->exit_lat &&
				best->cpu == prev_cpu &&
				!cass_prime_cpu(best, prime_cpu) &&
				best->cap_max >= uc_min &&
				fits_capacity(p_util, best->cap_max) &&
				best->eff_util <= best->cap_max)
				return best->cpu;
			/*
			 * Also early-exit for an obvious packed choice: non-idle, fits,
			 * not overloaded, and prev_cpu.
			*/
			if (!best->exit_lat &&
				best->cpu == prev_cpu &&
				!cass_prime_cpu(best, prime_cpu) &&
				best->cap_max >= uc_min &&
				fits_capacity(p_util, best->cap_max) &&
				best->eff_util <= best->cap_max)
				return best->cpu;
		}
	}

	return best->cpu;
}

static int cass_select_task_rq(struct task_struct *p, int prev_cpu,
			       int wake_flags, bool rt)
{
	bool sync;

	/* Don't balance on exec since we don't know what @p will look like */
	if (wake_flags & SD_BALANCE_EXEC)
		return prev_cpu;

	/*
	 * If there aren't any valid CPUs which are active, then just return the
	 * first valid CPU since it's possible for certain types of tasks to run
	 * on inactive CPUs.
	 */
	if (unlikely(!cpumask_intersects(p->cpus_ptr, cpu_active_mask)))
		return cpumask_first(p->cpus_ptr);

	/* cass_best_cpu() needs the CFS task's utilization, so sync it up */
	if (!rt && !(wake_flags & SD_BALANCE_FORK))
		sync_entity_load_avg(&p->se);

	sync = (wake_flags & WF_SYNC) && !(current->flags & PF_EXITING);
	return cass_best_cpu(p, prev_cpu, sync, rt);
}

static int cass_select_task_rq_fair(struct task_struct *p, int prev_cpu,
				    int wake_flags)
{
	return cass_select_task_rq(p, prev_cpu, wake_flags, false);
}

int cass_select_task_rq_rt(struct task_struct *p, int prev_cpu, int wake_flags)
{
	return cass_select_task_rq(p, prev_cpu, wake_flags, true);
}
