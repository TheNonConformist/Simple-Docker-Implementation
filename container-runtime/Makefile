obj-m += monitor.o

KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)
WORKLOAD_LDFLAGS ?= -static

USER_TARGETS := engine

all: $(USER_TARGETS) module

ci: WORKLOAD_LDFLAGS =
ci: $(USER_TARGETS)

module: monitor.ko

engine: engine.c monitor_ioctl.h
	gcc -O2 -Wall -Wextra -D_GNU_SOURCE -o engine engine.c -lpthread

monitor.ko: monitor.c monitor_ioctl.h
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	if [ -d "$(KDIR)" ]; then $(MAKE) -C $(KDIR) M=$(PWD) clean; fi
	rm -f $(USER_TARGETS) *.o *.mod *.mod.c *.symvers *.order *.cmd .*.cmd
	rm -f *.log
	rm -rf logs
	rm -f /tmp/mini_runtime.sock

distclean: clean
	rm -f Module.symvers modules.order
	rm -rf rootfs-*/

.PHONY: all ci module clean distclean
