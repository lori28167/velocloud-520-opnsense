#!/bin/sh
# Gira sul VeloCloud. Test porte WAN GE1/GE2 (vcigb2/vcigb3): link dal PHY 88E1514 e ping verso il PC
# (il PC ha 10.99.2.2 e 10.99.3.2).  uso: sh wantest.sh [secondi]
END=$(( $(date +%s) + ${1:-300} )); prev=""; ok=""
ifconfig vcigb2 inet 10.99.2.1/24 up; ifconfig vcigb3 inet 10.99.3.1/24 up
echo "WANTEST avviato"
while [ "$(date +%s)" -lt "$END" ]; do
	cur=""
	for n in 2 3; do ifconfig vcigb$n | grep -q "status: active" && cur="$cur $n"; done
	if [ "$cur" != "$prev" ]; then
		[ -n "$cur" ] && sleep 3
		for n in $cur; do
			media=$(ifconfig vcigb$n | sed -n 's/.*media: Ethernet autoselect (\(.*\))/\1/p')
			if out=$(ping -c 5 -t 8 -q 10.99.$n.2 2>&1); then
				rtt=$(echo "$out" | sed -n 's|.*= [0-9.]*/\([0-9.]*\)/.*|\1|p'); res="ping OK (${rtt} ms)"
				case " $ok " in *" $n "*) ;; *) ok="$ok $n";; esac
			else res="ping FALLITO"; fi
			echo "RISULTATO vcigb$n: link $media, $res"
		done
		[ -z "$cur" ] && echo "WANTEST nessun link"
		prev=$cur
	fi
	[ $(echo $ok | wc -w) -eq 2 ] && { echo "WANTEST COMPLETATO: GE1 e GE2 OK"; exit 0; }
	sleep 1
done
echo "WANTEST scaduto, OK:$ok"
