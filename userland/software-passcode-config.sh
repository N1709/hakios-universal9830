# Sourced from userland/build.sh after changing into userland/.
# Preserve explicit overrides. Unprofiled builds in this S20 checkout follow
# its tracked boot line; an explicit other DEVICE keeps its existing behavior.
if [ -z "${SOFTWARE_PASSCODE+x}" ]; then
	SOFTWARE_PASSCODE=0
	if [ "${DEVICE:-x1slte}" = x1slte ] &&
	    grep -Eq '(^|[[:space:]])softpasscode=1([[:space:]]|$)' ../devices/exynos990/x1slte/x1slte.cmdline; then
		SOFTWARE_PASSCODE=1
	fi
fi
case "$SOFTWARE_PASSCODE" in
0|1) ;;
*) echo 'build.sh: SOFTWARE_PASSCODE must be 0 or 1' >&2; exit 1 ;;
esac
