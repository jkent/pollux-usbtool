#
#  Copyright (C) 2013 Jeff Kent <jeff@jkent.net>
#
#  This program is free software; you can redistribute it and/or modify
#  it under the terms of the GNU General Public License version 2 as
#  published by the Free Software Foundation.
#
#  This program is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU General Public License for more details.
#
#  You should have received a copy of the GNU General Public License
#  along with this program; if not, write to the Free Software
#  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
#

NAME    = usbtool
TARGET  = $(NAME).bin

DIRS    = ./ \
          ./udc/

BAREMETAL = ./baremetal

CC      = $(CROSS_COMPILE)gcc
OBJCOPY = $(CROSS_COMPILE)objcopy
OBJDUMP = $(CROSS_COMPILE)objdump
SIZE    = $(CROSS_COMPILE)size

# pedantic removed due to braced expressions
CFLAGS  = -std=gnu99 -Wall -fms-extensions \
          -mcpu=arm926ej-s -mlittle-endian -msoft-float \
          -ffast-math
ASFLAGS = -Wa,
LDFLAGS =
LIBS = -lgcc -lc $(BAREMETAL)/baremetal.a
INCLUDE = -I$(BAREMETAL)/include -I./include -I./src

ifneq ($(NODEBUG), 1)
	CFLAGS += -O0 -g3 -DDEBUG
else
	CFLAGS += -Os
endif

.SECONDARY:
.PHONY: all
all: $(TARGET)
	@$(MAKE) -s deps

-include obj/deps.mk

define new_c_rule
$(2)%.o: $(1)%.c
	@mkdir -p $$(@D)
	$(CC) -c $$(CFLAGS) $$(INCLUDE) -o $$@ -c $$<
	@$(CC) -MM $$(CFLAGS) $$(INCLUDE) $$< | \
	  sed -e 's/.*:/$$(subst /,\/,$$@):/' > $(2)$$*.d
endef

define new_s_rule
$(2)%.o: $(1)%.S
	@mkdir -p $$(@D)
	$(CC) -c $$(CFLAGS) $$(INCLUDE) -Wa,--defsym,_start=0 -o $$@ $$<
	@$(CC) -MM $$(CFLAGS) $$(INCLUDE) -Wa,--defsym,_start=0 $$< | \
	  sed -e 's/.*:/$$(subst /,\/,$$@):/' > $(2)$$*.d
endef

$(foreach src_dir,$(sort $(DIRS)), \
	$(eval C_SRCS += $(wildcard $(src_dir)*.c)) \
	$(eval S_SRCS += $(wildcard $(src_dir)*.S)) \
	$(eval obj_dir = $(subst ./,./obj/,$(src_dir))) \
	$(eval $(call new_c_rule,$(src_dir),$(obj_dir))) \
	$(eval $(call new_s_rule,$(src_dir),$(obj_dir))) \
)

OBJS    = $(subst ./,./obj/,$(S_SRCS:.S=.o) $(C_SRCS:.c=.o))

./obj/%.lds: $(BAREMETAL)/%.lds
	@mkdir -p $(@D)
	$(CC) -E -P -x c $(INCLUDE) -o $@ $<
	@$(CC) -MM -x c $(INCLUDE) $< | \
	  sed -e 's/.*:/$(subst /,\/,$@):/' > obj/$*.lds.d

%.elf: $(BAREMETAL)/baremetal.lds $(BAREMETAL)/baremetal.a $(OBJS)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(LDFLAGS) -Wl,-M,-Map,$(NAME).map -T $^ $(LIBS) -o $@

%.bin: %.elf
	$(OBJCOPY) -S -I elf32-littlearm -O binary $< $@
	@echo
	@$(SIZE) --target=binary $@

%.dis: %.elf
	$(OBJDUMP) -d -m armv5te $< > $@

.PHONY: deps
deps: $(TARGET)
	@rm -f obj/deps.mk
	@$(foreach directory,$(subst ./,./obj/,$(DIRS)), \
	  $(foreach file,$(wildcard $(directory)/*.d), \
		cat $(file) >> ./obj/deps.mk; \
	  ) \
	)

.PHONY: clean
clean:
	rm -rf obj
	rm -f $(NAME).bin $(NAME).elf $(NAME).map $(NAME).dis

.PHONY: boot
boot: all
	${MICROMON_DIR}/bootstrap.py $(TARGET) 115200
