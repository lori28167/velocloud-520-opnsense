/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright (c) 2026 Lorenzo Cozzaglio
 *
 * VeloCloud Edge 520/540 ("edge5x0") support for the Intel I354 functions
 * wired to the two Marvell 88E6176 switches.
 *
 * Board wiring (from VeloCloud's GPL igb_vc.c, as documented by the OpenWrt
 * port at github.com/gallops-gannets/velo540openwrt):
 *
 *   func 0/1  SerDes to switch A/B port 4, no PHY.  The switch SMI is on
 *             this function's own MDIO pins, single-chip addressing
 *             (port registers at 0x10-0x16, Global 1 at 0x1b, Global 2 at
 *             0x1c; internal PHYs and the SerDes behind the Global 2 SMI
 *             indirection).
 *   func 2/3  88E1514 WAN PHYs (GE1/GE2) at addresses 0/1 on a bit-bang
 *             MDIO bus on SoC core-well GPIOs, held in reset by pin 4 of a
 *             PCA9557 (0x18) on a bit-bang I2C bus.  Only the PHY register
 *             access is rerouted; the SGMII/copper path stays stock.
 *
 * The board is identified by NVM words 0x06/0x07 = "Vc"/"5X".
 */

#include "if_em.h"
#include "vc_edge5x0.h"

#include <sys/sysctl.h>
#include <machine/cpufunc.h>

#define VC_NVM_WORD		0x06
#define VC_NVM_ID		0x5663	/* "Vc" */
#define VC_NVM_ID_EDGE5X0	0x3558	/* "5X" */

/* 88E6176, single-chip addressing */
#define SW_PORT(p)		(0x10 + (p))
#define SW_GLOBAL2		0x1c
#define SW_SMI_CMD		0x18
#define SW_SMI_DATA		0x19
#define SW_SMI_BUSY		0x8000
#define SW_SMI_C22_WRITE	0x1400
#define SW_SMI_C22_READ		0x1800
#define SW_SERDES_DEV		0x0f
#define SW_ID_MASK		0xfff0
#define SW_ID_88E6176		0x1760

/*
 * Atom C2000 LPC bridge: GPIO base in config register 0x48, core-well
 * GPIOs 0-31 at offsets 0x00 (USE_SEL, 1 = GPIO), 0x04 (IO_SEL, 1 = input)
 * and 0x08 (LVL).  LVL does not read back the level of output pins, so
 * it is written from a shadow copy.  32-bit accesses only.
 */
#define C2000_LPC_GBASE		0x48
#define C2000_GPIO_USE_SEL	0x00
#define C2000_GPIO_IO_SEL	0x04
#define C2000_GPIO_LVL		0x08

/* Board rev B (DMI board version 2.x) pins */
#define VC_GPIO_I2C_SDA		11
#define VC_GPIO_I2C_SCL		12
#define VC_GPIO_MDIO		13
#define VC_GPIO_MDC		14
#define VC_GPIO_MASK		((1u << VC_GPIO_I2C_SDA) | \
				 (1u << VC_GPIO_I2C_SCL) | \
				 (1u << VC_GPIO_MDIO) | (1u << VC_GPIO_MDC))

#define PCA9557_ADDR		0x18
#define PCA9557_IN		0
#define PCA9557_OUT		1
#define PCA9557_CFG		3	/* 1 = input */
#define PCA9557_WAN_RST		0x10	/* pin 4: 88E1514 reset, active low */

#define M88_PHY_ID1		0x0141

static struct mtx vc_bb_mtx;
MTX_SYSINIT(vc_bb_mtx, &vc_bb_mtx, "vcigb bit-bang", MTX_DEF);

static u_int vc_gpio_base;	/* 0 until set up */
static u32 vc_gpio_lvl;		/* shadow of GPIO_LVL */
static int vc_wan_ready;	/* 1 ready, -1 failed, 0 not tried */

static SYSCTL_NODE(_hw, OID_AUTO, vcwan, CTLFLAG_RD | CTLFLAG_MPSAFE, 0,
    "VeloCloud edge5x0 WAN PHY bring-up");
/*
 * Level of PCA9557 pin 4 that releases the 88E1514 reset.  The reset is
 * active low (the vendor's "relval" is the level left at driver unload,
 * not the run level); measured: PHYs answer only with the pin high.
 */
static int vc_rst_release = 1;
SYSCTL_INT(_hw_vcwan, OID_AUTO, rst_release, CTLFLAG_RWTUN, &vc_rst_release,
    0, "PCA9557 pin 4 level that releases the WAN PHY reset");
static int vc_dbg_mdio_addr, vc_dbg_mdio_reg;
SYSCTL_INT(_hw_vcwan, OID_AUTO, mdio_addr, CTLFLAG_RW, &vc_dbg_mdio_addr, 0,
    "GPIO MDIO address for mdio_val");
