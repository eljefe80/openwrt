// SPDX-License-Identifier: GPL-2.0-only
/*
 * Marvell Prestera 98DX3234 switch fabric, as wired on the Check Point
 * L-72W (1470/1490) appliance.
 *
 * The switch has no PCI/MBUS path to the Alpine AL31400 SoC; every
 * internal register is reached by tunnelling over clause-22 MDIO
 * transactions to PHY address 31 on the MDIO bus that al_eth creates
 * for eth1.
 *
 * The 16 LAN PHYs (2x Marvell 88E1685 octal) are not on any CPU MDIO
 * bus either -- they hang off the switch's own internal SMI master. So
 * this driver's main job is to expose that SMI master as a Linux
 * mii_bus, which makes the LAN PHYs ordinary phy_devices handled by
 * the in-tree Marvell PHY driver.
 *
 * Register layout, access procedures and the magic initialisation
 * values below are documented in docs/98dx3234-l72w-specs.md. Section
 * references in the comments point there.
 */

#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/phy.h>
#include <linux/mdio.h>
#include <linux/bitfield.h>
#include <linux/debugfs.h>
#include <linux/if_bridge.h>
#include <linux/of.h>
#include <linux/phylink.h>
#include <linux/platform_device.h>
#include <net/dsa.h>

#include "prestera_98dx3234_init_tbl.h"

/* SMI tunnel: MDIO register numbers on PHY address 31 (spec 3) */
#define PRESTERA_SMI_ADDR		31
#define SMI_CMD_HI			0	/* command/address word, write path */
#define SMI_CMD_LO			1
#define SMI_ADDR_HI			4	/* address word, read path */
#define SMI_ADDR_LO			5
#define SMI_DATA_HI			6	/* read result */
#define SMI_DATA_LO			7
#define SMI_PAYLOAD_HI			8	/* write payload / page value */
#define SMI_PAYLOAD_LO			9
#define SMI_STATUS			31
#define SMI_STATUS_READ_READY		BIT(0)	/* reads poll bit0 ... */
#define SMI_STATUS_WRITE_DONE		BIT(1)	/* ... writes poll bit1 */

#define SMI_POLL_TRIES			999
#define SMI_POLL_DELAY_US		50

/* Address word encoding (spec 3.1, 3.2) */
#define SMI_ADDR_MASK			0x0FFFFFFCu
#define SMI_SEL_NORMAL			0x30000002u
#define SMI_SEL_RIC			0x80000002u
#define SMI_PAGE_WORD_NORMAL		0x8001310Cu
#define SMI_PAGE_WORD_RIC		0x80013120u
#define SMI_PAGE_VALUE(addr)		(((addr) >> 28) | 0x10000u)

#define PRESTERA_ID_REG			2
#define PRESTERA_ID_MARVELL		0x0141

/* SMI master, for reaching the LAN PHYs (spec 5) */
#define SMI_MGMT			0x07004054u
/*
 * SMI_ALIGN must be programmed before any PHY access. Until it is, the
 * SMI master returns PHY data misaligned by one bit -- (data << 1) | 1 --
 * so PHY IDs are unrecognisable and bit15 is unreadable. Established by
 * bisection over the five 0x07xxxxxx registers the vendor writes:
 * SMI_ALIGN alone flips PHY ID1 from 0x0283 to the correct 0x0141, while
 * SMI_CFG0/SMI_CFG1 have no effect on it. The other three are programmed
 * anyway to match the vendor.
 */
#define SMI_ALIGN			0x07004200u
#define SMI_ALIGN_VALUE			0x002B0000u
#define SMI_CFG0			0x07004030u
#define SMI_CFG0_VALUE			0x0A418820u
#define SMI_CFG1			0x07004034u
#define SMI_CFG1_VALUE			0x00000140u
#define SMI_CFG2			0x07005110u
#define SMI_CFG2_VALUE			0x00001000u
#define SMI_MGMT_DATA			GENMASK(15, 0)
#define SMI_MGMT_PHYAD			GENMASK(20, 16)
#define SMI_MGMT_REGAD			GENMASK(25, 21)
#define SMI_MGMT_OP_READ		BIT(26)
#define SMI_MGMT_READ_VALID		BIT(27)

/* GE-MAC, per port (spec 6.1) */
#define GEMAC_BASE			0x12000000u
#define GEMAC_STRIDE			0x1000u
#define GEMAC_CTRL			0x00u
#define GEMAC_CTRL_PORT_EN		BIT(0)
#define GEMAC_CTRL_VALUE		0xC651u
#define GEMAC_REG0C			0x0Cu
#define GEMAC_REG0C_LAN			0xB2E8u
#define GEMAC_REG0C_WAN			0xB8ECu

#define PRESTERA_LAN_PORTS		16
/*
 * Silicon ports 24 and 25 both face the SoC; traffic was observed
 * arriving on the one wired to the al_eth MAC that mainline enumerates
 * as eth0 (PCIe 0000:00:00.0). Port 26 is WAN, 27 is disabled.
 */
#define PRESTERA_CPU_PORT		24
/*
 * DSA addresses ports 0..num_ports-1, so this deliberately stops at the CPU
 * port: the sixteen LAN jacks and the CPU port are all that the devicetree
 * describes. It also means port 26 is not reachable through DSA. Wiring the
 * WAN jack up as a user port would need this raised to 27 and a port@26 node
 * -- but which jack port 26 actually drives is only inferred from a vendor
 * log line, and it has no PHY on our SMI master (addresses 0-15 are the LAN
 * 88E1685s), so it would need a fixed-link and a cable to verify. Not done.
 */
#define PRESTERA_NUM_PORTS		(PRESTERA_CPU_PORT + 1)
#define PRESTERA_WAN_PORT		26

/*
 * ---- L2 forwarding: isolation, bridging and 802.1Q (spec 12.1-12.4) ----
 *
 * The fabric floods within a VLAN's member set and has no STP of its own
 * (spec 12.1), so forwarding is controlled through the VLAN members table.
 * One model covers both bridge modes:
 *
 *  - vlan-unaware (the default, and STP-blocked ports): each port is confined
 *    to an internal VID from a reserved high pool (spec 12.4). A standalone
 *    port's VID has member set {port, CPU} -- host traffic only, replacing the
 *    init table's all-ports flood. An unaware bridge shares one pool VID.
 *  - vlan-aware: the port carries the bridge's real 802.1Q VLANs; membership
 *    and PVID come from .port_vlan_add. Ingress VLAN filtering IS the members
 *    table (a frame from a non-member port on that VID is not forwarded), so
 *    there is no separate filter bit (spec 12.4). Egress 802.1Q tagging
 *    (tagged trunk) is unsupported on this SKU -- see prestera_port_vlan_add;
 *    only untagged/access VLANs are accepted.
 *
 * Real per-VID VLAN member/flood table (CPSS tableType 9), hardware write-validated
 * on stock 2026-09-30: raw base 0x03a00000, stride 0x20, DIRECT-WRITE (no trigger).
 * Found via cposd's runtime table-descriptor (funcBase 0x03a00000); proven by writing
 * it live -- zeroing a VID's member words stops that VID's forwarding, restoring resumes
 * it. Each entry is 6 words; per-port state is a 2-bit field: ports 0-11 in word1
 * (bits[2p:2p+1]), ports 12-15 in word0 (bits[24+2(p-12)]); value 0b01 = member(untagged),
 * bit1 = egress-tag (0b11 = member + 802.1Q-tagged). word0[1:0]=0x3 control; word2=0x400
 * carries the CPU/flood member; word3=0x000fff00 marks the entry valid (empty VIDs read
 * 0x000fff02). This REPLACES the old 0x14000024/0x14000064 table (tableType 0x98), which
 * RE + hardware proved inert for forwarding.
 * Native VID per port (PVID): reg 0x16001000 + port*0x10, VID in bits[26:15] (unchanged).
 * NOTE: the exact per-port bit index for the LAN jacks (vs the driver's silicon-port
 * number) is being confirmed during on-hardware validation; the formula below uses the
 * silicon-port convention (field at 2*port) and is the point to adjust if a jack maps to
 * a different bit.
 */
