Mods every Exynos 990 target gets, applied first. One folder per mod:
`module.prop` (id=<folder>, name=, description=, optional targets="x1slte y2s",
requires="TARGET_HAS_SPEN"), and any of `bootargs`, `root/`, `system/`,
`customize.sh` (runs with the merged TARGET_*/PLATFORM_* config, STAGE=root|system,
DEST, MODPATH), `disable`. See tools/release/targets.py.
