ifndef _DT
$(error devtool is required)
endif

srcdirs   := baremetal src
ld_script := baremetal/baremetal.ld
target    := usbtool.bin

include $(_DT)/mk/rules.mk

.PHONY: boot
boot: $(BUILD)/$(target)
	${MICROMON_DIR}/bootstrap.py $< 115200

