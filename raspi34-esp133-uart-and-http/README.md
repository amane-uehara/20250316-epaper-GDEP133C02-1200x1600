# GDEP133C02 電子ペーパー WiFi画像表示システム

WiFi/UART経由で画像を受信し、ESP32-S3で13.3インチ6色電子ペーパーに表示するシステム。

## 概要

### システム構成

| コンポーネント | 説明 |
|----------------|------|
| **Raspberry Pi** | 画像送信元。USB Type-Aポートを使用 |
| **ESP32-S3ボード** | WiFi/UART受信・EPD制御。USB Type-C（**USB2ポート**）で接続 |
| **GDEP133C02** | Good Display製 13.3インチ 6色電子ペーパー（1200x1600） |

```
[Raspberry Pi] ---USB-UART---> [ESP32-S3] ---SPI---> [GDEP133C02]
   USB-A          921600bps      USB-C(USB2)  10MHz    13.3inch 1200x1600

[スマホ/PC] ---WiFi(HTTP)---> [ESP32-S3] ---SPI---> [GDEP133C02]
                  Port 80
```

**注意**: ESP32-S3ボードには複数のUSB端子がある場合があります。**USB2**と書かれた端子に接続してください。

### ディレクトリ構成

```
esp133-pico-usb/
├── main/                # ファームウェアのソースコード
├── root-usb/            # Raspberry PiでUSBメモリを常にRead Onlyモードでmountするための設定
├── send_image.py        # 画像送信スクリプト（UART/HTTP対応）
├── CMakeLists.txt       # ESP-IDFビルド設定
├── sdkconfig            # ESP-IDF設定
├── sdkconfig.defaults   # ESP-IDF設定デフォルト値
├── build/               # ファームウェアビルド成果物
└── README.md            # 本ドキュメント
```

## 使い方（クイックスタート）

### 初回WiFi設定（プロビジョニング）

1. ESP32-S3の電源を入れると、保存済みWiFi認証情報がない場合SoftAPモードで起動
2. スマホ/PCからWiFiアクセスポイント **"EPD-Setup"** に接続（パスワードなし）
3. キャプティブポータルが自動的に開く（開かない場合はブラウザで http://192.168.4.1 を開く）
4. SSIDとパスワードを入力して送信
5. ESP32-S3が再起動し、入力したWiFiネットワークに接続
6. ESP-32-S3のIPアドレスは、WiFiネットワーク接続時にルーターによって適当に設定される。HTTPで画像更新する場合は何とかしてIPアドレスを調べる必要がある。以下では説明のため便宜的に `192.168.0.210` とする。

キャプティブポータルは内蔵DNSサーバにより実現しています。SoftAPモード中のすべてのDNSクエリを `192.168.4.1` に解決するため、スマホなどではWiFi接続時に自動的に設定画面が表示されます。

WiFi接続に10回失敗すると自動的にSoftAPプロビジョニングモードにフォールバックします。

### WiFi認証情報のリセット

```bash
curl -X POST http://<ESP32のIPアドレス>/wifi/reset
```

NVSに保存されたSSID/パスワードがクリアされ、ESP32-S3が再起動します。次回起動時にSoftAPプロビジョニングモードになります。

### 画像送信（UART）

```bash
python3 send_image.py photo.jpg
```

### 画像送信（WiFi/HTTP）

```bash
python3 send_image.py photo.jpg --http 192.168.0.210
```

IPアドレスだけ渡せば `http://` は自動補完されます。

## send_image.py

JPG/PNG画像を6色パレットに減色変換し、E6UPプロトコルでESP32-S3に送信するスクリプト。UART（シリアル）とHTTP（WiFi）の両方に対応しています。

### 画像処理パイプライン

1. **読み込み** — EXIF回転情報を適用し、RGBに変換
2. **自動回転** — 横長画像は90度回転してEPD（縦長1200x1600）に合わせる
3. **リサイズ** — アスペクト比を維持し、EPDサイズに収まるようLanczos補間でリサイズ。余白は白で埋め中央揃え
4. **画像調整** — 明度・コントラスト・彩度を調整（EPDでの表示に最適化するデフォルト値）
5. **6色パレット変換** — CIE Lab色空間での最近傍色探索＋Floyd-Steinbergディザリング
6. **4bpp変換** — パレットインデックスを4bppバイト列に変換（1バイト=2ピクセル）
7. **送信** — E6UPヘッダ（18バイト）＋ペイロード（960,000バイト）をUARTまたはHTTPで送信

### オプション一覧

#### 通信設定

