# ===========================================================================
#  Kernel module build file (Kbuild).
#
#      make            build library_driver.ko
#      make load       insmod it (needs root)
#      make unload     rmmod it   (needs root)
#      make info       show the loaded module
#      make clean      remove build artefacts
#
#  KDIR can be overridden when testing against headers other than the running
#  kernel, e.g.  make KDIR=/lib/modules/$(uname -r)/build
# ===========================================================================

obj-m += library_driver.o

# CURDIR is the *logical* directory, which keeps the symlinked path intact.
KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(CURDIR)

# Kernel modules must NOT inherit the userspace flags.
# -Wall only: the kernel headers themselves are not -Wextra clean, and the
# module build is not a userspace compilation.
CFLAGS_MODULE := -Wall $(EXTRA_CFLAGS)

.PHONY: all clean load unload info

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

load: all
	sudo insmod library_driver.ko
	ls -l /dev/library_driver

unload:
	sudo rmmod library_driver

info:
	lsmod | grep library_driver || echo "module not loaded"
	cat /dev/library_driver 2>/dev/null || echo "device not present"