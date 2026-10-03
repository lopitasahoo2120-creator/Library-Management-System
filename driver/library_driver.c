/* -------------------------------------------------------------------------
 *  library_driver.c - Linux character device driver for the Library
 *                     Management System (capstone project).
 *
 *  WHAT IT DOES
 *      The library application writes one short text event per significant
 *      operation, for example:
 *
 *          BOOK_ISSUE:101:7
 *
 *      The driver counts those events in the kernel and remembers the most
 *      recent one.  Any process may read the state back with a plain read(2):
 *
 *          cat /dev/library_driver
 *          events=42
 *          latest=BOOK_ISSUE:101:7
 *
 *  WHY A CHARACTER DEVICE?
 *      It is the simplest driver class that gives real user-space <-> kernel
 *      space communication (open/write/read/release) with no networking and
 *      no framework.  The specification asks for a genuine driver, not a
 *      simulation, so everything below uses real kernel APIs.
 *
 *  KERNEL APIs USED
 *      alloc_chrdev_region()  - reserve a dynamic major number
 *      cdev_init()/cdev_add()  - register the file operations
 *      class_create()          - create /sys/class/library_driver
 *      device_create()         - create the /dev/library_driver node
 *      copy_from_user()        - move the event into kernel memory
 *      copy_to_user()          - move the report into user memory
 *      mutex_lock()            - protect the shared state
 *      unregister_chrdev_region(), cdev_del(), class_destroy(),
 *      device_destroy()        - orderly unload
 *
 *  COMPATIBILITY
 *      Written against the modern (5.15+) character-device API: alloc_chrdev_region(),
 *      cdev_init()/cdev_add(), class_create() and device_create().  No legacy
 *      init_module()/register_chrdev() calls and no deprecated file_operations
 *      fields.  The only version-dependent call is class_create(), which is
 *      handled for both the upstream and the Ubuntu signature.
 * ------------------------------------------------------------------------- */

#include <linux/version.h>

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#define DRIVER_NAME   "library_driver"
#define DEVICE_NAME   "library_driver"
#define EVENT_MAX_LEN 256
#define REPORT_MAX_LEN (EVENT_MAX_LEN + 64)

/* -------------------------------------------------------------------------
 *  Module state
 * ------------------------------------------------------------------------- */

static dev_t             devt;
static struct cdev      library_cdev;
static struct class    *library_class;

/* The audit state every user sees.  Protected by event_lock because more than
 * one process may write to the device at the same time. */
static char             latest_event[EVENT_MAX_LEN];
static unsigned long    event_count;

static DEFINE_MUTEX(event_lock);

/* -------------------------------------------------------------------------
 *  open() / release()
 *
 *  Opening is only permission checking; the state itself lives in the module,
 *  which is exactly what makes this useful: any tool can inspect the counters
 *  without going through the library application.
 * ------------------------------------------------------------------------- */
static int driver_open(struct inode *inode, struct file *filp)
{
    /* Nothing to do on open beyond resetting the per-file cursor; the audit
     * state lives in the module, so it is shared by every opener. */
    (void)inode;
    filp->f_pos = 0;
    return 0;
}

static int driver_release(struct inode *inode, struct file *filp)
{
    /* Nothing to release: the state belongs to the module, not to the file. */
    (void)inode;
    (void)filp;
    return 0;
}

/* -------------------------------------------------------------------------
 *  read() - hand the report to user space.
 *
 *  The report is generated once per open(): after f_pos has advanced past 0 a
 *  further read() returns 0, which user space sees as end-of-file.  That keeps
 *  `cat /dev/library_driver` and the application's read loop both simple.
 * ------------------------------------------------------------------------- */
static ssize_t driver_read(struct file *filp, char __user *buf, size_t count,
                           loff_t *ppos)
{
    char report[REPORT_MAX_LEN];
    int  len;

    /* Already served for this open(). */
    if (*ppos > 0)
        return 0;

    mutex_lock(&event_lock);
    len = scnprintf(report, sizeof(report), "events=%lu\nlatest=%s\n",
                    event_count,
                    latest_event[0] != '\0' ? latest_event : "(none)");
    mutex_unlock(&event_lock);

    if (len < 0)
        return len;

    if (count < (size_t)len)
        len = (int)count;

    /* The per-file cursor (*ppos) tracks consumption, so filp itself is not
     * needed here. */
    (void)filp;

    /* copy_to_user() returns the number of bytes it could NOT copy. */
    if (copy_to_user(buf, report, len))
        return -EFAULT;

    *ppos += len;
    return len;
}

/* -------------------------------------------------------------------------
 *  write() - accept one event line from user space.
 *
 *  Validation performed in the kernel:
 *    * length within bounds
 *    * no embedded NUL bytes
 *    * printable characters only (no control characters except the newline)
 * ------------------------------------------------------------------------- */
