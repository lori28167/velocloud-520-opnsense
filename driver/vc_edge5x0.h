/*-
 * VeloCloud Edge 520/540 ("edge5x0") glue for the e1000/igb driver.
 */
#ifndef _VC_EDGE5X0_H_
#define _VC_EDGE5X0_H_

struct e1000_hw;
struct e1000_softc;

#define VC_ROLE_NONE	0
#define VC_ROLE_SWITCH	1	/* SerDes to a Marvell 88E6176, no PHY */
#define VC_ROLE_EXTPHY	2	/* 88E1514 WAN PHY on the GPIO MDIO bus */

bool	vc_edge5x0_phy_init(struct e1000_hw *hw);
void	vc_edge5x0_phy_ops(struct e1000_hw *hw);
bool	vc_edge5x0_phy_addr(struct e1000_hw *hw);
void	vc_edge5x0_attach_post(struct e1000_softc *sc);

#endif /* _VC_EDGE5X0_H_ */
