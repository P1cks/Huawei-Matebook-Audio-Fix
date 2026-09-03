obj-m += snd-acp-legacy-mach.o
snd-acp-legacy-mach-y := src/acp-legacy-mach.o src/acp3x-es83xx/acp3x-es83xx.o

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
