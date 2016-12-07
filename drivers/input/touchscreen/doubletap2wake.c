/*
 * drivers/input/touchscreen/doubletap2wake.c
 *
 *
 * Copyright (c) 2013, Dennis Rassmann <showp1984@gmail.com>
 * Copyright (c) 2016, jollaman999 <admin@jollaman999.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/err.h>
#include <linux/input/doubletap2wake.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/input.h>
#include <linux/fb.h>
#include <linux/hrtimer.h>
#include <asm-generic/cputime.h>

#ifdef CONFIG_TOUCHSCREEN_SCROFF_VOLCTR
#include <linux/input/scroff_volctr.h>
#endif

struct notifier_block dt2w_fb_notif;

/* uncomment since no touchscreen defines android touch, do that here */
//#define ANDROID_TOUCH_DECLARED

/* if Sweep2Wake is compiled it will already have taken care of this */
#ifdef CONFIG_TOUCHSCREEN_SWEEP2WAKE
#define ANDROID_TOUCH_DECLARED
#endif

/* Version, author, desc, etc */
#define DRIVER_AUTHOR "jollaman999 <admin@jollaman999.com>"
#define DRIVER_DESCRIPTION "Doubletap2wake for almost any device"
#define DRIVER_VERSION "2.0"
#define LOGTAG "[doubletap2wake]: "

MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESCRIPTION);
MODULE_VERSION(DRIVER_VERSION);
MODULE_LICENSE("GPLv2");

/* Tuneables */
#define DT2W_DEBUG		0
#define DT2W_DEFAULT		0

#define DT2W_FEATHER		200
#define DT2W_TIME_GAP		200
#define DT2W_VIB_STRENGTH	20	// Vibrator strength

/* Resources */
int dt2w_switch = DT2W_DEFAULT;
int dt2w_switch_tmp = 0;
static s64 tap_time_pre = 0;
static int touch_x = 0, touch_y = 0, touch_nr = 0, x_pre = 0, y_pre = 0;
static bool is_touching = false;
static bool scr_suspended = false;
static struct input_dev * doubletap2wake_pwrdev;
static DEFINE_MUTEX(pwrkeyworklock);
static DEFINE_MUTEX(switchlock);
static struct workqueue_struct *dt2w_input_wq;
static struct work_struct dt2w_input_work;

static bool registered = false;
static DEFINE_MUTEX(reg_lock);

// Vibrate when screen on
#ifdef CONFIG_QPNP_HAPTIC
extern void qpnp_hap_td_enable(int value);
#endif

/* Read cmdline for dt2w */
static int __init read_dt2w_cmdline(char *dt2w)
{
	if (strcmp(dt2w, "1") == 0) {
		pr_info("[cmdline_dt2w]: DoubleTap2Wake enabled. | dt2w='%s'\n", dt2w);
		dt2w_switch = 1;
	} else if (strcmp(dt2w, "0") == 0) {
		pr_info("[cmdline_dt2w]: DoubleTap2Wake disabled. | dt2w='%s'\n", dt2w);
		dt2w_switch = 0;
	} else {
		pr_info("[cmdline_dt2w]: No valid input found. Going with default: | dt2w='%u'\n", dt2w_switch);
	}
	return 1;
}
__setup("dt2w=", read_dt2w_cmdline);

/* reset on finger release */
static void doubletap2wake_reset(void)
{
	touch_nr = 0;
	tap_time_pre = 0;
	x_pre = 0;
	y_pre = 0;
}

/* PowerKey work func */
static void doubletap2wake_presspwr(struct work_struct * doubletap2wake_presspwr_work)
{
	if (!mutex_trylock(&pwrkeyworklock))
		return;
	input_event(doubletap2wake_pwrdev, EV_KEY, KEY_POWER, 1);
	input_event(doubletap2wake_pwrdev, EV_SYN, 0, 0);
	input_event(doubletap2wake_pwrdev, EV_KEY, KEY_POWER, 0);
	input_event(doubletap2wake_pwrdev, EV_SYN, 0, 0);

	// Vibrate when screen on
#ifdef CONFIG_QPNP_HAPTIC
	qpnp_hap_td_enable(DT2W_VIB_STRENGTH);
#endif
	mutex_unlock(&pwrkeyworklock);
}
static DECLARE_WORK(doubletap2wake_presspwr_work, doubletap2wake_presspwr);