SYSCTL_INT(_hw_vcwan, OID_AUTO, mdio_reg, CTLFLAG_RW, &vc_dbg_mdio_reg, 0,
    "GPIO MDIO register for mdio_val");

static u32
vc_func(struct e1000_hw *hw)
{
	return ((E1000_READ_REG(hw, E1000_STATUS) & E1000_STATUS_FUNC_MASK) >>
	    E1000_STATUS_FUNC_SHIFT);
}

static u16
vc_swfw_mask(struct e1000_hw *hw)
{
	switch (vc_func(hw)) {
	case 1:
		return (E1000_SWFW_PHY1_SM);
	case 2:
		return (E1000_SWFW_PHY2_SM);
	case 3:
		return (E1000_SWFW_PHY3_SM);
	default:
		return (E1000_SWFW_PHY0_SM);
	}
}

/*
 * One MDIO transaction on the I354's own MDIO master with an explicit
 * address: with single-chip addressing every switch register bank is a
 * different "PHY" address.
 */
static int
vc_mdic(struct e1000_hw *hw, int addr, int reg, bool write, u16 *val)
{
	u32 mdic;
	int i;

	if (addr < 0 || addr > 0x1f || reg < 0 || reg > MAX_PHY_REG_ADDRESS)
		return (EINVAL);

	mdic = E1000_READ_REG(hw, E1000_MDICNFG);
	mdic &= (E1000_MDICNFG_EXT_MDIO | E1000_MDICNFG_COM_MDIO);
	mdic |= (u32)addr << E1000_MDICNFG_PHY_SHIFT;
	E1000_WRITE_REG(hw, E1000_MDICNFG, mdic);

	mdic = ((u32)reg << E1000_MDIC_REG_SHIFT) |
	    ((u32)addr << E1000_MDIC_PHY_SHIFT);
	if (write)
		mdic |= E1000_MDIC_OP_WRITE | *val;
	else
		mdic |= E1000_MDIC_OP_READ;
	E1000_WRITE_REG(hw, E1000_MDIC, mdic);

	for (i = 0; i < E1000_GEN_POLL_TIMEOUT * 3; i++) {
		usec_delay(50);
		mdic = E1000_READ_REG(hw, E1000_MDIC);
		if (mdic & E1000_MDIC_READY)
			break;
	}
	if (!(mdic & E1000_MDIC_READY))
		return (ETIMEDOUT);
	if (mdic & E1000_MDIC_ERROR)
		return (EIO);
	if (!write)
		*val = (u16)mdic;
	return (0);
}

static int
vc_mdio(struct e1000_hw *hw, int addr, int reg, bool write, u16 *val)
{
	u16 mask = vc_swfw_mask(hw);
	int error;

	if (hw->mac.ops.acquire_swfw_sync(hw, mask) != E1000_SUCCESS)
		return (EBUSY);
	error = vc_mdic(hw, addr, reg, write, val);
	hw->mac.ops.release_swfw_sync(hw, mask);
	return (error);
}

static int
vc_mdio_read(struct e1000_hw *hw, int addr, int reg, u16 *val)
{
	return (vc_mdio(hw, addr, reg, false, val));
}

static int
vc_mdio_write(struct e1000_hw *hw, int addr, int reg, u16 val)
{
	return (vc_mdio(hw, addr, reg, true, &val));
}

/* Global 2 SMI indirection to the internal PHYs and the port 4 SerDes. */
static int
vc_smi_wait(struct e1000_hw *hw)
{
	u16 cmd;
	int error, i;

	for (i = 0; i < 100; i++) {
		error = vc_mdio_read(hw, SW_GLOBAL2, SW_SMI_CMD, &cmd);
		if (error != 0)
			return (error);
		if ((cmd & SW_SMI_BUSY) == 0)
			return (0);
		DELAY(100);
	}
	return (ETIMEDOUT);
}

static int
vc_smi_write(struct e1000_hw *hw, int dev, int reg, u16 val)
{
	int error;

	if ((error = vc_smi_wait(hw)) != 0 ||
	    (error = vc_mdio_write(hw, SW_GLOBAL2, SW_SMI_DATA, val)) != 0 ||
	    (error = vc_mdio_write(hw, SW_GLOBAL2, SW_SMI_CMD, SW_SMI_BUSY |
	    SW_SMI_C22_WRITE | (dev << 5) | reg)) != 0)
		return (error);
	return (vc_smi_wait(hw));
}

static int
vc_smi_read(struct e1000_hw *hw, int dev, int reg, u16 *val)
{
	int error;

	if ((error = vc_smi_wait(hw)) != 0 ||
	    (error = vc_mdio_write(hw, SW_GLOBAL2, SW_SMI_CMD, SW_SMI_BUSY |
	    SW_SMI_C22_READ | (dev << 5) | reg)) != 0 ||
	    (error = vc_smi_wait(hw)) != 0)
		return (error);
	return (vc_mdio_read(hw, SW_GLOBAL2, SW_SMI_DATA, val));
}

