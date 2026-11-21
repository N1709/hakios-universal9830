#!/bin/zsh
# hotspot-up.sh start [NAME [CHANNEL [PASSWORD]]] | stop | status -- the S20's Personal Hotspot, by hand (A, 2026-11-19).
#
#   cellular  CLAT46 on the data tunnel (Jio is IPv6-only): NAT64 prefix 64:ff9b::/96, 192.0.0.2, IPv4 default route
#   Wi-Fi     the chip as an access point (kern.redwood.wifi_ap), the phone 172.20.10.1/28 on en0
#   clients   rwdhcpd (addresses), rwpf (NAT from 172.20.10.0/28 to 192.0.0.2), IPv4 forwarding
#
# With a PASSWORD (8-63 characters): WPA2, the driver doing the handshakes. Tools in /redwood/bin (rwdhcpd, rwpf, clat-test) and
# /var/jb (ifconfig, route: Procursus network-cmds).
setopt err_return 2>/dev/null
export PATH=/var/jb/usr/bin:/var/jb/bin:/var/jb/usr/sbin:/var/jb/sbin:/usr/bin:/bin:/usr/sbin:/sbin	# (misd starts it with none)
B=/redwood/bin		# (the image's; /var/root by hand before 2026-11-19)
IFC=/var/jb/usr/sbin/ifconfig
RT=/var/jb/usr/sbin/route
SC=/var/jb/usr/sbin/sysctl
NAME=${2:-JesviTest}
CHAN=${3:-6}
PASS=${4:-}

cell_if() {	# the data tunnel: the utun holding the cellular service's address (redwood-cellular.log)
	grep -a "configd service redwood-cell published" /var/tmp/redwood-cellular.log | tail -1 | grep -oE "on utun[0-9]+" | cut -d' ' -f2
}

dhcpd_pid() {
	ps -ax -o pid=,command= | grep "$B/rwdhcpd en0" | grep -v grep | head -1 | sed 's/^ *//; s/ .*//'
}

case $1 in
start)
	C=$(cell_if)
	[ -n "$C" ] || { echo "hotspot: no cellular data tunnel (is mobile data up?)"; return 1; }
	$IFC $C | grep -q clat46 || $B/clat-test start $C 64:ff9b::/96 | grep -E "CLAT46|NAT64"
	$IFC $C | grep -q "inet 192.0.0.2" || $IFC $C inet 192.0.0.2 192.0.0.1 netmask 255.255.255.255
	$RT -n delete -inet default >/dev/null 2>&1
	$RT -n add -inet default -interface $C >/dev/null
	$SC -w kern.redwood.wifi_ap="start	$CHAN	$NAME${PASS:+	$PASS}" >/dev/null || { echo "hotspot: $($SC -n kern.redwood.wifi_ap)"; return 1; }
	echo "hotspot: $($SC -n kern.redwood.wifi_ap)"
	$IFC en0 inet 172.20.10.1 netmask 255.255.255.240 alias
	[ -n "$(dhcpd_pid)" ] || (nohup $B/rwdhcpd en0 8.8.8.8 >>/var/tmp/rwdhcpd.log 2>&1 </dev/null &)
	$B/rwpf nat $C 192.0.0.2 172.20.10.0/28
	MODE=open; [ -z "$PASS" ] || MODE=WPA2
	echo "hotspot: \"$NAME\" on channel $CHAN ($MODE), internet through $C"
	;;
stop)
	$B/rwpf flush
	p=$(dhcpd_pid); [ -z "$p" ] || kill $p
	$IFC en0 inet 172.20.10.1 -alias 2>/dev/null
	$SC -w kern.redwood.wifi_ap=stop >/dev/null
	echo "hotspot: off (Wi-Fi back to client mode; CLAT46 stays on for the phone's own IPv4)"
	;;
net-start)	# iOS's own switch already put the access point on the air (wifid): the network behind it (libredwood-mis)
	C=$(cell_if)
	[ -n "$C" ] && $IFC $C >/dev/null 2>&1 || { echo "hotspot: no cellular data tunnel yet (is mobile data up?)"; exit 1; }
	$IFC $C | grep -q clat46 || $B/clat-test start $C 64:ff9b::/96 | grep -E "CLAT46|NAT64"
	$IFC $C | grep -q "inet 192.0.0.2" || $IFC $C inet 192.0.0.2 192.0.0.1 netmask 255.255.255.255
	$RT -n delete -inet default >/dev/null 2>&1
	$RT -n add -inet default -interface $C >/dev/null
	$IFC en0 | grep -q "inet 172.20.10.1 " || $IFC en0 inet 172.20.10.1 netmask 255.255.255.240 alias
	$RT -n add -host 255.255.255.255 -interface en0 >/dev/null 2>&1
	[ -n "$(dhcpd_pid)" ] || (nohup $B/rwdhcpd en0 8.8.8.8 >>/var/tmp/rwdhcpd.log 2>&1 </dev/null &)
	$B/rwpf nat $C 192.0.0.2 172.20.10.0/28
	# Apple's "this is a phone's hotspot" element (devices then treat it as mobile data). Built in (a real iPhone's,
	# 2026-11-19); /var/db/redwood-hotspot-ie overrides it, an empty file turns it off.
	VIE=0017f206010103030000
	[ ! -f /var/db/redwood-hotspot-ie ] || VIE=$(cat /var/db/redwood-hotspot-ie)
	[ -z "$VIE" ] || $SC -w kern.redwood.wifi_ap_vendor_ie="$VIE" >/dev/null
	echo "hotspot: network up, internet through $C"
	;;
net-stop)
	$B/rwpf flush
	p=$(dhcpd_pid); [ -z "$p" ] || kill $p
	$IFC en0 inet 172.20.10.1 -alias 2>/dev/null
	echo "hotspot: network down"
	;;
status)
	$SC kern.redwood.wifi_ap
	echo "dhcpd: ${$(dhcpd_pid):-not running}; forwarding: $($SC -n net.inet.ip.forwarding)"
	;;
*)
	echo "usage: hotspot-up.sh start [NAME [CHANNEL [PASSWORD]]] | stop | net-start | net-stop | status"; return 2 ;;
esac
