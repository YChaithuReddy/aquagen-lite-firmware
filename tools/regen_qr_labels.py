#!/usr/bin/env python3
"""Regenerate every box QR (URL form) + a consolidated A4 label sheet.

The QR encodes a URL: https://ychaithureddy.github.io/aquagen-setup/?d=Gravity_water_NN
  • phone camera (no app)  -> opens the page -> "Download Operator App"
  • in-app scanner         -> reads the box id -> auto-joins (pw defaults to Config123)

Reads the device list from batch_gravity_50/master_sheet.csv (device_id column),
overwrites batch_gravity_50/qr/<device_id>.png, and writes
batch_gravity_50/Gravity_water_QR_labels_ALL150_A4.pdf.
"""
import csv, os, qrcode
from PIL import Image, ImageDraw, ImageFont

ROOT      = os.path.join(os.path.dirname(__file__), "..", "batch_gravity_50")
MASTER    = os.path.join(ROOT, "master_sheet.csv")
QR_DIR    = os.path.join(ROOT, "qr")
PDF_OUT   = os.path.join(ROOT, "Gravity_water_QR_labels_ALL150_A4.pdf")
BASE_URL  = "https://ychaithureddy.github.io/aquagen-setup/?d="

# A4 @ 150 DPI
PAGE_W, PAGE_H = 1240, 1754
COLS, ROWS     = 4, 6           # 24 labels / page
MARGIN         = 50
QR_PX          = 200

def load_font(size, bold=False):
    for p in ["/System/Library/Fonts/Supplemental/Arial Bold.ttf" if bold else
              "/System/Library/Fonts/Supplemental/Arial.ttf",
              "/System/Library/Fonts/Helvetica.ttc"]:
        try: return ImageFont.truetype(p, size)
        except Exception: pass
    return ImageFont.load_default()

def main():
    os.makedirs(QR_DIR, exist_ok=True)
    with open(MASTER) as f:
        ids = [r["device_id"] for r in csv.DictReader(f)]
    print(f"{len(ids)} devices from master_sheet")

    # 1) regenerate each PNG
    for did in ids:
        img = qrcode.make(BASE_URL + did)
        img.save(os.path.join(QR_DIR, f"{did}.png"))
    print(f"  ✓ wrote {len(ids)} QR PNGs -> {QR_DIR}")

    # 2) compose A4 pages
    font_id  = load_font(26, bold=True)
    font_tag = load_font(17)
    per_page = COLS * ROWS
    cell_w   = (PAGE_W - 2 * MARGIN) // COLS
    cell_h   = (PAGE_H - 2 * MARGIN) // ROWS
    pages    = []
    for start in range(0, len(ids), per_page):
        page = Image.new("RGB", (PAGE_W, PAGE_H), "white")
        d = ImageDraw.Draw(page)
        for k, did in enumerate(ids[start:start + per_page]):
            r, c = divmod(k, COLS)
            x0 = MARGIN + c * cell_w
            y0 = MARGIN + r * cell_h
            d.rectangle([x0, y0, x0 + cell_w - 8, y0 + cell_h - 8],
                        outline=(210, 210, 210), width=1)
            qr = Image.open(os.path.join(QR_DIR, f"{did}.png")).resize((QR_PX, QR_PX))
            qx = x0 + (cell_w - QR_PX) // 2
            page.paste(qr, (qx, y0 + 14))
            # device id (bold) + hint, centred
            tw = d.textlength(did, font=font_id)
            d.text((x0 + (cell_w - tw) // 2, y0 + QR_PX + 20), did, fill="black", font=font_id)
            hint = "Scan to set up"
            hw = d.textlength(hint, font=font_tag)
            d.text((x0 + (cell_w - hw) // 2, y0 + QR_PX + 52), hint, fill=(120, 120, 120), font=font_tag)
        pages.append(page.convert("1"))   # 1-bit -> CCITT (this Pillow has no libjpeg)
    pages[0].save(PDF_OUT, save_all=True, append_images=pages[1:], resolution=150.0)
    print(f"  ✓ {len(pages)} A4 pages -> {PDF_OUT}")

if __name__ == "__main__":
    main()
