# OPNsense on the VeloCloud Edge 520: all ports working

`if_vcigb` is a FreeBSD kernel module that makes every RJ45 port of a
**VeloCloud Edge 520** usable on **OPNsense 26.7**: the 8 LAN ports behind the two
Marvell switches and the 2 copper WAN ports (GE1/GE2). Stock OPNsense only drives
the two SFP cages on this box.

It is a loadable module, not a custom kernel: OPNsense stays stock and can be
updated, as long as the module is rebuilt for the new kernel (see below).

> **Status: experimental.** Tested on one unit: EDGE520, board rev 2.8 (Atom C2358,
> 4 GB), OPNsense 26.7 (`stable/26.7-n283674-12334a596709`). On that unit all 8 LAN
> ports and GE1/GE2 link at 1000/full and pass traffic, and everything comes up by
> itself at boot (tested with a reboot that puts the GPIOs and the PCA9557 back at
> their power-on defaults; a full power-off cycle with the current code is untested).
> Edge 540 shares the board design but is untested. Rev A boards (DMI board version
> 1.x) use a different WAN MDIO pinout and are not supported for GE1/GE2.
> No warranty: this is kernel code, a bug can panic the box.

## Ports

| Jacks | Interface | Hardware |
|---|---|---|
| SFP1 / SFP2 | `igb0` / `igb1` | Intel I350, stock `igb` driver |
| LAN5-8 | `vcigb0` | Marvell 88E6176 switch A behind Intel I354 function 0 |
| LAN1-4 | `vcigb1` | Marvell 88E6176 switch B behind Intel I354 function 1 |
| GE1 | `vcigb2` | Marvell 88E1514 PHY behind I354 function 2 |
| GE2 | `vcigb3` | Marvell 88E1514 PHY behind I354 function 3 |

Switch port map: LAN1=B1 LAN2=B0 LAN3=B3 LAN4=B2 LAN5=A2 LAN6=A0 LAN7=A1 LAN8=A3.

The switches run as two plain 4-port switches: each group of 4 jacks shares one
1 Gbit/s link to the CPU, and traffic between the two groups goes through the
firewall (bridge `vcigb0`+`vcigb1` in OPNsense if you want one LAN on all 8 jacks).
Per-port control/VLANs on the switches (FreeBSD `e6000sw`) is not done yet.

## Other things you need to know about this box

- **Serial console:** the mini-USB jack is a CP2102N wired to **COM2** (`0x2f8`),
  115200 8N1. OPNsense's serial image uses COM1, so you see nothing after the
  loader until you add the console lines below (edit the install stick from any
  FreeBSD system/VM, or the installed disk).
- **Watchdog:** coreboot arms the Atom's TCO watchdog (reboot after ~3 minutes).
  Loading FreeBSD's `ichwd` stops it, so no BIOS flashing is needed.
- **Boot order:** SeaBIOS boots USB before the internal disk (a 7.3 GB USB card
  reader, `da0`/`da1`). F12 opens the boot menu.
- **Wi-Fi (QCA988x):** not supported as an access point. FreeBSD has `ath10k`, but
  its LinuxKPI 802.11 layer only offers station and monitor modes. An Atheros
  AR93xx mini-PCIe card works as an AP with FreeBSD's native `ath(4)`.

## Install

1. Install OPNsense 26.7 normally (serial image), with the console lines below.
2. Build `if_vcigb.ko` for your exact kernel (next section), copy it to
   `/boot/modules/` and run `kldxref /boot/modules`.
3. Add to `/boot/loader.conf.local` (OPNsense does not overwrite this file):

```
console="comconsole"
comconsole_port="0x2f8"
comconsole_speed="115200"
hw.uart.console="io:0x2f8,br:115200"
ichwd_load="YES"
if_vcigb_load="YES"
hw.vcigb.switch_autoinit="1"
# optional, if you don't use the SFP cages:
#hint.igb.0.disabled="1"
#hint.igb.1.disabled="1"
```

4. Reboot and assign `vcigb0`..`vcigb3` in OPNsense.

At boot you should see `VeloCloud edge5x0: switch port`, `88E6176 0x1761 up` (twice)
and `WAN PHYs answering after ... ms` in `dmesg`.

## Build

