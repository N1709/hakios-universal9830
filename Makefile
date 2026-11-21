# Jesvi iOS: short names for the commands in ./build.sh and ./x1slte.sh. Run any of them yourself for options.
#
# Building (nothing here touches a phone)
#   make check           source checks: scripts parse, tests pass, no Apple/vendor files, no secrets (CI runs this)
#   make doctor          what this Mac still needs for a full build (toolchain, IPSW, firmware, UEFI)
#   make kernel          the XNU kernel for $(DEVICE)          -> out/kernel/mach.$(DEVICE)
#   make images          root.apfs + system.apfs from your own IPSW (userland/build-gpu-y2s.sh)
#   make release         every output release.conf asks for   -> dist/release/ (TWRP install ZIP, restore ZIP)
#   make restore         only the Jesvi-iOS-Restore ZIP (back to stock Android; no Apple inputs)
#   make devices         the devices this tree knows of, and how far each one works
#
# Developing on the phone (the device's own loop, see docs/release/building.md)
#   make dev-loop        rebuild the boot payload with the working boot line and pack it (./x1slte.sh loop)
#
# Without Apple's software at all
#   make qemu-open       the open kernel + a placeholder loader + the `hello` RAM disk, in QEMU
#
#   DEVICE=x1slte        the target under devices/exynos990/ (default x1slte)
#   JOBS=4               parallel kernel jobs
#   SECS=600             QEMU session length

DEVICE ?= x1slte
JOBS ?= 4
SECS ?= 600
ARGS_OPEN ?= cpus=1

.PHONY: help check doctor kernel images release restore devices dev-loop loader-open hello qemu-open clean

help:
	@sed -n '2,21p' Makefile | sed 's/^# \{0,1\}//'

check:
	bash ci/check.sh
	python3 tools/release/audit.py --output out/publication-audit.json

doctor:
	./build.sh doctor --device $(DEVICE) --images

kernel:
	./build.sh kernel --device $(DEVICE) --jobs $(JOBS)

images:
	bash userland/build-gpu-y2s.sh

release:
	./build.sh release --set TARGETS=$(DEVICE)

restore:
	./build.sh release --set TARGETS=$(DEVICE) --set OUTPUTS=restore

devices:
	./build.sh devices

dev-loop:
	./x1slte.sh loop

# The Apple-free path: no IPSW, no iOS user space; what an outside contributor can build and run.
loader-open:
	PLACEHOLDER=1 xnuloader/mkidentity.sh
	xnuloader/build.sh

hello:
	. ./xnu/xnu-env.sh && xnu-clang -arch arm64 -Os -static -nostdlib -ffreestanding \
		-fno-stack-protector -fno-builtin -Wl,-e,_start -Wl,-pagezero_size,0x100000000 \
		-o userland/hello userland/src/hello.c

qemu-open: loader-open hello
	[ -d xnu-src/xnu-8796.121.2 ] || xnu/apply.sh
	[ -x xnu-deps/host/bin/xnu-clang ] || xnu-deps/setup.sh
	./xnu/xnu-build.sh -j$(JOBS) -k
	mkdir -p logs
	xnuloader/mkpayload.py -o qemu/payload.bin \
		kernel=xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837 \
		ramdisk=userland/hello --cmdline "$(ARGS_OPEN)"
	qemu/run.sh $(SECS) | tee logs/qemu-open.log

clean:
	rm -rf out/kernel xnuloader/loader.obj xnuloader/loader.efi userland/obj
