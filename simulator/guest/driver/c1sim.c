// SPDX-License-Identifier: GPL-2.0-only
/* Device ABI adapter for unmodified C1-Slim userspace binaries. */
#include <linux/fs.h>
#include <linux/input.h>
#include <linux/leds.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/power_supply.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <asm/io.h>

#define FRAME_BYTES (296 * 152 / 8)
struct screen_packet { u32 sequence, full; u8 pixels[FRAME_BYTES]; };
struct control_packet { u16 source, code; s32 value; };
static struct screen_packet screen;
static DEFINE_SPINLOCK(screen_lock);
static DECLARE_WAIT_QUEUE_HEAD(screen_wait);
static struct input_dev *keys[2];
static int capacity = 80, plugged = 1;
static int fast_only = 1;
static struct platform_device *paper;
static struct power_supply *battery, *usb, *ac;
static struct led_classdev leds[4];

static void publish(bool full)
{
    unsigned long flags;
    spin_lock_irqsave(&screen_lock, flags);
    screen.full = full;
    ++screen.sequence;
    spin_unlock_irqrestore(&screen_lock, flags);
    wake_up_interruptible(&screen_wait);
}

static ssize_t paper_write(struct file *file, const char __user *data,
                          size_t size, loff_t *offset)
{
    u8 *pixels;
    unsigned long flags;
    if (size != FRAME_BYTES) return -EINVAL;
    pixels = memdup_user(data, size);
    if (IS_ERR(pixels)) return PTR_ERR(pixels);
    spin_lock_irqsave(&screen_lock, flags);
    memcpy(screen.pixels, pixels, size);
    screen.full = !READ_ONCE(fast_only);
    ++screen.sequence;
    spin_unlock_irqrestore(&screen_lock, flags);
    kfree(pixels);
    wake_up_interruptible(&screen_wait);
    return size;
}

static const struct file_operations paper_ops = {
    .owner = THIS_MODULE, .write = paper_write, .llseek = no_llseek,
};
static struct miscdevice paper_device = {
    .minor = MISC_DYNAMIC_MINOR, .name = "epaper_lcd", .fops = &paper_ops, .mode = 0666,
};

static ssize_t refresh_show(struct device *device, struct device_attribute *attr, char *buffer)
{ return sysfs_emit(buffer, "0\n"); }
static ssize_t refresh_store(struct device *device, struct device_attribute *attr,
                             const char *buffer, size_t size)
{ if (size && buffer[0] == '1') publish(true); return size; }
static DEVICE_ATTR_RW(refresh);
static ssize_t fast_refresh_only_show(struct device *device, struct device_attribute *attr, char *buffer)
{ return sysfs_emit(buffer, "%d\n", READ_ONCE(fast_only)); }
static ssize_t fast_refresh_only_store(struct device *device, struct device_attribute *attr,
                                       const char *buffer, size_t size)
{
    int value;
    if (kstrtoint(buffer, 0, &value) || (value != 0 && value != 1)) return -EINVAL;
    WRITE_ONCE(fast_only, value);
    return size;
}
static DEVICE_ATTR_RW(fast_refresh_only);
static struct attribute *paper_attrs[] = { &dev_attr_refresh.attr, &dev_attr_fast_refresh_only.attr, NULL };
static const struct attribute_group paper_group = { .name = "epaper", .attrs = paper_attrs };

