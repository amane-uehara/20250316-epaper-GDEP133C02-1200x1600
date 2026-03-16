# Raspberry Pi: USBメモリを /mnt/usb に自動マウント（抜き差し復帰 + 起動時 + cron）

## 目的
- USBメモリ（写真など読み出し専用）を **常に `/mnt/usb` で参照**できるようにする
- USBメモリを **突然抜かれても、再挿入・定期実行で復旧**できるようにする
- Raspberry Pi **起動時にもマウント**されるようにする
- 書き込み不要のため **read-only(ro) でマウント**する

## 前提
- Raspberry Pi OS (Lite含む) 環境。GUIがない場合、USBは挿しただけでは自動マウントされないことが多い
- USBは `vfat`（例: KIOXIA）だが、`exfat` 等にも対応
- USBは原則1本だけ刺さる想定（複数同時は未対応）
- ユーザー名はhoge

---

## 0. 状態確認（USBが見えているか）
USBを挿して以下を確認する。

```bash
lsblk -o NAME,RM,TRAN,FSTYPE,LABEL,UUID,MOUNTPOINT
````

例（USBが未マウントの状態）:

* `sda1` / `sdb1` が `vfat` 等で見える
* `MOUNTPOINT` が空なら未マウント

---

## 1. マウントポイント作成

```bash
sudo mkdir -p /mnt/usb
```

---

## 2. マウントスクリプト作成（抜き差し復帰対応）

### ファイル配置

`/home/hoge/usb-ensure-mounted.sh`

```bash
sudo /home/hoge/root-usb/usb-ensure-mounted.sh
```

### 中身

```bash
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
  # “空でもOK”。死んだマウント判定には stat が強い
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
  # RM=1 のパーティションを拾う（USBが1本前提で十分）
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
    vfat|exfat)
      log "mount ro $dev -> $MP (fstype=$fstype)"
      # 読み出しだけなので ro。uid/gid は環境により調整（通常1000でOK）
      mount -o ro,uid=1000,gid=1000,umask=022 "$dev" "$MP"
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

  # 抜き差しで /dev/sda1 -> /dev/sdb1 のように変化しても、
  # 古い source が消えていれば stale として掃除する
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
```

### 実行権限

```bash
sudo chmod +x /home/hoge/root-usb/usb-ensure-mounted.sh
```

---

## 3. 手動テスト

```bash
sudo umount /mnt/usb 2>/dev/null || sudo umount -l /mnt/usb 2>/dev/null || true
sudo /home/hoge/root-usb/usb-ensure-mounted.sh
lsblk -o NAME,FSTYPE,LABEL,MOUNTPOINT
ls /mnt/usb
```

成功例:

* `sda1` または `sdb1` が `/mnt/usb` にマウントされる
* `/mnt/usb` 配下が見える

---

## 4. 起動時にも自動でマウント（crontab）

マウント操作があるので **root の crontab** で実行する。

```bash
sudo crontab -e
```

```cron
# root
@reboot sleep 10; /home/hoge/usb-ensure-mounted.sh > /dev/null
```

---

## 5. 定期実行（cron）で保険

マウント操作があるので **root の crontab** で実行する。

```bash
sudo crontab -e
```

```cron
50 * * * * /home/hoge/usb-ensure-mounted.sh > /dev/null
```

---

## 6. よくある注意点

* 抜き差しでデバイス名は `sda1` → `sdb1` のように変わることがある（正常）
* 抜いた直後に `/mnt/usb` が mountpoint 扱いのまま残る場合がある
  → スクリプト内の `stale mount detected` と `umount -l` が復旧に効く
* GUI環境の場合、Desktopの自動マウント（/media/...）と競合することがある
  → 必要ならGUI側の自動マウントをOFFにする
* `uid/gid=1000` は環境により異なる場合がある
  → 値確認: `id -u`, `id -g`

---

## 7. 参考コマンド集

状態確認:

```bash
lsblk -o NAME,RM,TRAN,FSTYPE,LABEL,UUID,MOUNTPOINT
findmnt -T /mnt/usb -o TARGET,SOURCE,FSTYPE,OPTIONS || true
mountpoint /mnt/usb && echo yes || echo no
```

強制アンマウント（困ったとき）:

```bash
sudo umount /mnt/usb 2>/dev/null || sudo umount -l /mnt/usb 2>/dev/null || true
```

デバッグ（スクリプトの分岐確認）:

```bash
sudo bash -x /home/hoge/usb-ensure-mounted.sh
```
