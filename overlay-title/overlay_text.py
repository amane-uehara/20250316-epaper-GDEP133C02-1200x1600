#!/usr/bin/env python3
"""
画像の下部にテキストをオーバーレイするスクリプト
下部の明るさに応じて文字色を自動選択（白/黒）
"""

import argparse
import sys
import unicodedata
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont


def is_japanese_char(char: str) -> bool:
    """文字が日本語（ひらがな、カタカナ、漢字、全角記号）かどうか判定"""
    if len(char) != 1:
        return False
    name = unicodedata.name(char, "")
    # ひらがな、カタカナ、CJK漢字、全角記号などを日本語として扱う
    if any(keyword in name for keyword in ["HIRAGANA", "KATAKANA", "CJK", "FULLWIDTH"]):
        return True
    # 日本語の句読点や記号
    cp = ord(char)
    if 0x3000 <= cp <= 0x303F:  # 日本語句読点
        return True
    return False


def split_text_segments(text: str) -> list:
    """テキストを日本語/英語のセグメントに分割"""
    if not text:
        return []

    segments = []
    current_segment = text[0]
    current_is_ja = is_japanese_char(text[0])

    for char in text[1:]:
        char_is_ja = is_japanese_char(char)
        if char_is_ja == current_is_ja:
            current_segment += char
        else:
            segments.append((current_segment, current_is_ja))
            current_segment = char
            current_is_ja = char_is_ja

    segments.append((current_segment, current_is_ja))
    return segments


def get_average_brightness(image: Image.Image, region: tuple) -> float:
    """指定領域の平均明度を計算（0-255）"""
    cropped = image.crop(region)
    grayscale = cropped.convert("L")
    pixels = list(grayscale.getdata())
    return sum(pixels) / len(pixels) if pixels else 128


