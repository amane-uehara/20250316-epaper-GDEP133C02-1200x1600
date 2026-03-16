#!/usr/bin/env python3
"""
GDEP133C02 電子ペーパー画像送信スクリプト
JPG/PNG画像を6色パレットに変換してESP32-S3経由でEPDに表示する

依存: pillow のみ (pip3 install pillow)
"""

import sys
import struct
import zlib
import argparse
import os
import time
import math
from pathlib import Path

from PIL import Image, ImageEnhance

# EPD設定
EPD_WIDTH = 1200
EPD_HEIGHT = 1600
PAYLOAD_LEN = EPD_WIDTH * EPD_HEIGHT // 2  # 4bpp = 960000 bytes

# 6色パレット (RGB値) - EPDの実際の発色に近い値に調整
PALETTE_COLORS = [
    (0, 0, 0),        # 0: black
    (255, 255, 255),  # 1: white
    (255, 230, 0),    # 2: yellow
    (200, 0, 0),      # 3: red
    (0, 0, 200),      # 4: blue
    (0, 180, 0),      # 5: green
]

# パレットインデックスから4bpp値への変換
INDEX_TO_4BPP = [0x0, 0x1, 0x2, 0x3, 0x5, 0x6]

# 白色のパレットインデックス
WHITE_INDEX = 1

# ボーレート定数（シリアル送信時のみ使用）
try:
    import termios
    BAUDRATE_MAP = {
        9600: termios.B9600,
        19200: termios.B19200,
        38400: termios.B38400,
        57600: termios.B57600,
        115200: termios.B115200,
        230400: termios.B230400,
        460800: termios.B460800,
        921600: termios.B921600,
    }
except ImportError:
    BAUDRATE_MAP = {}


def rgb_to_lab(r, g, b):
    """RGB to CIE Lab色空間変換"""
    r, g, b = r / 255.0, g / 255.0, b / 255.0

    r = ((r + 0.055) / 1.055) ** 2.4 if r > 0.04045 else r / 12.92
    g = ((g + 0.055) / 1.055) ** 2.4 if g > 0.04045 else g / 12.92
    b = ((b + 0.055) / 1.055) ** 2.4 if b > 0.04045 else b / 12.92

    r, g, b = r * 100, g * 100, b * 100

    x = r * 0.4124564 + g * 0.3575761 + b * 0.1804375
    y = r * 0.2126729 + g * 0.7151522 + b * 0.0721750
    z = r * 0.0193339 + g * 0.1191920 + b * 0.9503041

    x, y, z = x / 95.047, y / 100.0, z / 108.883

    x = x ** (1/3) if x > 0.008856 else (7.787 * x) + (16/116)
    y = y ** (1/3) if y > 0.008856 else (7.787 * y) + (16/116)
    z = z ** (1/3) if z > 0.008856 else (7.787 * z) + (16/116)

    L = (116 * y) - 16
    a = 500 * (x - y)
    b_val = 200 * (y - z)

    return L, a, b_val


PALETTE_LAB = [rgb_to_lab(r, g, b) for r, g, b in PALETTE_COLORS]


def find_nearest_color_lab(r, g, b):
    """Lab色空間で最も近いパレット色のインデックスを返す"""
    L, a, b_val = rgb_to_lab(r, g, b)

    min_dist = float('inf')
    best_idx = 0

    for idx, (pL, pa, pb) in enumerate(PALETTE_LAB):
        dist = math.sqrt((L - pL) ** 2 + (a - pa) ** 2 + (b_val - pb) ** 2)
        if dist < min_dist:
            min_dist = dist
            best_idx = idx

    return best_idx


def convert_to_palette_fast(img: Image.Image, dither: bool = True) -> list:
    """高速パレット変換（Pillow組み込み使用、開発用）"""
    width, height = img.size

    # パレット画像を作成
    pal_img = Image.new('P', (1, 1))
    pal_data = []
    for color in PALETTE_COLORS:
        pal_data.extend(color)
    pal_data.extend([0] * (256 - len(PALETTE_COLORS)) * 3)
    pal_img.putpalette(pal_data)

    # quantizeでパレット変換
    if dither:
        quantized = img.quantize(colors=6, palette=pal_img, dither=Image.Dither.FLOYDSTEINBERG)
    else:
        quantized = img.quantize(colors=6, palette=pal_img, dither=Image.Dither.NONE)

    # 2D配列に変換
    pixels = quantized.load()
    result = [[pixels[x, y] for x in range(width)] for y in range(height)]

    print("高速変換完了")
    return result