| オプション | デフォルト | 説明 |
|------------|------------|------|
| `--http HOST` | なし | HTTPで送信（例: `192.168.0.210`） |
| `-p, --port` | /dev/ttyUSB0 | シリアルポート（UART送信時） |
| `-b, --baudrate` | 921600 | ボーレート（UART送信時） |

`--http` を指定しない場合はUART送信になります。

#### 回転設定

| オプション | デフォルト | 説明 |
|------------|------------|------|
| `-r, --rotate` | 自動 | 回転角度 (0, 90, 180, 270) |

- **自動（デフォルト）**: 横長画像は自動的に90度回転してEPD（縦長1200x1600）に合わせます
- **`-r 0`**: 回転なし。縦長画像をそのまま表示したい場合に使用

#### 画像調整

| オプション | デフォルト | 説明 |
|------------|------------|------|
| `--brightness` | 1.1 | 明度調整（1.0で変更なし） |
| `--contrast` | 1.1 | コントラスト調整（1.0で変更なし） |
| `--saturation` | 1.4 | 彩度調整（1.0で変更なし） |

EPDは通常のディスプレイと比べて色が淡く出るため、デフォルトで明度・コントラスト・彩度を少し強めています。

#### 変換モード

| オプション | 説明 |
|------------|------|
| （なし） | Lab色空間＋Floyd-Steinbergディザリング（高品質、低速） |
| `--fast` | Pillow組み込みquantize（高速、開発用） |
| `--magick` | ImageMagickで変換（高品質、要imagemagick） |
| `--no-dither` | ディザリングを無効にする |
| `--preview` | 送信せずプレビュー画像をPNG保存 |

#### 使用例

```bash
# 基本的な使い方（UART）
python3 send_image.py photo.jpg

# WiFi経由で送信（IPアドレスだけでOK）
python3 send_image.py photo.jpg --http 192.168.0.210

# 画像調整
python3 send_image.py photo.jpg --brightness 1.3 --saturation 1.8

# 回転なし＋ディザリング無効
python3 send_image.py photo.jpg -r 0 --no-dither --http 192.168.0.210

# プレビューのみ（送信しない）
python3 send_image.py photo.jpg --preview

# ImageMagickで高品質変換
python3 send_image.py photo.jpg --magick
```

### 依存パッケージ

```bash
sudo apt-get install libopenjp2-7
pip3 install pillow
# ImageMagickモードを使う場合
sudo apt-get install imagemagick
```

## ファームウェア

### アーキテクチャ

```
┌─ main.c (app_main)
│  ├─ epd_driver ─── GDEP133C02 (SPI, dual-chip)
│  ├─ uart_receiver ─── UART0 @ 921600bps (CH340)
│  ├─ wifi_manager ─── STA接続 / SoftAPプロビジョニング
│  │   └─ dns_server ─── キャプティブポータル用DNS
│  └─ http_server ─── HTTP API (Port 80)
│
└─ epd_mutex (FreeRTOS binary semaphore)
   └─ HTTP/UARTからのEPDアクセスを排他制御
```

- UART受信タスクはCore 1に固定。WiFi/HTTPはCore 0で動作
- 画像バッファ（960KB）はPSRAM上に確保
- EPDへの描画はmutexで排他制御され、HTTP/UARTの同時描画を防止

### ファイル構成

```
main/
├── main.c           # エントリポイント、epd_draw_4bpp()（mutex付き描画ラッパー）
├── epd_driver.c     # EPD SPIドライバ（GDEP133C02、デュアルチップ制御）
├── epd_driver.h
├── uart_receiver.c  # UART受信タスク、E6UPプロトコル処理
├── uart_receiver.h
├── http_server.c    # HTTP API（画像受信・ステータス・WiFiリセット）
├── http_server.h
├── wifi_manager.c   # WiFi STA接続＋SoftAPプロビジョニング（NVS保存）
├── wifi_manager.h
├── dns_server.c     # キャプティブポータル用DNSサーバ（SoftAP時のみ）
├── dns_server.h
├── usb_receiver.c   # USB Host CDC-ACM受信（未使用・参考実装）
├── usb_receiver.h
└── CMakeLists.txt   # ビルド設定
```

### 各モジュール詳細

#### epd_driver

GDEP133C02のSPIドライバ。デュアルチップ構成（CS0/CS1）で1200x1600ピクセルを制御する。