/*
 * Bring the 88E6176 up as a plain 4-port switch (sequence from the vendor
 * igb_vc.c via velo540-switch):
 *  - port 4 SerDes: power up, autoneg off, reset (SMI device 0xf, page 1)
 *  - port 4: clear PHYDetect, force MAC 1000/full
 *  - internal PHYs 0-3: clear power-down, reset, autoneg
 *  - ports 0-4 Forwarding, ports 5/6 (RGMII cross-links) Disabled
 */
static int
vc_switch_up(struct e1000_softc *sc)
{
	struct e1000_hw *hw = &sc->hw;
	u16 id, v;
	int error, d, p;

	if ((error = vc_mdio_read(hw, SW_PORT(0), 3, &id)) != 0)
		goto fail;
	if ((id & SW_ID_MASK) != SW_ID_88E6176) {
		device_printf(sc->dev, "no 88E6176 behind this port (id 0x%04x)\n",
		    id);
		return (ENXIO);
	}

	if ((error = vc_smi_write(hw, SW_SERDES_DEV, 22, 1)) != 0 ||
	    (error = vc_smi_read(hw, SW_SERDES_DEV, 0, &v)) != 0 ||
	    (error = vc_smi_write(hw, SW_SERDES_DEV, 0,
	    (v | 0x8100) & ~0x1800)) != 0)
		goto fail;
	pause("vcsw", hz);

	if ((error = vc_mdio_read(hw, SW_PORT(4), 0, &v)) != 0 ||
	    (error = vc_mdio_write(hw, SW_PORT(4), 0, v & ~0x1000)) != 0 ||
	    (error = vc_mdio_write(hw, SW_PORT(4), 1, 0x000e)) != 0)
		goto fail;

	for (d = 0; d < 4; d++) {
		if ((error = vc_smi_write(hw, d, 22, 0)) != 0 ||
		    (error = vc_smi_write(hw, d, 0, 0x9140)) != 0)
			goto fail;
	}

	for (p = 0; p <= 6; p++) {
		if ((error = vc_mdio_read(hw, SW_PORT(p), 4, &v)) != 0)
			goto fail;
		v &= ~3;
		if (p <= 4)
			v |= 3;
		if ((error = vc_mdio_write(hw, SW_PORT(p), 4, v)) != 0)
			goto fail;
	}

	if ((error = vc_mdio_read(hw, SW_PORT(4), 0, &v)) != 0)
		goto fail;
	device_printf(sc->dev, "88E6176 0x%04x up, port 4 status 0x%04x\n",
	    id, v);
	return (0);

fail:
	device_printf(sc->dev, "switch init failed: %d\n", error);
	return (error);
}

/* SoC core-well GPIO, see C2000_GPIO_* above. */
static void
vc_gpio_dir(int pin, bool out)
{
	u32 v;

	v = inl(vc_gpio_base + C2000_GPIO_IO_SEL);
	if (out)
		v &= ~(1u << pin);
	else
		v |= 1u << pin;
	outl(vc_gpio_base + C2000_GPIO_IO_SEL, v);
}

static void
vc_gpio_set(int pin, int val)
{
	if (val)
		vc_gpio_lvl |= 1u << pin;
	else
		vc_gpio_lvl &= ~(1u << pin);
	outl(vc_gpio_base + C2000_GPIO_LVL, vc_gpio_lvl);
}

static int
vc_gpio_get(int pin)
{
	return ((inl(vc_gpio_base + C2000_GPIO_LVL) >> pin) & 1);
}