def convert_with_imagemagick(img: Image.Image, dither: bool = True) -> list:
    """ImageMagickを使用した高品質パレット変換"""
    import subprocess
    import tempfile

    width, height = img.size

    # 一時ファイルを作成
    with tempfile.NamedTemporaryFile(suffix='.png', delete=False) as tmp_in:
        tmp_in_path = tmp_in.name
        img.save(tmp_in_path)

    # パレット画像を作成（6x1のPNG）
    with tempfile.NamedTemporaryFile(suffix='.png', delete=False) as tmp_pal:
        tmp_pal_path = tmp_pal.name
        pal_img = Image.new('RGB', (len(PALETTE_COLORS), 1))
        for i, color in enumerate(PALETTE_COLORS):
            pal_img.putpixel((i, 0), color)
        pal_img.save(tmp_pal_path)

    with tempfile.NamedTemporaryFile(suffix='.png', delete=False) as tmp_out:
        tmp_out_path = tmp_out.name

    try:
        # ImageMagickで変換
        dither_opt = "-dither FloydSteinberg" if dither else "-dither None"
        cmd = f"convert {tmp_in_path} {dither_opt} -remap {tmp_pal_path} {tmp_out_path}"
        result = subprocess.run(cmd, shell=True, capture_output=True, text=True)

        if result.returncode != 0:
            print(f"ImageMagick error: {result.stderr}")
            print("Pillowにフォールバック...")
            return convert_to_palette_fast(img, dither)

        # 結果を読み込み
        result_img = Image.open(tmp_out_path).convert('RGB')
        pixels = result_img.load()

        # RGBからパレットインデックスに変換
        palette_map = {color: i for i, color in enumerate(PALETTE_COLORS)}
        output = []
        for y in range(height):
            row = []
            for x in range(width):
                rgb = pixels[x, y]
                # 最も近い色を探す
                idx = palette_map.get(rgb, 0)
                if rgb not in palette_map:
                    # 完全一致しない場合、最も近い色を探す
                    min_dist = float('inf')
                    for i, (pr, pg, pb) in enumerate(PALETTE_COLORS):
                        dist = (rgb[0]-pr)**2 + (rgb[1]-pg)**2 + (rgb[2]-pb)**2
                        if dist < min_dist:
                            min_dist = dist
                            idx = i
                row.append(idx)
            output.append(row)

        print("ImageMagick変換完了")
        return output

    finally:
        # 一時ファイルを削除
        for f in [tmp_in_path, tmp_pal_path, tmp_out_path]:
            try:
                os.unlink(f)
            except:
                pass


def convert_to_palette_custom(img: Image.Image, dither: bool = True) -> list:
    """カスタム減色アルゴリズム（Lab色空間使用、Floyd-Steinbergディザリング）"""
    width, height = img.size
    pixels = list(img.get_flattened_data())

    image = []
    for y in range(height):
        row = []
        for x in range(width):
            idx = y * width + x
            r, g, b = pixels[idx][:3]
            row.append([float(r), float(g), float(b)])
        image.append(row)

    result = [[0] * width for _ in range(height)]

    for y in range(height):
        for x in range(width):
            r = max(0, min(255, int(image[y][x][0])))
            g = max(0, min(255, int(image[y][x][1])))
            b = max(0, min(255, int(image[y][x][2])))

            idx = find_nearest_color_lab(r, g, b)
            result[y][x] = idx

            if dither:
                new_r, new_g, new_b = PALETTE_COLORS[idx]
                err_r = image[y][x][0] - new_r
                err_g = image[y][x][1] - new_g
                err_b = image[y][x][2] - new_b

                if x + 1 < width:
                    image[y][x + 1][0] += err_r * 7 / 16
                    image[y][x + 1][1] += err_g * 7 / 16
                    image[y][x + 1][2] += err_b * 7 / 16
                if y + 1 < height:
                    if x > 0:
                        image[y + 1][x - 1][0] += err_r * 3 / 16
                        image[y + 1][x - 1][1] += err_g * 3 / 16
                        image[y + 1][x - 1][2] += err_b * 3 / 16
                    image[y + 1][x][0] += err_r * 5 / 16
                    image[y + 1][x][1] += err_g * 5 / 16
                    image[y + 1][x][2] += err_b * 5 / 16
                    if x + 1 < width:
                        image[y + 1][x + 1][0] += err_r * 1 / 16
                        image[y + 1][x + 1][1] += err_g * 1 / 16
                        image[y + 1][x + 1][2] += err_b * 1 / 16

        if y % 100 == 0:
            print(f"\r変換中... {y}/{height} ({100*y//height}%)", end="", flush=True)

    print(f"\r変換中... {height}/{height} (100%)")
    return result