The module must be built against the exact OPNsense kernel sources and options
(`uname -v` on the box gives the commit). In a FreeBSD 15.1 amd64 VM:

```sh
# OPNsense kernel sources at the commit from `uname -v`
mkdir -p /usr/src && cd /usr/src
fetch -o - https://codeload.github.com/opnsense/src/tar.gz/<full-commit-sha> \
  | tar -xzf - --strip-components=1 --include='*/sys/*' --include='*/share/mk/*'

# OPNsense kernel config: opnsense/tools config/26.7/SMP, minus the %%DEBUG%% line
# (GENERIC + options RSS etc.: the module must be built with the kernel's options)
fetch -o - https://raw.githubusercontent.com/opnsense/tools/master/config/26.7/SMP \
  | grep -v '%%DEBUG%%' > /usr/src/sys/amd64/conf/SMP
mkdir -p /usr/obj/SMP && cd /usr/src/sys/amd64/conf && config -d /usr/obj/SMP SMP

# module tree = OPNsense's e1000 driver + this repo's glue
sh driver/prepare.sh /usr/src /root/vcigb
cd /root/vcigb && make SYSDIR=/usr/src/sys KERNBUILDDIR=/usr/obj/SMP
```

**Rebuild after every OPNsense kernel update.** If the module doesn't load, the
I354 ports have no driver, but the box still boots and the serial console works.

## How it works

`prepare.sh` copies OPNsense's `sys/dev/e1000` into a separate module, renames the
`igb` driver to `vcigb`, makes it probe only the I354 SGMII functions (8086:1f41) with
a higher priority than `igb`, and adds three small hooks into `driver/vc_edge5x0.c`:

- The board is recognised by NVM words 0x06/0x07 = "Vc"/"5X".
- **Functions 0/1 (switches):** the link is treated as internal SerDes with a forced
  1000/full PCS link (no PHY). The switch is reached through the I354's own MDIO
  (MDIC, single-chip addressing) and brought up as a plain switch: port 4 SerDes
  power-up, MAC forced 1000/full, internal PHYs out of power-down, ports 0-4
  forwarding.
- **Functions 2/3 (GE1/GE2):** the 88E1514s sit on a bit-banged MDIO bus on SoC
  core-well GPIO 13 (MDIO) / 14 (MDC). Their reset is pin 4 of a PCA9557 at 0x18
  (active low) on a bit-banged I2C bus on GPIO 11/12. Only the PHY register access
  is rerouted; the rest is the stock igb SGMII/copper path.

Pitfalls found on the way, in case you port this elsewhere:

- coreboot leaves GPIO 13/14 in their native function. Writes to IO_SEL/LVL for a
  native pin are dropped, so USE_SEL must be set first (otherwise MDC stays an input
  and the PHYs never answer after a cold boot).
- The C2000 GPIO_LVL register reads output pins back as 0: keep a shadow copy.
- The vendor source's `relval = 0` for the PHY reset is the level left when the
  vendor driver unloads, not the run level. The reset is active low.

Debug knobs: `sysctl hw.vcwan` (GPIO registers, PCA9557 registers, MDIO scan of the
WAN bus) and `dev.vcigb.{0,1}.vc_mdio_addr/vc_mdio_reg/vc_mdio_val` (raw switch
registers), `dev.vcigb.{0,1}.vc_switch_init`.

## Tools

`tools/` has the helper scripts used during bring-up from a Linux PC on the serial
console (comments are in Italian): a logger that survives the CP2102N re-enumerating
on power cycles, scripts to run commands and transfer files over the console with
SHA-256 checks, and `porttest.sh` / `wantest.sh` that run on the box and report
link + ping for each jack.

## Credits and license

The register sequences and board wiring come from VeloCloud's GPL Linux sources
(the `igb_vc` and board/PCU drivers) as documented by the OpenWrt port at
<https://github.com/gallops-gannets/velo540openwrt>. Thanks also to the people in the
[modlog.net VeloCloud 520 thread](https://modlog.net/velocloud-520-to-opensource/) who
mapped this hardware first.

This repository is licensed under the GNU GPL v2 (`GPL-2.0-only`, see `LICENSE`). The
Intel e1000 sources that `prepare.sh` pulls from OPNsense are BSD-3-Clause and keep
their own headers; a built module combines both.
