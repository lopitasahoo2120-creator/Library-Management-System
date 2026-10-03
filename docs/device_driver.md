# The character device driver

`driver/library_driver.c` is a real Linux kernel module (C, kernel APIs only).
It creates `/dev/library_driver`, accepts one short text event per `write(2)`,
counts them in the kernel and returns the counter plus the most recent event on
`read(2)`.

## 1. User-space ↔ kernel-space path

```
  C++ application                 Linux kernel
  -----------------               ---------------------------------
  DriverClient::sendEvent()
    open("/dev/library_driver")  ->  cdev lookup by major number
    write("BOOK_ISSUE:101:7\n") ->  driver_write()
                                      copy_from_user()  -> kernel memory
                                      trim '\n'
                                      reject non-printable bytes
                                      store latest_event, event_count++
                                      mutex protects both

  DriverClient::readReport()
    read(buf, 512)              ->  driver_read()
                                      scnprintf into a kernel buffer
                                      copy_to_user()  -> user memory
                                      returns "events=N\nlatest=<text>\n"
```

There is no `ioctl` and no shared struct: the contract is deliberately "one
text line in, one small text report out". That keeps the userspace and kernelspace
sides understandable and keeps them independent of struct padding.

## 2. Kernel APIs used and why

| API | Purpose |
|---|---|
| `alloc_chrdev_region()` | reserve a major number dynamically, so the module never conflicts with an existing device |
| `cdev_init()` / `cdev_add()` | attach the `file_operations` and register them for the allocated major |
| `class_create()` / `device_create()` | ask udev to create `/dev/library_driver` with the right node type and permissions |
| `copy_from_user()` / `copy_to_user()` | safe transfer between user and kernel memory; these also validate the pointer |
| `mutex_lock()` / `mutex_unlock()` | protect `latest_event` and `event_count` against concurrent writers |
| `default_llseek` | give the device normal `lseek` semantics |
| `THIS_MODULE` in `.owner` | the kernel refuses to unload the module while a file is open |
| `unregister_chrdev_region()`, `cdev_del()`, `class_destroy()`, `device_destroy()` | exact reverse of init, so `rmmod` leaks nothing |

## 3. Kernel version compatibility

The module uses the modern character-device API introduced in Linux 5.15
(`alloc_chrdev_region`, `cdev_*`, `class_create`, `device_create`). No
`init_module()` / `register_chrdev()` calls and no removed `file_operations`
fields are used.

One call differs between distributions: upstream 5.15 changed `class_create()` to
take an owner argument, but **Ubuntu keeps the single-argument form** for
out-of-tree module compatibility (verified against the Ubuntu 24.04 kernel
6.8 headers, where `class_create(const char *name)` is what
`include/linux/device/class.h` declares). The module therefore compiles with the
Ubuntu signature and can be switched with:

```bash
make -C driver EXTRA_CFLAGS=-DLIBRARY_CLASS_CREATE_TAKES_OWNER   # upstream kernel
```

Always check the target kernel before building:

```bash
uname -r
ls /lib/modules/$(uname -r)/build      # headers must exist
```

If the running kernel has no matching headers, build against another kernel's
headers for a compile check (`make -C driver KDIR=/lib/modules/<other>/build`)
but remember the module can only be loaded on the kernel it was built for.

## 4. Input validation in the kernel

The driver is the trust boundary, so it validates before storing anything:

* length must be within `EVENT_MAX_LEN` (256)
* trailing `\r` / `\n` are trimmed
* an empty line is accepted and counted as a no-op
* every remaining byte must be printable ASCII (`0x20`-`0x7E`); anything else is
  rejected with `-EINVAL` and a `pr_warn`

This is why `DriverClient::sendEvent()` also enforces the 256-byte limit and
refuses an empty event - the user side and the kernel side agree.

## 5. The read protocol

`driver_read()` serves the report **once per `open()`**, using the file offset
as the marker:

```c
if (*ppos > 0) return 0;        /* already served: the reader sees EOF */
```

`open()` resets `f_pos` to 0. So both of these behave exactly as expected:

```bash
$ cat /dev/library_driver
events=42
latest=BOOK_ISSUE:101:7
$ echo "BOOK_RETURN:101:7" > /dev/library_driver
$ cat /dev/library_driver
events=43
latest=BOOK_RETURN:101:7
```

and the application's read loop terminates on the `0` return instead of spinning.

## 6. Graceful degradation (required behaviour)

The library system must never depend on the driver. `DriverClient::open()`
probes with `stat(2)`, verifies the path is a character device, and opens it
`O_RDWR`. If any step fails it simply stays closed:

```cpp
bool DriverClient::isAvailable() const noexcept { return fd_ >= 0; }
bool DriverClient::sendEvent(...) { if (fd_ < 0) return false; ... }
long long DriverClient::eventCount() { if (!readReport(out)) return -1; ... }
```

`Library::notifyDriver()` ignores the result, so a missing module costs exactly
one warning at start-up:

```
Warning: Library driver unavailable. Continuing without kernel event reporting.
```

Menu option 6 in the application shows the live state:

```
  Kernel driver   : /dev/library_driver (loaded)
  Driver report   : events=42
latest=BOOK_ISSUE:101:7
```

`DriverClientReportsAvailability` in the test suite works in both worlds: when
the module is loaded it asserts the counter actually increments by one, and when
it is absent it asserts every call degrades to `false`/`-1` instead of failing.

## 7. Build, load and verify

```bash
# build
make -C driver                      # needs linux-headers for the running kernel

# load (root required)
sudo insmod driver/library_driver.ko
ls -l /dev/library_driver           # crw------- 1 root root <major>,<minor>

# use
cat /dev/library_driver
echo "BOOK_ISSUE:101:7" > /dev/library_driver
cat /dev/library_driver
dmesg | tail -3                     # pr_info lines from the module

# unload
sudo rmmod library_driver
```

`make -C driver load` and `make -C driver unload` wrap the `insmod`/`rmmod`
pair, and `make -C driver info` prints both the `lsmod` line and the current
device report.

## 8. Note on WSL

WSL2 does not support loading kernel modules, so the driver can be *compiled*
inside WSL (against the Ubuntu headers) but not inserted. Demonstrating
`insmod` requires a real Ubuntu installation or a VM. The application is built
to behave correctly in both cases, which is exactly what
`DriverClientReportsAvailability` verifies.
