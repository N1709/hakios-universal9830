# Security reporting

Do not post secrets, personal identifiers, EFS dumps or unredacted modem logs in
public issues. Use the repository's private vulnerability reporting facility once
it has been enabled. If it is unavailable, ask the maintainer for a private channel
without including the sensitive details in the request.

This is experimental system software with privileged services. It has not been
qualified as a secure daily-use OS. Report the exact source commit, device model,
impact and a minimal sanitized reproduction. Debug services, SSH host keys and
network credentials need explicit review before a release.

Source CI has read-only repository access. Private images and firmware must never
enter public Actions artifacts or caches, and untrusted PR jobs must never run on
a worker holding private device inputs or connected hardware.
