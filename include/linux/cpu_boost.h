#ifndef _CPU_BOOST_H
#define _CPU_BOOST_H

#include <linux/types.h>

#if IS_ENABLED(CONFIG_CPU_BOOSTING)
void cpu_boost_max(unsigned int duration_ms);
void cpu_boost_kick(unsigned int duration_ms);
bool cpu_boost_active(int cpu);
#else
static inline void cpu_boost_max(unsigned int duration_ms)
{
}
static inline void cpu_boost_kick(unsigned int duration_ms)
{
}
static inline bool cpu_boost_active(int cpu)
{
	return false;
}
#endif /* CONFIG_CPU_BOOST_MAX */
#endif /* _CPU_BOOST_H */
