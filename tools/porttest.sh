#!/bin/sh
# Gira sul VeloCloud. Test porte LAN: rileva il link dai registri 88E6176 e fa ping verso il PC
# (il PC ha 10.99.0.2 e 10.99.1.2 sulla porta collegata).  uso: sh porttest.sh [secondi] ["porte richieste"]
# Mappa (dal port OpenWrt): LANn = vcigbN porta P dello switch
MAP="1:1:1 2:1:0 3:1:3 4:1:2 5:0:2 6:0:0 7:0:1 8:0:3"
END=$(( $(date +%s) + ${1:-600} )); WANT="${2:-1 2 3 4 5 6 7 8}"
ifconfig vcigb0 inet 10.99.0.1/24 up; ifconfig vcigb1 inet 10.99.1.1/24 up
sysctl dev.vcigb.0.vc_switch_init=1 dev.vcigb.1.vc_switch_init=1 >/dev/null
prev=""; ok=""
st_of() { sysctl -q dev.vcigb.$1.vc_mdio_addr=$((16 + $2)) dev.vcigb.$1.vc_mdio_reg=0 >/dev/null; sysctl -n dev.vcigb.$1.vc_mdio_val; }
echo "PORTTEST avviato"
while [ "$(date +%s)" -lt "$END" ]; do
	cur=""
	for m in $MAP; do
		lan=${m%%:*}; r=${m#*:}; n=${r%%:*}; p=${r#*:}
		st=$(st_of $n $p)
		[ $((st & 0x800)) -ne 0 ] && cur="$cur $lan"
	done
	if [ "$cur" != "$prev" ]; then
		[ -n "$cur" ] && sleep 3
		for m in $MAP; do
			lan=${m%%:*}; r=${m#*:}; n=${r%%:*}; p=${r#*:}
			case " $cur " in *" $lan "*) ;; *) continue;; esac
			st=$(st_of $n $p)
			case $(( (st >> 8) & 3 )) in 0) sp=10;; 1) sp=100;; 2) sp=1000;; *) sp="?";; esac
			[ $(( (st >> 10) & 1 )) -eq 1 ] && dx=full || dx=half
			sw=$([ $n -eq 0 ] && echo A || echo B)
			if out=$(ping -c 5 -t 8 -q 10.99.$n.2 2>&1); then
				rtt=$(echo "$out" | sed -n 's|.*= [0-9.]*/\([0-9.]*\)/.*|\1|p')
				res="ping OK (${rtt} ms)"; case " $ok " in *" $lan "*) ;; *) ok="$ok $lan";; esac
			else
				res="ping FALLITO"
			fi
			echo "RISULTATO LAN$lan = switch $sw porta $p (vcigb$n): link $sp $dx, $res"
		done
		[ -z "$cur" ] && echo "PORTTEST nessun link"
		prev=$cur
	fi
	done_all=1; for w in $WANT; do case " $ok " in *" $w "*) ;; *) done_all=0;; esac; done
	[ $done_all -eq 1 ] && { echo "PORTTEST COMPLETATO: porte$ok OK"; exit 0; }
	sleep 1
done
echo "PORTTEST scaduto, porte OK:$ok"