#define VLAN_ENTRY(vid)			(0x03a00000u + (u32)(vid) * 0x20u)
#define VLAN_ENTRY_W0_CTRL		0x00000003u	/* word0 low control bits */
#define VLAN_ENTRY_W2_CPU		0x00000400u	/* word2: CPU/flood member */
#define VLAN_ENTRY_W3_VALID		0x000fff00u	/* word3: entry valid */

#define PORT_VLAN_CFG(port)		(0x16001000u + (port) * 0x10u)
#define PORT_VLAN_VID			GENMASK(26, 15)	/* 12-bit native VID */

/*
 * Egress/global TPID table. Each 32-bit register packs two 16-bit TPID slots
 * ([31:16] and [15:0]); there is one copy in unit 0x1c (0x1c000430..43c) and
 * one in unit 0x16 (0x16000300..30c), four registers = eight slots each. The
 * vendor cold init stages 0x8100 in the high slot and then overwrites every
 * slot with 0x9876 (see prestera_98dx3234_init_tbl.h), so stock egresses its
 * 802.1Q tag with the non-standard TPID 0x9876. For a plain OpenWrt trunk we
 * want standard 0x8100, so after the init table runs we rewrite all slots to
 * 0x8100 (prestera_egress_tpid_init). Setting every slot avoids depending on
 * which slot the egress path selects -- a fact not yet pinned by RE.
 * NOTE: register-level change only; on-wire egress TPID not yet captured
 * (same open validation as VLAN_TAGGED_DATA, spec 12.4).
 */
#define TPID_TABLE_1C_BASE		0x1c000430u	/* unit 0x1c, stride 4, 4 regs */
#define TPID_TABLE_16_BASE		0x16000300u	/* unit 0x16, stride 4, 4 regs */
#define TPID_TABLE_REGS			4u
#define TPID_8021Q_BOTH_SLOTS		0x81008100u	/* both packed slots = 0x8100 */

/*
 * Internal VID pool (spec 12.4): reserved high VIDs for vlan-unaware use so
 * they never collide with user 802.1Q VLANs. Standalone port p -> ISOL_VID(p);
 * unaware bridge -> BRIDGE_VID(bridge.num). User VLANs in this range are
 * rejected. VID 4095 is the never-forwarding "drop" VID (members always 0).
 */
#define PRESTERA_N_VIDS			4096
#define PRESTERA_ISOL_VID(port)		((u16)(4064 + (port)))	 /* 4064..4079 */
#define PRESTERA_BRIDGE_VID(num)	((u16)(4080 + (num)))	 /* 4080..4094 */
#define PRESTERA_VID_POOL_LO		4064u
#define PRESTERA_VID_POOL_HI		4094u
#define PRESTERA_VID_DROP		4095u
#define PRESTERA_VID_MAX		0xFFFu

struct prestera {
	struct mii_bus *parent;		/* the al_eth bus carrying the tunnel */
	struct mii_bus *phy_bus;	/* SMI master exposed as an mii_bus */
	struct dsa_switch *ds;
	struct platform_device *pdev;
	struct mutex lock;		/* serialises multi-step SMI sequences */
	struct dentry *dbg;		/* debugfs: read-only register access */
	u32 dbg_addr;
	/* L2 forwarding shadow (the driver is the sole writer). */
	u32 *vlan_members;			/* [PRESTERA_N_VIDS]: bit p set per member */
	u32 *vlan_tagged;			/* [PRESTERA_N_VIDS]: bit p set => egress tagged */
	u16 isol_vid[PRESTERA_NUM_PORTS];	/* current internal VID when vlan-unaware */
	u16 bridge_vid[PRESTERA_NUM_PORTS];	/* unaware-bridge pool VID, 0 if standalone */
	u16 port_pvid[PRESTERA_NUM_PORTS];	/* user PVID when vlan-aware */
	bool vlan_aware[PRESTERA_NUM_PORTS];	/* bridge vlan_filtering is on */
	bool port_forwarding[PRESTERA_NUM_PORTS]; /* STP: is the port forwarding */
};


/*
 * One round of the CtrlMgmt write primitive (spec 3.3). Caller holds
 * the lock and has already formed the full address word.
 */
static int prestera_ctrlmgmt_write(struct prestera *p, u32 addr_word, u32 value)
{
	int ret, i;

	ret = mdiobus_write(p->parent, PRESTERA_SMI_ADDR, SMI_CMD_HI,
			     addr_word >> 16);
	if (ret)
		return ret;
	ret = mdiobus_write(p->parent, PRESTERA_SMI_ADDR, SMI_CMD_LO,
			     addr_word & 0xffff);
	if (ret)
		return ret;
	ret = mdiobus_write(p->parent, PRESTERA_SMI_ADDR, SMI_PAYLOAD_HI,
			     value >> 16);
	if (ret)
		return ret;
	ret = mdiobus_write(p->parent, PRESTERA_SMI_ADDR, SMI_PAYLOAD_LO,
			     value & 0xffff);
	if (ret)
		return ret;

	for (i = 0; i < SMI_POLL_TRIES; i++) {
		ret = mdiobus_read(p->parent, PRESTERA_SMI_ADDR, SMI_STATUS);
		if (ret < 0)
			return ret;
		if (ret & SMI_STATUS_WRITE_DONE)
			return 0;
		udelay(SMI_POLL_DELAY_US);
	}
	return -ETIMEDOUT;
}

/*
 * The top nibble of the target address is not carried in the address
 * word -- it travels in a separate page-select write, and the page word
 * must match the selector (spec 3.2). Mismatching them addresses a real
 * but different window.
 */
static int prestera_page_select(struct prestera *p, u32 addr, u32 sel)
{
	u32 page_word = (sel == SMI_SEL_RIC) ? SMI_PAGE_WORD_RIC
					     : SMI_PAGE_WORD_NORMAL;

	return prestera_ctrlmgmt_write(p, page_word, SMI_PAGE_VALUE(addr));
}