/* PowerKey trigger */
static void doubletap2wake_pwrtrigger(void)
{
	schedule_work(&doubletap2wake_presspwr_work);
}

/* unsigned */
static unsigned int calc_feather(int coord, int prev_coord)
{
	int calc_coord = 0;
	calc_coord = coord-prev_coord;
	if (calc_coord < 0)
		calc_coord = calc_coord * (-1);
	return calc_coord;
}

/* init a new touch */
static void new_touch(int x, int y)
{
	tap_time_pre = ktime_to_ms(ktime_get_real());
	x_pre = x;
	y_pre = y;
	touch_nr++;
}

/* Doubletap2wake main function */
static void detect_doubletap2wake(int x, int y)
{
	int tmp;
	bool change_switch = false;

#if DT2W_DEBUG
	pr_info(LOGTAG"x,y(%4d,%4d)\n", x, y);
#endif

	if (!scr_suspended)
		return;

	if (!is_touching) {
		is_touching = true;

		if (dt2w_switch_tmp)
			change_switch = true;

		mutex_lock(&switchlock);
		if (change_switch) {
			tmp = dt2w_switch;
			dt2w_switch = 1;
		}

		// Make enable to set touch counts (Max : 10) - by jollaman999
		if (touch_nr == 0) {
			new_touch(x, y);
		// Make enable to set touch counts (Max : 10) - by jollaman999
		} else if (touch_nr >= 1 && touch_nr <= dt2w_switch) {
			if (((calc_feather(x, x_pre) < DT2W_FEATHER) || (calc_feather(y, y_pre) < DT2W_FEATHER))
			&& ((ktime_to_ms(ktime_get_real()) - tap_time_pre) < DT2W_TIME_GAP)) {
				tap_time_pre = ktime_to_ms(ktime_get_real());
				touch_nr++;
			} else {
				doubletap2wake_reset();
				new_touch(x, y);
			}
		} else {
			doubletap2wake_reset();
			new_touch(x, y);
		}
		// Make enable to set touch counts (Max : 10) - by jollaman999
		if (touch_nr > dt2w_switch) {
			pr_info(LOGTAG"ON\n");
			doubletap2wake_pwrtrigger();
			doubletap2wake_reset();
		}

		if (change_switch)
			dt2w_switch = tmp;
		mutex_unlock(&switchlock);
	}
}

static void dt2w_input_callback(struct work_struct *unused)
{
	detect_doubletap2wake(touch_x, touch_y);
}

static void dt2w_input_event(struct input_handle *handle, unsigned int type,
				unsigned int code, int value)
{
	if ((!scr_suspended) || (!dt2w_switch && !dt2w_switch_tmp))
		return;

	/* You can debug here with 'adb shell getevent -l' command. */
	switch(code) {
		case ABS_MT_SLOT:
			doubletap2wake_reset();
			break;

		case ABS_MT_TRACKING_ID:
			if (value == 0xffffffff)
				is_touching = false;
			break;

		case ABS_MT_POSITION_X:
			touch_x = value;
			queue_work(dt2w_input_wq, &dt2w_input_work);
			break;

		case ABS_MT_POSITION_Y:
			touch_y = value;
			queue_work(dt2w_input_wq, &dt2w_input_work);
			break;

		default:
			break;
	}
}

static int input_dev_filter(struct input_dev *dev)
{
	if (strstr(dev->name, "synaptics_rmi4_i2c"))
		return 0;
	else
		return 1;
}

static int dt2w_input_connect(struct input_handler *handler,
				struct input_dev *dev, const struct input_device_id *id)
{
	struct input_handle *handle;
	int error;

	if (input_dev_filter(dev))
		return -ENODEV;

