#!/bin/bash
# Checks the development environment for this fork and says what is missing.
# It never installs anything, starts Docker, or touches a TV. See AGENTS.md.
#
#   bash tools/dev-setup.sh
#
# Exit status: 0 when host tests can run, 1 when something they need is missing.
set -u
cd "$(dirname "$0")/.." || exit 1
need=0
ok()   { printf '  ok    %s\n' "$1"; }
miss() { printf '  MISS  %s\n        %s\n' "$1" "$2"; [ "${3:-}" = optional ] || need=1; }
has()  { command -v "$1" >/dev/null 2>&1; }
os=$(uname)

echo "Host tests"
for tool in cc python3 openssl curl perl; do
  if has "$tool"; then ok "$tool"; else miss "$tool" "install it with your package manager"; fi
done
if has rg; then ok "ripgrep (rg)"
else miss "ripgrep (rg)" "tests/mkvass_pgs.sh uses it: brew install ripgrep / apt-get install ripgrep"; fi
if [ "$os" = Darwin ]; then
  prefix=$(brew --prefix 2>/dev/null || echo /opt/homebrew)
  if [ -f "$prefix/include/libavformat/avformat.h" ]; then ok "FFmpeg headers ($prefix)"
  else miss "FFmpeg headers" "tests/moviebuffer.sh links host FFmpeg: brew install ffmpeg"; fi
  if [ -f "$prefix/include/SDL2/SDL.h" ]; then ok "SDL2 headers"
  else miss "SDL2 headers" "settings/UI tests need them: brew install sdl2" optional; fi
  if has timeout || has gtimeout; then ok "timeout"
  else miss "timeout" "only tools/testa-tudo.sh needs it (tools/test-host.sh does not): brew install coreutils" optional; fi
else
  if [ -f "${NUVIO_DTS_ROOT:-/tmp/nuvio-dts-host}/include/libavformat/avformat.h" ]; then ok "host FFmpeg (${NUVIO_DTS_ROOT:-/tmp/nuvio-dts-host})"
  else miss "host FFmpeg" "CC=gcc NUVIO_DTS_HOST=1 NUVIO_DTS_ROOT=/tmp/nuvio-dts-host bash tools/build-dts-ffmpeg.sh"; fi
fi

echo "webOS build"
if has docker && docker info >/dev/null 2>&1; then
  ok "docker daemon"
  if docker image inspect nuvio-webos-sdk >/dev/null 2>&1; then ok "image nuvio-webos-sdk"
  else miss "image nuvio-webos-sdk" "docker build --platform linux/arm64 -t nuvio-webos-sdk tools/" optional; fi
elif has docker; then
  miss "docker daemon" "not running; on macOS with OrbStack: orb start" optional
else
  miss "docker" "install OrbStack (macOS) or Docker to build the .ipk" optional
fi
if [ -f local.properties ]; then
  if git check-ignore -q local.properties; then ok "local.properties (git-ignored)"
  else miss "local.properties is NOT ignored" "fix .gitignore before committing anything"; fi
else
  miss "local.properties" "needed only to package an .ipk; ask the repo owner for it" optional
fi
if has ares-package || [ -n "${NUVIO_ARES_PACKAGE:-}" ]; then ok "ares-package"
else miss "ares-package" "webOS CLI (npm i -g @webos-tools/cli), or set NUVIO_ARES_PACKAGE" optional; fi

echo "Git"
if git remote get-url origin 2>/dev/null | grep -q 'iqui27/nuvio-native-legacy'; then ok "origin = upstream"
else miss "origin" "git remote add origin https://github.com/iqui27/nuvio-native-legacy.git" optional; fi
if git remote get-url fork >/dev/null 2>&1; then ok "fork = $(git remote get-url fork)"
else miss "fork remote" "git remote add fork https://github.com/maanya145/nuvio-native-legacy.git" optional; fi

[ $need -eq 0 ] && echo "Host tests can run: bash tools/test-host.sh" || echo "Fix the MISS lines above before running host tests."
exit $need
