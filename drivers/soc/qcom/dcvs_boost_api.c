// SPDX-License-Identifier: GPL-2.0-only

#include <linux/err.h>
#include <linux/export.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/rcupdate.h>

#include <soc/qcom/dcvs_boost.h>

static DEFINE_MUTEX(dcvs_boost_ops_lock);
static const struct qcom_dcvs_boost_ops __rcu *dcvs_boost_ops;

int qcom_dcvs_boost_register_ops(const struct qcom_dcvs_boost_ops *ops)
{
	int ret = 0;

	if (!ops || !ops->kick || !ops->kick_max)
		return -EINVAL;

	mutex_lock(&dcvs_boost_ops_lock);
	if (rcu_access_pointer(dcvs_boost_ops))
		ret = -EBUSY;
	else
		rcu_assign_pointer(dcvs_boost_ops, ops);
	mutex_unlock(&dcvs_boost_ops_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(qcom_dcvs_boost_register_ops);

void qcom_dcvs_boost_unregister_ops(const struct qcom_dcvs_boost_ops *ops)
{
	mutex_lock(&dcvs_boost_ops_lock);
	if (rcu_access_pointer(dcvs_boost_ops) == ops)
		RCU_INIT_POINTER(dcvs_boost_ops, NULL);
	mutex_unlock(&dcvs_boost_ops_lock);

	synchronize_rcu();
}
EXPORT_SYMBOL_GPL(qcom_dcvs_boost_unregister_ops);

void qcom_dcvs_bus_boost_kick(unsigned int duration_ms)
{
	const struct qcom_dcvs_boost_ops *ops;

	rcu_read_lock();
	ops = rcu_dereference(dcvs_boost_ops);
	if (ops)
		ops->kick(duration_ms);
	rcu_read_unlock();
}
EXPORT_SYMBOL_GPL(qcom_dcvs_bus_boost_kick);

void qcom_dcvs_bus_boost_kick_max(unsigned int duration_ms)
{
	const struct qcom_dcvs_boost_ops *ops;

	rcu_read_lock();
	ops = rcu_dereference(dcvs_boost_ops);
	if (ops)
		ops->kick_max(duration_ms);
	rcu_read_unlock();
}
EXPORT_SYMBOL_GPL(qcom_dcvs_bus_boost_kick_max);

MODULE_DESCRIPTION("QCOM DCVS boost API bridge");
MODULE_LICENSE("GPL");
