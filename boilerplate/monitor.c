#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pid.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/timer.h>
#include <linux/uaccess.h>
#include <linux/sched/mm.h>
#include <linux/version.h>

#include "monitor_ioctl.h"

#define DEVICE_NAME "container_monitor"
#define CHECK_INTERVAL_SEC 1

/* ================= NODE ================= */
struct monitor_entry {
    pid_t pid;
    char container_id[32];
    unsigned long soft_limit;
    unsigned long hard_limit;
    int soft_warned;
    struct list_head list;
};

/* ================= GLOBAL ================= */
static LIST_HEAD(monitor_list);
static DEFINE_MUTEX(monitor_lock);

static struct timer_list monitor_timer;
static dev_t dev_num;
static struct cdev c_dev;
static struct class *cl;

/* ================= RSS ================= */
static long get_rss_bytes(pid_t pid)
{
    struct task_struct *task;
    struct mm_struct *mm;
    long rss_pages = 0;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (!task) {
        rcu_read_unlock();
        return -1;
    }
    get_task_struct(task);
    rcu_read_unlock();

    mm = get_task_mm(task);
    if (mm) {
        rss_pages = get_mm_rss(mm);
        mmput(mm);
    }

    put_task_struct(task);
    return rss_pages * PAGE_SIZE;
}

/* ================= TIMER ================= */
static void timer_callback(struct timer_list *t)
{
    struct monitor_entry *e, *tmp;

    mutex_lock(&monitor_lock);

    list_for_each_entry_safe(e, tmp, &monitor_list, list)
    {
        long rss = get_rss_bytes(e->pid);

        /* process exited */
        if (rss < 0) {
            list_del(&e->list);
            kfree(e);
            continue;
        }

        /* SOFT LIMIT */
        if (!e->soft_warned && rss > e->soft_limit) {
            printk(KERN_WARNING
                "[monitor] SOFT LIMIT container=%s pid=%d rss=%ld limit=%lu\n",
                e->container_id, e->pid, rss, e->soft_limit);
            e->soft_warned = 1;
        }

        /* HARD LIMIT */
        if (rss > e->hard_limit) {
            struct task_struct *task;

            rcu_read_lock();
            task = pid_task(find_vpid(e->pid), PIDTYPE_PID);
            if (task)
                send_sig(SIGKILL, task, 1);
            rcu_read_unlock();

            printk(KERN_WARNING
                "[monitor] HARD LIMIT container=%s pid=%d rss=%ld limit=%lu\n",
                e->container_id, e->pid, rss, e->hard_limit);

            list_del(&e->list);
            kfree(e);
        }
    }

    mutex_unlock(&monitor_lock);

    mod_timer(&monitor_timer, jiffies + CHECK_INTERVAL_SEC * HZ);
}

/* ================= IOCTL ================= */
static long monitor_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
    struct monitor_request req;

    if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
        return -EFAULT;

    /* REGISTER */
    if (cmd == MONITOR_REGISTER) {
        struct monitor_entry *e;

        e = kmalloc(sizeof(*e), GFP_KERNEL);
        if (!e) return -ENOMEM;

        e->pid = req.pid;
        strncpy(e->container_id, req.container_id, sizeof(e->container_id) - 1);
        e->container_id[31] = '\0';

        e->soft_limit = req.soft_limit_bytes;
        e->hard_limit = req.hard_limit_bytes;
        e->soft_warned = 0;

        INIT_LIST_HEAD(&e->list);

        mutex_lock(&monitor_lock);
        list_add(&e->list, &monitor_list);
        mutex_unlock(&monitor_lock);

        printk(KERN_INFO
            "[monitor] REGISTER %s pid=%d\n",
            e->container_id, e->pid);

        return 0;
    }

    /* UNREGISTER */
    if (cmd == MONITOR_UNREGISTER) {
        struct monitor_entry *e, *tmp;

        mutex_lock(&monitor_lock);

        list_for_each_entry_safe(e, tmp, &monitor_list, list)
        {
            if (e->pid == req.pid) {
                list_del(&e->list);
                kfree(e);
                mutex_unlock(&monitor_lock);

                printk(KERN_INFO
                    "[monitor] UNREGISTER pid=%d\n", req.pid);

                return 0;
            }
        }

        mutex_unlock(&monitor_lock);
        return -ENOENT;
    }

    return -EINVAL;
}

/* ================= FILE OPS ================= */
static struct file_operations fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = monitor_ioctl,
};

/* ================= INIT ================= */
static int __init monitor_init(void)
{
    alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME);

    cl = class_create(THIS_MODULE, DEVICE_NAME);
    device_create(cl, NULL, dev_num, NULL, DEVICE_NAME);

    cdev_init(&c_dev, &fops);
    cdev_add(&c_dev, dev_num, 1);

    timer_setup(&monitor_timer, timer_callback, 0);
    mod_timer(&monitor_timer, jiffies + CHECK_INTERVAL_SEC * HZ);

    printk(KERN_INFO "[monitor] loaded\n");
    return 0;
}

/* ================= EXIT ================= */
static void __exit monitor_exit(void)
{
    struct monitor_entry *e, *tmp;

    del_timer_sync(&monitor_timer);

    mutex_lock(&monitor_lock);
    list_for_each_entry_safe(e, tmp, &monitor_list, list)
    {
        list_del(&e->list);
        kfree(e);
    }
    mutex_unlock(&monitor_lock);

    cdev_del(&c_dev);
    device_destroy(cl, dev_num);
    class_destroy(cl);
    unregister_chrdev_region(dev_num, 1);

    printk(KERN_INFO "[monitor] unloaded\n");
}

module_init(monitor_init);
module_exit(monitor_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Task 4 Memory Monitor");