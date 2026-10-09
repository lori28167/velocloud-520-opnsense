#!/bin/sh
# Builds the if_vcigb module tree from the OPNsense e1000 sources.
# Usage (in the build VM): sh prepare.sh /usr/src /root/vcigb-src
set -eu
SRC=${1:-/usr/src}
DST=${2:-/root/vcigb}
HERE=$(dirname "$0")

rm -rf "$DST"
mkdir -p "$DST"
cp "$SRC"/sys/dev/e1000/*.[ch] "$DST"/
cp "$HERE"/vc_edge5x0.c "$HERE"/vc_edge5x0.h "$DST"/
cd "$DST"

# --- e1000_hw.h: per-port VeloCloud state
grep -q 'bool clear_semaphore_once;' e1000_hw.h
sed -i '' '/bool clear_semaphore_once;/a\
	u8 vc_role;		/* VeloCloud edge5x0, see vc_edge5x0.c */\
	u8 vc_mdio_addr;\
	u8 vc_mdio_reg;
' e1000_hw.h

# --- e1000_82575.c: hook before the PHY probe
sed -i '' 's|^#include "e1000_api.h"$|#include "e1000_api.h"\
#include "vc_edge5x0.h"|' e1000_82575.c
sed -i '' '/phy->ops.write_i2c_byte = e1000_write_i2c_byte_generic;/a\
\
	/* VeloCloud edge5x0: switch-facing SerDes ports have no PHY */\
	if (vc_edge5x0_phy_init(hw)) {\
		phy->type = e1000_phy_none;\
		goto out;\
	}
' e1000_82575.c

grep -q '/\* Set phy->phy_addr and phy->id. \*/' e1000_82575.c
sed -i '' '/\/\* Set phy->phy_addr and phy->id. \*\//i\
	/* VeloCloud edge5x0: WAN PHY registers via the GPIO MDIO bus */\
	vc_edge5x0_phy_ops(hw);\

' e1000_82575.c
sed -i '' '/DEBUGFUNC("e1000_get_phy_id_82575");/a\
\
	/* VeloCloud edge5x0: WAN PHY address is fixed by the function */\
	if (vc_edge5x0_phy_addr(hw)) {\
		ret_val = e1000_get_phy_id(hw);\
		goto out;\
	}
' e1000_82575.c

# --- if_em.c: rename to vcigb, I354 SGMII only, no em driver
sed -i '' 's|^#include "if_em.h"$|#include "if_em.h"\
#include "vc_edge5x0.h"|' if_em.c
sed -i '' '/^static device_method_t em_methods\[\] = {$/i\
/* VeloCloud edge5x0: only the I354 SGMII functions, ahead of stock igb */\
static int\
vcigb_probe(device_t dev)\
{\
	int error;\
\
	if (pci_get_vendor(dev) != 0x8086 ||\
	    pci_get_device(dev) != E1000_DEV_ID_I354_SGMII)\
		return (ENXIO);\
	error = iflib_device_probe(dev);\
	if (error > 0)\
		return (error);\
	return (BUS_PROBE_VENDOR);\
}\

' if_em.c
sed -i '' '/^static device_method_t igb_methods\[\] = {$/,/DEVMETHOD_END/s/iflib_device_probe/vcigb_probe/' if_em.c
sed -i '' 's|^static driver_t em_driver = {$|static driver_t em_driver __unused = {|' if_em.c
sed -i '' -e '/^DRIVER_MODULE(em, pci, em_driver, 0, 0);$/d' \
    -e '/^MODULE_DEPEND(em, /d' \
    -e '/^IFLIB_PNP_INFO(pci, em, em_vendor_info_array);$/d' \
    -e '/^IFLIB_PNP_INFO(pci, igb, igb_vendor_info_array);$/d' if_em.c
sed -i '' -e 's|"igb", igb_methods|"vcigb", igb_methods|' \
    -e 's|^DRIVER_MODULE(igb, pci, igb_driver, 0, 0);$|DRIVER_MODULE(vcigb, pci, igb_driver, 0, 0);|' \
    -e 's|^MODULE_DEPEND(igb, |MODULE_DEPEND(vcigb, |' \
    -e 's|^static SYSCTL_NODE(_hw, OID_AUTO, em, |static SYSCTL_NODE(_hw, OID_AUTO, vcigb, |' \
    -e 's|(_hw_em, |(_hw_vcigb, |g' if_em.c
sed -i '' '/INIT_DEBUGOUT("em_if_attach_post: end");/i\
	vc_edge5x0_attach_post(sc);\

' if_em.c

# --- sanity: every edit landed
grep -q 'u8 vc_role;' e1000_hw.h
grep -q 'vc_edge5x0_phy_init(hw)' e1000_82575.c
grep -q 'vc_edge5x0_phy_ops(hw);' e1000_82575.c
grep -q 'if (vc_edge5x0_phy_addr(hw))' e1000_82575.c
grep -q 'DRIVER_MODULE(vcigb, pci' if_em.c
grep -q 'DEVMETHOD(device_probe, vcigb_probe)' if_em.c
grep -q 'vc_edge5x0_attach_post(sc);' if_em.c
! grep -qE '^(DRIVER_MODULE|MODULE_DEPEND)\((em|igb),' if_em.c
! grep -q '_hw_em\b' if_em.c

# --- Makefile: sources as in sys/modules/em, headers from this directory
cat > Makefile <<'EOF'
.PATH:	${.CURDIR}
KMOD=	if_vcigb
SRCS=	device_if.h bus_if.h pci_if.h opt_ddb.h opt_inet.h opt_inet6.h \
	opt_rss.h ifdi_if.h
SRCS+=	vc_edge5x0.c if_em.c em_txrx.c e1000_osdep.c igb_txrx.c
SRCS+=	e1000_api.c e1000_base.c e1000_phy.c e1000_nvm.c e1000_mac.c \
	e1000_manage.c e1000_vf.c e1000_mbx.c e1000_i210.c
SRCS+=	e1000_80003es2lan.c e1000_ich8lan.c e1000_82571.c e1000_82575.c
SRCS+=	e1000_82540.c e1000_82542.c e1000_82541.c e1000_82543.c
CFLAGS+=	-I${.CURDIR}

.include <bsd.kmod.mk>
EOF
echo "prepared in $DST"