static int bridge_open(struct inode *inode, struct file *file)
{
    file->private_data = kzalloc(sizeof(u32), GFP_KERNEL);
    return file->private_data ? 0 : -ENOMEM;
}
static int bridge_release(struct inode *inode, struct file *file)
{ kfree(file->private_data); return 0; }
static ssize_t bridge_read(struct file *file, char __user *buffer, size_t size, loff_t *offset)
{
    struct screen_packet *packet;
    u32 *last = file->private_data;
    unsigned long flags;
    int error;
    if (size < sizeof(*packet)) return -EINVAL;
    if (*last == READ_ONCE(screen.sequence)) {
        if (file->f_flags & O_NONBLOCK) return -EAGAIN;
        error = wait_event_interruptible(screen_wait, *last != READ_ONCE(screen.sequence));
        if (error) return error;
    }
    packet = kmalloc(sizeof(*packet), GFP_KERNEL);
    if (!packet) return -ENOMEM;
    spin_lock_irqsave(&screen_lock, flags);
    memcpy(packet, &screen, sizeof(*packet));
    spin_unlock_irqrestore(&screen_lock, flags);
    error = copy_to_user(buffer, packet, sizeof(*packet)) ? -EFAULT : 0;
    if (!error) *last = packet->sequence;
    kfree(packet);
    return error ? error : sizeof(struct screen_packet);
}
static __poll_t bridge_poll(struct file *file, poll_table *wait)
{
    poll_wait(file, &screen_wait, wait);
    return *(u32 *)file->private_data != READ_ONCE(screen.sequence) ? EPOLLIN | EPOLLRDNORM : 0;
}
static ssize_t bridge_write(struct file *file, const char __user *buffer, size_t size, loff_t *offset)
{
    struct control_packet command;
    if (size != sizeof(command)) return -EINVAL;
    if (copy_from_user(&command, buffer, size)) return -EFAULT;
    if (command.source < 2) {
        if (command.code > KEY_MAX || command.value < 0 || command.value > 2) return -EINVAL;
        input_event(keys[command.source], EV_KEY, command.code, command.value);
        input_sync(keys[command.source]);
    } else if (command.source == 2) {
        if (command.value < 0 || command.value > 100 || command.code > 1) return -EINVAL;
        WRITE_ONCE(capacity, command.value);
        WRITE_ONCE(plugged, command.code);
        power_supply_changed(battery);
        power_supply_changed(usb);
    } else return -EINVAL;
    return size;
}
static const struct file_operations bridge_ops = {
    .owner = THIS_MODULE, .open = bridge_open, .release = bridge_release,
    .read = bridge_read, .write = bridge_write, .poll = bridge_poll, .llseek = no_llseek,
};
static struct miscdevice bridge_device = {
    .minor = MISC_DYNAMIC_MINOR, .name = "c1sim", .fops = &bridge_ops, .mode = 0600,
};

static enum power_supply_property battery_properties[] = {
    POWER_SUPPLY_PROP_CAPACITY, POWER_SUPPLY_PROP_STATUS,
    POWER_SUPPLY_PROP_VOLTAGE_NOW, POWER_SUPPLY_PROP_CURRENT_NOW,
    POWER_SUPPLY_PROP_CHARGE_FULL, POWER_SUPPLY_PROP_CHARGE_NOW,
};
static enum power_supply_property online_properties[] = { POWER_SUPPLY_PROP_ONLINE };
static int supply_get(struct power_supply *supply, enum power_supply_property property,
                      union power_supply_propval *value)
{
    switch (property) {
    case POWER_SUPPLY_PROP_CAPACITY: value->intval = READ_ONCE(capacity); break;
    case POWER_SUPPLY_PROP_STATUS:
        value->intval = READ_ONCE(plugged) ? POWER_SUPPLY_STATUS_CHARGING : POWER_SUPPLY_STATUS_DISCHARGING; break;
    case POWER_SUPPLY_PROP_ONLINE:
        value->intval = !strcmp(supply->desc->name, "usb") ? READ_ONCE(plugged) : 0; break;
    case POWER_SUPPLY_PROP_VOLTAGE_NOW: value->intval = 3900000; break;
    case POWER_SUPPLY_PROP_CURRENT_NOW: value->intval = READ_ONCE(plugged) ? 100000 : -60000; break;
    case POWER_SUPPLY_PROP_CHARGE_FULL: value->intval = 1000000; break;
    case POWER_SUPPLY_PROP_CHARGE_NOW: value->intval = READ_ONCE(capacity) * 10000; break;
    default: return -EINVAL;
    }
    return 0;
}
static const struct power_supply_desc battery_desc = {
    .name = "battery", .type = POWER_SUPPLY_TYPE_BATTERY,
    .properties = battery_properties, .num_properties = ARRAY_SIZE(battery_properties), .get_property = supply_get,
};
static const struct power_supply_desc usb_desc = {
    .name = "usb", .type = POWER_SUPPLY_TYPE_USB,
    .properties = online_properties, .num_properties = ARRAY_SIZE(online_properties), .get_property = supply_get,
};
static const struct power_supply_desc ac_desc = {
    .name = "ac", .type = POWER_SUPPLY_TYPE_MAINS,
    .properties = online_properties, .num_properties = ARRAY_SIZE(online_properties), .get_property = supply_get,
};
static void led_set(struct led_classdev *led, enum led_brightness value) { }