static int
vc_gpio_setup(void)
{
	device_t lpc;
	char *ver;
	u32 v, io;

	if (vc_gpio_base != 0)
		return (0);

	ver = kern_getenv("smbios.planar.version");
	if (ver == NULL || ver[0] != '2') {
		printf("vcigb: board version %s: WAN pinout unknown\n",
		    ver != NULL ? ver : "?");
		freeenv(ver);
		return (ENXIO);
	}
	freeenv(ver);

	lpc = pci_find_dbsf(0, 0, 31, 0);
	if (lpc == NULL || pci_get_vendor(lpc) != 0x8086 ||
	    (pci_get_device(lpc) & 0xfffc) != 0x1f38)
		return (ENXIO);
	v = pci_read_config(lpc, C2000_LPC_GBASE, 4) & 0xff80;
	if (v == 0)
		return (ENXIO);
	vc_gpio_base = v;

	mtx_lock(&vc_bb_mtx);
	/*
	 * Take the pins from their native function first (coreboot leaves
	 * MDIO/MDC native): IO_SEL and LVL writes to a pin that is still
	 * native are dropped, which left MDC an input after a cold boot.
	 */
	v = inl(vc_gpio_base + C2000_GPIO_USE_SEL);
	if ((v & VC_GPIO_MASK) != VC_GPIO_MASK)
		outl(vc_gpio_base + C2000_GPIO_USE_SEL, v | VC_GPIO_MASK);
	/* Latch our pins low, MDC driven, MDIO/SDA/SCL released. */
	vc_gpio_lvl = inl(vc_gpio_base + C2000_GPIO_LVL) & ~VC_GPIO_MASK;
	outl(vc_gpio_base + C2000_GPIO_LVL, vc_gpio_lvl);
	io = inl(vc_gpio_base + C2000_GPIO_IO_SEL);
	io |= VC_GPIO_MASK;
	io &= ~(1u << VC_GPIO_MDC);
	outl(vc_gpio_base + C2000_GPIO_IO_SEL, io);
	mtx_unlock(&vc_bb_mtx);

	printf("vcigb: GPIO base 0x%x, USE_SEL 0x%08x -> 0x%08x, IO_SEL 0x%08x\n",
	    vc_gpio_base, v, inl(vc_gpio_base + C2000_GPIO_USE_SEL),
	    inl(vc_gpio_base + C2000_GPIO_IO_SEL));
	if ((inl(vc_gpio_base + C2000_GPIO_IO_SEL) & (1u << VC_GPIO_MDC)) != 0) {
		printf("vcigb: MDC GPIO %d did not become an output\n",
		    VC_GPIO_MDC);
		return (ENXIO);
	}
	return (0);
}

/* Clause 22 bit-bang MDIO, same bit timing as Linux mdio-bitbang. */
static void
vc_mdio_bit_out(int b)
{
	vc_gpio_set(VC_GPIO_MDIO, b);
	DELAY(1);
	vc_gpio_set(VC_GPIO_MDC, 1);
	DELAY(1);
	vc_gpio_set(VC_GPIO_MDC, 0);
}

static int
vc_mdio_bit_in(void)
{
	vc_gpio_set(VC_GPIO_MDC, 1);
	DELAY(1);
	vc_gpio_set(VC_GPIO_MDC, 0);
	DELAY(1);
	return (vc_gpio_get(VC_GPIO_MDIO));
}

static void
vc_mdio_release(void)
{
	/* Leave the latch low before turning the line around. */
	vc_gpio_set(VC_GPIO_MDIO, 0);
	vc_gpio_dir(VC_GPIO_MDIO, false);
}

static void
vc_mdio_cmd(int op, int phy, int reg)
{
	int i;

	vc_gpio_dir(VC_GPIO_MDIO, true);
	for (i = 0; i < 32; i++)
		vc_mdio_bit_out(1);
	vc_mdio_bit_out(0);
	vc_mdio_bit_out(1);
	vc_mdio_bit_out((op >> 1) & 1);
	vc_mdio_bit_out(op & 1);
	for (i = 4; i >= 0; i--)
		vc_mdio_bit_out((phy >> i) & 1);
	for (i = 4; i >= 0; i--)
		vc_mdio_bit_out((reg >> i) & 1);
}

static int
vc_bb_mdio_read(int phy, int reg, u16 *val)
{
	u16 v = 0;
	int i;

	mtx_lock(&vc_bb_mtx);
	vc_mdio_cmd(2, phy, reg);
	vc_mdio_release();
	if (vc_mdio_bit_in() != 0) {
		/* No PHY drove the turnaround low: flush and fail. */
		for (i = 0; i < 32; i++)
			vc_mdio_bit_in();
		mtx_unlock(&vc_bb_mtx);
		return (EIO);
	}
	for (i = 0; i < 16; i++)
		v = (v << 1) | vc_mdio_bit_in();
	vc_mdio_bit_in();
	mtx_unlock(&vc_bb_mtx);
	*val = v;
	return (0);
}

static int
vc_bb_mdio_write(int phy, int reg, u16 val)
{
	int i;

	mtx_lock(&vc_bb_mtx);
	vc_mdio_cmd(1, phy, reg);
	vc_mdio_bit_out(1);
	vc_mdio_bit_out(0);
	for (i = 15; i >= 0; i--)
		vc_mdio_bit_out((val >> i) & 1);
	vc_mdio_release();
	vc_mdio_bit_in();
	mtx_unlock(&vc_bb_mtx);
	return (0);
}

/*
 * Bit-bang I2C master, open drain emulated with the direction bit: the
 * latch stays low, "high" means released to the board pull-ups.
 */
static void
vc_i2c_line(int pin, int high)
{
	int i;

	vc_gpio_dir(pin, !high);
	if (high && pin == VC_GPIO_I2C_SCL)
		for (i = 0; i < 1000 && vc_gpio_get(pin) == 0; i++)
			DELAY(1);	/* clock stretching */
	DELAY(5);
}