def get_font(size: int, font_path: str = None, verbose: bool = False) -> ImageFont.FreeTypeFont:
    """利用可能なフォントを取得（細めのRegularフォント優先）"""
    # ユーザー指定のフォントがあれば優先
    if font_path:
        if Path(font_path).exists():
            if verbose:
                print(f"フォント読み込み: {font_path}")
            return ImageFont.truetype(font_path, size)
        else:
            print(f"警告: 指定されたフォントが見つかりません: {font_path}", file=sys.stderr)

    font_paths = [
        # 日本語対応フォントを優先（Raspberry Pi OS標準パス）
        "/usr/share/fonts/truetype/fonts-japanese-gothic.ttf",
        "/usr/share/fonts/truetype/fonts-japanese-mincho.ttf",
        "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/freefont/FreeSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    ]
    for path in font_paths:
        if Path(path).exists():
            if verbose:
                print(f"フォント読み込み: {path}")
            return ImageFont.truetype(path, size)
    # フォールバック（サイズ指定可能な形式）
    try:
        return ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", size)
    except OSError:
        print(f"警告: フォントが見つかりません。サイズ指定が効かない可能性があります", file=sys.stderr)
        return ImageFont.load_default()


def overlay_text(
    input_path: str,
    output_path: str,
    text: str,
    size_ratio: float = 0.05,
    margin_ratio: float = 0.02,
    font_path: str = None,
    font_path_ja: str = None,
    font_path_en: str = None,
    horizontal: str = "center",
    vertical: str = "bottom",
    verbose: bool = False,
) -> None:
    """
    画像にテキストをオーバーレイ

    Args:
        input_path: 入力画像パス
        output_path: 出力画像パス
        text: オーバーレイするテキスト
        size_ratio: 文字サイズ（画像高さに対する比率）
        margin_ratio: 余白（画像サイズに対する比率）
        font_path: フォントファイルパス（省略時は自動検出、日英共通）
        font_path_ja: 日本語用フォントパス（指定時は日本語部分に使用）
        font_path_en: 英語用フォントパス（指定時は英語部分に使用）
        horizontal: 水平位置（left/center/right）
        vertical: 垂直位置（top/bottom）
        verbose: デバッグ情報を表示
    """
    image = Image.open(input_path).convert("RGB")
    width, height = image.size

    font_size = max(1, int(height * size_ratio))
    margin = int(height * margin_ratio)

    # フォントの取得（日英別指定がある場合は個別に、なければ共通フォント）
    use_separate_fonts = font_path_ja or font_path_en
    if use_separate_fonts:
        font_ja = get_font(font_size, font_path_ja, verbose)
        font_en = get_font(font_size, font_path_en, verbose)
        if verbose:
            print(f"日本語フォント: {font_path_ja or '自動検出'}")
            print(f"英語フォント: {font_path_en or '自動検出'}")
    else:
        font = get_font(font_size, font_path, verbose)

    if verbose:
        print(f"画像サイズ: {width}x{height}, フォントサイズ: {font_size}px (比率: {size_ratio})")

    draw = ImageDraw.Draw(image)

    # テキストサイズの計算
    if use_separate_fonts:
        segments = split_text_segments(text)
        text_width = 0
        text_height = 0
        for seg_text, is_ja in segments:
            seg_font = font_ja if is_ja else font_en
            bbox = draw.textbbox((0, 0), seg_text, font=seg_font)
            text_width += bbox[2] - bbox[0]
            text_height = max(text_height, bbox[3] - bbox[1])
    else:
        bbox = draw.textbbox((0, 0), text, font=font)
        text_width = bbox[2] - bbox[0]
        text_height = bbox[3] - bbox[1]

    # テキスト位置の計算
    margin_x = int(width * margin_ratio)
    margin_y = margin  # 既に計算済み

    # 水平位置
    if horizontal == "left":
        x = margin_x
    elif horizontal == "right":
        x = width - text_width - margin_x
    else:  # center
        x = (width - text_width) // 2

    # 垂直位置
    if vertical == "top":
        y = margin_y
    else:  # bottom
        y = height - text_height - margin_y

    # テキスト領域の明るさを計算
    sample_region = (
        max(0, x - 10),
        max(0, y - 5),
        min(width, x + text_width + 10),
        min(height, y + text_height + 5),
    )
    brightness = get_average_brightness(image, sample_region)

    # 明るさに応じて文字色と背景色を選択
    # 背景は画像に馴染む色、文字は反対色
    if brightness > 128:
        # 明るい画像 → 白背景 + 黒文字
        text_color = (0, 0, 0)
        bg_color = (255, 255, 255, 200)
    else:
        # 暗い画像 → 黒背景 + 白文字
        text_color = (255, 255, 255)
        bg_color = (0, 0, 0, 200)

    # 背景バーのパディング（フォントサイズに比例）
    pad_x = max(4, font_size // 8)
    pad_y = max(4, font_size // 6)
    bg_box = (
        x - pad_x,
        y - pad_y,
        x + text_width + pad_x,
        y + text_height + pad_y,
    )

    # 半透明背景を描画するため一時的にRGBAに変換
    overlay = Image.new("RGBA", image.size, (0, 0, 0, 0))
    overlay_draw = ImageDraw.Draw(overlay)
    overlay_draw.rectangle(bg_box, fill=bg_color)
    image = Image.alpha_composite(image.convert("RGBA"), overlay).convert("RGB")

    # テキスト描画
    draw = ImageDraw.Draw(image)
    if use_separate_fonts:
        # セグメントごとに描画
        current_x = x
        for seg_text, is_ja in segments:
            seg_font = font_ja if is_ja else font_en
            draw.text((current_x, y), seg_text, font=seg_font, fill=text_color)
            bbox = draw.textbbox((0, 0), seg_text, font=seg_font)
            current_x += bbox[2] - bbox[0]
    else:
        draw.text((x, y), text, font=font, fill=text_color)

    image.save(output_path, quality=95)
    print(f"保存しました: {output_path}")


def main():
    parser = argparse.ArgumentParser(
        description="画像の下部にテキストをオーバーレイする"
    )
    parser.add_argument("input", help="入力画像ファイル")
    parser.add_argument("text", help="オーバーレイするテキスト")
    parser.add_argument(
        "-s", "--size",
        type=float,
        default=0.05,
        help="文字サイズ（画像高さに対する比率、デフォルト: 0.05）"
    )
    parser.add_argument(
        "-o", "--output",
        help="出力ファイル名（省略時は入力ファイル名_overlay.jpg）"
    )
    parser.add_argument(
        "-f", "--font",
        help="フォントファイルパス（日英共通）"
    )
    parser.add_argument(
        "--font-ja",
        help="日本語用フォントファイルパス"
    )
    parser.add_argument(
        "--font-en",
        help="英語用フォントファイルパス"
    )
    parser.add_argument(
        "--horizontal", "-H",
        choices=["left", "center", "right"],
        default="center",
        help="水平位置（left/center/right、デフォルト: center）"
    )
    parser.add_argument(
        "--vertical", "-V",
        choices=["top", "bottom"],
        default="bottom",
        help="垂直位置（top/bottom、デフォルト: bottom）"
    )
    parser.add_argument(
        "-v", "--verbose",
        action="store_true",
        help="デバッグ情報を表示"
    )

    args = parser.parse_args()

    input_path = Path(args.input)
    if not input_path.exists():
        print(f"エラー: ファイルが見つかりません: {input_path}", file=sys.stderr)
        sys.exit(1)

    if args.output:
        output_path = args.output
    else:
        output_path = str(input_path.stem) + "_overlay" + input_path.suffix

    overlay_text(args.input, output_path, args.text, args.size,
                 font_path=args.font, font_path_ja=args.font_ja,
                 font_path_en=args.font_en, horizontal=args.horizontal,
                 vertical=args.vertical, verbose=args.verbose)


if __name__ == "__main__":
    main()