static int prestera_read_locked(struct prestera *p, u32 addr, u32 sel, u32 *val)
{
	u32 addr_word = (addr & SMI_ADDR_MASK) | sel;
	int ret, i, hi, lo;

	ret = prestera_page_select(p, addr, sel);
	if (ret)
		return ret;

	ret = mdiobus_write(p->parent, PRESTERA_SMI_ADDR, SMI_ADDR_HI,
			     addr_word >> 16);
	if (ret)
		return ret;
	ret = mdiobus_write(p->parent, PRESTERA_SMI_ADDR, SMI_ADDR_LO,
			     addr_word & 0xffff);
	if (ret)
		return ret;

	for (i = 0; i < SMI_POLL_TRIES; i++) {
		ret = mdiobus_read(p->parent, PRESTERA_SMI_ADDR, SMI_STATUS);
		if (ret < 0)
			return ret;
		if (ret & SMI_STATUS_READ_READY)
			break;
		udelay(SMI_POLL_DELAY_US);
	}
	if (i == SMI_POLL_TRIES)
		return -ETIMEDOUT;

	hi = mdiobus_read(p->parent, PRESTERA_SMI_ADDR, SMI_DATA_HI);
	if (hi < 0)
		return hi;
	lo = mdiobus_read(p->parent, PRESTERA_SMI_ADDR, SMI_DATA_LO);
	if (lo < 0)
		return lo;

	*val = ((u32)hi << 16) | (u32)lo;
	return 0;
}

static int prestera_write_locked(struct prestera *p, u32 addr, u32 sel, u32 val)
{
	int ret;

	ret = prestera_page_select(p, addr, sel);
	if (ret)
		return ret;

	return prestera_ctrlmgmt_write(p, (addr & SMI_ADDR_MASK) | sel, val);
}

static int prestera_reg_read(struct prestera *p, u32 addr, u32 sel, u32 *val)
{
	int ret;

	mutex_lock(&p->lock);
	ret = prestera_read_locked(p, addr, sel, val);
	mutex_unlock(&p->lock);
	return ret;
}

static int prestera_reg_write(struct prestera *p, u32 addr, u32 sel, u32 val)
{
	int ret;

	mutex_lock(&p->lock);
	ret = prestera_write_locked(p, addr, sel, val);
	mutex_unlock(&p->lock);
	return ret;
}

/* ---- SMI master: the LAN PHYs as a Linux mii_bus (spec 5) ---- */

static int prestera_phy_read(struct prestera *p, int phy, int reg)
{
	u32 cmd, val;
	int ret, i;

	if (phy > 31 || reg > 31)
		return -EINVAL;

	cmd = FIELD_PREP(SMI_MGMT_PHYAD, phy) |
	      FIELD_PREP(SMI_MGMT_REGAD, reg) | SMI_MGMT_OP_READ;

	mutex_lock(&p->lock);
	ret = prestera_write_locked(p, SMI_MGMT, SMI_SEL_NORMAL, cmd);
	if (ret)
		goto out;

	for (i = 0; i < SMI_POLL_TRIES; i++) {
		ret = prestera_read_locked(p, SMI_MGMT, SMI_SEL_NORMAL, &val);
		if (ret)
			goto out;
		if (val & SMI_MGMT_READ_VALID) {
			ret = val & SMI_MGMT_DATA;
			goto out;
		}
		udelay(SMI_POLL_DELAY_US);
	}
	ret = -ETIMEDOUT;
out:
	mutex_unlock(&p->lock);
	return ret;
}

static int prestera_phy_write(struct prestera *p, int phy, int reg, u16 val)
{
	u32 cmd;

	if (phy > 31 || reg > 31)
		return -EINVAL;

	cmd = FIELD_PREP(SMI_MGMT_PHYAD, phy) |
	      FIELD_PREP(SMI_MGMT_REGAD, reg) | val;

	return prestera_reg_write(p, SMI_MGMT, SMI_SEL_NORMAL, cmd);
}

/* mii_bus shims for the fallback bus in prestera_register_phy_bus(). */
static int prestera_mdio_read(struct mii_bus *bus, int phy, int reg)
{
	return prestera_phy_read(bus->priv, phy, reg);
}

static int prestera_mdio_write(struct mii_bus *bus, int phy, int reg, u16 val)
{
	return prestera_phy_write(bus->priv, phy, reg, val);
}

/* ---- bring-up ---- */

/*
 * GE-MAC port enable (spec 6.1). The vendor writes the 0x0C register
 * first, then the control register with bit0 set.
 */
static int prestera_smi_cfg_init(struct prestera *p)
{
	static const struct {
		u32 addr;
		u32 val;
	} cfg[] = {
		{ SMI_CFG0,  SMI_CFG0_VALUE },
		{ SMI_CFG1,  SMI_CFG1_VALUE },
		{ SMI_ALIGN, SMI_ALIGN_VALUE },
		{ SMI_CFG2,  SMI_CFG2_VALUE },
	};
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(cfg); i++) {
		ret = prestera_reg_write(p, cfg[i].addr, SMI_SEL_NORMAL,
					  cfg[i].val);
		if (ret)
			return ret;
	}
	return 0;
}

static int __maybe_unused prestera_mac_init(struct prestera *p)
{
	unsigned int port;
	int ret;

	for (port = 0; port < PRESTERA_LAN_PORTS; port++) {
		u32 base = GEMAC_BASE + port * GEMAC_STRIDE;

		ret = prestera_reg_write(p, base + GEMAC_REG0C,
					  SMI_SEL_NORMAL, GEMAC_REG0C_LAN);
		if (ret)
			return ret;
		ret = prestera_reg_write(p, base + GEMAC_CTRL,
					  SMI_SEL_NORMAL, GEMAC_CTRL_VALUE);
		if (ret)
			return ret;
	}

	ret = prestera_reg_write(p, GEMAC_BASE + PRESTERA_WAN_PORT * GEMAC_STRIDE +
				  GEMAC_REG0C, SMI_SEL_NORMAL, GEMAC_REG0C_WAN);
	if (ret)
		return ret;

	return prestera_reg_write(p, GEMAC_BASE + PRESTERA_WAN_PORT * GEMAC_STRIDE +
				   GEMAC_CTRL, SMI_SEL_NORMAL, GEMAC_CTRL_VALUE);
}

/*
 * 88E1685 bring-up (spec 7.4), replayed verbatim from the vendor's cold
 * boot. The PHYs come out of reset with BMCR bit11 (power down) set, and
 * the configuration that actually enables copper lives on Marvell vendor
 * pages 0xFD/0xFF/0x12 -- page 0 alone is not sufficient. reg22 is the
 * page-select register; the sequence returns to page 0 before finishing.
 */
static const struct {
	u8 reg;
	u16 val;
} prestera_phy_initseq[] = {
	{ 22, 0x00FD }, {  8, 0x0B53 }, {  7, 0x200D }, { 22, 0x0000 },
	{ 22, 0x0004 }, { 27, 0x3FA0 }, { 22, 0x0000 },
	{ 22, 0x0012 }, { 27, 0x0000 }, { 22, 0x0000 },
	{ 22, 0x00FD }, {  8, 0x0B53 }, {  7, 0x200D }, { 22, 0x0000 },
	{ 22, 0x00FF }, { 17, 0xB030 }, { 16, 0x215C },
	{ 22, 0x0003 }, { 16, 0x1117 }, { 22, 0x0000 },
	{ 16, 0x3360 },
	{  0, 0x9140 }, {  0, 0x1140 }, {  0, 0x9140 },
};

