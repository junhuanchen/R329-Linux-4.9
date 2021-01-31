/*
 * drivers/input/keyboard/sunxi-gpio-ir-tx.c
 *
 * Copyright (c) 2013-2018 Allwinnertech Co., Ltd.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/module.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/irq.h>
#include <linux/uaccess.h>
#include <asm/irq.h>
#include <linux/io.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/poll.h>
#include <linux/cdev.h>
#include <linux/gpio.h>
#include <linux/ioport.h>
#include <linux/input.h>
#include <linux/miscdevice.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/sunxi-gpio.h>
#include <linux/of_gpio.h>

#include "sunxi-ir-keymap.h"
#include "sunxi-gpio-ir-rx.h"

#define GPIO_IR_RX_DRIVER_NAME "sunxi-gpio-ir-rx"

//DEBUG_INIT | DEBUG_INT | DEBUG_DATA_INFO | DEBUG_ERR;
static u32 debug_mask = 0x13;

#define dprintk(level_mask, fmt, arg...)				\
do {									\
	if (unlikely(debug_mask & level_mask))				\
		pr_warn("%s()%d - "fmt, __func__, __LINE__, ##arg);	\
} while (0)

/*report input key event*/
#define REPORT_INPUT_KEYCODE

#define GPIO_IR_RAW_BUF_SIZE   128
struct gpio_ir_raw_buffer {
	unsigned int dcnt; /*Packet Count*/
	unsigned int timebuf[GPIO_IR_RAW_BUF_SIZE];
};

static struct gpio_ir_raw_buffer gpio_ir_rawbuf;
static struct gpio_ir_raw_buffer gpio_ir_tmpbuf;
static struct gpio_ir_rx_info *rx_info;

static inline void gpio_ir_reset_rawbuffer(void)
{
	gpio_ir_rawbuf.dcnt = 0;
}

static inline void gpio_ir_write_rawbuffer(unsigned int interval)
{
	if (gpio_ir_rawbuf.dcnt < GPIO_IR_RAW_BUF_SIZE)
		gpio_ir_rawbuf.timebuf[gpio_ir_rawbuf.dcnt++] = interval;
	else
		dprintk(DEBUG_ERR, "IR Rx Buffer Full!!\n");
}

static inline unsigned char gpio_ir_read_rawbuffer(void)
{
	unsigned char data = 0x00;

	if (gpio_ir_rawbuf.dcnt > 0)
	data = gpio_ir_rawbuf.timebuf[--gpio_ir_rawbuf.dcnt];
	return data;
}

static inline int gpio_ir_rawbuffer_empty(void)
{
	return (gpio_ir_rawbuf.dcnt == 0);
}

static inline int gpio_ir_rawbuffer_full(void)
{
	return (gpio_ir_rawbuf.dcnt >= GPIO_IR_RAW_BUF_SIZE);
}

static irqreturn_t gpio_ir_rx_isr(int irq, void *dev_id)
{
	static ktime_t ktime_tap;

	if (true == rx_info->sampler_enable) {
		/*begin to record the time interval*/
		gpio_ir_reset_rawbuffer();
		rx_info->sampler_enable = false;
		rx_info->ktime = ktime_get();

	} else {
		ktime_tap = ktime_sub(ktime_get(), rx_info->ktime);
		if (!gpio_ir_rawbuffer_full())
			gpio_ir_write_rawbuffer((unsigned int)ktime_to_us(ktime_tap));

		/*save the current time*/
		rx_info->ktime = ktime_get();
	}
	mod_timer(&rx_info->sample_timer, jiffies + (HZ/40));

	return IRQ_HANDLED;
}