def palette_to_4bpp(palette_data: list) -> bytes:
    """パレットインデックス配列を4bpp形式に変換する（左右反転なし）"""
    height = len(palette_data)
    width = len(palette_data[0])
    result = bytearray(PAYLOAD_LEN)

    for y in range(height):
        for x in range(0, width, 2):
            byte_idx = y * (width // 2) + (x // 2)
            idx1 = palette_data[y][x]
            idx2 = palette_data[y][x + 1]
            p1 = INDEX_TO_4BPP[idx1] if idx1 < len(INDEX_TO_4BPP) else 0
            p2 = INDEX_TO_4BPP[idx2] if idx2 < len(INDEX_TO_4BPP) else 0
            result[byte_idx] = (p1 << 4) | p2

        if y % 200 == 0:
            print(f"\r4bpp変換中... {y}/{height} ({100*y//height}%)", end="", flush=True)

    print(f"\r4bpp変換中... {height}/{height} (100%)")
    return bytes(result)


def adjust_image(img: Image.Image, brightness: float, contrast: float, saturation: float) -> Image.Image:
    """画像の明度、コントラスト、彩度を調整"""
    if brightness != 1.0:
        enhancer = ImageEnhance.Brightness(img)
        img = enhancer.enhance(brightness)

    if contrast != 1.0:
        enhancer = ImageEnhance.Contrast(img)
        img = enhancer.enhance(contrast)

    if saturation != 1.0:
        enhancer = ImageEnhance.Color(img)
        img = enhancer.enhance(saturation)

    return img


def load_and_resize_image(image_path: str, rotate: int = None) -> Image.Image:
    """
    画像を読み込み、EPDサイズにリサイズする

    - rotate=None: 自動回転（横長画像は90度回転）
    - rotate=0: 回転なし
    - rotate=90/180/270: 指定角度で回転
    - アスペクト比を維持し、余白は白で埋める
    """
    img = Image.open(image_path)

    # EXIF回転情報を適用
    try:
        from PIL import ExifTags
        for orientation in ExifTags.TAGS.keys():
            if ExifTags.TAGS[orientation] == 'Orientation':
                break
        exif = img._getexif()
        if exif is not None:
            orientation_value = exif.get(orientation)
            if orientation_value == 3:
                img = img.rotate(180, expand=True)
            elif orientation_value == 6:
                img = img.rotate(270, expand=True)
            elif orientation_value == 8:
                img = img.rotate(90, expand=True)
    except (AttributeError, KeyError, IndexError):
        pass

    # RGBに変換
    if img.mode != 'RGB':
        img = img.convert('RGB')

    # 自動回転: 横長画像は-90度回転してEPD（縦長）に合わせる
    if rotate is None:
        if img.width > img.height:
            print("横長画像を-90度回転します")
            img = img.rotate(90, expand=True)
    elif rotate != 0:
        img = img.rotate(-rotate, expand=True)

    # アスペクト比を維持してリサイズ（収まるサイズに）
    img_ratio = img.width / img.height
    epd_ratio = EPD_WIDTH / EPD_HEIGHT

    if img_ratio > epd_ratio:
        # 画像が横長 → 幅を基準にリサイズ
        new_width = EPD_WIDTH
        new_height = int(new_width / img_ratio)
    else:
        # 画像が縦長 → 高さを基準にリサイズ
        new_height = EPD_HEIGHT
        new_width = int(new_height * img_ratio)

    img = img.resize((new_width, new_height), Image.Resampling.LANCZOS)

    # 白い背景に配置（中央揃え）
    background = Image.new('RGB', (EPD_WIDTH, EPD_HEIGHT), (255, 255, 255))
    offset_x = (EPD_WIDTH - new_width) // 2
    offset_y = (EPD_HEIGHT - new_height) // 2
    background.paste(img, (offset_x, offset_y))

    return background


def open_serial(port: str, baudrate: int):
    """シリアルポートを開く"""
    fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    attrs = termios.tcgetattr(fd)
    speed = BAUDRATE_MAP.get(baudrate, termios.B921600)
    attrs[4] = speed
    attrs[5] = speed
    attrs[2] &= ~termios.PARENB
    attrs[2] &= ~termios.CSTOPB
    attrs[2] &= ~termios.CSIZE
    attrs[2] |= termios.CS8
    attrs[2] |= termios.CLOCAL
    attrs[2] |= termios.CREAD
    attrs[0] = 0
    attrs[1] = 0
    attrs[3] = 0
    attrs[6][termios.VMIN] = 0
    attrs[6][termios.VTIME] = 10
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    termios.tcflush(fd, termios.TCIOFLUSH)
    import fcntl
    flags = fcntl.fcntl(fd, fcntl.F_GETFL)
    fcntl.fcntl(fd, fcntl.F_SETFL, flags & ~os.O_NONBLOCK)
    return fd


def serial_write(fd, data: bytes):
    os.write(fd, data)


def serial_readline(fd, timeout: float = 1.0) -> bytes:
    result = bytearray()
    start = time.time()
    while time.time() - start < timeout:
        try:
            ch = os.read(fd, 1)
            if ch:
                result.extend(ch)
                if ch == b'\n':
                    break
            else:
                time.sleep(0.01)
        except BlockingIOError:
            time.sleep(0.01)
    return bytes(result)


def send_to_epd(payload: bytes, port: str, baudrate: int = 921600) -> bool:
    crc = zlib.crc32(payload) & 0xFFFFFFFF
    hdr = b"E6UP" + struct.pack("<BHHBI", 1, EPD_WIDTH, EPD_HEIGHT, 0, len(payload)) + struct.pack("<I", crc)

    print(f"シリアルポート {port} に接続中...")
    fd = open_serial(port, baudrate)

    try:
        termios.tcflush(fd, termios.TCIOFLUSH)

        print(f"ヘッダー送信中... ({len(hdr)} bytes)")
        serial_write(fd, hdr)

        print(f"画像データ送信中... ({len(payload)} bytes)")
        chunk_size = 4096
        for i in range(0, len(payload), chunk_size):
            chunk = payload[i:i + chunk_size]
            serial_write(fd, chunk)
            if i % (chunk_size * 50) == 0:
                print(f"\r送信中... {i}/{len(payload)} ({100*i//len(payload)}%)", end="", flush=True)
        print(f"\r送信中... {len(payload)}/{len(payload)} (100%)")

        termios.tcdrain(fd)

        print("ESP32からの応答を待機中...")
        success = False
        for _ in range(100):
            line = serial_readline(fd, timeout=30.0)  # EPDリフレッシュは時間がかかる
            if not line:
                break
            text = line.decode(errors="ignore").strip()
            if text:
                print(f"  {text}")
            if "OK recv" in text or "OK draw" in text:
                success = True
            if "Display refresh complete" in text or "refresh complete" in text.lower():
                break

        return success
    finally:
        os.close(fd)


def send_to_epd_http(payload: bytes, url: str) -> bool:
    """HTTPで画像データを送信"""
    import urllib.request

    crc = zlib.crc32(payload) & 0xFFFFFFFF
    hdr = b"E6UP" + struct.pack("<BHHBI", 1, EPD_WIDTH, EPD_HEIGHT, 0, len(payload)) + struct.pack("<I", crc)
    data = hdr + payload

    target = url.rstrip('/') + '/image'
    print(f"HTTP送信中: {target} ({len(data)} bytes)")

    req = urllib.request.Request(
        target,
        data=data,
        method='POST',
        headers={'Content-Type': 'application/octet-stream'},
    )

    try:
        with urllib.request.urlopen(req, timeout=120) as resp:
            body = resp.read().decode(errors='ignore').strip()
            print(f"応答: {resp.status} {body}")
            return resp.status == 200
    except urllib.error.HTTPError as e:
        print(f"HTTPエラー: {e.code} {e.read().decode(errors='ignore').strip()}")
        return False
    except Exception as e:
        print(f"送信エラー: {e}")
        return False


def save_preview(palette_data: list, output_path: str):
    """プレビュー画像を保存"""
    height = len(palette_data)
    width = len(palette_data[0])

    img = Image.new('RGB', (width, height))
    pixels = img.load()

    for y in range(height):
        for x in range(width):
            idx = palette_data[y][x]
            pixels[x, y] = PALETTE_COLORS[idx]

    img.save(output_path)


def main():
    parser = argparse.ArgumentParser(
        description='GDEP133C02 電子ペーパーに画像を送信',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog='''
使用例:
  python3 send_image.py photo.jpg
  python3 send_image.py photo.jpg --brightness 1.2 --saturation 1.5
  python3 send_image.py photo.jpg -r 0 --preview   # 自動回転無効
        ''')
    parser.add_argument('image', help='送信する画像ファイル (JPG/PNG)')
    parser.add_argument('-p', '--port', default='/dev/ttyUSB0', help='シリアルポート (default: /dev/ttyUSB0)')
    parser.add_argument('-b', '--baudrate', type=int, default=921600, help='ボーレート (default: 921600)')
    parser.add_argument('-r', '--rotate', type=int, default=None, choices=[0, 90, 180, 270],
                        help='回転角度 (default: 自動, 0=回転なし)')
    parser.add_argument('--brightness', type=float, default=1.1, help='明度調整 (default: 1.1)')
    parser.add_argument('--contrast', type=float, default=1.1, help='コントラスト調整 (default: 1.1)')
    parser.add_argument('--saturation', type=float, default=1.4, help='彩度調整 (default: 1.4)')
    parser.add_argument('--no-dither', action='store_true', help='ディザリングを無効にする')
    parser.add_argument('--fast', action='store_true', help='高速変換モード（Pillow、開発用）')
    parser.add_argument('--magick', action='store_true', help='ImageMagickで変換（高品質、要imagemagick）')
    parser.add_argument('--preview', action='store_true', help='送信せずにプレビュー画像を保存')
    parser.add_argument('--http', metavar='HOST', help='HTTPで送信 (例: 192.168.0.210)')

    args = parser.parse_args()

    if not Path(args.image).exists():
        print(f"エラー: ファイルが見つかりません: {args.image}")
        sys.exit(1)

    print(f"画像を読み込み中: {args.image}")
    img = load_and_resize_image(args.image, args.rotate)
    print(f"出力サイズ: {img.width}x{img.height}")

    print(f"画像調整中... (明度={args.brightness}, コントラスト={args.contrast}, 彩度={args.saturation})")
    img = adjust_image(img, args.brightness, args.contrast, args.saturation)

    if args.magick:
        print("6色パレットに変換中（ImageMagick）...")
        palette_data = convert_with_imagemagick(img, dither=not args.no_dither)
    elif args.fast:
        print("6色パレットに変換中（高速モード）...")
        palette_data = convert_to_palette_fast(img, dither=not args.no_dither)
    else:
        print("6色パレットに変換中（Lab色空間使用）...")
        palette_data = convert_to_palette_custom(img, dither=not args.no_dither)

    if args.preview:
        preview_path = Path(args.image).stem + "_preview.png"
        save_preview(palette_data, preview_path)
        print(f"プレビュー画像を保存しました: {preview_path}")
        return

    print("4bpp形式に変換中...")
    payload = palette_to_4bpp(palette_data)
    print(f"ペイロードサイズ: {len(payload)} bytes")

    print("EPDに送信中...")
    if args.http:
        http_url = args.http
        if not http_url.startswith('http://') and not http_url.startswith('https://'):
            http_url = 'http://' + http_url
        success = send_to_epd_http(payload, http_url)
    else:
        success = send_to_epd(payload, args.port, args.baudrate)

    if success:
        print("完了!")
    else:
        print("警告: 応答を完全に受信できませんでした")


if __name__ == "__main__":
    main()
