#!/usr/bin/env python3
"""
AquaGen Lite — office batch provisioning tool.

For a batch of N boxes, this:
  1. Creates N device identities in Azure IoT Hub (via `az iot hub device-identity create`).
  2. Generates a per-device NVS image (device_id + device_key + ap_password) for the BAKE-AT-FLASH
     path — flashed alongside firmware so the box ships pre-provisioned (no DPS needed).
  3. Generates a QR label PNG per box (encodes AP SSID + AP password [+ device id]) for the installer.
  4. Writes a master CSV (serial <-> device_id <-> ap_password) — your single source of truth.

DPS path: if you prefer zero-touch, skip steps 1-2 here and use the DPS build of the firmware
(set DPS_ID_SCOPE + DPS_GROUP_SYMMETRIC_KEY in iot_configs.h). This tool still does steps 3-4.

Requirements:
  pip install qrcode[pil] segno
  Azure CLI logged in (`az login`) with the IoT extension (`az extension add --name azure-iot`).
  ESP-IDF on PATH for nvs_partition_gen.py (bake-at-flash NVS image), or pass --no-nvs.

Usage:
  python provision_batch.py --hub fluxgen-testhub --count 150 --prefix AQG --out ./batch_2026_06
"""
import argparse
import csv
import json
import os
import secrets
import string
import subprocess
import sys


def rand_ap_password(n=12):
    alphabet = string.ascii_letters + string.digits
    return "".join(secrets.choice(alphabet) for _ in range(n))


def _az(cmd, login):
    if login:
        cmd = cmd + ["--login", login]
    return subprocess.run(cmd, capture_output=True, text=True)


def az_create_device(hub, device_id, login=None):
    """Create the device (or reuse it if it already exists) and return its primary key.
    Idempotent: re-running fetches the existing key via `device-identity show` instead of failing."""
    base = ["az", "iot", "hub", "device-identity"]
    out = _az(base + ["create", "--hub-name", hub, "--device-id", device_id, "--output", "json"], login)
    if out.returncode != 0:
        # Likely already exists → fetch its current key.
        show = _az(base + ["show", "--hub-name", hub, "--device-id", device_id, "--output", "json"], login)
        if show.returncode != 0:
            raise RuntimeError(f"az create/show failed for {device_id}: {out.stderr.strip()}")
        out = show
    info = json.loads(out.stdout)
    return info["authentication"]["symmetricKey"]["primaryKey"]


def make_nvs_csv(path, device_id, device_key, ap_ssid, ap_password):
    """Write the nvs_partition_gen CSV for one device (namespace 'aquagen')."""
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["key", "type", "encoding", "value"])
        w.writerow(["aquagen", "namespace", "", ""])
        w.writerow(["device_id", "data", "string", device_id])
        w.writerow(["device_key", "data", "string", device_key])
        w.writerow(["ap_ssid", "data", "string", ap_ssid])
        w.writerow(["ap_pw", "data", "string", ap_password])


def _idf_python():
    """nvs_partition_gen needs the ESP-IDF python env (has the esp_idf_nvs_partition_gen module),
    which is separate from the system python that runs this tool (and has qrcode)."""
    import glob
    cand = sorted(glob.glob(os.path.expanduser("~/.espressif/python_env/*/bin/python")))
    return cand[-1] if cand else sys.executable


def build_nvs_bin(idf_path, csv_path, bin_path, size="0x9000"):
    gen = os.path.join(idf_path, "components", "nvs_flash", "nvs_partition_generator",
                       "nvs_partition_gen.py")
    cmd = [_idf_python(), gen, "generate", csv_path, bin_path, size]
    subprocess.run(cmd, check=True)


def make_qr(path, ap_ssid, ap_password, device_id):
    import qrcode
    # Payload the Flutter app parses on scan.
    payload = json.dumps({"ssid": ap_ssid, "pw": ap_password, "did": device_id},
                         separators=(",", ":"))
    img = qrcode.make(payload)
    img.save(path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--hub", required=True, help="Azure IoT Hub name")
    ap.add_argument("--count", type=int, required=True)
    ap.add_argument("--prefix", default="AQG", help="device id prefix")
    ap.add_argument("--sep", default="-", help="separator between prefix and number")
    ap.add_argument("--digits", type=int, default=4, help="zero-padded number width")
    ap.add_argument("--start", type=int, default=1, help="starting serial number")
    ap.add_argument("--ap-ssid", default="FluxgenConnect", help="SoftAP name (ignored if --ssid-as-id)")
    ap.add_argument("--ssid-as-id", action="store_true",
                    help="use each device's id as its SoftAP name (unique per box)")
    ap.add_argument("--ap-password", help="fixed SoftAP password for all boxes "
                    "(default: a unique random password per box)")
    ap.add_argument("--out", required=True, help="output directory")
    ap.add_argument("--no-azure", action="store_true", help="skip Azure device creation (QR/CSV only)")
    ap.add_argument("--no-nvs", action="store_true", help="skip NVS image (DPS path)")
    ap.add_argument("--idf-path", default=os.environ.get("IDF_PATH", ""))
    ap.add_argument("--login-file", help="path to a file holding the IoT Hub connection string "
                    "(auth via --login, no 'az login' needed)")
    args = ap.parse_args()

    login = None
    if args.login_file:
        with open(os.path.expanduser(args.login_file)) as f:
            login = f.read().strip()

    os.makedirs(args.out, exist_ok=True)
    qr_dir = os.path.join(args.out, "qr"); os.makedirs(qr_dir, exist_ok=True)
    nvs_dir = os.path.join(args.out, "nvs"); os.makedirs(nvs_dir, exist_ok=True)

    master = os.path.join(args.out, "master_sheet.csv")
    rows = []
    for i in range(args.start, args.start + args.count):
        device_id = f"{args.prefix}{args.sep}{i:0{args.digits}d}"
        ap_pw = args.ap_password if args.ap_password else rand_ap_password()
        ap_ssid = device_id if args.ssid_as_id else args.ap_ssid
        key = ""
        try:
            if not args.no_azure:
                key = az_create_device(args.hub, device_id, login)
        except RuntimeError as e:
            print(f"  ! {e}", file=sys.stderr)

        if not args.no_nvs and key:
            if not args.idf_path:
                print("  ! IDF_PATH not set; skipping NVS image (use --no-nvs for DPS).", file=sys.stderr)
            else:
                csv_path = os.path.join(nvs_dir, f"{device_id}.csv")
                bin_path = os.path.join(nvs_dir, f"{device_id}.bin")
                make_nvs_csv(csv_path, device_id, key, ap_ssid, ap_pw)
                build_nvs_bin(args.idf_path, csv_path, bin_path)

        make_qr(os.path.join(qr_dir, f"{device_id}.png"), ap_ssid, ap_pw, device_id)
        rows.append({"serial": i, "device_id": device_id, "ap_ssid": ap_ssid,
                     "ap_password": ap_pw, "key_created": bool(key)})
        print(f"  ✓ {device_id}")

    with open(master, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=["serial", "device_id", "ap_ssid", "ap_password", "key_created"])
        w.writeheader()
        w.writerows(rows)

    print(f"\nDone. {len(rows)} devices.")
    print(f"  Master sheet: {master}")
    print(f"  QR labels:    {qr_dir}/")
    if not args.no_nvs:
        print(f"  NVS images:   {nvs_dir}/   (flash with: esptool.py write_flash 0x9000 <device>.bin)")


if __name__ == "__main__":
    main()