- SPI3_HOST、クロック10MHz、DMA有効、最大32KBトランザクション
- 画像書き込みは行単位で2つのドライバICに分割送信（各IC: 1200x800ピクセル、300バイト/行）
- リフレッシュ完了はBusyピンの立ち下がりエッジ割り込みで検出（最大120秒待機）
- ロードスイッチ（GPIO 45）でEPD電源を制御

#### uart_receiver

UART0（CH340 USB-UARTブリッジ）経由でE6UPフレームを受信する。

- ボーレート: 921600bps、RXバッファ: 64KB
- 起動時に `READY\n` を送信し、ヘッダ→ペイロードの順に受信
- CRC32検証後、`epd_draw_4bpp()` で描画。結果を `OK recv\n` / `ERR ...\n` で返答
- Core 1に固定（priority 5）

#### http_server

WiFi経由でHTTP APIを提供する。

- `POST /image` — E6UPバイナリを受信してEPDに描画（Content-Length必須）
- `GET /status` — `{"status":"ready"}` を返す
- `POST /wifi/reset` — NVSのWiFi認証情報をクリアして再起動
- スタックサイズ16KB、タイムアウト30秒

#### wifi_manager

WiFi STA接続とSoftAPプロビジョニングを管理する。

- NVS名前空間 `wifi_cfg` にSSID/パスワードを保存
- STA接続が10回失敗するとSoftAPモード（SSID: `EPD-Setup`、認証なし）にフォールバック
- SoftAPモードではHTMLフォーム（`/`）とPOSTハンドラ（`/save`）でWiFi設定を受け付ける
- URL-decodeに対応（`%XX`、`+` → スペース）

#### dns_server

SoftAPプロビジョニング時のキャプティブポータル用DNSサーバ。

- UDPポート53で待ち受け、すべてのDNSクエリに `192.168.4.1`（SoftAPゲートウェイ）のAレコードを返答
- TTL 10秒、ポインタ圧縮対応

### sdkconfig.defaults

| 設定 | 値 | 説明 |
|------|----|------|
| PSRAM | Octal 80MHz | 960KB画像バッファ用 |
| コンソール | UART0（デフォルト） | CH340 USB-UARTブリッジ経由 |
| FreeRTOS Tick | 1000Hz | 1ms解像度 |
| ログレベル | INFO | デフォルトログレベル |

### ビルド環境の構築

#### 必要なパッケージ（Ubuntu/Debian）

```bash
sudo apt-get update && sudo apt-get install -y \
    build-essential \
    cmake \
    ninja-build \
    python3 \
    python3-pip \
    python3-venv \
    git \
    flex \
    bison \
    gperf \
    ccache \
    libffi-dev \
    libssl-dev \
    libusb-1.0-0 \
    dfu-util
```

#### ESP-IDFのインストール

```bash
cd ~
git clone --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
./install.sh esp32s3
```

#### 環境変数の設定

ビルド前に毎回実行するか、`.bashrc`に追加してください。

```bash
. ~/esp-idf/export.sh
```

### ビルド

```bash
idf.py set-target esp32s3
idf.py build
```

### 書き込み

```bash
# ESP32-S3に直接書き込む場合（ESP-IDF環境がある場合）
idf.py -p /dev/ttyUSB0 flash
```

#### Raspberry Piから書き込む場合

1. `build`ディレクトリをRaspberry Piに転送

```bash
scp -r build pi@raspberrypi:~/epd_build
```

2. esptoolをインストール（未導入の場合）

```bash
pip3 install esptool
```

3. ESP32-S3に書き込み

```bash
cd ~/epd_build
python3 -m esptool --chip esp32s3 --port /dev/ttyUSB0 --baud 921600 \
  write-flash --flash-mode dio --flash-size 2MB --flash-freq 80m \
  0x0 bootloader/bootloader.bin \
  0x8000 partition_table/partition-table.bin \
  0x10000 esp133-pico-usb.bin
```

## HTTP API

### 通常モード（STA接続時）

| メソッド | エンドポイント | 説明 | レスポンス |
|----------|----------------|------|------------|
| `POST` | `/image` | E6UPフレーム送信（バイナリ） | `OK\n` |
| `GET` | `/status` | ステータス確認 | `{"status":"ready"}` |
| `POST` | `/wifi/reset` | WiFi認証情報クリア＋再起動 | （再起動） |

### プロビジョニングモード（SoftAP時）

| メソッド | エンドポイント | 説明 |
|----------|----------------|------|
| `GET` | `/` | WiFi設定フォーム（HTML） |
| `POST` | `/save` | SSID/パスワード保存＋再起動 |
| `*` | `/*` | ワイルドカード — すべて設定フォームにリダイレクト |

### curlでの使用例