static int __maybe_unused prestera_phy_init(struct prestera *p)
{
	unsigned int phy, i;
	int ret;

	for (phy = 0; phy < PRESTERA_LAN_PORTS; phy++) {
		for (i = 0; i < ARRAY_SIZE(prestera_phy_initseq); i++) {
			ret = prestera_phy_write(p, phy,
						  prestera_phy_initseq[i].reg,
						  prestera_phy_initseq[i].val);
			if (ret) {
				dev_err(&p->pdev->dev,
					"phy%u init step %u failed: %d\n",
					phy, i, ret);
				return ret;
			}
		}
	}
	return 0;
}

/* ---- vendor initialisation sequence (spec 9, 10.3) ---- */

/*
 * Replay the captured vendor init. See prestera_98dx3234_init_tbl.h for
 * why the reads are included.
 *
 * The switch does not forward a single frame without this, which was
 * established by measurement: with the CPU-port MAC configured correctly
 * but this sequence skipped, rx_packets stays at 0.
 */
static int prestera_run_init_table(struct prestera *p,
				   const struct prestera_init_op *tbl,
				   size_t n, u32 sel, const char *what)
{
	size_t i;
	int ret;

	for (i = 0; i < n; i++) {
		u32 addr = PRESTERA_OP_ADDR(tbl[i].addr);

		if (PRESTERA_OP_IS_RD(tbl[i].addr)) {
			u32 dummy;

			ret = prestera_reg_read(p, addr, sel, &dummy);
		} else {
			ret = prestera_reg_write(p, addr, sel, tbl[i].val);
		}

		if (ret) {
			dev_err(&p->pdev->dev,
				"%s init failed at op %zu (0x%08x): %d\n",
				what, i, addr, ret);
			return ret;
		}
	}
	return 0;
}

/*
 * Point the egress/global TPID table at the standard 802.1Q ethertype.
 * The vendor init leaves every slot at 0x9876; overwrite all of them with
 * 0x8100 so trunk ports egress normal 802.1Q tags. The regs live under the
 * NORMAL selector (the block is in prestera_init_normal[]). Idempotent.
 */
static int prestera_egress_tpid_init(struct prestera *p)
{
	u32 base[] = { TPID_TABLE_1C_BASE, TPID_TABLE_16_BASE };
	unsigned int b, i;
	int ret;

	for (b = 0; b < ARRAY_SIZE(base); b++) {
		for (i = 0; i < TPID_TABLE_REGS; i++) {
			ret = prestera_reg_write(p, base[b] + i * 4u,
						 SMI_SEL_NORMAL,
						 TPID_8021Q_BOTH_SLOTS);
			if (ret) {
				dev_err(&p->pdev->dev,
					"TPID init failed at 0x%08x: %d\n",
					base[b] + i * 4u, ret);
				return ret;
			}
		}
	}
	return 0;
}

static int prestera_fabric_init(struct prestera *p)
{
	int ret;

	/*
	 * Replayed unconditionally. The table is idempotent (see its
	 * header), so there is no reason to branch on whether a previous
	 * boot already ran it -- and running it always means the fabric's
	 * configuration comes from this driver rather than from whatever
	 * state a warm reboot happened to leave behind.
	 */
	ret = prestera_run_init_table(p, prestera_init_normal,
				      ARRAY_SIZE(prestera_init_normal),
				      SMI_SEL_NORMAL, "normal");
	if (ret)
		return ret;

	ret = prestera_run_init_table(p, prestera_init_ric,
				      ARRAY_SIZE(prestera_init_ric),
				      SMI_SEL_RIC, "ric");
	if (ret)
		return ret;

	return prestera_egress_tpid_init(p);
}

/* ---- L2 forwarding: VLAN members table + per-port PVID ---- */

/*
 * Write one VID's member set through the indirect table window (spec 9.2).
 * The two register writes and the busy poll must not interleave with another
 * indirect-window access, so the whole sequence holds the lock. bit p in
 * @members means silicon port p is a member.
 */
/*
 * Program one VID's entry in the real member/flood table (VLAN_ENTRY(vid), a
 * direct-write RAM table, no trigger). @members / @tagged are per-silicon-port
 * bitmaps (bit p == silicon port p). Builds the 2-bit-per-port fields, sets the
 * CPU/flood word and the valid word, and writes the four words under the lock.
 * An all-zero member set leaves the entry with no ports -> the VID does not
 * forward (verified: this is how a VID is effectively "off").
 */
/*
 * DSA/netdev port (lanN = port N-1) -> fabric port for the member table. The 16
 * RJ45 jacks are wired to the switch in QSGMII 4-port lanes, reordered vs the
 * sequential DSA numbering: block[(port)/4] + (port)%4, block = {12,0,4,8}.
 * Derived from on-wire enumeration (LAN1->12, LAN8->3, LAN11->6) + the cold-init
 * (fabric ports 0-15 are the GE jacks) + the bijection. The member table
 * (VLAN_ENTRY) is fabric-port-indexed; PVID (PORT_VLAN_CFG) stays DSA/MAC-indexed.
 */
static const u8 prestera_dsa_to_fabric[PRESTERA_LAN_PORTS] = {
	12, 13, 14, 15,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11,
};

static int prestera_vlan_members_write(struct prestera *p, u16 vid, u32 members,
				       u32 tagged)
{
	u32 base = VLAN_ENTRY(vid);
	u32 w0 = VLAN_ENTRY_W0_CTRL, w1 = 0, w2 = 0;
	int ret, port;

	for (port = 0; port < PRESTERA_LAN_PORTS; port++) {
		u32 f, fab;

		if (!(members & BIT(port)))
			continue;
		f = (tagged & BIT(port)) ? 0x3u : 0x1u;	/* bit0 member, bit1 tag */
		fab = prestera_dsa_to_fabric[port];
		if (fab < 12)
			w1 |= f << (2 * fab);
		else
			w0 |= f << (24 + 2 * (fab - 12));
	}
	if (members & BIT(PRESTERA_CPU_PORT))
		w2 = VLAN_ENTRY_W2_CPU;

	mutex_lock(&p->lock);
	ret = prestera_write_locked(p, base + 0x0, SMI_SEL_NORMAL, w0);
	if (!ret)
		ret = prestera_write_locked(p, base + 0x4, SMI_SEL_NORMAL, w1);
	if (!ret)
		ret = prestera_write_locked(p, base + 0x8, SMI_SEL_NORMAL, w2);
	if (!ret)
		ret = prestera_write_locked(p, base + 0xc, SMI_SEL_NORMAL,
					    VLAN_ENTRY_W3_VALID);
	mutex_unlock(&p->lock);
	return ret;
}

/*
 * Set a user port's native VID (PVID), which lives in bits[26:15] of word 0 of
 * the port's VLAN config entry (0x16001000 + port*0x10).
 *
 * The entry is three words (+0/+4/+8) and the hardware latches it only when the
 * LAST word is written -- verified on hardware: writing word 0 alone never
 * commits (the readback stays at the old value), while writing all three words
 * in ascending order commits. So this reads the whole entry, edits the VID in
 * word 0, and writes words 0,1,2 in order, preserving words 1 and 2 (word 1
 * holds live per-port state, not zero). The +8 write is what commits.
 */
