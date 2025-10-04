// SPDX-License-Identifier: GPL-2.0
/*
 * Xiaomi LHBM Control Driver
 * 
 * Copyright (c) 2024 Xiaomi
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/miscdevice.h>
#include <linux/sysfs.h>
#include <linux/mutex.h>
#include <linux/of.h>

/* External function declaration for LHBM control */
extern int mi_disp_set_fod_queue_work(u32 fod_btn, bool from_touch);

#define LHBM_DEVICE_NAME "xiaomi_lhbm"
#define LHBM_CLASS_NAME "lhbm"

struct xiaomi_lhbm_data {
    struct device *dev;
    struct class *lhbm_class;
    bool lhbm_enabled;
    struct mutex lock;
};

static struct xiaomi_lhbm_data *lhbm_data;

static ssize_t lhbm_enable_show(struct device *dev,
                               struct device_attribute *attr,
                               char *buf)
{
    struct xiaomi_lhbm_data *data = dev_get_drvdata(dev);
    ssize_t ret;
    
    mutex_lock(&data->lock);
    ret = snprintf(buf, PAGE_SIZE, "%d\n", data->lhbm_enabled);
    mutex_unlock(&data->lock);
    
    return ret;
}

static ssize_t lhbm_enable_store(struct device *dev,
                                struct device_attribute *attr,
                                const char *buf, size_t count)
{
    struct xiaomi_lhbm_data *data = dev_get_drvdata(dev);
    unsigned int input;
    int ret;
    
    ret = kstrtouint(buf, 10, &input);
    if (ret < 0)
        return ret;
    
    mutex_lock(&data->lock);
    
    if (input == 1) {
        if (!data->lhbm_enabled) {
            pr_info("xiaomi_lhbm: Enabling LHBM\n");
            data->lhbm_enabled = true;
            
            // 调用实际的LHBM启用逻辑
            mi_disp_set_fod_queue_work(1, false); // 启用LHBM
        }
    } else if (input == 0) {
        if (data->lhbm_enabled) {
            pr_info("xiaomi_lhbm: Disabling LHBM\n");
            data->lhbm_enabled = false;
            
            // 调用实际的LHBM禁用逻辑
            mi_disp_set_fod_queue_work(0, false); // 禁用LHBM
        }
    } else {
        pr_warn("xiaomi_lhbm: Invalid input %u, only 0 or 1 allowed\n", input);
        mutex_unlock(&data->lock);
        return -EINVAL;
    }
    
    mutex_unlock(&data->lock);
    
    return count;
}

static DEVICE_ATTR(lhbm_enable, 0644, lhbm_enable_show, lhbm_enable_store);

static struct attribute *lhbm_attrs[] = {
    &dev_attr_lhbm_enable.attr,
    NULL,
};

static struct attribute_group lhbm_attr_group = {
    .attrs = lhbm_attrs,
};

static int xiaomi_lhbm_probe(struct platform_device *pdev)
{
    struct device *dev = &pdev->dev;
    struct xiaomi_lhbm_data *data;
    int ret;
    
    pr_info("xiaomi_lhbm: Probing LHBM control driver\n");
    
    data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
    if (!data)
        return -ENOMEM;
    
    mutex_init(&data->lock);
    data->dev = dev;
    data->lhbm_enabled = false;
    
    // 创建设备类
    data->lhbm_class = class_create(THIS_MODULE, LHBM_CLASS_NAME);
    if (IS_ERR(data->lhbm_class)) {
        ret = PTR_ERR(data->lhbm_class);
        pr_err("xiaomi_lhbm: Failed to create class\n");
        goto class_err;
    }
    
    // 创建设备
    data->dev = device_create(data->lhbm_class, NULL, 0, NULL, LHBM_DEVICE_NAME);
    if (IS_ERR(data->dev)) {
        ret = PTR_ERR(data->dev);
        pr_err("xiaomi_lhbm: Failed to create device\n");
        goto device_err;
    }
    
    dev_set_drvdata(data->dev, data);
    platform_set_drvdata(pdev, data);
    
    // 创建sysfs属性
    ret = sysfs_create_group(&data->dev->kobj, &lhbm_attr_group);
    if (ret) {
        pr_err("xiaomi_lhbm: Failed to create sysfs group\n");
        goto sysfs_err;
    }
    
    lhbm_data = data;
    
    pr_info("xiaomi_lhbm: LHBM control driver probed successfully\n");
    
    return 0;

sysfs_err:
    device_destroy(data->lhbm_class, 0);
device_err:
    class_destroy(data->lhbm_class);
class_err:
    return ret;
}

static int xiaomi_lhbm_remove(struct platform_device *pdev)
{
    struct xiaomi_lhbm_data *data = platform_get_drvdata(pdev);
    
    if (data) {
        sysfs_remove_group(&data->dev->kobj, &lhbm_attr_group);
        device_destroy(data->lhbm_class, 0);
        class_destroy(data->lhbm_class);
        mutex_destroy(&data->lock);
    }
    
    pr_info("xiaomi_lhbm: LHBM control driver removed\n");
    
    return 0;
}

static const struct of_device_id xiaomi_lhbm_of_match[] = {
    { .compatible = "xiaomi,lhbm" },
    {},
};
MODULE_DEVICE_TABLE(of, xiaomi_lhbm_of_match);

static struct platform_driver xiaomi_lhbm_driver = {
    .probe = xiaomi_lhbm_probe,
    .remove = xiaomi_lhbm_remove,
    .driver = {
        .name = "xiaomi-lhbm",
        .of_match_table = of_match_ptr(xiaomi_lhbm_of_match),
        .owner = THIS_MODULE,
    },
};

static int __init xiaomi_lhbm_init(void)
{
    pr_info("xiaomi_lhbm: Initializing LHBM control driver\n");
    return platform_driver_register(&xiaomi_lhbm_driver);
}

static void __exit xiaomi_lhbm_exit(void)
{
    platform_driver_unregister(&xiaomi_lhbm_driver);
    pr_info("xiaomi_lhbm: LHBM control driver unloaded\n");
}

module_init(xiaomi_lhbm_init);
module_exit(xiaomi_lhbm_exit);

MODULE_DESCRIPTION("Xiaomi LHBM Control Driver");
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Xiaomi");