```bash
# ステータス確認
curl http://192.168.0.210/status

# WiFiリセット
curl -X POST http://192.168.0.210/wifi/reset
```

## E6UPプロトコル

UART/HTTP共通のバイナリ画像転送プロトコル。

### ヘッダ構造（18バイト）

| オフセット | フィールド | サイズ | 値 |
|------------|------------|--------|-----|
| 0 | Magic | 4バイト | `"E6UP"` (0x45 0x36 0x55 0x50) |
| 4 | Version | 1バイト | `1` |
| 5 | Width | 2バイト (LE) | `1200` |
| 7 | Height | 2バイト (LE) | `1600` |
| 9 | Format | 1バイト | `0` (4bpp palette) |
| 10 | PayloadLen | 4バイト (LE) | `960000` |
| 14 | CRC32 | 4バイト (LE) | ペイロードのCRC32 |

### 4bpp パレット値

| 色 | 4bpp値 | RGB参考値 |
|----|--------|-----------|
| 黒 | 0x0 | (0, 0, 0) |
| 白 | 0x1 | (255, 255, 255) |
| 黄 | 0x2 | (255, 230, 0) |
| 赤 | 0x3 | (200, 0, 0) |
| 青 | 0x5 | (0, 0, 200) |
| 緑 | 0x6 | (0, 180, 0) |

- 1バイトに2ピクセル格納（上位4ビット=左ピクセル、下位4ビット=右ピクセル）
- パケット合計: 18（ヘッダ）+ 960,000（ペイロード）= **960,018バイト**

## UART通信

- **ブリッジIC**: CH340（ESP32-S3ボード上のUSB-UARTブリッジ）
- **UARTポート**: UART0（GPIO 43: TX / GPIO 44: RX）
- **ボーレート**: 921,600 bps
- **設定**: 8N1（データ8ビット、パリティなし、ストップ1ビット）
- **RXバッファ**: 64KB

**注意**: ESP32-S3ボードの**USB2**ポートに接続してください。USB1ポート（USB-JTAG）ではありません。

### UART応答メッセージ

| メッセージ | 意味 |
|------------|------|
| `READY\n` | ESP32-S3が受信準備完了 |
| `OK recv\n` | 画像データの受信・CRC検証が成功 |
| `OK draw\n` | EPDへの描画が完了 |
| `ERR magic\n` | ヘッダのマジックバイトが不正 |
| `ERR version\n` | プロトコルバージョンが不正 |
| `ERR format\n` | 画像フォーマットが不正 |
| `ERR resolution\n` | 解像度が不正（1200x1600以外） |
| `ERR payload_len\n` | ペイロード長が不正 |
| `ERR recv\n` | ペイロード受信がタイムアウト |
| `ERR crc\n` | CRC32検証に失敗 |
| `ERR draw\n` | EPD描画に失敗 |

## ハードウェア

### GPIOピン配置（ESP32-S3）

| 機能 | GPIO |
|------|------|
| SPI CLK | 9 |
| SPI MOSI (DATA0) | 41 |
| SPI MISO (DATA1) | 40 |
| CS0 | 18 |
| CS1 | 17 |
| Reset | 6 |
| Busy | 7 |
| Load Switch | 45 |

### EPDデュアルチップ構成

GDEP133C02は2つのドライバICで上半分・下半分を制御するデュアルチップ構成です。

- **CS0**: 上半分（1200x800ピクセル）
- **CS1**: 下半分（1200x800ピクセル）
- 各行は600バイト（1200ピクセル / 2）で、前半300バイトをCS0、後半300バイトをCS1に送信

### 対応パネル

- **GDEP133C02**: Good Display製 13.3インチ 6色電子ペーパー
- 解像度: 1200 x 1600
- 対応色: 黒、白、黄、赤、青、緑
- SPI: 10MHz、DMA有効
- サンプルコード: https://www.good-display.com/companyfile/1755.html (GDEP133C02.zip)

## 注意事項

- EPDのリフレッシュには数十秒かかります（Busyピンで最大120秒待機）
- UART0を使用しているため、ログ出力と画像受信が同一ポートで混在します
- PSRAMを使用して960KBの画像バッファを確保しています（Octal PSRAM, 80MHz）
- sdkconfigのflash-size設定は2MBです（実際のフラッシュは16MBですが設定上2MB）
- HTTP画像受信とUART画像受信はmutexで排他制御されており同時描画されません
- EPD描画中に別の入力ソースから描画要求があると、mutex取得タイムアウト（5秒）でエラーになります

## ライセンス

MIT License
