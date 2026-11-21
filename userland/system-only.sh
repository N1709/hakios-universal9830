#!/bin/bash
# The system-image step of userland/build.sh on its own (B, 2026-11-13): Files host staged into /Applications.
set -e
cd "$(dirname "$(readlink -f "$0")")"
source ../xnu/xnu-env.sh
IOS=../ios165
CACHE=$IOS/20F66__iPhone10,3_6
SILEO_BIN="chmod cp mkdir mv rm"
SYSTEM=1
cmp -s ../prebuilt/userland/LocalFilesHost obj/LocalFilesHost || cp ../prebuilt/userland/LocalFilesHost obj/
[ ! -f system.apfs ] || [ -f system.apfs.bak-nov7 ] || mv system.apfs system.apfs.bak-nov7
if [ -f $IOS/system.manifest ] && [ -f $CACHE/dyld_shared_cache_arm64 ] &&
	{ [ ! -f system.apfs ] || [ obj/stage0 -nt system.apfs ] || [ obj/LocalFilesHost -nt system.apfs ] ||
	  [ ../tools/storage/stage-provider.py -nt system.apfs ] || [ $IOS/system.manifest -nt system.apfs ] ||
	  [ $IOS/cryptex -nt system.apfs ] || [ $IOS/cryptex-app -nt system.apfs ] || [ -n "$SYSTEM" ]; }; then
	# what ios165/select-system.py has chosen since ios165/rootfs was extracted is not in it, and would not be
	# in the image either (so it was once with the audio plug-ins and the system's sounds: iOS found no
	# speaker and played nothing, though our device was there and the kernel's side worked)
	if [ -f $IOS/.done-rootfs ] && [ $IOS/select-system.py -nt $IOS/.done-rootfs ]; then
		echo "build.sh: ios165/select-system.py has changed since ios165/rootfs was extracted: run KEEP=1 ios165/fetch.sh" >&2
		exit 1
	fi
	rm -rf sysroot
	cp -al $IOS/rootfs sysroot                      # hard links: no copies of the 0.5 GB
	# launchd must load our choice of jobs, not all 500 that iOS has
	rm -rf sysroot/System/Library/LaunchDaemons sysroot/System/Library/NanoLaunchDaemons \
		sysroot/System/Library/LaunchAngels sysroot/System/Library/xpc/launchd.plist
	ln -s /redwood/LaunchDaemons sysroot/System/Library/LaunchDaemons
	mkdir -p sysroot/redwood sysroot/sbin
	# where the kernel mounts the iOS root container off the phone's flash (boot-arg apfs_volume=):
	# iOS has no /System/Volumes, and nothing can make a directory in this image once it is built
	mkdir -p sysroot/System/Volumes/iOS
	# the applications iOS installs on its first boot are system applications here (see select-system.py)
	if [ -d sysroot/private/var/staged_system_apps ]; then
		mv sysroot/private/var/staged_system_apps/* sysroot/Applications/
		rm -rf sysroot/private/var
	fi
	mv sysroot/sbin/launchd sysroot/sbin/launchd.ios
	cp obj/stage0 sysroot/sbin/launchd
	# the audio DSP's firmware, the phone's own from ../fw-audio (never in the repository), where the kernel reads
	# it once the root is mounted (bsd/dev/redwood_adsp.c, adsp=1): 20 MB that do not fit beside GL=1 in the small image
	if [ -f ../fw-audio/adsp.mdt ]; then
		mkdir -p sysroot/fw
		cp ../fw-audio/adsp.mdt ../fw-audio/adsp.b0[2-9] ../fw-audio/adsp.b[1-9][0-9] sysroot/fw/
	fi
	# x1slte: ABOX's Calliope DSP firmware (bsd/dev/redwood_exynos_sound.c, snd_calliope=1), the phone's /vendor/firmware
	# calliope_*.bin and abox_tplg.bin, from ../fw-sound (Samsung's: never committed). In the system image: 4 MB that do not
	# fit in the small one's 14.4 MB slot; the kernel reads them from /fw once iOS's volume is the root.
	for f in calliope_sram.bin calliope_dram.bin calliope_sram_2.bin calliope_dram_2.bin abox_tplg.bin; do
		if [ -f ../fw-sound/$f ]; then
			mkdir -p sysroot/fw
			install -m 0644 ../fw-sound/$f sysroot/fw/
		fi
	done
	# ... and the speaker amplifiers' configuration, the phone's /vendor/etc/aw882xx_acf.bin (bsd/dev/redwood_speaker.c,
	# for sound=3): Android's own register values, which are not in the repository either
	if [ -f ../fw-audio/aw882xx_acf.bin ]; then
		mkdir -p sysroot/fw
		cp ../fw-audio/aw882xx_acf.bin sysroot/fw/
	fi
	# the package manager's prefix, when tools/mk-jb.sh has fetched one: the Procursus bootstrap with
	# Sileo in it, as /jb. It cannot live at /var/jb in the image -- /private/var is a memory
	# filesystem made afresh at every boot -- so init copies it there (src/init.c, copy_tree).
	if [ -d ../jb/root/var/jb ]; then
		rm -rf sysroot/jb
		cp -a ../jb/root/var/jb sysroot/jb
		# the applications of the prefix are system applications here: LaunchServices builds its
		# database afresh at every boot by walking /Applications, which is how their icons appear
		for app in sysroot/jb/Applications/*.app; do
			[ -d "$app" ] && cp -a "$app" sysroot/Applications/
		done
		# Sileo moves, copies and removes files as root with /bin/mv, /bin/cp, /bin/rm, /bin/mkdir and
		# /bin/chmod -- where a jailbreak with a writable root keeps them -- and iOS's /bin has df and ps
		# alone: every one of those spawns failed with nothing said, and the package it had downloaded never
		# reached apt's archives ("Unable to fetch some archives"). The prefix's own programs, by those names.
		for c in $SILEO_BIN; do
			ln -sf /var/jb/usr/bin/$c sysroot/bin/$c
		done
		echo "  the package manager's prefix: $(du -sh sysroot/jb | cut -f1) as /jb (init puts it at /var/jb)"
	fi
	# An application of this port's own, registered the same free way the prefix's are: a plain
	# directory in /Applications, which the LaunchServices database is rebuilt from at every boot
	# by walking that directory -- no installd, no container, no uicache. cp -a and not a link,
	# because a bundle of symbolic links is refused ("Unable to register") and the boot-time
	# registrar passes over mount points.
	#
	# This is in the SYSTEM image and not the small one, so a change to it costs a 2.5 GB rebuild
	# and a re-upload: SYSTEM=1 is what makes this line run again.
	if [ -d obj/RedwoodTriangle.app ]; then
		cp -a obj/RedwoodTriangle.app sysroot/Applications/
		echo "  RedwoodTriangle.app in /Applications (com.redwood.triangle, scheme redwoodtriangle:)"
	fi
	# Applications from outside this tree: every .app in ../apps, which is kept out of the
	# repository the way ../fw-* and ../jb are, so that nothing of somebody else's package can
	# reach it. They go into /Applications beside the port's own on exactly the same terms --
	# a plain directory, which LaunchServices finds by walking that folder at every boot.
	#
	# cp -aL and not cp -a: a bundle made of symbolic links is refused ("Unable to register"),
	# so whatever a link points at is copied as the file itself.
	for app in ../apps/*.app; do
		[ -d "$app" ] || continue
		cp -aL "$app" sysroot/Applications/
		echo "  ${app##*/} in /Applications ($(du -sm "$app" | cut -f1) MB from ../apps)"
	done
	# our audio device, where the audio server looks for such plug-ins (and for no others: Apple's are left out)
	mkdir -p sysroot/System/Library/Audio/Plug-Ins/HAL
	ln -s /redwood/lib/RedwoodAudio.driver sysroot/System/Library/Audio/Plug-Ins/HAL/RedwoodAudio.driver
	# ... and our Metal plug-in, at the one path Metal's own discovery can open: step 3 composes
	# "/System/Library/Extensions/" + MetalPluginName + ".bundle" and opens it with NSBundle
	# (docs/metal.md 1). Same shape as the audio plug-in above -- a link into /redwood/lib, where
	# the bundle itself travels in the small image -- because this directory is iOS's own and
	# nothing can be added to it after the image is built.
	#
	# This link is inert on a boot that does not ask for Metal: nothing composes that path until
	# MetalPluginName is on the IOAcceleratorES service, and only boot-arg metal=1 lets a process
	# put it there (iokit/Kernel/MiniIOMobileFramebuffer.cpp). So it is not behind a switch of its
	# own -- a switch here would cost a 2.5 GB system.apfs rebuild and a re-upload every time it
	# was flipped, and buy nothing a missing boot-arg does not already buy.
	mkdir -p sysroot/System/Library/Extensions
	ln -s /redwood/lib/RedwoodMetal.bundle sysroot/System/Library/Extensions/RedwoodMetal.bundle
	# the OS cryptex: on a device a separate volume grafted at /System/Cryptexes/OS. It holds the dyld
	# shared cache (dyld looks there first) and a few libraries and resources outside the cache.
	mkdir -p sysroot/System/Cryptexes
	if [ -d $IOS/cryptex ]; then cp -al $IOS/cryptex sysroot/System/Cryptexes/OS; fi
	# the App cryptex, the same way: Safari's daemons (eight launchd jobs run them from there), its settings and
	# the bundles around it, which the root's links reach (System/Library/PreferenceBundles/MobileSafariSettings.bundle...)
	if [ -d $IOS/cryptex-app ]; then cp -al $IOS/cryptex-app sysroot/System/Cryptexes/App; fi
	D=sysroot/System/Cryptexes/OS/System/Library/Caches/com.apple.dyld
	mkdir -p $D
	for f in $CACHE/dyld_shared_cache_arm64*; do
		case "$f" in *.symbols) continue ;; esac     # symbolication data, never mapped
		# -f: a cryptex fetched with the cache already in it would otherwise stop the build
		ln -f "$f" $D/
	done
	# A containing app gives the local provider its data/app-group containers.
	# Relocate rather than duplicate it: a metadata-only standalone registration
	# can otherwise win PlugInKit election over the working app extension.
	python3 ../tools/storage/stage-provider.py sysroot obj/LocalFilesHost \
		$IOS/system.manifest obj/system-files.manifest
	# zlib where it saves room, as Apple keeps the root (a small file whole in its attribute): 5.4 GB of
	# files in 2.5 GB, what fits into the phone's RAM beside iOS
	python3 ../tools/apfs/mkapfs.py -z -o system.apfs System=sysroot,manifest=obj/system-files.manifest | tail -1
	[ -z "$CHECK" ] || ../tools/apfs/check-image.sh system.apfs System=sysroot,manifest=obj/system-files.manifest
	rm -rf sysroot
fi
ls -la system.apfs