static int __init c1sim_init(void)
{
    int error, i;
    static const char *const names[] = { "c1sim-led1", "c1sim-led2", "c1sim-led3", "c1sim-led4" };
    /* Direct kernel boot skips firmware ELCR setup. Malta routes PCI to
     * IRQ 10/11; these must be level triggered when AC97 shares the disk
     * interrupt, otherwise an asserted sound IRQ can hide a disk edge. */
    outb(inb(0x4d1) | 0x0c, 0x4d1);
    memset(screen.pixels, 0, FRAME_BYTES);
    screen.sequence = 1;
    paper = platform_device_register_simple("e0266a128", -1, NULL, 0);
    if (IS_ERR(paper)) return PTR_ERR(paper);
    error = sysfs_create_group(&paper->dev.kobj, &paper_group);
    if (error) goto fail_paper;
    for (i = 0; i < 2; ++i) {
        int code;
        keys[i] = input_allocate_device();
        if (!keys[i]) { error = -ENOMEM; goto fail_keys; }
        keys[i]->name = i ? "C1-Slim buttons" : "C1-Slim keyboard";
        keys[i]->id.bustype = BUS_VIRTUAL;
        for (code = 1; code <= KEY_MAX; ++code) input_set_capability(keys[i], EV_KEY, code);
        error = input_register_device(keys[i]);
        if (error) { input_free_device(keys[i]); keys[i] = NULL; goto fail_keys; }
    }
    battery = power_supply_register(&paper->dev, &battery_desc, NULL);
    if (IS_ERR(battery)) { error = PTR_ERR(battery); goto fail_keys; }
    usb = power_supply_register(&paper->dev, &usb_desc, NULL);
    if (IS_ERR(usb)) { error = PTR_ERR(usb); goto fail_battery; }
    ac = power_supply_register(&paper->dev, &ac_desc, NULL);
    if (IS_ERR(ac)) { error = PTR_ERR(ac); goto fail_usb; }
    for (i = 0; i < 4; ++i) {
        leds[i].name = names[i]; leds[i].max_brightness = 1; leds[i].brightness_set = led_set;
        error = led_classdev_register(&paper->dev, &leds[i]);
        if (error) goto fail_leds;
    }
    error = misc_register(&paper_device);
    if (error) goto fail_leds;
    error = misc_register(&bridge_device);
    if (error) goto fail_display;
    pr_info("c1sim: epaper, evdev, battery and LEDs ready\n");
    return 0;
fail_display: misc_deregister(&paper_device);
fail_leds:
    while (i-- > 0) led_classdev_unregister(&leds[i]);
    power_supply_unregister(ac);
fail_usb: power_supply_unregister(usb);
fail_battery: power_supply_unregister(battery);
fail_keys:
    for (i = 0; i < 2; ++i) if (keys[i]) input_unregister_device(keys[i]);
    sysfs_remove_group(&paper->dev.kobj, &paper_group);
fail_paper: platform_device_unregister(paper);
    return error;
}

static void __exit c1sim_exit(void)
{
    int i;
    misc_deregister(&bridge_device); misc_deregister(&paper_device);
    for (i = 0; i < 4; ++i) led_classdev_unregister(&leds[i]);
    power_supply_unregister(ac); power_supply_unregister(usb); power_supply_unregister(battery);
    for (i = 0; i < 2; ++i) input_unregister_device(keys[i]);
    sysfs_remove_group(&paper->dev.kobj, &paper_group);
    platform_device_unregister(paper);
}
module_init(c1sim_init);
module_exit(c1sim_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("C1-Slim device ABI for QEMU MIPS Linux");