static int prestera_port_set_pvid(struct prestera *p, int port, u16 vid)
{
	u32 base = PORT_VLAN_CFG(port), w[3];
	int ret, i;

	mutex_lock(&p->lock);
	for (i = 0; i < 3; i++) {
		ret = prestera_read_locked(p, base + i * 4, SMI_SEL_NORMAL, &w[i]);
		if (ret)
			goto out;
	}
	w[0] = (w[0] & ~(u32)PORT_VLAN_VID) | FIELD_PREP(PORT_VLAN_VID, vid);
	for (i = 0; i < 3; i++) {
		ret = prestera_write_locked(p, base + i * 4, SMI_SEL_NORMAL, w[i]);
		if (ret)
			goto out;
	}
out:
	mutex_unlock(&p->lock);
	return ret;
}

/*
 * The hardware member word for a VID, derived from the shadow: drop the bits of
 * ports STP has blocked, and add the CPU port whenever the VID has any member
 * (so the bridge can always reach it and BPDUs flow). An empty VID writes 0 --
 * it forwards nowhere.
 */
static u32 prestera_hw_members(struct prestera *p, u16 vid)
{
	bool user_vid = (vid < PRESTERA_VID_POOL_LO);
	u32 m = 0;
	int port;

	for (port = 0; port < PRESTERA_LAN_PORTS; port++) {
		if (!(p->vlan_members[vid] & BIT(port)))
			continue;
		if (!p->port_forwarding[port])	/* STP-blocked: no egress */
			continue;
		/*
		 * The shadow can hold a port in both an internal pool VID and its
		 * user VLANs across a vlan_filtering flip; only one is live. A user
		 * VID is live on vlan-aware ports, a pool VID on unaware ports.
		 */
		if (user_vid != p->vlan_aware[port])
			continue;
		m |= BIT(port);
	}
	return m ? (m | BIT(PRESTERA_CPU_PORT)) : 0;
}

/*
 * The egress-tag bitmap for a VID: the live member ports (same liveness rules as
 * prestera_hw_members) that this VID marks tagged. The CPU port is never tagged --
 * host frames ride the DSA path untagged. An empty result leaves egress untagged.
 */
static u32 prestera_hw_tagged(struct prestera *p, u16 vid)
{
	bool user_vid = (vid < PRESTERA_VID_POOL_LO);
	u32 t = 0;
	int port;

	for (port = 0; port < PRESTERA_LAN_PORTS; port++) {
		if (!(p->vlan_tagged[vid] & BIT(port)))
			continue;
		if (!(p->vlan_members[vid] & BIT(port)))
			continue;
		if (!p->port_forwarding[port])
			continue;
		if (user_vid != p->vlan_aware[port])
			continue;
		t |= BIT(port);
	}
	return t;
}

/* Push one VID's shadow membership + egress-tag state to the fabric. */
static int prestera_vlan_apply(struct prestera *p, u16 vid)
{
	return prestera_vlan_members_write(p, vid, prestera_hw_members(p, vid),
					   prestera_hw_tagged(p, vid));
}

/* Add or remove a port in a VID's shadow, then re-push that VID. */
static int prestera_vlan_set_member(struct prestera *p, u16 vid, int port,
				    bool member)
{
	if (member)
		p->vlan_members[vid] |= BIT(port);
	else
		p->vlan_members[vid] &= ~BIT(port);
	return prestera_vlan_apply(p, vid);
}

/*
 * A port's effective PVID: its user PVID when vlan-aware (the drop VID until one
 * is set), otherwise its internal isolation/bridge VID.
 */
static u16 prestera_port_eff_pvid(struct prestera *p, int port)
{
	if (p->vlan_aware[port])
		return p->port_pvid[port] ? p->port_pvid[port]
					  : PRESTERA_VID_DROP;
	return p->isol_vid[port];
}

static int prestera_port_apply_pvid(struct prestera *p, int port)
{
	return prestera_port_set_pvid(p, port, prestera_port_eff_pvid(p, port));
}

/* Re-push every VID this port is a member of (used on a mode/STP change). */
static int prestera_vlan_reapply_port(struct prestera *p, int port)
{
	int vid, ret;

	for (vid = 1; vid < PRESTERA_N_VIDS; vid++) {
		if (!(p->vlan_members[vid] & BIT(port)))
			continue;
		ret = prestera_vlan_apply(p, vid);
		if (ret)
			return ret;
	}
	return 0;
}

/* The internal VID a port uses when vlan-unaware: its bridge's pool VID if
 * bridged, else its own standalone isolation VID.
 */
static u16 prestera_unaware_vid(struct prestera *p, int port)
{
	return p->bridge_vid[port] ? p->bridge_vid[port]
				   : PRESTERA_ISOL_VID(port);
}

/*
 * Drop a port from every user VLAN and return it to vlan-unaware forwarding on
 * its current internal VID (standalone or bridge). Used on bridge-leave and
 * when vlan_filtering is turned off. Membership in internal pool VIDs is left
 * in the shadow; the mode gate in prestera_hw_members() makes it live.
 */
static int prestera_port_to_unaware(struct prestera *p, int port)
{
	u16 uvid = prestera_unaware_vid(p, port);
	int vid, ret;

	for (vid = 1; vid < PRESTERA_VID_POOL_LO; vid++) {
		if (!(p->vlan_members[vid] & BIT(port)))
			continue;
		p->vlan_members[vid] &= ~BIT(port);
		ret = prestera_vlan_apply(p, vid);
		if (ret)
			return ret;
	}

	p->vlan_aware[port] = false;
	p->port_pvid[port] = 0;
	p->isol_vid[port] = uvid;
	p->vlan_members[uvid] |= BIT(port);
	ret = prestera_vlan_apply(p, uvid);
	if (ret)
		return ret;
	return prestera_port_apply_pvid(p, port);
}

/* ---- DSA ---- */

/*
 * Frames between the fabric and the CPU port carry a 4-byte Marvell DSA
 * tag at offset 12 -- the format the in-tree tagger already implements,
 * so there is nothing bespoke to write here. Measured on ingress:
 * cmd=3 (FORWARD), src_dev=16, src_port = the front-panel port. Egress
 * needs cmd=1 (FROM_CPU) with the same src_dev; dev=0 is discarded by
 * the fabric. See spec 10.2.
 */
static enum dsa_tag_protocol prestera_get_tag_protocol(struct dsa_switch *ds,
						       int port,
						       enum dsa_tag_protocol m)
{
	return DSA_TAG_PROTO_DSA;
}

