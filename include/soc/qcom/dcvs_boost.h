/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __SOC_QCOM_DCVS_BOOST_H__
#define __SOC_QCOM_DCVS_BOOST_H__

struct qcom_dcvs_boost_ops {
	void (*kick)(unsigned int duration_ms);
	void (*kick_max)(unsigned int duration_ms);
};

#if defined(CONFIG_QCOM_DCVS_BOOST_API)
int qcom_dcvs_boost_register_ops(const struct qcom_dcvs_boost_ops *ops);
void qcom_dcvs_boost_unregister_ops(const struct qcom_dcvs_boost_ops *ops);
void qcom_dcvs_bus_boost_kick(unsigned int duration_ms);
void qcom_dcvs_bus_boost_kick_max(unsigned int duration_ms);
#else
static inline int
qcom_dcvs_boost_register_ops(const struct qcom_dcvs_boost_ops *ops)
{
	return 0;
}

static inline void
qcom_dcvs_boost_unregister_ops(const struct qcom_dcvs_boost_ops *ops)
{
}
static inline void qcom_dcvs_bus_boost_kick(unsigned int duration_ms)
{
}
static inline void qcom_dcvs_bus_boost_kick_max(unsigned int duration_ms)
{
}
#endif /* CONFIG_QCOM_DCVS_BOOST_API */
#endif /* __SOC_QCOM_DCVS_BOOST_H__ */

