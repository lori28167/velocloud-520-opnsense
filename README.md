# OPNsense su VeloCloud Edge 520

Driver FreeBSD `if_vcigb` che fa funzionare su OPNsense 26.7 tutte le porte del
VeloCloud Edge 520 (scheda rev 2.8, Atom C2358):

| Porte | Interfaccia | Chip |
|---|---|---|
| SFP1/SFP2 | `igb0`, `igb1` | Intel I350 (driver standard) |
| LAN5-8 | `vcigb0` | switch A Marvell 88E6176 dietro I354 func 0 |
| LAN1-4 | `vcigb1` | switch B Marvell 88E6176 dietro I354 func 1 |
| GE1 | `vcigb2` | PHY Marvell 88E1514, MDIO bit-bang su GPIO |
| GE2 | `vcigb3` | PHY Marvell 88E1514, MDIO bit-bang su GPIO |

Mappa LAN (porta dello switch): LAN1=B1 LAN2=B0 LAN3=B3 LAN4=B2 LAN5=A2 LAN6=A0 LAN7=A1 LAN8=A3.
Gli switch funzionano come due switch "stupidi" da 4 porte: ogni gruppo condivide 1 Gbit verso la CPU.

## Come funziona

`if_vcigb` è una copia del driver e1000/igb di OPNsense, rinominata, che si aggancia solo
alle I354 (8086:1f41) con priorità più alta di `igb`. Le aggiunte sono in `driver/vc_edge5x0.c`:

- riconosce la scheda dalle parole NVM 0x06/0x07 = "Vc"/"5X";
- func 0/1: SerDes forzata 1000/full senza PHY, accesso MDIO agli switch tramite MDIC,
  avvio degli switch (`dev.vcigb.N.vc_switch_init=1`, oppure `hw.vcigb.switch_autoinit=1` al boot);
- func 2/3: GPIO del SoC (base in LPC 0:31:0 reg 0x48), I2C bit-bang su GPIO 11/12 verso il
  PCA9557@0x18 (pin 4 = reset dei 88E1514, attivo basso), MDIO bit-bang su GPIO 13/14;
  il resto del percorso SGMII/rame è quello standard di igb.

Debug: `sysctl hw.vcwan` (registri GPIO, PCA9557, scansione MDIO) e `dev.vcigb.{0,1}.vc_mdio_*`.

Riferimenti: driver GPL originale VeloCloud e port OpenWrt
<https://github.com/gallops-gannets/velo540openwrt> (sequenze dei registri). Il codice qui
deriva da quelle sequenze: tenerne conto prima di pubblicarlo.

## Configurazione del VeloCloud (`/boot/loader.conf.local`)

```
console="comconsole"
comconsole_port="0x2f8"              # la mini-USB è COM2
comconsole_speed="115200"
hw.uart.console="io:0x2f8,br:115200"
ichwd_load="YES"                     # ferma il watchdog TCO (riavvio dopo 3 min)
if_vcigb_load="YES"
hw.vcigb.switch_autoinit="1"
```

Il modulo sta in `/boot/modules/if_vcigb.ko`.

## Ricompilare

Il modulo va compilato contro i sorgenti esatti del kernel OPNsense installato
(`uname -v` sul box, es. `stable/26.7-n283674-12334a596709`) con la configurazione `SMP`
di opnsense/tools (GENERIC + `options RSS` ...). Nella VM FreeBSD 15.1:

```
# sorgenti: https://codeload.github.com/opnsense/src/tar.gz/<commit> -> /usr/src (sys/ e share/mk/)
# config:   opnsense/tools config/26.7/SMP -> /usr/src/sys/amd64/conf/SMP (senza la riga %%DEBUG%%)
mkdir -p /usr/obj/SMP && cd /usr/src/sys/amd64/conf && config -d /usr/obj/SMP SMP
sh driver/prepare.sh /usr/src /root/vcigb
cd /root/vcigb && make SYSDIR=/usr/src/sys KERNBUILDDIR=/usr/obj/SMP
```

**Dopo ogni aggiornamento del kernel di OPNsense il modulo va ricompilato.** Se non si
carica, le I354 restano senza driver ma le SFP (`igb0`/`igb1`) funzionano comunque.

## Strumenti (`tools/`)

Dal PC, con la mini-USB su `/dev/ttyUSB0` e picocom chiuso:

- `con-reader` (in background) registra la console in `tools/con.log`;
- `send`, `rsh` mandano comandi / script sh al VeloCloud;
- `push` trasferisce un file via seriale verificando lo SHA-256;
- `porttest.sh`, `wantest.sh` girano sul VeloCloud e testano link + ping delle porte;
- `vssh` entra nella VM di compilazione (QEMU, localhost:2222).

## Stato

- LAN1-8: link 1000 full su tutte; ping OK su LAN1-7 (LAN8 da ritestare con il test corretto).
- GE1/GE2: link 1000baseT full, ping OK.
- Watchdog fermato da `ichwd`.
- Wi-Fi (QCA988x): ath10k esiste in FreeBSD ma LinuxKPI non supporta la modalità AP.