static int prestera_dsa_setup(struct dsa_switch *ds)
{
	struct prestera *p = ds->priv;
	int port, ret;

	/*
	 * The fabric is already initialised by prestera_fabric_init() at
	 * module init, before phylib touches the PHYs. Re-running it here
	 * would be actively harmful (see the init table header).
	 *
	 * What setup must do is undo the init table's default of flooding
	 * between all front-panel ports (spec 12.1), which is a broadcast
	 * storm waiting for any two ports on the same L2 network. Start every
	 * user port isolated: its own reserved internal VID, members
	 * {port, CPU}. Bridging and 802.1Q open forwarding from this baseline.
	 */
	for (port = 0; port < PRESTERA_LAN_PORTS; port++) {
		p->bridge_vid[port] = 0;
		p->port_forwarding[port] = true;
		ret = prestera_port_to_unaware(p, port);
		if (ret) {
			dev_err(ds->dev,
				"prestera: port %d isolation setup failed: %d\n",
				port, ret);
			return ret;
		}
	}

	ds->mtu_enforcement_ingress = false;
	dev_info(ds->dev,
		 "prestera: DSA switch registered, %d user ports isolated, cpu port %d\n",
		 PRESTERA_LAN_PORTS, PRESTERA_CPU_PORT);
	return 0;
}

/*
 * The LAN ports each drive an 88E1685 over the fabric's internal GMII; the
 * PHY does the copper side and reports 10/100 half+full and 1000baseT
 * half+full (support mask 0x62ff, read live over the SMI tunnel).
 *
 * The CPU port has no PHY at all: it is a fixed internal SERDES that must be
 * 1G SGMII full duplex with link management off, which is the pairing that
 * makes traffic flow (spec 10.1). The devicetree gives it a fixed-link, so
 * phylink never negotiates -- but the capability still has to be declared
 * or validation rejects the fixed speed too.
 */
static void prestera_phylink_get_caps(struct dsa_switch *ds, int port,
				      struct phylink_config *config)
{
	if (port == PRESTERA_CPU_PORT) {
		__set_bit(PHY_INTERFACE_MODE_SGMII,
			  config->supported_interfaces);
		config->mac_capabilities = MAC_1000FD;
		return;
	}

	__set_bit(PHY_INTERFACE_MODE_GMII, config->supported_interfaces);
	__set_bit(PHY_INTERFACE_MODE_INTERNAL, config->supported_interfaces);
	config->mac_capabilities = MAC_SYM_PAUSE | MAC_ASYM_PAUSE |
				   MAC_10 | MAC_100 | MAC_1000;
}

static int prestera_dsa_phy_read(struct dsa_switch *ds, int port, int reg)
{
	struct prestera *p = ds->priv;

	if (port < 0 || port >= PRESTERA_LAN_PORTS)
		return 0xffff;
	return prestera_phy_read(p, port, reg);
}

static int prestera_dsa_phy_write(struct dsa_switch *ds, int port, int reg,
				  u16 val)
{
	struct prestera *p = ds->priv;

	if (port < 0 || port >= PRESTERA_LAN_PORTS)
		return -ENODEV;
	return prestera_phy_write(p, port, reg, val);
}

/*
 * Bridging. .setup leaves every port isolated on its own internal VID; these
 * ops open and close forwarding as the bridge changes.
 *
 * A vlan-unaware bridge shares one internal pool VID among its ports. A
 * vlan-aware bridge instead carries real 802.1Q VLANs (see the port_vlan ops);
 * .port_vlan_filtering flips a port between the two. .port_stp_state_set drops
 * a blocked port out of egress on every VID it is in, so it neither floods to
 * peers nor hears from them, while its own ingress still reaches the CPU.
 */
static int prestera_port_bridge_join(struct dsa_switch *ds, int port,
				     struct dsa_bridge bridge,
				     bool *tx_fwd_offload,
				     struct netlink_ext_ack *extack)
{
	struct prestera *p = ds->priv;
	u16 bvid = PRESTERA_BRIDGE_VID(bridge.num);
	int ret;

	if (port < 0 || port >= PRESTERA_LAN_PORTS)
		return -EINVAL;
	if (bvid < PRESTERA_VID_POOL_LO || bvid > PRESTERA_VID_POOL_HI) {
		NL_SET_ERR_MSG_MOD(extack,
				   "too many bridges for the internal VID pool");
		return -EOPNOTSUPP;
	}

	/* Move from the standalone isolation VID onto the bridge's shared VID. */
	ret = prestera_vlan_set_member(p, PRESTERA_ISOL_VID(port), port, false);
	if (ret)
		return ret;
	p->bridge_vid[port] = bvid;
	p->isol_vid[port] = bvid;
	ret = prestera_vlan_set_member(p, bvid, port, true);
	if (ret)
		return ret;
	return prestera_port_apply_pvid(p, port);
}

static void prestera_port_bridge_leave(struct dsa_switch *ds, int port,
				       struct dsa_bridge bridge)
{
	struct prestera *p = ds->priv;

	if (port < 0 || port >= PRESTERA_LAN_PORTS)
		return;

	/* Drop the bridge's shared VID, return to standalone isolation. */
	prestera_vlan_set_member(p, p->bridge_vid[port], port, false);
	p->bridge_vid[port] = 0;
	p->port_forwarding[port] = true;
	if (prestera_port_to_unaware(p, port))
		dev_err(ds->dev, "prestera: re-isolating port %d failed\n",
			port);
}

static void prestera_port_stp_state_set(struct dsa_switch *ds, int port,
					u8 state)
{
	struct prestera *p = ds->priv;
	bool forwarding = (state == BR_STATE_FORWARDING);

	if (port < 0 || port >= PRESTERA_LAN_PORTS)
		return;
	if (p->port_forwarding[port] == forwarding)
		return;

	p->port_forwarding[port] = forwarding;
	if (prestera_vlan_reapply_port(p, port))
		dev_err(ds->dev, "prestera: stp state on port %d failed\n",
			port);
}

/*
 * 802.1Q offload. Ingress VLAN filtering on this silicon IS the members table
 * (a frame from a non-member port on its VID is not forwarded), so
 * .port_vlan_filtering has no dedicated register -- it only flips which VID
 * scheme the port runs under.
 *
 * Tagged/trunk VLANs are REJECTED on user ports (prestera_port_vlan_add returns
 * -EOPNOTSUPP): egress 802.1Q tagging is unsupported on this 98DX3234 SKU.
 * Confirmed by reverse-engineering the stock CPSS firmware plus on-wire tests
 * (2026-10): the per-port egress tag-state table (CPSS tableType 0x4e) and its
 * EVLAN/EPORT mode both sit beyond the device's table count (numTables == 0x25,
 * so CPSS returns GT_BAD_PARAM), the generic egress-tag fabric unit (0x3b) is
 * entirely unmapped on this silicon, and the eVLAN member per-port tag field
 * does NOT drive egress tagging (a bounded per-port register sweep found no
 * control; stock firmware tags only CPU-routed traffic via the DSA tag, never
 * autonomous bridged transit). Accepting a tagged member would silently egress
 * untagged, so we reject it instead. See the RE write-up for the full chase.
 *
 * STOCK-VALIDATED (byte-for-byte, 2026-09-30): the member-table encoding below
 * matches stock VID1 exactly (w0=0x55000003 all-ports + ctrl, w1=0x00555555,
 * w2=0x00000400 CPU, w3=0x000fff00 valid, w4-7=0); and the TPID table at
 * 0x16000300 reads 0x98769876 on stock (CP's non-standard 0x9876), which
 * prestera_egress_tpid_init normalizes to 0x8100.
 */