static unsigned int gpio_ir_rx_packet_handler(unsigned int *buf,
		unsigned int dcnt)
{
	unsigned int val_1 = 0x00, val_0 = 0x00;
	unsigned int val_top = 0x00, val_btm = 0x00;
	unsigned int code = 0;
	int bitCnt = 0;
	unsigned int i = 0;

	dprintk(DEBUG_DATA_INFO, "dcnt = %d\n", (int)dcnt);

	/* Find Lead '1' , 9ms high*/
	for (i = 0; i < dcnt; i++) {
		val_1 = buf[i];
		val_0 = buf[i + 1];
		if (val_1 > GPIO_IR_L1_MIN && val_0 > GPIO_IR_L0_MIN)
			break;
	}
	dprintk(DEBUG_DATA_INFO, "%d start head found = %d\n", __LINE__, i);

	/* go decoding */
	code = 0;
	bitCnt = 0;

	for (i = i + 2; i < dcnt; i += 2) {
		val_top = buf[i];
			val_btm = buf[i + 1];
		if (val_top > GPIO_IR_PMAX ||
					val_top < GPIO_IR_PMIN ||
					val_btm > GPIO_IR_PMAX ||
					val_btm < GPIO_IR_PMIN) {
			dprintk(DEBUG_DATA_INFO, "%d: Error Pulse found : %d\n",
					__LINE__, i);
			goto error_code;
		}

		/*to judge bit 0 0r bit 1, just check the second level interval*/
		if (val_btm > GPIO_IR_DMID)  {
			/* data '1' */
			code |= 1 << bitCnt;
		} else {
			code &= ~(1 << bitCnt);
		}
		bitCnt++;
		if (bitCnt == 32)
		break; /* decode over */
	}
	return code;

error_code:
	dprintk(DEBUG_ERR, "%d: packet handler error\n", __LINE__);
	return GPIO_IR_ERROR_CODE;
}

static int gpio_ir_code_valid(unsigned int code)
{
	unsigned int tmp1, tmp2;

#ifdef IR_CHECK_ADDR_CODE
	/* Check Address Value */
	if ((code & 0xffff) != (IR_ADDR_CODE & 0xffff))
		return 0; /* Address Error */

	tmp1 = code & 0x00ff0000;
	tmp2 = (code & 0xff000000) >> 8;
	return ((tmp1 ^ tmp2) == 0x00ff0000);  /* Check User Code */

#else
	/* Do Not Check Address Value */
	tmp1 = code & 0x00ff00ff;
	tmp2 = (code & 0xff00ff00) >> 8;
	return (((tmp1 ^ tmp2) & 0x00ff0000) == 0x00ff0000);

#endif
}

static void gpio_ir_rx_tsklet(unsigned long tsklet_data)
{
	unsigned int code;
	int code_valid;

#ifdef RAW_DATA_DUMP
	int i;

	for (int i = 0; i < gpio_ir_tmpbuf.dcnt; i++) {
		pr_info("%d  ", gpio_ir_tmpbuf.timebuf[i]);
		if ((i+1)%8 == 0)
			pr_info("\n");
	}
	pr_info("\n");
#endif

	code = gpio_ir_rx_packet_handler(gpio_ir_tmpbuf.timebuf,
			gpio_ir_tmpbuf.dcnt);
	code_valid = gpio_ir_code_valid(code);

	if ((code != GPIO_IR_ERROR_CODE) && (code != GPIO_IR_REPEAT_CODE))
		dprintk(DEBUG_INT, "ir addr code = 0x%x\n",  code & 0xffff);

	if (rx_info->timer_used) {
		if (code_valid) {
			/*the previous-key(old key) is released*/
			input_report_key(rx_info->ir_dev,
				ir_keycodes[(rx_info->ir_code >> 16) & 0xff],
				0);
			input_sync(rx_info->ir_dev);
			dprintk(DEBUG_INT, "IR KEY UP\n");
			 rx_info->key_count = 0;
		}

		/*if the same  or repeat key, delay to report key up*/
		if ((code == GPIO_IR_REPEAT_CODE) || (code_valid))
			mod_timer(&rx_info->report_timer, jiffies + (HZ/5));
		} else {
			if (code_valid) {
			/*init timer, the kery would be release*/
			mod_timer(&rx_info->report_timer, jiffies + (HZ/5));
			rx_info->timer_used = 1;
		}
	}

	if (rx_info->timer_used) {
		/*some key repeat times*/
		rx_info->key_count++;
		/*one new key, report key down*/
		if (rx_info->key_count == 1) {
			/*update saved code with a new valid code*/
			if (code_valid)
				rx_info->ir_code = code;

			dprintk(DEBUG_INT, "ir key code: 0x%x\n",
				ir_keycodes[(rx_info->ir_code >> 16) & 0xff]);

			/*key down report*/
			input_report_key(rx_info->ir_dev,
				ir_keycodes[(rx_info->ir_code >> 16) & 0xff],
				1);
			input_sync(rx_info->ir_dev);
		}
	}

	dprintk(DEBUG_DATA_INFO,
			"Rx Packet End, code=0x%x, ir_code=0x%x, rx_info->timer_used=%d\n",
			(int)code, (int)rx_info->ir_code, rx_info->timer_used);
}

