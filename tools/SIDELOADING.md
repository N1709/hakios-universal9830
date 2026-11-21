# S20 sideloading checkpoint — 2026-11-11

Installed official TrollStore 2.1.1 and opa334 ldid v2.1.5-procursus7-new on the
live iOS 16.5 / 20F66 system. Existing Sileo remains installed.

Sources:
https://github.com/opa334/TrollStore/releases/tag/2.1.1
https://github.com/opa334/ldid/releases/tag/v2.1.5-procursus7-new

Official downloads are saved outside the kernel repo in work/sideloading.
The phone lacks gzip and /Applications is read-only. Decompressed the tar on
the Mac, extracted into /var/root/trollstore-bootstrap, then invoked the
unmodified official trollstorehelper install-trollstore command as root.
Its placeholder install failed, but its built-in MCMAppContainer fallback
succeeded with exit 0. App registered as System, with a data container.
Installed ldid using the helper's install-ldid command (exit 0).

Bundle: com.opa334.TrollStore
App: /private/var/containers/Bundle/Application/D69A535A-8959-49B4-84C5-82CBEE707301/TrollStore.app
Data: /private/var/mobile/Containers/Data/Application/D5DEDC96-D285-4A61-B7B6-BE5EAE05D3AC

Opened with uiopen --bundleid com.opa334.TrollStore. Process remained running
(PID 499), registration and signing-helper presence verified. Evidence:
logs/sideloading/trollstore-20261111.txt. No third-party IPA installation or
post-reboot persistence test performed at this checkpoint.

Usage: download an IPA compatible with iOS 16.5 and arm64; share it from Files
to TrollStore, or use TrollStore's + import control. TrollStore is an IPA
installer, not an App Store catalogue. Use independently obtained legitimate
IPAs; App Store DRM-encrypted downloads are not made runnable by this setup.
Some apps depend on hardware/services absent in this port.

If registration is lost after rebuilding the icon cache, SSH as root and run:
uicache -s -p /private/var/containers/Bundle/Application/D69A535A-8959-49B4-84C5-82CBEE707301/TrollStore.app
uiopen --bundleid com.opa334.TrollStore

Do not overwrite a system app with the persistence helper on this read-only
root. Preserve the registered container and data on the writable /var volume.