	handle = kzalloc(sizeof(struct input_handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;

	handle->dev = dev;
	handle->handler = handler;
	handle->name = "dt2w";

	error = input_register_handle(handle);
	if (error)
		goto err2;

	error = input_open_device(handle);
	if (error)
		goto err1;

	return 0;
err1:
	input_unregister_handle(handle);
err2:
	kfree(handle);
	return error;
}

static void dt2w_input_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static const struct input_device_id dt2w_ids[] = {
	{ .driver_info = 1 },
	{ },
};

static struct input_handler dt2w_input_handler = {
	.event		= dt2w_input_event,
	.connect	= dt2w_input_connect,
	.disconnect	= dt2w_input_disconnect,
	.name		= "dt2w_inputreq",
	.id_table	= dt2w_ids,
};

static int register_dt2w(void)
{
	int rc = 0;

	mutex_lock(&reg_lock);

	if (registered) {
#if DT2W_DEBUG
		pr_info(LOGTAG"%s already registered\n", __func__);
#endif
		goto out;
	}

	dt2w_input_wq = create_workqueue("dt2wiwq");
	if (!dt2w_input_wq) {
		pr_err("%s: Failed to create dt2wiwq workqueue\n", __func__);
		mutex_unlock(&reg_lock);
		return -EFAULT;
	}
	INIT_WORK(&dt2w_input_work, dt2w_input_callback);
	rc = input_register_handler(&dt2w_input_handler);
	if (rc) {
		pr_err("%s: Failed to register dt2w_input_handler\n", __func__);
		goto err;
	}

	registered = true;
out:
	mutex_unlock(&reg_lock);
#if DT2W_DEBUG
	pr_info(LOGTAG"%s done\n", __func__);
#endif

	return rc;
err:
	flush_workqueue(dt2w_input_wq);
	destroy_workqueue(dt2w_input_wq);
	cancel_work_sync(&dt2w_input_work);
	mutex_unlock(&reg_lock);

	return rc;
}

static void unregister_dt2w(void)
{
	mutex_lock(&reg_lock);

	if(!registered) {
#if DT2W_DEBUG
		pr_info(LOGTAG"%s already unregistered\n", __func__);
#endif
		goto out;
	}

	input_unregister_handler(&dt2w_input_handler);
	flush_workqueue(dt2w_input_wq);
	destroy_workqueue(dt2w_input_wq);
	cancel_work_sync(&dt2w_input_work);

	registered = false;
out:
	mutex_unlock(&reg_lock);
#if DT2W_DEBUG
	pr_info(LOGTAG"%s done\n", __func__);
#endif
}

/*
 * SYSFS stuff below here
 */
static ssize_t dt2w_doubletap2wake_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	size_t count = 0;

	count += sprintf(buf, "%d\n", dt2w_switch);

	return count;
}

static ssize_t dt2w_doubletap2wake_dump(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	mutex_lock(&switchlock);
	// Make enable to set touch counts (Max : 10) - by jollaman999
	// You should tap 1 more from set number to wake your device.
	if (buf[0] >= '0' && buf[0] <= '9' && buf[1] == '\n') {
                if (dt2w_switch != buf[0] - '0')
					dt2w_switch = buf[0] - '0';
	}
	mutex_unlock(&switchlock);

	if (dt2w_switch)
		register_dt2w();
	else
		unregister_dt2w();

	return count;
}

static DEVICE_ATTR(doubletap2wake, (S_IWUSR|S_IRUGO),
	dt2w_doubletap2wake_show, dt2w_doubletap2wake_dump);

static ssize_t dt2w_doubletap2wake_tmp_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	size_t count = 0;

	count += sprintf(buf, "%d\n", dt2w_switch_tmp);

	return count;
}

static ssize_t dt2w_doubletap2wake_tmp_dump(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	if ((buf[0] == '0' || buf[0] == '1') && buf[1] == '\n') {
                if (dt2w_switch_tmp != buf[0] - '0')
					dt2w_switch_tmp = buf[0] - '0';
	}

	if (dt2w_switch_tmp)
		register_dt2w();
	else
		unregister_dt2w();

	return count;
}

static DEVICE_ATTR(doubletap2wake_tmp, (S_IWUSR|S_IRUGO),
	dt2w_doubletap2wake_tmp_show, dt2w_doubletap2wake_tmp_dump);

