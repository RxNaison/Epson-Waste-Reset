#!/usr/bin/env python3
"""
  addresses / reset   legacy flat arrays - the ONLY thing old clients in the
                      field read (they auto-update from the /database.json OTA
                      URL and ignore keys they don't know)
  pad_groups          structured groups with a machine-readable 'kind'
                      ('platen'/'main') - what current clients prefer

Once the old clients are considered dead, drop the flat arrays here and switch to the
envelope form { "schema_version": N, "models": { ... } } - the C++ loader
already understands it
"""
import tomllib
import json
import urllib.request

URL = "https://codeberg.org/atufi/reinkpy/raw/branch/main/reinkpy/epson.toml"

print(f"[*] Fetching upstream database from {URL}...")


def normalize_desc(desc):
    d = desc.strip()
    if d in ("Waste counter (platen pad)", "Platen pad counters", "Waste counters (?)"):
        return "Platen Pad Counter"
    elif d in ("Waste counter (main pad)", "Waste counter"):
        return "Main Pad Counter"
    return d


KIND_BY_DESC = {
    "Platen Pad Counter": "platen",
    "Main Pad Counter": "main",
}

try:
    req = urllib.request.Request(URL, headers={'User-Agent': 'Mozilla/5.0 (EWR-Updater)'})
    response = urllib.request.urlopen(req)
    toml_data = response.read().decode('utf-8')

    parsed = tomllib.loads(toml_data)
    db = {}

    for printer in parsed.get("EPSON", []):
        if "models" not in printer or "wkey" not in printer:
            continue

        pad_groups = []
        for mem in printer.get("mem", []):
            raw_desc = mem.get("desc", "")
            if "Waste" in raw_desc or "Platen" in raw_desc:
                desc = normalize_desc(raw_desc)
                addrs = mem.get("addr", [])
                resets = mem.get("reset", [])
                if not resets:
                    resets = [0] * len(addrs)

                if pad_groups and pad_groups[-1]["desc"] == desc:
                    pad_groups[-1]["addresses"].extend(addrs)
                    pad_groups[-1]["reset"].extend(resets)
                else:
                    pad_groups.append({
                        "desc": desc,
                        "kind": KIND_BY_DESC.get(desc, ""),
                        "addresses": list(addrs),
                        "reset": list(resets)
                    })

        flat_addresses = [a for pg in pad_groups for a in pg["addresses"]]
        flat_resets = [r for pg in pad_groups for r in pg["reset"]]

        for model in printer["models"]:
            db[model] = {
                "rkey": printer.get("rkey", 0),
                "wkey": printer.get("wkey", ""),
                "wkey1": printer.get("wkey1", ""),
                "rlen": printer.get("rlen", 2),
                "wlen": printer.get("wlen", 2),
                "mem_high": printer.get("mem_high", 2047),
                # old clients read ONLY these
                "addresses": flat_addresses,
                "reset": flat_resets,
                # current clients prefer these
                "pad_groups": pad_groups
            }

    with open("database.json", "w", encoding="utf-8") as f:
        json.dump(db, f, indent=4)

    print(f"[+] Success! Generated database.json with {len(db)} supported models.")

except Exception as e:
    print(f"[-] FATAL ERROR: Could not update database. {e}")
    exit(1)
