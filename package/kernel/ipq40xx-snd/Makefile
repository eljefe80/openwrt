include $(TOPDIR)/rules.mk

PKG_NAME:=ipq40xx-snd
PKG_RELEASE:=1

PKG_LICENSE:=GPLv2

PKG_SOURCE_URL:=https://github.com/eljefe80/ipq40xx-snd.git
PKG_SOURCE_PROTO:=git
PKG_SOURCE_DATE:=2023-12-15
PKG_SOURCE_VERSION:=cb85766a2866c0477f29915471c34da91ed4e9b0

PKG_BUILD_PARALLEL:=1

include $(INCLUDE_DIR)/kernel.mk
include $(INCLUDE_DIR)/package.mk

define KernelPackage/snd-simple-card/description
 Kernel modules for the soc simple-audio-card.
endef

define KernelPackage/snd-simple-card
  SUBMENU:=Sound Support
  TITLE:=SOC Simple Audio Card Support
  DEPENDS:=+kmod-sound-soc-core
  KCONFIG:=CONFIG_SND_SIMPLE_CARD
  FILES:=\
	$(LINUX_DIR)/sound/soc/generic/snd-soc-simple-card.ko \
	$(LINUX_DIR)/sound/soc/generic/snd-soc-simple-card-utils.ko
  AUTOLOAD:=$(call AutoLoad,35,snd-soc-simple-card)
  PROVIDES:=kmod-snd-simple-card
endef

define KernelPackage/ipq40xx-snd
  SUBMENU:=Sound Support
  TITLE:=ipq40xx sound driver
  DEPENDS:=+kmod-sound-core +kmod-snd-simple-card
  FILES:=\
	$(PKG_BUILD_DIR)/tas5782m.ko \
	$(PKG_BUILD_DIR)/snd-soc-ipq40xx-cpu.ko \
	$(PKG_BUILD_DIR)/snd-soc-ipq40xx-i2s.ko
  AUTOLOAD:=$(call AutoProbe,snd-soc-ipq40xx-i2s snd-soc-ipq40xx-cpu tas5782m)
  PROVIDES:=kmod-ipq40xx-snd
endef

EXTRA_KCONFIG:= \
  CONFIG_SND_QCA_SOC_IPQ40XX=m \
  CONFIG_SND_IPQ40XX_SOC_CODEC_TAS5782M=m \
  CONFIG_SND_IPQ40XX_SOC_I2S_PCM=m \
  CONFIG_SND_IPQ40XX_SOC_CPU_DAI=m

NOSTDINC_FLAGS := \
	$(KERNEL_NOSTDINC_FLAGS) \
	-I$(PKG_BUILD_DIR) \

define Build/Configure
endef

define Build/Compile/ipq40xx-snd
	+$(KERNEL_MAKE) $(PKG_JOBS) \
		M="$(PKG_BUILD_DIR)" \
		NOSTDINC_FLAGS="$(NOSTDINC_FLAGS)" \
		modules
endef

define Build/Compile/snd-simple-card
endef

define Build/Compile
	$(call Build/Compile/ipq40xx-snd)
	$(call Build/Compile/snd-simple-card)
endef

define KernelPackage/ipq40xx-snd/config
endef

$(eval $(call KernelPackage,ipq40xx-snd))
$(eval $(call KernelPackage,snd-simple-card,+kmod-sound-soc-core))
