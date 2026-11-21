#!/bin/bash
# Disposable GitHub macOS setup only; local/self-hosted builders keep their tools.
set -euo pipefail
[[ ${GITHUB_ACTIONS:-} == true && ${RUNNER_ENVIRONMENT:-} == github-hosted && $(uname -s) == Darwin ]] || {
  echo 'This setup is only for disposable GitHub-hosted macOS runners.' >&2; exit 1;
}

# Keep the selected Xcode. Reclaim other preinstalled versions and simulator data,
# which this command-line iOS cross-build never uses. Never run this on a user's Mac.
python3 - <<'PY'
from pathlib import Path
import subprocess
selected = Path(subprocess.check_output(['xcode-select', '-p'], text=True).strip()).resolve()
for app in Path('/Applications').glob('Xcode*.app'):
    if not app.is_symlink() and app.resolve() not in selected.parents:
        subprocess.run(['sudo', 'rm', '-rf', str(app)], check=True)
sim = Path('/Library/Developer/CoreSimulator')
if sim.is_dir():
    subprocess.run(['sudo', 'rm', '-rf', str(sim)], check=True)
PY
brew install coreutils gnu-tar gnu-sed findutils bison m4 sevenzip llvm lld ldid \
  ninja meson cmake pkg-config autoconf automake libtool zstd
brew install blacktop/tap/ipsw
export PATH="$(brew --prefix llvm)/bin:$(brew --prefix lld)/bin:$PATH"
set +u
source common/macos-env.sh
set -u
mkdir -p "$RUNNER_TEMP/image-tools"
if ! command -v 7z >/dev/null; then ln -s "$(command -v 7zz)" "$RUNNER_TEMP/image-tools/7z"; fi
export PATH="$RUNNER_TEMP/image-tools:$PATH"
python3 -m venv "$RUNNER_TEMP/image-python"
"$RUNNER_TEMP/image-python/bin/pip" install capstone==5.0.7 cryptography lz4 pyelftools
export PATH="$RUNNER_TEMP/image-python/bin:$PATH"
printf '%s\n' "$PATH" >> "$GITHUB_PATH"
printf 'HACKIOS_IMAGE_CACHE=%s/private-images\n' "$RUNNER_TEMP" >> "$GITHUB_ENV"
python3 - <<'PY'
import shutil
free = shutil.disk_usage('.').free / 1024**3
print(f'Hosted disk free after tool setup: {free:.1f} GiB')
if free < 45:
    raise SystemExit('Full images need at least 45 GiB free after setup; this runner is too small. No partial package will be uploaded.')
PY
# Also prepares the pinned SDK and lzbitmap sources needed by sysimage/fetch.sh.
bash xnu-deps/setup.sh --stage1
