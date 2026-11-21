# Jesvi IOS device identity and development defaults

Fresh builds seed the computer/display name **Jesvi IOS** and the DNS-safe
local hostname **Jesvi-IOS** in the normal SystemConfiguration preferences.
The existing init skeleton copies this seed only when the preferences file
does not exist, preserving later user names and network configuration.
The USB product string and diagnostic Bonjour service name use Jesvi IOS.

For an existing data volume, run `/redwood/bin/device_name --set-default` as
root. This calls configd's name APIs, commits and applies the change while
preserving other network settings. Running without arguments only inspects.
An entitled MobileGestalt read on the live phone confirmed UserAssignedDeviceName
as Jesvi IOS. Unentitled apps may still receive “iPhone” under iOS name privacy;
DeviceName is the model description and is not rewritten.

The AMFI stand-in exposes read-only `security.mac.amfi.developer_mode_status`
and `security.mac.amfi.developer_mode_resolved` as 1 from boot, reflecting the
research kernel's existing development-code policy. This avoids asking native
amfid to arm a hardware-backed mode that this port does not implement.
A temporary amfid-only test first verified the cause. The permanent kernel
then booted with both OIDs equal to 1, no temporary amfid injection, and the
native TrollStore check returning 0. A debugging-entitled test IPA installed
with exit 0 after that reboot and was removed. Developer Mode therefore needs
no per-boot toggle in this port.

Native USB mux now supplies the iPhone-style Mac discovery and trust path,
alongside the existing serial and Ethernet functions. The user accepted Trust,
and the Mac's native pairing, session validation and AFC file-sharing checks
succeeded. Finder shows Jesvi IOS, model iPhone X and storage. See
`tools/usb/README.md` for the transport and tests. The original management panel reported Activation Failed. The user later
opted into a Finder-only local compatibility policy; it opens the management
tabs while the native activation API outside lockdownd stays Unactivated.
Backup/encryption readiness and media categorization remain separate limits;
no successful end-to-end backup or media sync is claimed.

The intermittent cold Safari error is deferred at the user's request. Normal
DNS and HTTPS probes succeeded (DNS ~0.03 s; HTTPS ~0.06–0.09 s), which does
not establish what fails in a cold WebKit process. No speculative Safari or
network workaround has been applied.

## Apple Account and iCloud

These USB changes do not establish native Apple Account/iCloud support. The
live native activation API returned `Unactivated`. The existing activation
chain still lacks supported Secure Enclave/UIK operations and an Apple-issued
device identity. Full sign-in, push-backed synchronization and iCloud services
remain unverified and blocked beyond the USB transport. Seeing iCloud Drive in
Files alone does not establish account authentication or synchronization.
