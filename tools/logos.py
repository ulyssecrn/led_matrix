#!/usr/bin/env python3
"""Fetch logos for the board's ticker symbols, shrink them to 16x16 RGB565 and upload them.

Usage: python3 tools/logos.py [host] [--preview out.png]     (needs Pillow)

Symbols come from the board's ticker list, logos from financialmodelingprep.com.
The board only stores and draws the raw pixels: decoding PNGs there would not fit in RAM.
"""
import argparse
import base64
import io
import re
import sys
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

from PIL import Image

SIZE = 16
LOGO_URL = "https://financialmodelingprep.com/image-stock/{}.png"


def http(url, data=None, headers=None):
    req = urllib.request.Request(url, data=data, headers={"User-Agent": "Mozilla/5.0", **(headers or {})})
    with urllib.request.urlopen(req, timeout=20) as r:
        return r.read()


def ota_password():
    secrets = (Path(__file__).resolve().parent.parent / "secrets.h").read_text()
    return re.search(r'#define OTA_PASSWORD\s+"(.*)"', secrets).group(1)


def pixels(img):
    return list(img.get_flattened_data() if hasattr(img, "get_flattened_data") else img.getdata())


def close(c1, c2):
    return max(abs(a - b) for a, b in zip(c1[:3], c2[:3])) < 40


def to_logo(png):
    img = Image.open(io.BytesIO(png)).convert("RGBA")
    w, h = img.size
    corners = [img.getpixel(p) for p in ((0, 0), (w - 1, 0), (0, h - 1), (w - 1, h - 1))]
    if corners[0][3] == 255 and all(close(c, corners[0]) for c in corners):
        # Opaque background (usually white): make pixels close to it transparent.
        # All four corners must agree, or logos that touch the corners (Microsoft) lose a color.
        img.putdata([(r, g, b, 0) if close((r, g, b), corners[0]) else (r, g, b, a) for r, g, b, a in pixels(img)])
    bbox = img.getchannel("A").getbbox()
    if bbox:
        img = img.crop(bbox)  # fill the 16x16 instead of keeping the source's margins
    side = max(img.size)
    square = Image.new("RGBA", (side, side))
    square.paste(img, ((side - img.width) // 2, (side - img.height) // 2))
    px = pixels(square.resize((SIZE, SIZE), Image.LANCZOS))

    opaque = [(r, g, b) for r, g, b, a in px if a > 128]
    if opaque and sum(0.299 * r + 0.587 * g + 0.114 * b for r, g, b in opaque) / len(opaque) < 70:
        px = [(255, 255, 255, a) for r, g, b, a in px]  # dark logos would vanish on the black panel
    out = Image.new("RGB", (SIZE, SIZE))
    out.putdata([(r * a // 255, g * a // 255, b * a // 255) for r, g, b, a in px])
    return out


def rgb565(img):
    data = bytearray()
    for r, g, b in pixels(img):
        data += ((r >> 3) << 11 | (g >> 2) << 5 | b >> 3).to_bytes(2, "little")
    return bytes(data)


def upload(host, sym, data, password):
    boundary = "ledlogo"
    body = (f'--{boundary}\r\nContent-Disposition: form-data; name="logo"; filename="{sym}.bin"\r\n'
            "Content-Type: application/octet-stream\r\n\r\n").encode() + data + f"\r\n--{boundary}--\r\n".encode()
    auth = base64.b64encode(f"admin:{password}".encode()).decode()
    http(f"http://{host}/logo?sym={urllib.parse.quote(sym)}", body,
         {"Content-Type": f"multipart/form-data; boundary={boundary}", "Authorization": f"Basic {auth}"})


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host", nargs="?", default="ledclock.local")
    ap.add_argument("--preview", help="only write an 8x enlarged preview PNG (4-bit color, like the panel)")
    ap.add_argument("--symbols", help="comma separated, instead of asking the board")
    args = ap.parse_args()

    symbols = (args.symbols or http(f"http://{args.host}/tickers").decode()).split(",")
    logos = {}
    for sym in (s.split(":")[0].strip().upper() for s in symbols):  # drop ":Label"
        if not sym:
            continue
        try:
            logos[sym] = to_logo(http(LOGO_URL.format(urllib.parse.quote(sym))))
        except (urllib.error.URLError, OSError) as e:
            print(f"{sym}: no logo ({e})")

    if args.preview:
        sheet = Image.new("RGB", (len(logos) * (SIZE + 2) * 8, SIZE * 8))
        for i, img in enumerate(logos.values()):
            panel = img.point(lambda v: v & 0xF0)  # what 4-bit color depth keeps
            sheet.paste(panel.resize((SIZE * 8, SIZE * 8), Image.NEAREST), (i * (SIZE + 2) * 8, 0))
        sheet.save(args.preview)
        print(f"preview of {', '.join(logos)} written to {args.preview}")
        return

    password = ota_password()
    for sym, img in logos.items():
        upload(args.host, sym, rgb565(img), password)
        print(f"{sym}: uploaded")


if __name__ == "__main__":
    sys.exit(main())