static void ir_sample_timer_handle(unsigned long arg)
{
	/*timeout: one frame was receive finish*/
	memcpy(&gpio_ir_tmpbuf, &gpio_ir_rawbuf,
			sizeof(struct gpio_ir_raw_buffer));
	rx_info->sampler_enable = true;
	kill_fasync(&rx_info->ir_fasync, SIGIO, POLL_IN);
	/*to parse the raw buffer, and report the key event*/
#ifdef REPORT_INPUT_KEYCODE
	tasklet_schedule(&rx_info->tsklet);
#endif

}

static void ir_report_timer_handle(unsigned long arg)
{
	/*timeout: report key release*/
	input_report_key(rx_info->ir_dev,
			ir_keycodes[(rx_info->ir_code >> 16) & 0xff], 0);
	input_sync(rx_info->ir_dev);
	rx_info->key_count = 0;
	rx_info->timer_used = 0;
	dprintk(DEBUG_DATA_INFO, "key timeout, report keyup: 0x%x\n",
			ir_keycodes[(rx_info->ir_code >> 16) & 0xff]);
}

static int gpio_ir_rx_open(struct inode *inode, struct file *file)
{
	int ret = 0;

	dprintk(DEBUG_INIT, "gpio_ir_rx_open\n");
	gpio_ir_reset_rawbuffer();
	return ret ?: nonseekable_open(inode, file);
}

static int gpio_ir_rx_release(struct inode *inode, struct file *file)
{
	dprintk(DEBUG_INIT, "gpio_ir_rx_release\n");
	gpio_ir_reset_rawbuffer();
	return 0;
}

ssize_t gpio_ir_rx_read(struct file *file, char __user *buf,
			size_t size, loff_t *ppos)
{
	int ret;

	dprintk(DEBUG_INIT, "gpio_ir_rx_read, size: %d\n", size);
	if (size != sizeof(struct gpio_ir_raw_buffer))
		return -EINVAL;

	ret = copy_to_user(buf, &gpio_ir_tmpbuf,
		sizeof(struct gpio_ir_raw_buffer));
	gpio_ir_reset_rawbuffer();

	return ret;
}

static int gpio_ir_rx_fasync(int fd, struct file *file, int on)
{
	dprintk(DEBUG_INIT, "enter init fansync_helper\n");
	if (!rx_info)
		return -EIO;

	return fasync_helper(fd, file, on, &rx_info->ir_fasync);
}

static const struct file_operations gpio_ir_rx_fops = {
	.owner =	THIS_MODULE,
	.read = gpio_ir_rx_read,
	.fasync = gpio_ir_rx_fasync,
	.release = gpio_ir_rx_release,
	.open = gpio_ir_rx_open,

};

static struct miscdevice gpio_ir_rx_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "gpio_ir_rx",
	.fops = &gpio_ir_rx_fops,
};

static int __init gpio_ir_rx_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct gpio_config config;
	int i;
	int ret;

	dprintk(DEBUG_INIT, "\nenter gpio ir rx probe\n");
	rx_info = devm_kzalloc(dev, sizeof(struct gpio_ir_rx_info), GFP_KERNEL);
	if (!rx_info) {
		dev_err(dev, "can't allocate gpio ir-rx memory\n");
		return -ENOMEM;
	}

	/*ir gpio rx pin*/
	rx_info->ir_rx_gpio = of_get_named_gpio_flags(dev->of_node,
			"gpio-rx", 0,
			(enum of_gpio_flags *)&config);

	dprintk(DEBUG_INIT, "%s, line:%d, gpio ir rx: %d!\n",
		__func__, __LINE__, rx_info->ir_rx_gpio);

	ret = devm_gpio_request(dev, rx_info->ir_rx_gpio, "ir_rx_gpio");
	if (ret < 0) {
		dev_err(dev, "can't request ir_rx_gpio gpio %d\n",
			rx_info->ir_rx_gpio);
		goto err_free_mem;
	}

	ret = gpio_direction_input(rx_info->ir_rx_gpio);
	if (ret < 0) {
		dev_err(dev, "can't request input direction ir_rx_gpio gpio %d\n",
			rx_info->ir_rx_gpio);
		goto err_free_gpio;
	}

	rx_info->ir_rx_gpio_irq = gpio_to_irq(rx_info->ir_rx_gpio);
	if (IS_ERR_VALUE(rx_info->ir_rx_gpio_irq)) {
		dev_err(dev, "map gpio [%d] to virq failed, errno = %d\n",
				rx_info->ir_rx_gpio, rx_info->ir_rx_gpio_irq);
		goto err_free_gpio;
	}

	dprintk(DEBUG_INIT, "gpio irq: %d\n", rx_info->ir_rx_gpio_irq);

	/* register input device for ir */
	rx_info->ir_dev = input_allocate_device();
	if (!rx_info->ir_dev) {
		dev_err(dev, "not enough memory for input device\n");
		goto err_free_gpio;
	}

	rx_info->ir_dev->name = "sunxi-gpio-ir";
	rx_info->ir_dev->phys = "gpioIR/input1";

	rx_info->ir_dev->id.bustype = BUS_VIRTUAL;
	rx_info->ir_dev->id.vendor = 0x0002;
	rx_info->ir_dev->id.product = 0x0005;
	rx_info->ir_dev->id.version = 0x0100;

