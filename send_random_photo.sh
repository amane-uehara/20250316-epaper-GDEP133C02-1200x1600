#!/bin/bash
# 毎時cron実行用: USBの写真をランダムに1枚選び、EPDに送信する
set -euo pipefail

PHOTO_DIR="/mnt/usb/photo-frame"
OUTPUT="/tmp/epaper-1200-1600-title.jpg"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ESP_HOST="192.168.0.210"

# 1. JPGファイルをランダムに1つ取得
photo=$(find "$PHOTO_DIR" -type f \( -iname '*.jpg' -o -iname '*.jpeg' \) | shuf -n 1)

if [ -z "$photo" ]; then
    echo "エラー: JPGファイルが見つかりません: $PHOTO_DIR" >&2
    exit 1
fi

echo "選択: $photo"

# 2. /photo-frame/ 以下の相対パスをタイトルとして取得
title="${photo#*photo-frame/}"

# 3. overlay-title で画像右下にタイトルを重ねて /tmp に出力
python3 "$SCRIPT_DIR/overlay-title/overlay_text.py" \
    "$photo" "$title" \
    -H right -V bottom \
    -o "$OUTPUT"

# 4. send_image.py で ESP32 に HTTP 送信
python3 "$SCRIPT_DIR/raspi34-esp133-uart-and-http/send_image.py" \
    "$OUTPUT" --http "$ESP_HOST"