static void
vc_i2c_start(void)
{
	vc_i2c_line(VC_GPIO_I2C_SDA, 1);
	vc_i2c_line(VC_GPIO_I2C_SCL, 1);
	vc_i2c_line(VC_GPIO_I2C_SDA, 0);
	vc_i2c_line(VC_GPIO_I2C_SCL, 0);
}

static void
vc_i2c_stop(void)
{
	vc_i2c_line(VC_GPIO_I2C_SDA, 0);
	vc_i2c_line(VC_GPIO_I2C_SCL, 1);
	vc_i2c_line(VC_GPIO_I2C_SDA, 1);
}

static int
vc_i2c_write_byte(u8 b)
{
	int i, nak;

	for (i = 7; i >= 0; i--) {
		vc_i2c_line(VC_GPIO_I2C_SDA, (b >> i) & 1);
		vc_i2c_line(VC_GPIO_I2C_SCL, 1);
		vc_i2c_line(VC_GPIO_I2C_SCL, 0);
	}
	vc_i2c_line(VC_GPIO_I2C_SDA, 1);
	vc_i2c_line(VC_GPIO_I2C_SCL, 1);
	nak = vc_gpio_get(VC_GPIO_I2C_SDA);
	vc_i2c_line(VC_GPIO_I2C_SCL, 0);
	return (nak ? EIO : 0);
}

static u8
vc_i2c_read_byte(bool ack)
{
	u8 b = 0;
	int i;

	vc_i2c_line(VC_GPIO_I2C_SDA, 1);
	for (i = 0; i < 8; i++) {
		vc_i2c_line(VC_GPIO_I2C_SCL, 1);
		b = (b << 1) | vc_gpio_get(VC_GPIO_I2C_SDA);
		vc_i2c_line(VC_GPIO_I2C_SCL, 0);
	}
	vc_i2c_line(VC_GPIO_I2C_SDA, ack ? 0 : 1);
	vc_i2c_line(VC_GPIO_I2C_SCL, 1);
	vc_i2c_line(VC_GPIO_I2C_SCL, 0);
	vc_i2c_line(VC_GPIO_I2C_SDA, 1);
	return (b);
}

static int
vc_i2c_reg_read(u8 addr, u8 reg, u8 *val)
{
	int error;

	mtx_lock(&vc_bb_mtx);
	vc_i2c_start();
	if ((error = vc_i2c_write_byte(addr << 1)) == 0 &&
	    (error = vc_i2c_write_byte(reg)) == 0) {
		vc_i2c_start();
		if ((error = vc_i2c_write_byte((addr << 1) | 1)) == 0)
			*val = vc_i2c_read_byte(false);
	}
	vc_i2c_stop();
	mtx_unlock(&vc_bb_mtx);
	return (error);
}

static int
vc_i2c_reg_write(u8 addr, u8 reg, u8 val)
{
	int error;

	mtx_lock(&vc_bb_mtx);
	vc_i2c_start();
	if ((error = vc_i2c_write_byte(addr << 1)) == 0 &&
	    (error = vc_i2c_write_byte(reg)) == 0)
		error = vc_i2c_write_byte(val);
	vc_i2c_stop();
	mtx_unlock(&vc_bb_mtx);
	return (error);
}

/*
 * Once per load: set up the GPIOs, take the 88E1514s out of reset through
 * PCA9557 pin 4 (touching no other pin: the same expander drives other
 * board resets), and check that both PHYs answer.
 */
static int
vc_wan_init(device_t dev)
{
	u16 id0 = 0, id1 = 0;
	u8 out, cfg, rel;
	int i;

	if (vc_wan_ready != 0)
		return (vc_wan_ready > 0 ? 0 : ENXIO);
	vc_wan_ready = -1;

	if (vc_gpio_setup() != 0)
		return (ENXIO);
	if (vc_i2c_reg_read(PCA9557_ADDR, PCA9557_OUT, &out) != 0 ||
	    vc_i2c_reg_read(PCA9557_ADDR, PCA9557_CFG, &cfg) != 0) {
		device_printf(dev, "PCA9557@0x%x does not answer\n",
		    PCA9557_ADDR);
		return (ENXIO);
	}
	device_printf(dev, "PCA9557@0x%x out 0x%02x cfg 0x%02x\n",
	    PCA9557_ADDR, out, cfg);

	/* Pulse the reset unless pin 4 already drives the release level. */
	rel = vc_rst_release ? PCA9557_WAN_RST : 0;
	if ((cfg & PCA9557_WAN_RST) != 0 || (out & PCA9557_WAN_RST) != rel) {
		out = (out & ~PCA9557_WAN_RST) | (rel ^ PCA9557_WAN_RST);
		if (vc_i2c_reg_write(PCA9557_ADDR, PCA9557_OUT, out) != 0 ||
		    vc_i2c_reg_write(PCA9557_ADDR, PCA9557_CFG,
		    cfg & ~PCA9557_WAN_RST) != 0)
			return (ENXIO);
		pause("vcrst", hz / 5);
		out = (out & ~PCA9557_WAN_RST) | rel;
		if (vc_i2c_reg_write(PCA9557_ADDR, PCA9557_OUT, out) != 0)
			return (ENXIO);
	}

	/* 100 ms after release is not always enough: poll for up to 3 s. */
	for (i = 0; i < 30; i++) {
		pause("vcphy", hz / 10);
		if (vc_bb_mdio_read(0, PHY_ID1, &id0) == 0 &&
		    vc_bb_mdio_read(1, PHY_ID1, &id1) == 0 &&
		    id0 == M88_PHY_ID1 && id1 == M88_PHY_ID1)
			break;
	}
	if (i == 30) {
		device_printf(dev, "WAN PHYs not found (0x%04x 0x%04x)\n",
		    id0, id1);
		return (ENXIO);
	}
	device_printf(dev, "WAN PHYs answering after %d ms\n", (i + 1) * 100);
	vc_wan_ready = 1;
	return (0);
}

