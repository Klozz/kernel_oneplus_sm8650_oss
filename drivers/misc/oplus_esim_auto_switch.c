// SPDX-License-Identifier: GPL-2.0-only
/*
 * Automatic eSIM/Physical SIM2 Switcher Driver
 * Copyright (C) 2025 The YAAP Project
 *
 * This driver automatically manages eSIM power based on physical SIM2 detection
 * - Disables eSIM when physical SIM2 is inserted
 * - Enables eSIM when physical SIM2 is removed
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/workqueue.h>
#include <linux/delay.h>
#include <linux/pinctrl/consumer.h>
#include <linux/mutex.h>

#define DRIVER_NAME "esim-auto-switch"
#define SIM_DETECT_DEBOUNCE_MS 300

struct esim_switch_data {
    int sim2_detect_gpio;
    int esim_control_gpio;

    struct pinctrl *pinctrl;
    struct pinctrl_state *sim2_pull_high;
    struct pinctrl_state *sim2_pull_low;
    struct pinctrl_state *sim2_no_pull;

    int irq_number;
    struct delayed_work detect_work;

    bool esim_enabled;
    bool sim2_present;

    struct mutex lock;

    struct device *dev;
};

/* External QMI functions for modem control */
extern int uim_qmi_power_up_req(u8 slot_id);
extern int uim_qmi_power_down_req(u8 slot_id);

/**
 * enable_esim() - Enable eSIM power
 */
static void enable_esim(struct esim_switch_data *data)
{
    if (data->esim_enabled) {
        dev_info(data->dev, "eSIM already enabled\n");
        return;
    }

    if (!gpio_is_valid(data->esim_control_gpio)) {
        dev_warn(data->dev, "eSIM control GPIO invalid; skipping enable\n");
        data->esim_enabled = false;
        return;
    }

    dev_info(data->dev, "Enabling eSIM\n");

    /* Set eSIM control GPIO high */
    gpio_direction_output(data->esim_control_gpio, 1);

    /* Power up via QMI */
    uim_qmi_power_up_req(2);

    data->esim_enabled = true;

    dev_info(data->dev, "eSIM enabled successfully\n");
}

/**
 * disable_esim() - Disable eSIM power
 */
static void disable_esim(struct esim_switch_data *data)
{
    if (!data->esim_enabled) {
        dev_info(data->dev, "eSIM already disabled\n");
        return;
    }

    if (!gpio_is_valid(data->esim_control_gpio)) {
        dev_warn(data->dev, "eSIM control GPIO invalid; skipping disable\n");
        data->esim_enabled = false;
        return;
    }

    dev_info(data->dev, "Disabling eSIM\n");

    /* Power down via QMI */
    uim_qmi_power_down_req(2);
    msleep(200);

    /* Set eSIM control GPIO low */
    gpio_direction_output(data->esim_control_gpio, 0);

    data->esim_enabled = false;

    dev_info(data->dev, "eSIM disabled successfully\n");
}

/**
 * check_sim2_present() - Check if physical SIM2 is present
 */
static bool check_sim2_present(struct esim_switch_data *data)
{
    bool present = false;
    int i, value;

    if (!gpio_is_valid(data->sim2_detect_gpio))
        return false;

    /* Pull detection pin */
    if (!IS_ERR_OR_NULL(data->pinctrl) && !IS_ERR_OR_NULL(data->sim2_no_pull)) {
        pinctrl_select_state(data->pinctrl, data->sim2_no_pull);
        msleep(5);
    }

    /* Read multiple times for consistency */
    for (i = 0; i < 3; i++) {
        value = gpio_get_value(data->sim2_detect_gpio);
        if (value == 0) {
            present = true;
            break;
        }
        msleep(50);
    }

    dev_dbg(data->dev, "SIM2 detection: %s\n", present ? "PRESENT" : "ABSENT");

    return present;
}

/**
 * sim_detect_work_handler() - Delayed work handler for debouncing
 */
static void sim_detect_work_handler(struct work_struct *work)
{
    struct esim_switch_data *data = container_of(work,
                                                   struct esim_switch_data,
                                                   detect_work.work);
    bool sim2_present;

    mutex_lock(&data->lock);

    if (gpio_is_valid(data->sim2_detect_gpio))
        sim2_present = check_sim2_present(data);
    else
        sim2_present = data->sim2_present;

    if (sim2_present != data->sim2_present) {
        dev_info(data->dev, "SIM2 state changed: %s\n",
                 sim2_present ? "INSERTED" : "REMOVED");

        data->sim2_present = sim2_present;

        if (sim2_present) {
            disable_esim(data);
        } else {
            enable_esim(data);
        }
    }

    mutex_unlock(&data->lock);
}

/**
 * sim_detect_irq_handler() - Interrupt handler
 */
