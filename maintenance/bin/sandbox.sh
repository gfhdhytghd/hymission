#!/bin/bash
# sandbox.sh [--rw DIR]... [--ro DIR]... [--dev NODE]... [--env K=V]... -- COMMAND...
# Bubblewrap jail for building and running code that is not the user's own: read-only system, no network,
# no home directory, private /tmp and /run, a minimal /dev without GPU nodes. Nothing inside can reach the
# live compositor (no WAYLAND_DISPLAY, no host runtime dir) or the user's files beyond the paths given.
set -euo pipefail
args=(--ro-bind /usr /usr --ro-bind /etc /etc --ro-bind /sys /sys --symlink usr/lib /lib --symlink usr/lib64 /lib64
      --symlink usr/bin /bin --symlink usr/bin /sbin --proc /proc --dev /dev --tmpfs /tmp --tmpfs /run --tmpfs /var/tmp
      --dir "/run/user/$(id -u)" --unshare-all --die-with-parent --new-session
      --clearenv --setenv PATH /usr/local/bin:/usr/bin:/bin --setenv HOME /tmp/home --dir /tmp/home
      --setenv XDG_RUNTIME_DIR "/run/user/$(id -u)" --setenv LANG C.UTF-8 --setenv TERM dumb)
[ -d /opt ] && args+=(--ro-bind /opt /opt)
while [ $# -gt 0 ]; do
  case "$1" in
    --rw) args+=(--bind "$2" "$2"); shift 2 ;;
    --ro) args+=(--ro-bind "$2" "$2"); shift 2 ;;
    --env) args+=(--setenv "${2%%=*}" "${2#*=}"); shift 2 ;;
    --dev) args+=(--dev-bind "$2" "$2"); shift 2 ;;
    --) shift; break ;;
    *) echo "sandbox.sh: unknown option $1" >&2; exit 2 ;;
  esac
done
exec bwrap "${args[@]}" -- "$@"