/* Bring-up/debug knobs under hw.vcwan. */
static int
vc_sysctl_pca(SYSCTL_HANDLER_ARGS)
{
	int error, reg = arg2, v;
	u8 val;

	if (vc_gpio_setup() != 0)
		return (ENXIO);
	if (vc_i2c_reg_read(PCA9557_ADDR, reg, &val) != 0)
		return (EIO);
	v = val;
	error = sysctl_handle_int(oidp, &v, 0, req);
	if (error != 0 || req->newptr == NULL)
		return (error);
	if (reg == PCA9557_IN || v < 0 || v > 0xff)
		return (EINVAL);
	return (vc_i2c_reg_write(PCA9557_ADDR, reg, v) != 0 ? EIO : 0);
}
SYSCTL_PROC(_hw_vcwan, OID_AUTO, pca_in, CTLTYPE_INT | CTLFLAG_RD |
    CTLFLAG_MPSAFE, NULL, PCA9557_IN, vc_sysctl_pca, "I",
    "PCA9557@0x18 input port");
SYSCTL_PROC(_hw_vcwan, OID_AUTO, pca_out, CTLTYPE_INT | CTLFLAG_RW |
    CTLFLAG_MPSAFE, NULL, PCA9557_OUT, vc_sysctl_pca, "I",
    "PCA9557@0x18 output port");
SYSCTL_PROC(_hw_vcwan, OID_AUTO, pca_cfg, CTLTYPE_INT | CTLFLAG_RW |
    CTLFLAG_MPSAFE, NULL, PCA9557_CFG, vc_sysctl_pca, "I",
    "PCA9557@0x18 configuration (1 = input)");

static int
vc_sysctl_mdio(SYSCTL_HANDLER_ARGS)
{
	int error, v;
	u16 val;

	if (vc_gpio_setup() != 0)
		return (ENXIO);
	if (vc_bb_mdio_read(vc_dbg_mdio_addr & 0x1f, vc_dbg_mdio_reg & 0x1f,
	    &val) != 0)
		v = -1;
	else
		v = val;
	error = sysctl_handle_int(oidp, &v, 0, req);
	if (error != 0 || req->newptr == NULL)
		return (error);
	if (v < 0 || v > 0xffff)
		return (EINVAL);
	return (vc_bb_mdio_write(vc_dbg_mdio_addr & 0x1f,
	    vc_dbg_mdio_reg & 0x1f, v));
}
SYSCTL_PROC(_hw_vcwan, OID_AUTO, mdio_val, CTLTYPE_INT | CTLFLAG_RW |
    CTLFLAG_MPSAFE, NULL, 0, vc_sysctl_mdio, "I",
    "GPIO MDIO register at mdio_addr/mdio_reg (-1 = no answer)");

static int
vc_sysctl_scan(SYSCTL_HANDLER_ARGS)
{
	char buf[32 * 9 + 1];
	int a, len = 0;
	u16 v;

	if (vc_gpio_setup() != 0)
		return (ENXIO);
	buf[0] = '\0';
	for (a = 0; a < 32; a++)
		if (vc_bb_mdio_read(a, PHY_ID1, &v) == 0)
			len += snprintf(buf + len, sizeof(buf) - len, "%d:%04x ",
			    a, v);
	return (sysctl_handle_string(oidp, buf, sizeof(buf), req));
}
SYSCTL_PROC(_hw_vcwan, OID_AUTO, mdio_scan, CTLTYPE_STRING | CTLFLAG_RD |
    CTLFLAG_MPSAFE, NULL, 0, vc_sysctl_scan, "A",
    "PHY_ID1 of every address that drives the turnaround");

