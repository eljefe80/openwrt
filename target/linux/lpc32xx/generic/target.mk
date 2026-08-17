BOARDNAME:=Generic

DEFAULT_PACKAGES += kmod-leds-gpio kmod-gpio-button-hotplug

define Target/Description
	Build generic firmware for NXP LPC32x0 based boards
	using the ARMv5/ARM926EJ-S instruction set.
endef
