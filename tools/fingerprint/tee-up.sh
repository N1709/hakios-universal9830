#!/bin/zsh
# tee-up.sh [FP-COMMAND...]: on a booted phone with the TEEGRIS kernel -- start TEEGRIS, the storage daemon, the driver
# TAs (as tzts_daemon does), power the sensor (`fp power`), then send each FP-COMMAND ("N[:OP] [HEX]", default "1:2" =
# PREPARE with op 2) to the fingerprint TA. Prints the result lines and the TA's own log lines.
S="/var/jb/usr/sbin/sysctl"
CMDS=("${@:-1:2}")
ssh s20 "ps -A -o comm= | grep -q teetsd || (nohup /var/root/teetsd >/dev/null 2>&1 &)
if ! $S kern.redwood.tee | grep -q 'state: up'; then $S -w kern.redwood.tee=start >/dev/null 2>&1; sleep 6
for f in /var/root/tee/driver/*; do $S -w kern.redwood.tee=\"openkeep \$f\" >/dev/null; sleep 3; done; fi
$S -w kern.redwood.tee='fp power' >/dev/null; sleep 1"
for c in $CMDS; do ssh s20 "$S -w kern.redwood.tee='fp $c' >/dev/null; sleep 4; $S kern.redwood.tee | grep last:"; done
curl -s -m 20 'http://10.77.0.2:8080/tail?log=0&dmesg=0' | grep -aE 'redwood-tee: fp|securefp|SW> .*(ERR|finger|FP|qbt|spi|SPI)' | tail -50 | cut -c1-230