static ssize_t driver_write(struct file *filp, const char __user *buf,
                            size_t count, loff_t *ppos)
{
    char    raw[EVENT_MAX_LEN + 2];
    size_t  len;
    size_t  i;

    /* Writing is append-only; the file offset is irrelevant here. */
    (void)filp;
    (void)ppos;

    if (count == 0)
        return 0;

    /* One extra byte so a trailing newline can be detected and dropped. */
    if (count > EVENT_MAX_LEN + 1)
        return -EINVAL;

    if (copy_from_user(raw, buf, count))
        return -EFAULT;

    /* Trim the trailing newline that every well-behaved writer appends. */
    len = count;
    while (len > 0 && (raw[len - 1] == '\n' || raw[len - 1] == '\r'))
        len--;

    if (len == 0)
        return (ssize_t)count;    /* an empty line is not an error */

    /* Reject anything that is not plain text. */
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)raw[i];
        if (c < 0x20 || c > 0x7E) {
            pr_warn(DRIVER_NAME ": rejecting non-printable byte 0x%02x\n", c);
            return -EINVAL;
        }
    }

    mutex_lock(&event_lock);
    memcpy(latest_event, raw, len);
    latest_event[len] = '\0';
    event_count++;
    pr_info(DRIVER_NAME ": event #%lu recorded: %s\n", event_count, latest_event);
    mutex_unlock(&event_lock);

    return (ssize_t)count;
}

/* -------------------------------------------------------------------------
 *  file_operations
 *
 *  .owner guarantees the module cannot be unloaded while it is open.
 * ------------------------------------------------------------------------- */
static const struct file_operations library_fops = {
    .owner   = THIS_MODULE,
    .open    = driver_open,
    .release = driver_release,
    .read    = driver_read,
    .write   = driver_write,
    .llseek  = default_llseek,
};

/* -------------------------------------------------------------------------
 *  Module initialisation: reserve a major number, register the cdev, and ask
 *  udev to create /dev/library_driver for us.
 * ------------------------------------------------------------------------- */
static int __init library_driver_init(void)
{
    int ret;

    /* 1. Dynamic major number, minor 0 - one device node. */
    ret = alloc_chrdev_region(&devt, 0, 1, DRIVER_NAME);
    if (ret < 0) {
        pr_err(DRIVER_NAME ": alloc_chrdev_region failed (%d)\n", ret);
        return ret;
    }

    /* 2. Hook our file operations onto the cdev. */
    cdev_init(&library_cdev, &library_fops);
    library_cdev.owner = THIS_MODULE;

    ret = cdev_add(&library_cdev, devt, 1);
    if (ret < 0) {
        pr_err(DRIVER_NAME ": cdev_add failed (%d)\n", ret);
        unregister_chrdev_region(devt, 1);
        return ret;
    }

    /* 3. Create /sys/class/library_driver and let udev make the node.
     *
     * Upstream 5.15 added an owner argument to class_create(); Ubuntu keeps
     * the historical single-argument form for out-of-tree module
     * compatibility (verified against the Ubuntu 24.04 / kernel 6.8
     * headers).  This project targets Ubuntu, so the single-argument form is
     * the default; build with
     *     make EXTRA_CFLAGS=-DLIBRARY_CLASS_CREATE_TAKES_OWNER
     * to target an upstream kernel instead. */
#ifdef LIBRARY_CLASS_CREATE_TAKES_OWNER
    library_class = class_create(THIS_MODULE, DEVICE_NAME);
#else
    library_class = class_create(DEVICE_NAME);
#endif
    if (IS_ERR(library_class)) {
        ret = PTR_ERR(library_class);
        pr_err(DRIVER_NAME ": class_create failed (%d)\n", ret);
        cdev_del(&library_cdev);
        unregister_chrdev_region(devt, 1);
        return ret;
    }

    if (IS_ERR(device_create(library_class, NULL, devt, NULL, DEVICE_NAME))) {
        pr_err(DRIVER_NAME ": device_create failed\n");
        class_destroy(library_class);
        cdev_del(&library_cdev);
        unregister_chrdev_region(devt, 1);
        return -ENODEV;
    }

    pr_info(DRIVER_NAME ": loaded, /dev/" DEVICE_NAME " is ready (major %d)\n",
            MAJOR(devt));
    return 0;
}

/* -------------------------------------------------------------------------
 *  Module teardown - the exact reverse of init, so nothing is leaked.
 * ------------------------------------------------------------------------- */
static void __exit library_driver_exit(void)
{
    device_destroy(library_class, devt);
    class_destroy(library_class);
    cdev_del(&library_cdev);
    unregister_chrdev_region(devt, 1);
    pr_info(DRIVER_NAME ": unloaded, %lu event(s) seen during this session\n",
            event_count);
}

module_init(library_driver_init);
module_exit(library_driver_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Library Management System - Capstone Project");
MODULE_DESCRIPTION("Character device reporting library catalogue events");
MODULE_VERSION("1.0");