# GDEP133C02 電子ペーパー UART画像表示システム

Raspberry PiからUSB-UART経由で画像を送信し、ESP32-S3で受信してGDEP133C02電子ペーパーに表示するシステム。

## 概要

### システム構成

以下の3つのハードウェアで構成されます。

| コンポーネント | 説明 |
|----------------|------|
| **Raspberry Pi** | 画像送信元。USB Type-Aポートを使用 |
| **ESP32-S3ボード** | UART受信・EPD制御。USB Type-C（**USB2ポート**）で接続 |
| **GDEP133C02** | Good Display製 13.3インチ 6色電子ペーパー（1200x1600） |

```
[Raspberry Pi] ---USB---> [ESP32-S3] ---SPI---> [GDEP133C02]
   USB-A         921600bps   USB-C(USB2)  10MHz    13.3inch 1200x1600
```

**注意**: ESP32-S3ボードには複数のUSB端子がある場合があります。**USB2**と書かれた端子に接続してください。

### ディレクトリ構成

```
epd_uart_rx_photo/
├── build/           # ファームウェア → Raspberry Piに設置し、そこからESP32-S2に転送して書き込み
├── main/            # ファームウェアのソースコード
├── send_image.py    # 画像送信スクリプト → Raspberry Piに配置し、ESP32-S2に画像データを送信する
├── CMakeLists.txt   # ESP-IDFビルド設定
├── sdkconfig        # ESP-IDF設定
└── README.md        # 本ドキュメント
```

### ファイルの配置先

| ファイル | 配置先 | 用途 |
|----------|--------|------|
| `build/` | Raspberry Pi | ファームウェア書き込み用 |
| `send_image.py` | Raspberry Pi | 画像送信用（常用） |
| `main/`, `CMakeLists.txt`, `sdkconfig` | 開発PC | ファームウェア再ビルド用 |

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

### 対応パネル

- **GDEP133C02**: Good Display製 13.3インチ 6色電子ペーパー
- 解像度: 1200 x 1600
- 対応色: 黒、白、黄、赤、青、緑
- サンプルコード: https://www.good-display.com/companyfile/1755.html (GDEP133C02.zip)

## ファームウェア（ESP32-S3）

### ファイル構成

```
main/
├── main.c           # メインエントリ、epd_draw_4bpp()実装
├── uart_receiver.c  # UART受信タスク、E6UPプロトコル処理
├── epd_driver.c     # EPD SPIドライバ
├── epd_driver.h     # EPD API定義
└── CMakeLists.txt   # ビルド設定
```

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

`.bashrc`に追加する場合:

```bash
echo '. ~/esp-idf/export.sh' >> ~/.bashrc
```

### ビルド

```bash
# ESP-IDF環境をセットアップ後
idf.py build
```

### 書き込み

```bash
# ESP32-S3に直接書き込む場合（ESP-IDF環境がある場合）
idf.py -p /dev/ttyUSB0 flash
```

### Raspberry Piから書き込む場合

ESP-IDF環境がないRaspberry Piからファームウェアを書き込む場合は、以下の手順で行います。

1. `build`ディレクトリをRaspberry Piに転送

```bash
# 開発PC側で実行
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
  0x10000 epd_uart_rx_photo.bin
```

### 通信プロトコル（E6UP）

```
ヘッダー (18バイト):
  Magic     : "E6UP" (4バイト)
  Version   : 1 (1バイト)
  Width     : 1200 (2バイト, little-endian)
  Height    : 1600 (2バイト, little-endian)
  Format    : 0 = 4bpp palette (1バイト)
  PayloadLen: 960000 (4バイト, little-endian)
  CRC32     : ペイロードのCRC32 (4バイト, little-endian)

ペイロード (960000バイト):
  4bpp画像データ (1ピクセル = 4ビット, 2ピクセル/バイト)
```

### 4bpp パレット値

| 色 | 値 |
|----|----|
| 黒 | 0x0 |
| 白 | 0x1 |
| 黄 | 0x2 |
| 赤 | 0x3 |
| 青 | 0x5 |
| 緑 | 0x6 |

## 送信スクリプト（Raspberry Pi）

### send_image.py

JPG/PNG画像を6色パレットに変換してESP32-S3経由でEPDに表示するスクリプト。

- Lab色空間を使用した高精度な色変換
- Floyd-Steinbergディザリングによる階調表現
- 自動回転（横長画像を縦長EPDに合わせる）
- 明度・コントラスト・彩度の調整機能
- ImageMagickを使用した高品質変換モード

### 依存パッケージ

```bash
sudo apt-get install libopenjp2-7
pip3 install pillow pyserial
```

### 使い方

```bash
# 基本的な使い方
python3 send_image.py photo.jpg

# オプション
python3 send_image.py photo.jpg -r 90              # 90度回転
python3 send_image.py photo.jpg --fast             # 高速変換（開発用）
python3 send_image.py photo.jpg --preview          # プレビュー画像保存
python3 send_image.py photo.jpg --brightness 1.3   # 明度調整
python3 send_image.py photo.jpg --saturation 1.8   # 彩度調整
```

### オプション詳細

#### 通信設定

