# Compile module under Linux 2.6.x using something like:
# make -C /lib/modules/`uname -r`/build SUBDIRS=$PWD modules

obj-m += ioctl_module.o 
KDIR ?= /lib/modules/$(shell uname -r)/build 
PWD := $(shell pwd) 

all: module user 
module: 
	$(MAKE) -C $(KDIR) M=$(PWD) modules 

user: ioctl_test 
ioctl_test: ioctl_test.c 
	gcc -O2 -Wall -o $@ $< 

clean: 
	$(MAKE) -C $(KDIR) M=$(PWD) clean 
	$(RM) ioctl_test