static int
vc_sysctl_gpio(SYSCTL_HANDLER_ARGS)
{
	char buf[96];

	if (vc_gpio_setup() != 0)
		return (ENXIO);
	snprintf(buf, sizeof(buf), "use_sel=%08x io_sel=%08x lvl=%08x shadow=%08x",
	    inl(vc_gpio_base + C2000_GPIO_USE_SEL),
	    inl(vc_gpio_base + C2000_GPIO_IO_SEL),
	    inl(vc_gpio_base + C2000_GPIO_LVL), vc_gpio_lvl);
	return (sysctl_handle_string(oidp, buf, sizeof(buf), req));
}
SYSCTL_PROC(_hw_vcwan, OID_AUTO, gpio, CTLTYPE_STRING | CTLFLAG_RD |
    CTLFLAG_MPSAFE, NULL, 0, vc_sysctl_gpio, "A",
    "Core-well GPIO USE_SEL/IO_SEL/LVL and the LVL shadow");

static s32
vc_ext_read_reg(struct e1000_hw *hw, u32 offset, u16 *data)
{
	if (offset > MAX_PHY_REG_ADDRESS)
		return (-E1000_ERR_PARAM);
	if (vc_bb_mdio_read(hw->phy.addr, offset, data) != 0)
		return (-E1000_ERR_PHY);
	return (E1000_SUCCESS);
}

static s32
vc_ext_write_reg(struct e1000_hw *hw, u32 offset, u16 data)
{
	if (offset > MAX_PHY_REG_ADDRESS)
		return (-E1000_ERR_PARAM);
	vc_bb_mdio_write(hw->phy.addr, offset, data);
	return (E1000_SUCCESS);
}

/*
 * Switch-facing functions: the far end is the 88E6176's port 4 SerDes.
 * Force the I354 side to 1000/full with PCS autoneg off, like the vendor's
 * igb_vc_setup_sgmii_link_forced(); the stock SGMII path would look for a
 * PHY that does not exist.
 */
static s32
vc_setup_serdes_forced(struct e1000_hw *hw)
{
	u32 reg;

	E1000_WRITE_REG(hw, E1000_SCTL, E1000_SCTL_DISABLE_SERDES_LOOPBACK);

	reg = E1000_READ_REG(hw, E1000_PCS_CFG0);
	reg |= E1000_PCS_CFG_PCS_EN;
	E1000_WRITE_REG(hw, E1000_PCS_CFG0, reg);

	reg = E1000_READ_REG(hw, E1000_CTRL);
	reg |= E1000_CTRL_SLU | E1000_CTRL_SPD_1000 | E1000_CTRL_FD |
	    E1000_CTRL_FRCSPD | E1000_CTRL_FRCDPX;
	E1000_WRITE_REG(hw, E1000_CTRL, reg);

	reg = E1000_READ_REG(hw, E1000_PCS_LCTL);
	reg &= ~(E1000_PCS_LCTL_AN_ENABLE | E1000_PCS_LCTL_AN_RESTART |
	    E1000_PCS_LCTL_AN_TIMEOUT);
	reg |= E1000_PCS_LCTL_FLV_LINK_UP | E1000_PCS_LCTL_FSV_1000 |
	    E1000_PCS_LCTL_FDV_FULL | E1000_PCS_LCTL_FSD |
	    E1000_PCS_LCTL_FORCE_FCTRL | E1000_PCS_LCTL_FORCE_LINK;
	E1000_WRITE_REG(hw, E1000_PCS_LCTL, reg);
	E1000_WRITE_FLUSH(hw);

	return (E1000_SUCCESS);
}

/*
 * Called at the top of e1000_init_phy_params_82575(), after the MAC and
 * NVM parameters are set up.  Returns true when this function is a
 * switch-facing port, which then has no PHY.  WAN functions return false
 * and continue down the stock copper path, with the PHY access rerouted
 * by vc_edge5x0_phy_ops() and vc_edge5x0_phy_addr().
 */
bool
vc_edge5x0_phy_init(struct e1000_hw *hw)
{
	struct e1000_osdep *osdep = hw->back;
	u16 nvm[2] = { 0, 0 };

	if (hw->mac.type != e1000_i354)
		return (false);
	if (hw->nvm.ops.read(hw, VC_NVM_WORD, 2, nvm) != E1000_SUCCESS)
		return (false);
	if (nvm[0] != VC_NVM_ID || nvm[1] != VC_NVM_ID_EDGE5X0)
		return (false);

	if (vc_func(hw) > 1) {
		if (vc_wan_init(osdep->dev) == 0)
			hw->dev_spec._82575.vc_role = VC_ROLE_EXTPHY;
		return (false);
	}

	hw->dev_spec._82575.vc_role = VC_ROLE_SWITCH;
	hw->phy.media_type = e1000_media_type_internal_serdes;
	hw->mac.ops.setup_physical_interface = vc_setup_serdes_forced;
	return (true);
}

