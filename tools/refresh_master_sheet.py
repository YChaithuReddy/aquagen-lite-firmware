#!/usr/bin/env python3
"""
AquaGen fleet master sheet — ONE sheet, always current.

Pulls every device's live state from Azure IoT Hub (site name + GPS that the operator app saved,
firmware, online/offline, last-seen, health) and merges it with the local provisioning data
(serial, AP password) and the baked Azure device key/connection string. Writes a single CSV that
you can open in Excel/Numbers. Run it any time — or let the scheduler run it automatically.

Usage:  python3 tools/refresh_master_sheet.py
Output: ~/Downloads/AquaGen-Master-Sheet-FULL.csv   (⚠ contains device keys — keep local)
"""
import csv, json, os, re, subprocess, sys

AZ   = "/opt/homebrew/bin/az"
BASE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "batch_gravity_50")
HUB  = os.path.expanduser("~/aquagen_hub.txt")
OUT  = os.path.expanduser("~/Downloads/AquaGen-Master-Sheet-FULL.csv")

def main():
    conn = open(HUB).read().strip()
    host = re.search(r"HostName=([^;]+)", conn).group(1)

    # Live twin data for the whole fleet (server-side query → not truncated by --top).
    twins = {}
    try:
        out = subprocess.check_output(
            [AZ, "iot", "hub", "device-twin", "list", "--login", conn, "--top", "5000"],
            stderr=subprocess.DEVNULL)
        for t in json.loads(out):
            twins[t.get("deviceId")] = t
    except Exception as e:
        print("WARN: could not pull twins from Azure:", e, file=sys.stderr)

    def devkey(did):
        p = os.path.join(BASE, "nvs", f"{did}.csv")
        if not os.path.exists(p):
            return ""
        for row in csv.reader(open(p)):
            if len(row) >= 4 and row[0] == "device_key":
                return row[3]
        return ""

    installed = online = 0
    with open(OUT, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["serial", "device_id", "site_name", "gps", "azure_state", "last_seen_utc",
                    "firmware", "rssi", "modbus_ok", "restart_count", "safe_mode",
                    "azure_device_key", "azure_connection_string", "ap_ssid", "ap_password"])
        for r in csv.DictReader(open(os.path.join(BASE, "master_sheet.csv"))):
            did = r["device_id"]; key = devkey(did)
            cs  = f"HostName={host};DeviceId={did};SharedAccessKey={key}" if key else ""
            t   = twins.get(did, {}); rep = t.get("properties", {}).get("reported", {})
            state = t.get("connectionState", "")
            site  = rep.get("site_name", "")
            if site: installed += 1
            if state == "Connected": online += 1
            w.writerow([r.get("serial", ""), did, site, rep.get("site_gps", ""), state,
                        t.get("lastActivityTime", ""), rep.get("firmware_version", ""),
                        rep.get("rssi", ""), rep.get("modbus_ok", ""), rep.get("restart_count", ""),
                        rep.get("safe_mode", ""), key, cs, r.get("ap_ssid", ""), r.get("ap_password", "")])
    print(f"OK  {OUT}\n    installed(site set): {installed}/50   online now: {online}/50")

if __name__ == "__main__":
    main()
