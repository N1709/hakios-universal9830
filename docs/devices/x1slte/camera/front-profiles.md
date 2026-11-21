# Optional front-camera comparison profile

2026-11-14. **Built/host-checked; not installed while waiting for device readiness.**

User confirmed the front returned to normal after removing the development
library. Keep the original front as default. The user requests a flag for the
newer experimental front path, independently of rear research. Experimental
is not verified Samsung factory tuning. Display brightness/ALS behaviour is
separate and is unchanged; the70% override request was withdrawn.

## Implementation

`tools/camera/build-profile-media.sh OUTPUT` builds two isolated ISP backends.
`prepare-profile-sources.py` obtains original sources from pinned ancestor
`d5b02944ec77e3d63dd550db8af0780a42d8b973`. The original device factory, object
ownership, stream start/stop and cross-camera exclusion remain shared. At the
worker-body boundary only:

- Rear uses the current experimental worker.
- Front defaults to the original worker and original ISP implementation.
- Front opts into the experimental worker only when the active tuning directory's
  `redwood-front-profile` contains `experimental`.

Selection is per stream, not during a frame. Both backend implementations have
separate exported ISP symbols and black state. The generated original ISP source
and header are byte-identical to the pinned commit. The build compares complete
stream/output/camera/device declarations (ignoring comments/whitespace) and
refuses a changed shared ABI. Experimental private-framework bindings initialize
with pthread_once; the original stream lifecycle continues to own mutual
exclusion and worker shutdown. No mixed device arrays or orphaned stream objects
are created. Still needs a live front/rear/switching regression test.

`build-profile-media.sh` creates the signed dual-profile library and a
`profile-build.json` manifest. Current local candidate SHA-256:
`ea002ab83015bab7bdcd3cf59e85971567b43918c881bb05d634b25e61b1fbd2`.
Existing code has two CoreVideo deprecation warnings; compile/link succeeds.
Two host source/layout tests pass and both original/experimental symbols are
present. These checks do not substitute for visual/device validation.

## Switch after installing the dual-profile library

Install its manifest as `/var/redwood/camera-profile-build.json` alongside the
usual reversible development override. Do not enable the rejected dark-grid
profile during this comparison.

```sh
python3 tools/camera/front-profile.py status
python3 tools/camera/front-profile.py original
python3 tools/camera/front-profile.py experimental
```

Close/reopen Camera to start the selected worker. The tool changes no rear
profile, does not restart processes, and never reboots. It refuses changes if
the dual-profile install manifest is absent. Missing/unknown flag defaults to
original. Worker logs identify front/rear and selected backend. Camera opens on
front by existing user preference: explicitly select rear for every app test;
confirm sensor dimensions/register logs rather than inferring from app launch.

## Rollback

Remove/rename `/var/redwood/libredwood-media-dev.dylib` and restart mediaserverd
with Camera closed. This restores the installed original system library, as
already visually confirmed by the user. No protected data/root image/kernel
change is required. Keep original front as default until the optional profile
is explicitly selected by the user.