| オプション | デフォルト | 説明 |
|------------|------------|------|
| `-p, --port` | /dev/ttyUSB0 | シリアルポート |
| `-b, --baudrate` | 921600 | ボーレート |

ESP32-S3がUSBで接続されている場合、通常は`/dev/ttyUSB0`で認識されます。複数のUSBデバイスがある場合は`/dev/ttyUSB1`などを指定してください。

#### 回転設定

| オプション | デフォルト | 説明 |
|------------|------------|------|
| `-r, --rotate` | 自動 | 回転角度 (0, 90, 180, 270) |

- **自動（デフォルト）**: 横長画像は自動的に90度回転してEPD（縦長1200x1600）に合わせます
- **`-r 0`**: 回転なし。縦長画像をそのまま表示したい場合に使用
- **`-r 90/180/270`**: 指定角度で回転

```bash
python3 send_image.py landscape.jpg          # 横長→自動で90度回転
python3 send_image.py portrait.jpg -r 0      # 縦長→回転なし
python3 send_image.py photo.jpg -r 180       # 上下反転
```

#### 画像調整

| オプション | デフォルト | 説明 |
|------------|------------|------|
| `--brightness` | 1.1 | 明度調整 |
| `--contrast` | 1.1 | コントラスト調整 |
| `--saturation` | 1.4 | 彩度調整 |

すべて`1.0`で変更なし。値を大きくすると効果が強くなります。

- **brightness（明度）**: 画像全体の明るさを調整。EPDは暗く見えやすいためデフォルトで少し明るめ（1.1）
- **contrast（コントラスト）**: 明暗の差を調整。のっぺりした画像に効果的
- **saturation（彩度）**: 色の鮮やかさを調整。EPDの6色は淡いため、デフォルトで強め（1.4）

```bash
# 暗い画像を明るく
python3 send_image.py dark_photo.jpg --brightness 1.3

# 色を鮮やかに（赤や緑を強調）
python3 send_image.py pale_photo.jpg --saturation 2.0

# メリハリをつける
python3 send_image.py flat_photo.jpg --contrast 1.3

# 組み合わせ
python3 send_image.py photo.jpg --brightness 1.2 --contrast 1.2 --saturation 1.6
```

**注意**: 彩度を上げすぎると（2.0以上）、色が飽和してベタ塗りになる場合があります。

#### 変換モード

| オプション | 説明 |
|------------|------|
| （なし） | Lab色空間＋Floyd-Steinbergディザリング（高品質、低速） |
| `--fast` | Pillow組み込みquantize（高速、開発用） |
| `--magick` | ImageMagickで変換（高品質、要imagemagick） |
| `--no-dither` | ディザリングを無効にする |

##### デフォルトモード（Lab色空間）

```bash
python3 send_image.py photo.jpg
```

- CIE Lab色空間で色距離を計算し、最も近いパレット色を選択
- Floyd-Steinbergディザリングで量子化誤差を周囲のピクセルに拡散
- 高品質だが、1200x1600の全ピクセルを処理するため時間がかかる

##### 高速モード（--fast）

```bash
python3 send_image.py photo.jpg --fast
```

- Pillowの組み込み`quantize()`関数を使用
- 変換速度が大幅に向上
- 品質はやや劣るが、開発・デバッグ時に便利

##### ImageMagickモード（--magick）

```bash
# imagemagickのインストールが必要
sudo apt-get install imagemagick

python3 send_image.py photo.jpg --magick
```

- ImageMagickの`convert`コマンドを使用した変換
- 高品質なディザリングアルゴリズム
- 彩度を上げても色飽和しにくい

##### ディザリング無効（--no-dither）

```bash
python3 send_image.py photo.jpg --no-dither
```

- ディザリングを無効にし、最近傍色のみで変換
- イラストやロゴなど、ベタ塗りが多い画像に適している
- 写真には不向き（階調が失われる）

#### プレビュー

| オプション | 説明 |
|------------|------|
| `--preview` | 送信せずプレビュー画像を保存 |

```bash
python3 send_image.py photo.jpg --preview
# → photo_preview.png が生成される
```

EPDに送信せず、変換結果をPNG画像として保存します。パラメータ調整時に便利です。

### 画質調整のヒント

| 問題 | 対処法 |
|------|--------|
| 全体的に暗い | `--brightness 1.3` |
| 色が薄い | `--saturation 1.8` |
| 赤が弱い | `--saturation 2.0` |
| のっぺり | `--contrast 1.3` |
| 色がベタ塗りになる | 彩度を下げる、または`--magick`を使用 |
| 変換が遅い | `--fast`を使用 |

## EPDドライバAPI

```c
// 初期化
esp_err_t epd_init(void);

// 終了
void epd_deinit(void);

// 画像書き込み (4bpp, 960000バイト)
esp_err_t epd_write_image(const uint8_t *data, size_t len);

// 表示更新（リフレッシュ）
esp_err_t epd_refresh(void);
```

## 注意事項

- EPDのリフレッシュには数十秒かかる場合があります
- UART0を使用しているため、ログ出力と画像受信が混在します
- PSRAMを使用して960KBの画像バッファを確保しています

## ライセンス

MIT License