static irqreturn_t sim_detect_irq_handler(int irq, void *dev_id)
{
    struct esim_switch_data *data = (struct esim_switch_data *)dev_id;

    dev_dbg(data->dev, "SIM detect interrupt\n");

    cancel_delayed_work(&data->detect_work);
    schedule_delayed_work(&data->detect_work,
                          msecs_to_jiffies(SIM_DETECT_DEBOUNCE_MS));

    return IRQ_HANDLED;
}

/**
 * esim_switch_probe() - Driver probe function
 */
static int esim_switch_probe(struct platform_device *pdev)
{
    struct esim_switch_data *data;
    struct device *dev = &pdev->dev;
    int ret;

    dev_info(dev, "eSIM auto-switch driver probing\n");

    data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
    if (!data)
        return -ENOMEM;

    data->dev = dev;
    mutex_init(&data->lock);
    INIT_DELAYED_WORK(&data->detect_work, sim_detect_work_handler);

    data->sim2_present       = false;
    data->esim_enabled       = false;
    data->sim2_detect_gpio   = -EINVAL;
    data->esim_control_gpio  = -EINVAL;
    data->irq_number         = -EINVAL;

    platform_set_drvdata(pdev, data);

    data->sim2_detect_gpio = of_get_named_gpio(dev->of_node,
                                               "sim2-detect-gpios", 0);
    if (data->sim2_detect_gpio == -EPROBE_DEFER)
        return -EPROBE_DEFER;
    if (gpio_is_valid(data->sim2_detect_gpio)) {
        ret = devm_gpio_request(dev, data->sim2_detect_gpio, "sim2-detect");
        if (ret)
            return ret;
        gpio_direction_input(data->sim2_detect_gpio);
    } else {
        dev_warn(dev, "SIM2 detect GPIO not valid; sim2_active will reflect last known state\n");
    }

    data->esim_control_gpio = of_get_named_gpio(dev->of_node, "esim-control-gpios", 0);
    if (data->esim_control_gpio == -EPROBE_DEFER)
        return -EPROBE_DEFER;
    if (gpio_is_valid(data->esim_control_gpio)) {
        ret = devm_gpio_request(dev, data->esim_control_gpio, "esim-control");
        if (ret)
            return ret;
    } else {
        dev_warn(dev, "eSIM control GPIO not valid; eSIM power control disabled until available\n");
    }

    data->pinctrl = devm_pinctrl_get(dev);
    if (!IS_ERR(data->pinctrl)) {
        data->sim2_pull_high = pinctrl_lookup_state(data->pinctrl,
                                                     "sim2-pull-high");
        data->sim2_pull_low = pinctrl_lookup_state(data->pinctrl,
                                                    "sim2-pull-low");
        data->sim2_no_pull = pinctrl_lookup_state(data->pinctrl,
                                                   "sim2-no-pull");

        if (IS_ERR(data->sim2_no_pull)) {
            dev_warn(dev, "Failed to get pinctrl states, using defaults\n");
        }
    }

    if (gpio_is_valid(data->sim2_detect_gpio)) {
        data->irq_number = gpio_to_irq(data->sim2_detect_gpio);
        if (data->irq_number == -EPROBE_DEFER)
            return -EPROBE_DEFER;
        if (data->irq_number >= 0) {
            ret = devm_request_irq(dev, data->irq_number, sim_detect_irq_handler,
                                   IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
                                   DRIVER_NAME, data);
            if (ret)
                dev_warn(dev, "IRQ request failed (%d); will rely on polling/init work\n", ret);
        } else {
            dev_warn(dev, "gpio_to_irq failed (%d); will rely on polling/init work\n", data->irq_number);
        }
    }

    schedule_delayed_work(&data->detect_work, msecs_to_jiffies(1000));

    dev_info(dev, "eSIM auto-switch driver initialized successfully\n");
    dev_info(dev, "SIM2 detect GPIO: %d, IRQ: %d\n",
             data->sim2_detect_gpio, data->irq_number);
    dev_info(dev, "eSIM control GPIO: %d\n", data->esim_control_gpio);

    return 0;
}

/**
 * esim_switch_remove() - Driver remove function
 */
static int esim_switch_remove(struct platform_device *pdev)
{
    struct esim_switch_data *data = platform_get_drvdata(pdev);

    cancel_delayed_work_sync(&data->detect_work);

    dev_info(data->dev, "eSIM auto-switch driver removed\n");

    return 0;
}

static const struct of_device_id esim_switch_dt_match[] = {
    { .compatible = "oplus,esim-auto-switch", },
    { }
};
MODULE_DEVICE_TABLE(of, esim_switch_dt_match);

static struct platform_driver esim_switch_driver = {
    .probe = esim_switch_probe,
    .remove = esim_switch_remove,
    .driver = {
        .name = DRIVER_NAME,
        .of_match_table = esim_switch_dt_match,
    },
};

module_platform_driver(esim_switch_driver);

MODULE_DESCRIPTION("Oplus eSIM/Physical SIM2 Switcher Driver");
MODULE_AUTHOR("The YAAP Project");
MODULE_LICENSE("GPL v2");
