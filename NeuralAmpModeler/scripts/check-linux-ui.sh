#!/usr/bin/env bash
set -euo pipefail

package="$1"
host="$2"
unpacked="$(mktemp -d)"
trap 'rm -rf "$unpacked"' EXIT
dpkg-deb -x "$package" "$unpacked"

standalone="$unpacked/usr/bin/nam-on-steroids"
module="$(find "$unpacked/usr/lib/vst3" -type f -name '*.so' -print -quit)"
test -x "$standalone"
test -f "$module"

xvfb-run -a -s '-screen 0 1280x800x24' env GDK_BACKEND=x11 bash -euo pipefail -c '
  app="$1"
  log="$2"
  "$app" >"$log" 2>&1 &
  app_pid=$!
  trap "kill $app_pid 2>/dev/null || true; wait $app_pid 2>/dev/null || true" EXIT
  if ! window="$(timeout 20s xdotool search --sync --onlyvisible --limit 1 --name "^NAM On Steroids$")"; then
    cat "$log"
    echo "The installed standalone did not open its X11 window." >&2
    exit 1
  fi
  kill -0 "$app_pid"
  if [[ -n "${NAM_LINUX_EDITOR_PREVIEW:-}" ]]; then
    import -window "$window" "$NAM_LINUX_EDITOR_PREVIEW"
  fi
  echo "Packaged standalone opened its graphical window over X11"
' nam-standalone-check "$standalone" "$unpacked/standalone.log"

xvfb-run -a -s '-screen 0 1280x800x24' env GDK_BACKEND=x11 timeout 30s "$host" "$module"