static int prestera_port_vlan_filtering(struct dsa_switch *ds, int port,
					bool vlan_filtering,
					struct netlink_ext_ack *extack)
{
	struct prestera *p = ds->priv;

	if (port < 0 || port >= PRESTERA_NUM_PORTS)
		return -EINVAL;
	/* CPU/DSA ports carry all VLANs implicitly (prestera_hw_members always
	 * includes the CPU port); there is no per-port filtering state for them.
	 */
	if (port >= PRESTERA_LAN_PORTS)
		return 0;
	if (p->vlan_aware[port] == vlan_filtering)
		return 0;

	if (vlan_filtering) {
		/* Leave the internal pool VID; user VLANs arrive via vlan_add.
		 * Until a PVID VLAN is added the port parks on the drop VID.
		 */
		p->vlan_aware[port] = true;
		p->port_pvid[port] = 0;
		if (prestera_vlan_reapply_port(p, port))
			return -EIO;
		return prestera_port_apply_pvid(p, port);
	}

	return prestera_port_to_unaware(p, port);
}

static int prestera_port_vlan_add(struct dsa_switch *ds, int port,
				  const struct switchdev_obj_port_vlan *vlan,
				  struct netlink_ext_ack *extack)
{
	struct prestera *p = ds->priv;
	u16 vid = vlan->vid;
	int ret;

	if (port < 0 || port >= PRESTERA_NUM_PORTS)
		return -EINVAL;
	/* Host/CPU-port VLAN adds: the CPU is already a member of every VID
	 * (prestera_hw_members), so accept and no-op -- returning an error here
	 * would abort the whole bridge join (dsa_port_host_vlan_add).
	 */
	if (port >= PRESTERA_LAN_PORTS)
		return 0;
	if (!vid)			/* priority-tagged: nothing to program */
		return 0;
	if (vid >= PRESTERA_VID_POOL_LO) {
		NL_SET_ERR_MSG_MOD(extack,
				   "VID collides with the driver's reserved range");
		return -EOPNOTSUPP;
	}
	/*
	 * Egress 802.1Q tagging is NOT supported on this 98DX3234 SKU, confirmed by
	 * reverse-engineering the stock CPSS firmware plus on-wire tests: the per-port
	 * egress tag-state table (CPSS tableType 0x4e) and its EVLAN/EPORT mode sit
	 * beyond the device's table count (numTables == 0x25), the generic egress-tag
	 * fabric unit (0x3b) is unmapped on this silicon, and the eVLAN member per-port
	 * tag field does not drive egress tagging (stock tags only CPU-routed traffic
	 * via the DSA tag, never autonomous transit). So reject tagged (trunk)
	 * membership on user ports instead of accepting it and silently egressing
	 * untagged. Untagged/access VLANs and PVID-based separation work fully.
	 */
	if (!(vlan->flags & BRIDGE_VLAN_INFO_UNTAGGED)) {
		NL_SET_ERR_MSG_MOD(extack,
				   "802.1Q egress tagging is unsupported on this switch; use untagged/access VLANs");
		return -EOPNOTSUPP;
	}
	p->vlan_tagged[vid] &= ~BIT(port);

	ret = prestera_vlan_set_member(p, vid, port, true);
	if (ret)
		return ret;
	if (vlan->flags & BRIDGE_VLAN_INFO_PVID) {
		p->port_pvid[port] = vid;
		ret = prestera_port_apply_pvid(p, port);
	}
	return ret;
}

static int prestera_port_vlan_del(struct dsa_switch *ds, int port,
				  const struct switchdev_obj_port_vlan *vlan)
{
	struct prestera *p = ds->priv;
	u16 vid = vlan->vid;
	int ret;

	if (port < 0 || port >= PRESTERA_NUM_PORTS)
		return -EINVAL;
	if (port >= PRESTERA_LAN_PORTS)	/* CPU/DSA port: implicit member, no-op */
		return 0;
	if (!vid || vid >= PRESTERA_VID_POOL_LO)
		return 0;

	p->vlan_tagged[vid] &= ~BIT(port);	/* clear tag before re-pushing entry */
	ret = prestera_vlan_set_member(p, vid, port, false);
	if (ret)
		return ret;
	if (p->port_pvid[port] == vid) {
		p->port_pvid[port] = 0;		/* -> drop VID */
		ret = prestera_port_apply_pvid(p, port);
	}
	return ret;
}

static const struct dsa_switch_ops prestera_dsa_ops = {
	.get_tag_protocol	= prestera_get_tag_protocol,
	.setup			= prestera_dsa_setup,
	.phylink_get_caps	= prestera_phylink_get_caps,
	.phy_read		= prestera_dsa_phy_read,
	.phy_write		= prestera_dsa_phy_write,
	.port_vlan_filtering	= prestera_port_vlan_filtering,
	.port_vlan_add		= prestera_port_vlan_add,
	.port_vlan_del		= prestera_port_vlan_del,
	.port_bridge_join	= prestera_port_bridge_join,
	.port_bridge_leave	= prestera_port_bridge_leave,
	.port_stp_state_set	= prestera_port_stp_state_set,
};

/* ---- discovery ---- */

/*
 * al_eth numbers its MDIO buses at runtime, so the bus carrying the
 * tunnel cannot be hardcoded: look for the one where address 31
 * answers with the Marvell OUI.
 */
static struct mii_bus *prestera_find_parent(void)
{
	struct mii_bus *bus;
	char id[MII_BUS_ID_SIZE];
	int i, val;

	for (i = 0; i < 32; i++) {
		snprintf(id, sizeof(id), "%d", i);
		bus = mdio_find_bus(id);
		if (!bus)
			continue;

		val = mdiobus_read(bus, PRESTERA_SMI_ADDR, PRESTERA_ID_REG);
		if (val == PRESTERA_ID_MARVELL)
			return bus;

		put_device(&bus->dev);
	}
	return NULL;
}

/*
 * A bus of our own over the same SMI tunnel, exposing the sixteen LAN
 * 88E1685s. Only used when DSA does not come up: DSA builds an equivalent
 * bus from the phy_read/phy_write ops, and two phylib instances page-selecting
 * the same PHY register file (spec 7.2) would fight.
 */
static int prestera_register_phy_bus(struct prestera *p)
{
	int ret;

	p->phy_bus = mdiobus_alloc();
	if (!p->phy_bus)
		return -ENOMEM;

	p->phy_bus->name = "prestera-98dx3234-smi";
	p->phy_bus->read = prestera_mdio_read;
	p->phy_bus->write = prestera_mdio_write;
	p->phy_bus->priv = p;
	p->phy_bus->parent = &p->pdev->dev;
	p->phy_bus->phy_mask = ~GENMASK(PRESTERA_LAN_PORTS - 1, 0);
	snprintf(p->phy_bus->id, MII_BUS_ID_SIZE, "prestera-smi");

	ret = mdiobus_register(p->phy_bus);
	if (ret) {
		dev_err(&p->pdev->dev, "mdiobus_register failed: %d\n", ret);
		mdiobus_free(p->phy_bus);
		p->phy_bus = NULL;
		return ret;
	}

	dev_info(&p->pdev->dev, "%u LAN phys on '%s'\n",
		 PRESTERA_LAN_PORTS, p->phy_bus->id);
	return 0;
}