static ssize_t dt2w_version_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	size_t count = 0;

	count += sprintf(buf, "%s\n", DRIVER_VERSION);

	return count;
}

static ssize_t dt2w_version_dump(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	return count;
}

static DEVICE_ATTR(doubletap2wake_version, (S_IWUSR|S_IRUGO),
	dt2w_version_show, dt2w_version_dump);

static int dt2w_fb_notifier_callback(struct notifier_block *self,
				unsigned long event, void *data)
{
	struct fb_event *evdata = data;
	int *blank;

	if (!dt2w_switch && !dt2w_switch_tmp)
		return 0;

	if (event == FB_EVENT_BLANK) {
		blank = evdata->data;

		switch (*blank) {
		case FB_BLANK_UNBLANK:
			scr_suspended = false;
			unregister_dt2w();
			break;
		case FB_BLANK_POWERDOWN:
			scr_suspended = true;
			if (dt2w_switch || dt2w_switch_tmp)
				register_dt2w();
#ifdef CONFIG_TOUCHSCREEN_SCROFF_VOLCTR
			else if (sovc_force_off && !dt2w_switch)
				unregister_dt2w();
#endif
			break;
		}
	}

	return 0;
}

struct notifier_block dt2w_fb_notif = {
	.notifier_call = dt2w_fb_notifier_callback,
};

/*
 * INIT / EXIT stuff below here
 */
#ifdef ANDROID_TOUCH_DECLARED
extern struct kobject *android_touch_kobj;
#else
struct kobject *android_touch_kobj;
EXPORT_SYMBOL_GPL(android_touch_kobj);
#endif
static int __init doubletap2wake_init(void)
{
	int rc = 0;

	doubletap2wake_pwrdev = input_allocate_device();
	if (!doubletap2wake_pwrdev) {
		pr_err("Can't allocate suspend autotest power button\n");
		goto err_alloc_dev;
	}

	input_set_capability(doubletap2wake_pwrdev, EV_KEY, KEY_POWER);
	doubletap2wake_pwrdev->name = "dt2w_pwrkey";
	doubletap2wake_pwrdev->phys = "dt2w_pwrkey/input0";

	rc = input_register_device(doubletap2wake_pwrdev);
	if (rc) {
		pr_err("%s: input_register_device err=%d\n", __func__, rc);
		goto err_input_dev;
	}

#ifndef ANDROID_TOUCH_DECLARED
	android_touch_kobj = kobject_create_and_add("android_touch", NULL) ;
	if (android_touch_kobj == NULL) {
		pr_warn("%s: android_touch_kobj create_and_add failed\n", __func__);
	}
#endif

	rc = fb_register_client(&dt2w_fb_notif);
	if (rc) {
		pr_warn("%s: fb register failed\n", __func__);
	}

	rc = sysfs_create_file(android_touch_kobj, &dev_attr_doubletap2wake.attr);
	if (rc) {
		pr_warn("%s: sysfs_create_file failed for doubletap2wake\n", __func__);
	}
	rc = sysfs_create_file(android_touch_kobj, &dev_attr_doubletap2wake_tmp.attr);
	if (rc) {
		pr_warn("%s: sysfs_create_file failed for doubletap2wake_tmp\n", __func__);
	}
	rc = sysfs_create_file(android_touch_kobj, &dev_attr_doubletap2wake_version.attr);
	if (rc) {
		pr_warn("%s: sysfs_create_file failed for doubletap2wake_version\n", __func__);
	}

err_input_dev:
	input_free_device(doubletap2wake_pwrdev);
err_alloc_dev:
	pr_info(LOGTAG"%s done\n", __func__);

	return 0;
}

static void __exit doubletap2wake_exit(void)
{
#ifndef ANDROID_TOUCH_DECLARED
	kobject_del(android_touch_kobj);
#endif
	unregister_dt2w();
	input_unregister_device(doubletap2wake_pwrdev);
	input_free_device(doubletap2wake_pwrdev);
	fb_unregister_client(&dt2w_fb_notif);
}

module_init(doubletap2wake_init);
module_exit(doubletap2wake_exit);