#ifdef REPORT_REPEAT_KEY_VALUE
	rx_info->ir_dev->evbit[0] = BIT_MASK(EV_KEY) | BIT_MASK(EV_REP);
#else
	rx_info->ir_dev->evbit[0] = BIT_MASK(EV_KEY);
#endif

	for (i = 0; i < 256; i++)
		set_bit(ir_keycodes[i], rx_info->ir_dev->keybit);

	ret = input_register_device(rx_info->ir_dev);
	if (ret) {
		dev_err(dev, "register input device exception, exit\n");
		goto err_free_input;
	}

	init_timer(&rx_info->report_timer);
	rx_info->report_timer.function = ir_report_timer_handle;

	/* initialize tasklet for gpio_ir_rx_hrtimer_callback */
	tasklet_init(&rx_info->tsklet, gpio_ir_rx_tsklet,
			(unsigned long)rx_info);

	init_timer(&rx_info->sample_timer);
	rx_info->sample_timer.function = ir_sample_timer_handle;
	rx_info->sampler_enable = true;

	//mutex_init(&rx_info->lock);

	ret = request_irq(rx_info->ir_rx_gpio_irq, gpio_ir_rx_isr,
			IRQF_TRIGGER_FALLING | IRQF_TRIGGER_RISING,
			"ir_rx_gpio_irq", NULL);
	if (ret != 0) {
		dev_err(dev, "irq request failed!\n");
		goto err_free_gpio;
	}

	ret = misc_register(&gpio_ir_rx_miscdev);
	if (ret) {
		dev_err(dev, "%s: cannot register miscdev on minor=%d (%d)\n",
				__func__, MISC_DYNAMIC_MINOR, ret);
		goto err_misc_register;
	}

	dprintk(DEBUG_INIT, "%s: gpio ir rx probe succeed!)\n", __func__);

	return 0;

err_misc_register:
	del_timer(&rx_info->report_timer);
	del_timer(&rx_info->sample_timer);

err_free_input:
	input_unregister_device(rx_info->ir_dev);

err_free_gpio:
	gpio_free(rx_info->ir_rx_gpio);

err_free_mem:
	devm_kfree(dev, rx_info);
	return ret;

}

static int gpio_ir_rx_remove(struct platform_device *pdev)
{
	del_timer(&rx_info->report_timer);
	del_timer(&rx_info->sample_timer);
	input_unregister_device(rx_info->ir_dev);
	misc_deregister(&gpio_ir_rx_miscdev);
	free_irq(rx_info->ir_rx_gpio_irq, NULL);
	gpio_free(rx_info->ir_rx_gpio);
	return 0;
}

static const struct of_device_id gpio_ir_rx_match[] = {
	{ .compatible = "allwinner,gpio-ir-rx", },
	{ },
};
MODULE_DEVICE_TABLE(of, gpio_ir_rx_match);

#ifdef CONFIG_PM
static int gpio_ir_rx_resume(struct device *dev)
{
	return 0;
}

static int gpio_ir_rx_suspend(struct device *dev)
{
	return 0;
}

static const struct dev_pm_ops gpio_ir_rx_pm_ops = {
	.suspend = gpio_ir_rx_suspend,
	.resume = gpio_ir_rx_resume,
};
#endif

static struct platform_driver gpio_ir_rx_driver = {
	.driver = {
		.name	= GPIO_IR_RX_DRIVER_NAME,
		.owner	= THIS_MODULE,
		.of_match_table = gpio_ir_rx_match,
#ifdef CONFIG_PM
		.pm = &gpio_ir_rx_pm_ops,
#endif
	},
	.probe  = gpio_ir_rx_probe,
	.remove = gpio_ir_rx_remove,
};

module_platform_driver(gpio_ir_rx_driver);
MODULE_DESCRIPTION("Remote GPIO IR driver");
MODULE_AUTHOR("xudong");
MODULE_LICENSE("GPL");
