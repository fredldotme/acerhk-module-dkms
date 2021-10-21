# change KERNELSRC to the location of your kernel build tree only if
# autodetection does not work
#KERNELSRC=/usr/src/linux
KERNELSRC?=/lib/modules/`uname -r`/build
# Starting with 2.6.18, the kernel version is in utsrelease.h instead of version.h, accomodate both cases
KERNELVERSION=$(shell awk -F\" '/REL/ {print $$2}' $(shell grep -s -l REL $(KERNELSRC)/include/linux/version.h $(KERNELSRC)/include/linux/utsrelease.h $(KERNELSRC)/include/generated/utsrelease.h))
KERNELMAJOR=$(shell echo $(KERNELVERSION)|head -c3)

# next line is for kernel 2.6, if you integrate the driver in the kernel tree
# /usr/src/linux/drivers/acerhk - or something similar
# don't forget to add the following line to the parent dir's Makefile:
# (/usr/src/linux/drivers/Makefile)
# obj-m                           += acerhk/
CONFIG_ACERHK?=m
obj-$(CONFIG_ACERHK) += acerhk.o

EXTRA_CFLAGS+=-c -Wall -Wstrict-prototypes -Wno-trigraphs -O2 -fno-strict-aliasing -fno-common -pipe
INCLUDE=-I$(KERNELSRC)/include
ifeq ($(KERNELMAJOR), 3.2)
TARGET := acerhk.ko
else ifeq ($(KERNELMAJOR), 3.0)
TARGET := acerhk.ko
else ifeq ($(KERNELMAJOR), 2.6)
TARGET := acerhk.ko
else
TARGET := acerhk.o
endif

SOURCE := acerhk.c

all: $(TARGET)

help:
	@echo Possible targets:
	@echo -e all\\t- default target, builds kernel module
	@echo -e install\\t- copies module binary to /lib/modules/$(KERNELVERSION)/extra/
	@echo -e clean\\t- removes all binaries and temporary files

# this target is only for me, don't use it yourself (Olaf)
export:
	sh export.sh

acerhk.ko: $(SOURCE) acerhk.h
	$(MAKE) -C $(KERNELSRC) SUBDIRS=$(shell pwd) CONFIG_FUNCTION_TRACER= modules

acerhk.o: $(SOURCE) acerhk.h
	$(CC) $(INCLUDE) $(CFLAGS) -DMODVERSIONS -DMODULE -D__KERNEL__ -o $(TARGET) $(SOURCE)

asm:	$(SOURCE)
ifeq ($(KERNELMAJOR), 2.6)
	$(CC) $(INCLUDE) $(INCLUDE)/asm-i386/mach-default $(CFLAGS) -fverbose-asm -S -DMODVERSIONS -DMODULE -D__KERNEL__ $(SOURCE)
else
	$(CC) $(INCLUDE) $(CFLAGS) -fverbose-asm -S -DMODVERSIONS -DMODULE -D__KERNEL__ $(SOURCE)
endif

clean:
	rm -f *~ *.o *.s *.ko .acerhk* *.mod.c

load:	$(TARGET)
	insmod $(TARGET)

unload:
	rmmod acerhk

install: $(TARGET)
	mkdir -p /lib/modules/$(KERNELVERSION)/extra
	cp -v $(TARGET) /lib/modules/$(KERNELVERSION)/extra/
	depmod -a