/* In e1000_init_phy_params_82575(), after the stock read/write_reg choice. */
void
vc_edge5x0_phy_ops(struct e1000_hw *hw)
{
	if (hw->dev_spec._82575.vc_role != VC_ROLE_EXTPHY)
		return;
	hw->phy.ops.read_reg = vc_ext_read_reg;
	hw->phy.ops.write_reg = vc_ext_write_reg;
}

/* At the top of e1000_get_phy_id_82575(): func 2 -> PHY 0, func 3 -> PHY 1. */
bool
vc_edge5x0_phy_addr(struct e1000_hw *hw)
{
	if (hw->dev_spec._82575.vc_role != VC_ROLE_EXTPHY)
		return (false);
	hw->phy.addr = vc_func(hw) - 2;
	return (true);
}

/* WAN jack LEDs (vendor igb_vc_setup_m88e1514): page 3, LED0 = gigabit
 * link, LED1 = 10/100 link + activity; the PHY default is both solid on. */
static void
vc_wan_leds(struct e1000_hw *hw)
{
	vc_bb_mdio_write(hw->phy.addr, 22, 3);
	vc_bb_mdio_write(hw->phy.addr, 16, 0x0817);
	vc_bb_mdio_write(hw->phy.addr, 17, 0x0005);
	vc_bb_mdio_write(hw->phy.addr, 22, 0);
}

/* Raw MDIO for bring-up: set vc_mdio_addr/vc_mdio_reg, then read or write vc_mdio_val. */
static int
vc_sysctl_mdio_val(SYSCTL_HANDLER_ARGS)
{
	struct e1000_softc *sc = arg1;
	struct e1000_dev_spec_82575 *ds = &sc->hw.dev_spec._82575;
	u16 val = 0;
	int error, v;

	error = vc_mdio_read(&sc->hw, ds->vc_mdio_addr, ds->vc_mdio_reg, &val);
	if (error != 0)
		return (error);
	v = val;
	error = sysctl_handle_int(oidp, &v, 0, req);
	if (error != 0 || req->newptr == NULL)
		return (error);
	if (v < 0 || v > 0xffff)
		return (EINVAL);
	return (vc_mdio_write(&sc->hw, ds->vc_mdio_addr, ds->vc_mdio_reg, v));
}

static int
vc_sysctl_switch_init(SYSCTL_HANDLER_ARGS)
{
	struct e1000_softc *sc = arg1;
	int error, v = 0;

	error = sysctl_handle_int(oidp, &v, 0, req);
	if (error != 0 || req->newptr == NULL || v == 0)
		return (error);
	return (vc_switch_up(sc));
}

void
vc_edge5x0_attach_post(struct e1000_softc *sc)
{
	struct e1000_dev_spec_82575 *ds = &sc->hw.dev_spec._82575;
	struct sysctl_ctx_list *ctx;
	struct sysctl_oid_list *child;
	int autoinit = 0;

	if (ds->vc_role == VC_ROLE_EXTPHY) {
		device_printf(sc->dev,
		    "VeloCloud edge5x0: WAN port, 88E1514 at GPIO MDIO address %u\n",
		    sc->hw.phy.addr);
		vc_wan_leds(&sc->hw);
		return;
	}
	if (ds->vc_role != VC_ROLE_SWITCH)
		return;

	device_printf(sc->dev,
	    "VeloCloud edge5x0: switch port, forced 1000/full SerDes\n");

	ctx = device_get_sysctl_ctx(sc->dev);
	child = SYSCTL_CHILDREN(device_get_sysctl_tree(sc->dev));
	SYSCTL_ADD_U8(ctx, child, OID_AUTO, "vc_mdio_addr", CTLFLAG_RW,
	    &ds->vc_mdio_addr, 0, "MDIO address for vc_mdio_val");
	SYSCTL_ADD_U8(ctx, child, OID_AUTO, "vc_mdio_reg", CTLFLAG_RW,
	    &ds->vc_mdio_reg, 0, "MDIO register for vc_mdio_val");
	SYSCTL_ADD_PROC(ctx, child, OID_AUTO, "vc_mdio_val",
	    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_MPSAFE, sc, 0,
	    vc_sysctl_mdio_val, "I", "Read/write the selected MDIO register");
	SYSCTL_ADD_PROC(ctx, child, OID_AUTO, "vc_switch_init",
	    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_MPSAFE, sc, 0,
	    vc_sysctl_switch_init, "I",
	    "Write 1 to bring the 88E6176 up as a plain 4-port switch");

	TUNABLE_INT_FETCH("hw.vcigb.switch_autoinit", &autoinit);
	if (autoinit)
		vc_switch_up(sc);
}
