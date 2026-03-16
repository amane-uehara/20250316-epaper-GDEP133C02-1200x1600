#!/usr/bin/env bash
set -euo pipefail

MP="/mnt/usb"
LOGTAG="[usb-mount]"

log(){ echo "$LOGTAG $*"; }

is_mounted() {
  mountpoint -q "$MP"
}

mounted_source() {
  findmnt -rn -T "$MP" -o SOURCE 2>/dev/null || true
}

healthy_read() {
  # “空でもOK”だが、死んだマウント判定にはstatの方が強い
  timeout 2 stat -t "$MP" >/dev/null 2>&1
}

cleanup_mount() {
  if is_mounted; then
    log "unmounting $MP (stale or unhealthy)"
    umount "$MP" 2>/dev/null || umount -l "$MP" 2>/dev/null || true
  fi
}

pick_usb_partition() {
  # -P（pairs）を使うので -r（raw）は付けない
  while IFS= read -r line; do
    eval "$line"  # NAME="..." TYPE="..." RM="..." FSTYPE="..." ...
    if [[ "${TYPE:-}" == "part" && "${RM:-0}" == "1" && -n "${FSTYPE:-}" ]]; then
      echo "$NAME"   # 例: /dev/sda1 や /dev/sdb1
      return 0
    fi
  done < <(lsblk -pno NAME,TYPE,RM,FSTYPE,LABEL,MOUNTPOINT -P)
  return 1
}

mount_ro() {
  local dev="$1"
  local fstype
  fstype="$(lsblk -npo FSTYPE "$dev" | head -n1 || true)"

  mkdir -p "$MP"

  case "$fstype" in
    vfat)
      log "mount ro $dev -> $MP (fstype=$fstype)"
      mount -o ro,uid=1000,gid=1000,umask=022,iocharset=utf8,codepage=932 "$dev" "$MP"
      ;;
    exfat)
      log "mount ro $dev -> $MP (fstype=$fstype)"
      mount -o ro,uid=1000,gid=1000,umask=022,iocharset=utf8 "$dev" "$MP"
      ;;
    *)
      log "mount ro $dev -> $MP (fstype=$fstype)"
      mount -o ro "$dev" "$MP"
      ;;
  esac
}

# --- main ---

if is_mounted; then
  src="$(mounted_source)"

  # ここが重要：抜き差しで /dev/sda1 → /dev/sdb1 に変わった時、
  # 古い /dev/sda1 が存在しないなら「死んだマウント」として掃除する
  if [[ "$src" == /dev/* ]] && [ -n "$src" ] && [ ! -b "$src" ]; then
    log "stale mount detected (source missing): $src"
    cleanup_mount
  fi

  if is_mounted && healthy_read; then
    log "already mounted and healthy: $MP (source=$(mounted_source))"
    exit 0
  fi

  cleanup_mount
fi

dev="${1:-}"
if [ -z "$dev" ]; then
  dev="$(pick_usb_partition || true)"
fi

if [ -z "${dev:-}" ] || [ ! -b "$dev" ]; then
  log "no usb partition found"
  exit 0
fi

log "selected device: $dev"
mount_ro "$dev"