/*
 * Register access for bring-up: write an address to reg_addr, then read or
 * write reg_value. Goes through the driver's own lock, so it cannot interleave
 * with DSA's PHY polling on the SMI tunnel. An unimplemented address reads
 * 0x000BADAD (spec 3.6).
 *
 * The write path is a bring-up diagnostic:
 * it drives a raw SMI write to any address, which is how the indirect-window
 * table protocol and the per-port config writes get validated on hardware.
 * It can obviously wedge the fabric if misused -- it exists only to bring the
 * driver up, not for production.
 */
static int prestera_dbg_value_get(void *data, u64 *val)
{
	struct prestera *p = data;
	u32 v;
	int ret;

	ret = prestera_reg_read(p, p->dbg_addr, SMI_SEL_NORMAL, &v);
	if (ret)
		return ret;
	*val = v;
	return 0;
}

static int prestera_dbg_value_set(void *data, u64 val)
{
	struct prestera *p = data;

	return prestera_reg_write(p, p->dbg_addr, SMI_SEL_NORMAL, (u32)val);
}
DEFINE_DEBUGFS_ATTRIBUTE(prestera_dbg_value_fops, prestera_dbg_value_get,
			 prestera_dbg_value_set, "0x%08llx\n");

static void prestera_debugfs_init(struct prestera *p)
{
	p->dbg = debugfs_create_dir("prestera-98dx3234", NULL);
	debugfs_create_x32("reg_addr", 0600, p->dbg, &p->dbg_addr);
	debugfs_create_file_unsafe("reg_value", 0600, p->dbg, p,
				   &prestera_dbg_value_fops);
}

static int prestera_probe(struct platform_device *pdev)
{
	struct prestera *p;
	u32 probe;
	int ret;

	p = kzalloc(sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;
	mutex_init(&p->lock);
	p->pdev = pdev;

	p->vlan_members = kcalloc(PRESTERA_N_VIDS, sizeof(*p->vlan_members),
				  GFP_KERNEL);
	if (!p->vlan_members) {
		kfree(p);
		return -ENOMEM;
	}
	p->vlan_tagged = kcalloc(PRESTERA_N_VIDS, sizeof(*p->vlan_tagged),
				 GFP_KERNEL);
	if (!p->vlan_tagged) {
		kfree(p->vlan_members);
		kfree(p);
		return -ENOMEM;
	}

	/*
	 * al_eth creates the MDIO bus carrying the tunnel, and there is no
	 * devicetree link from here to it, so defer until it exists rather
	 * than failing a probe that would succeed a moment later.
	 */
	p->parent = prestera_find_parent();
	if (!p->parent) {
		kfree(p->vlan_tagged);
		kfree(p->vlan_members);
		kfree(p);
		return dev_err_probe(&pdev->dev, -EPROBE_DEFER,
				     "no switch answering at addr %d on any mdio bus\n",
				     PRESTERA_SMI_ADDR);
	}

	/*
	 * Run the full vendor initialisation first. It supersedes the
	 * hand-derived prestera_mac_init()/prestera_phy_init() steps --
	 * those are reverse-engineered subsets of this same sequence, so
	 * running them adds nothing and would only re-apply a less complete
	 * version of it. They are kept in the file as documentation of what
	 * the fabric sequence does for the MACs and the 88E1685s.
	 *
	 * This also runs again on a deferred probe, which is fine: the
	 * table is idempotent (see its header).
	 */
	ret = prestera_fabric_init(p);
	if (ret)
		goto err_put;

	/*
	 * Re-assert only the SMI-master config, so PHY reads are
	 * well-defined regardless of where the vendor sequence left it.
	 * These are plain config registers, safe to write again.
	 */
	ret = prestera_smi_cfg_init(p);
	if (ret) {
		dev_err(&pdev->dev, "SMI config failed: %d\n", ret);
		goto err_put;
	}

	if (prestera_reg_read(p, GEMAC_BASE, SMI_SEL_NORMAL, &probe) == 0)
		dev_info(&pdev->dev, "fabric up, port0 MAC ctrl 0x%04x\n",
			 probe & 0xffff);

	/*
	 * Register as a DSA switch. The tagger is the in-tree Marvell DSA
	 * one (spec 10.2), so the CPU port's frames are demuxed to
	 * per-port netdevs without any bespoke tagging code here. DSA
	 * builds its own mii_bus for the user ports on top of our
	 * phy_read/phy_write ops.
	 */
	p->ds = kzalloc(sizeof(*p->ds), GFP_KERNEL);
	if (!p->ds) {
		ret = -ENOMEM;
		goto err_put;
	}
	p->ds->dev = &pdev->dev;
	p->ds->num_ports = PRESTERA_NUM_PORTS;
	p->ds->ops = &prestera_dsa_ops;
	p->ds->priv = p;

	ret = dsa_register_switch(p->ds);
	if (ret == -EPROBE_DEFER) {
		kfree(p->ds);
		put_device(&p->parent->dev);
		kfree(p->vlan_tagged);
		kfree(p->vlan_members);
		kfree(p);
		return ret;
	}
	if (ret) {
		/*
		 * Without a usable ports description this fails with -ENODEV.
		 * Fall back to exposing just the LAN PHYs on an mii_bus of our
		 * own: not a switch, but enough to see and drive link, and it
		 * keeps a broken devicetree from leaving nothing at all.
		 */
		dev_info(&pdev->dev,
			 "DSA not registered (%d); falling back to a bare mii_bus\n",
			 ret);
		kfree(p->ds);
		p->ds = NULL;

		ret = prestera_register_phy_bus(p);
		if (ret)
			goto err_put;
	}

	platform_set_drvdata(pdev, p);
	prestera_debugfs_init(p);
	return 0;

err_put:
	put_device(&p->parent->dev);
	kfree(p->vlan_tagged);
	kfree(p->vlan_members);
	kfree(p);
	return ret;
}

static void prestera_remove(struct platform_device *pdev)
{
	struct prestera *p = platform_get_drvdata(pdev);

	if (!p)
		return;

	debugfs_remove_recursive(p->dbg);

	if (p->ds) {
		dsa_unregister_switch(p->ds);
		kfree(p->ds);
		p->ds = NULL;
	}

	if (p->phy_bus) {
		mdiobus_unregister(p->phy_bus);
		mdiobus_free(p->phy_bus);
		p->phy_bus = NULL;
	}

	put_device(&p->parent->dev);
	kfree(p->vlan_tagged);
	kfree(p->vlan_members);
	kfree(p);
}

static const struct of_device_id prestera_of_match[] = {
	{ .compatible = "checkpoint,l72w-prestera-98dx3234" },
	{ }
};
MODULE_DEVICE_TABLE(of, prestera_of_match);

static struct platform_driver prestera_driver = {
	.probe	= prestera_probe,
	.remove	= prestera_remove,
	.driver	= {
		.name		= "prestera-98dx3234",
		.of_match_table	= prestera_of_match,
	},
};
module_platform_driver(prestera_driver);

MODULE_DESCRIPTION("Marvell Prestera 98DX3234 DSA switch (Check Point L-72W)");
MODULE_LICENSE("GPL");
