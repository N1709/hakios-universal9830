# Safari favourites on a clean data volume

`/redwood/bin/safari_favourites` runs as mobile from
`com.redwood.safari-favourites`. It adds these approved links to Safari's current
Favourites folder once per data volume:

- TrollStore: https://github.com/opa334/TrollStore
- AltStore: https://altstore.io/
- SideStore: https://sidestore.io/
- Jesvi iOS: https://github.com/jesvi-eulah/Jesvi-iOS (requires access while private)

The first three URLs were checked against the projects' own sites on 2026-11-20.
The project URL is this repository's existing remote.

The helper uses WebBookmarks' normal save API under its sync lock. It never
opens or edits Bookmarks.db directly, changes the selected Favourites folder,
reorders/removes existing bookmarks, or clears iCloud sync state. Existing URLs
within that folder (including subfolders) are skipped. After successful saves it
re-reads the native collection and only then writes
`/var/mobile/Library/Preferences/.redwood-safari-favourites-v1`. The marker means
later deletions or edits by the user are respected. Partial failure leaves the
marker absent; the boot job retries every 60 seconds and skips completed URLs.

The selectors and signatures were verified from the actual 20F66 arm64 cache:
`WebBookmarkCollection +safariBookmarkCollection`, `+lockSync`, `+unlockSync`,
`-favoritesFolder`, `-descendantsOfBookmarkFolder:`, `-configuration`,
`-saveBookmark:`; `WBCollectionConfiguration -collectionType`; and
`WebBookmark -initWithTitle:address:parentID:collectionType:`. The older initializer
without collectionType does not exist in this cache. Required APIs are checked
at runtime, and root execution is refused to avoid using root's bookmark store.

Build integration compiles/signs the helper, copies it into the immutable image,
and installs the launch job. Our own generated binary is also in
`prebuilt/userland/safari_favourites` for the existing PREBUILT build path; it is
built solely from `userland/src/safari_favourites.m` with Foundation and contains
no bookmark database, account state, downloaded applications or credentials.

Validation so far: arm64 compile with `-Wall -Wextra -Werror`, launch plist lint,
shell syntax, source diff checks. Phone/native persistence testing still needed:
run the job as mobile, verify four links in Safari, run it twice without duplicate
entries, delete one after success and reboot (it must stay deleted). A clean
volume must create the links using only image files. Do not mark these phone
checks complete until observed.
