#!/usr/bin/env python3
"""
Maintains database.json - the only database file in this repo.

One command:

  build     fetch every upstream source over the network, parse it in
            memory, and merge the result AROUND the committed
            database.json. Nothing but database.json is written: no facts
            files, no provenance sidecar, no staging folders.

The one merge rule:

  Whatever is committed in database.json WINS. The build only fills in
  fields the committed file lacks and appends models it does not have
  yet, so a rebuild can never overwrite a hand edit. Editing
  database.json (and opening a PR) is the whole contribution workflow;
  to deliberately re-pull a field from upstream, delete it from the
  committed file and rebuild.

Sources, in gap-filling priority order (facts merge, their files do not):
    reinkpy     epson.toml    write path: the shipped base (AGPL-3.0)
    ezreset     devices.xml   counter masks + service limits, commit
                              steps, names, RCMODE recovery (no license)
    reink       printers.c    waste addresses of the old R220-class
                              models, cross-check, plus per-color
                              cartridge ink-reset maps (GPL-3.0-or-later)
    gutenprint  escp2.xml     marketing name variants (GPL-2.0-or-later)

  Every source is fetched fresh on every run; one that cannot be reached
  is skipped with a note. Nothing is lost by a skip: everything a source
  contributed in the past already lives in database.json.

Upstream sources never silently change what the tool writes. When they
disagree among themselves on a write-path value (rkey, wkey, wkey1, rlen,
wlen, mem_high, addresses/reset), the model keeps its shipped values and
gets "conflict": true (so the exe can warn before writing); the build
prints the details. Additive enrichments old clients ignore do merge: pad
counters with masks + service limits, the post-reset commit step
("close"), detection "aliases", and the firmware-recovery ("RCMODE")
channel.

Output guarantees:
  - database.json stays readable by every client in the field: the flat
    addresses/reset arrays and all known keys keep their committed values.
  - Deterministic given identical upstream bytes: sorted keys, fixed
    formatting, no timestamps. A database change is a reviewable diff, so
    the weekly workflow commits only when something new turned up.
"""
import argparse
import copy
import io
import json
import re
import sys
import tarfile
import urllib.request
import xml.etree.ElementTree as ET
from pathlib import Path

try:
    import tomllib
except ModuleNotFoundError:  # pragma: no cover - needs Python 3.11+
    tomllib = None

SCHEMA_VERSION = 4

REPO_ROOT = Path(__file__).resolve().parent.parent

# Every source is fetched over the network, fresh, on every build. Licenses
# and the facts-not-files policy are documented in docs/SOURCES.md.
SOURCE_URLS = {
    "reinkpy": "https://codeberg.org/atufi/reinkpy/raw/branch/main/reinkpy/epson.toml",
    "ezreset": "https://raw.githubusercontent.com/CiRIP/ez-reset/master/src/ez_reset/devices.xml",
    "reink": "https://raw.githubusercontent.com/lion-simba/reink/master/printers.c",
    "gutenprint": "https://raw.githubusercontent.com/deepin-community/gutenprint/master/src/xml/printers/escp2.xml",
}

SOURCE_ORDER = ("reinkpy", "ezreset", "reink", "gutenprint")

WRITE_PATH_FIELDS = ("rkey", "wkey", "wkey1", "rlen", "wlen", "mem_high", "addresses", "reset", "ink_groups")


_NOISE_WORDS = ("EPSON", "STYLUS", "PHOTO", "COLOR", "COLOUR", "SERIES")


def norm_name(name):
    """Conservative key for cross-source model matching.

    "Epson Stylus R220" / "Stylus Photo R220" / "R220"  -> "R220"
    "ET-2800 Series" / "ET-2800"                        -> "ET2800"

    Over-aggressive stripping risks false joins, so only obvious noise
    words go. Anything that does not match lands in the coverage report
    instead of being guessed at.
    """
    s = name.upper()
    for w in _NOISE_WORDS:
        s = re.sub(r"\b" + w + r"\b", " ", s)
    return re.sub(r"[\s\-_]+", "", s)


def parse_hex_tokens(text):
    """Parse whitespace-separated hex byte/word tokens. Returns (values, bad)."""
    vals, bad = [], []
    for tok in (text or "").split():
        try:
            vals.append(int(tok, 16))
        except ValueError:
            bad.append(tok)
    return vals, bad


# ---------------------------------------------------------------------------
# reinkpy adapter -- the shipped write-path base.
# The extraction below is a byte-exact port of update_db.py so that every
# value old clients already rely on stays identical.
# ---------------------------------------------------------------------------

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


def counter_spec(desc, mem):
    """Schema 4 read spec for one pad group (only when the limit is known)."""
    addrs = list(mem.get("addr", []))
    limit = mem.get("max") or mem.get("limit")
    if not addrs or not limit:
        return None

    return {"desc": desc, "max": int(limit), "bytes": addrs}


def extract_reinkpy(raw_bytes):
    if tomllib is None:
        raise RuntimeError("Python 3.11+ required (tomllib)")
    parsed = tomllib.loads(raw_bytes.decode("utf-8"))

    specs = {}
    models = {}
    for printer in parsed.get("EPSON", []):
        if "models" not in printer or "wkey" not in printer:
            continue

        pad_groups = []
        for mem in printer.get("mem", []):
            raw_desc = mem.get("desc", "")
            if "Waste" not in raw_desc and "Platen" not in raw_desc:
                continue

            desc = normalize_desc(raw_desc)
            addrs = list(mem.get("addr", []))
            resets = list(mem.get("reset", [])) or [0] * len(addrs)

            if pad_groups and pad_groups[-1]["desc"] == desc:
                group = pad_groups[-1]
                group["addresses"].extend(addrs)
                group["reset"].extend(resets)
            else:
                group = {
                    "desc": desc,
                    "kind": KIND_BY_DESC.get(desc, ""),
                    "addresses": list(addrs),
                    "reset": list(resets),
                }
                pad_groups.append(group)

            spec = counter_spec(desc, mem)
            if spec:
                group.setdefault("counters", []).append(spec)

        spec_id = sorted(printer["models"])[0]
        while spec_id in specs:  # two upstream entries sharing a first model
            spec_id += "+"
        specs[spec_id] = {
            "rkey": printer.get("rkey", 0),
            "wkey": printer.get("wkey", ""),
            "wkey1": printer.get("wkey1", ""),
            "rlen": printer.get("rlen", 2),
            "wlen": printer.get("wlen", 2),
            "mem_high": printer.get("mem_high", 2047),
            "pad_groups": pad_groups,
        }
        for model in printer["models"]:
            models[model] = spec_id  # last wins, same as update_db.py

    if not models:
        raise RuntimeError("upstream returned no usable models")
    return {"specs": specs, "models": models}


# ---------------------------------------------------------------------------
# ez-reset adapter -- counter byte maps (incl. masks), service limits, the
# post-reset commit step, and the marketing/MDL name rows used for aliases.
# ---------------------------------------------------------------------------

def _int_auto(tok):
    tok = tok.strip()
    return int(tok, 16) if tok.lower().startswith("0x") else int(tok, 10)


def _load_xml(raw_bytes):
    text = raw_bytes.decode("utf-8", errors="replace")
    try:
        return ET.fromstring(text)
    except ET.ParseError:
        # The file in the wild carries stray control chars / bare ampersands.
        text = re.sub(r"[\x00-\x08\x0B\x0C\x0E-\x1F]", "", text)
        text = re.sub(r"&(?![A-Za-z]+;|#[0-9]+;|#x[0-9A-Fa-f]+;)", "&amp;", text)
        return ET.fromstring(text)


def _parse_counter(counter_el, notes, spec_name):
    """Normalize both counter forms to {"bytes": [...], "max": int?}.

    Old form:  <counter>0x0D 0x0C<min>20000</min><max>46750</max></counter>
    New form:  <counter><entry>0x30 0x31</entry>
                        <entry>0x2F<filter>0x0F -1 254</filter></entry>
                        <max>6346</max></counter>
    A filter is "<mask> <shift> <weight>" with shift -1 = derive from mask,
    which is exactly what the exe's mask handling does.
    """
    limit = None
    max_el = counter_el.find("max")
    if max_el is not None and (max_el.text or "").strip():
        try:
            limit = _int_auto(max_el.text)
        except ValueError:
            notes.append(spec_name + ": bad counter max " + repr(max_el.text))

    bytes_out = []
    entries = counter_el.findall("entry")
    if entries:
        for entry in entries:
            addrs, bad = parse_hex_tokens(entry.text)
            if bad:
                notes.append(spec_name + ": unparsed counter tokens " + repr(bad))
            flt = entry.find("filter")
            if flt is None:
                bytes_out.extend(addrs)
                continue
            toks = (flt.text or "").split()
            try:
                mask = int(toks[0], 16)
                shift = int(toks[1], 10)
                weight = _int_auto(toks[2])
            except (IndexError, ValueError):
                notes.append(spec_name + ": bad counter filter " + repr(flt.text))
                return None
            derived = (mask & -mask).bit_length() - 1 if mask else 0
            if shift not in (-1, derived):
                notes.append(spec_name + ": unsupported filter shift " + str(shift))
                return None
            for a in addrs:
                bytes_out.append({"addr": a, "mask": mask, "weight": weight})
    else:
        raw = counter_el.text or ""
        for child in counter_el:
            raw += " " + (child.tail or "")
        addrs, bad = parse_hex_tokens(raw)
        if bad:
            notes.append(spec_name + ": unparsed counter tokens " + repr(bad))
        bytes_out.extend(addrs)

    if not bytes_out:
        return None
    out = {"bytes": bytes_out}
    if limit:
        out["max"] = limit
    return out


def _parse_close(close_el, notes, spec_name):
    """Parse a read-modify-write commit step into {"addr", "and"?, "or"?}.

    <close><query>0x100 $byte</query>
           <var>$byte : $byte 0xFE &</var>
           <write>0x100 $byte</write></close>
    """
    q_vals, _ = parse_hex_tokens((close_el.findtext("query") or "").replace("$byte", " "))
    w_vals, _ = parse_hex_tokens((close_el.findtext("write") or "").replace("$byte", " "))
    if not q_vals or not w_vals or q_vals[0] != w_vals[0]:
        notes.append(spec_name + ": close query/write address mismatch")
        return None

    var = (close_el.findtext("var") or "").strip()
    body = var.split(":", 1)[1] if ":" in var else var
    and_mask = None
    or_mask = None
    pend = None
    for tok in body.split():
        if tok == "$byte":
            continue
        if tok in ("&", "|"):
            if pend is None:
                notes.append(spec_name + ": close var not understood " + repr(var))
                return None
            if tok == "&":
                and_mask = pend if and_mask is None else (and_mask & pend)
            else:
                or_mask = pend if or_mask is None else (or_mask | pend)
            pend = None
            continue
        vals, bad = parse_hex_tokens(tok)
        if bad or not vals:
            notes.append(spec_name + ": close var not understood " + repr(var))
            return None
        pend = vals[0]
    if and_mask is None and or_mask is None:
        notes.append(spec_name + ": close var has no operation " + repr(var))
        return None

    out = {"addr": q_vals[0]}
    if and_mask is not None:
        out["and"] = and_mask
    if or_mask is not None:
        out["or"] = or_mask
    return out


def extract_ezreset(raw_bytes):
    root = _load_xml(raw_bytes)
    version = root.get("version")
    notes = []

    rows = []
    for pr in root.iter("printer"):
        if (pr.get("brand") or "").lower() != "epson":
            continue
        specs_attr = (pr.get("specs") or "").split(",")
        rows.append({
            "title": (pr.get("title") or "").strip(),
            "short": (pr.get("short") or "").strip(),
            "model": (pr.get("model") or "").strip(),
            "spec": specs_attr[1].strip() if len(specs_attr) > 1 else "",
        })
    rows.sort(key=lambda r: (r["spec"], r["title"]))

    specs = {}
    for el in root.iter():
        service = el.find("service")
        if service is None or el.tag == "printer":
            continue
        name = el.tag
        if name in specs:
            notes.append(name + ": duplicate spec element, last one wins")
        spec = {}

        rl = service.findtext("readlen")
        sl = service.findtext("sendlen")
        if rl:
            spec["rlen"] = int(rl.strip(), 16)
        if sl:
            spec["wlen"] = int(sl.strip(), 16)
        fac, bad = parse_hex_tokens(service.findtext("factory"))
        if len(fac) >= 2 and not bad:
            spec["rkey"] = fac[0] | (fac[1] << 8)  # little-endian on the wire
        kw, bad = parse_hex_tokens(service.findtext("keyword"))
        if kw and not bad:
            spec["wkey"] = "".join(chr(b) for b in kw)

        mem = el.find("memory")
        if mem is not None and (mem.findtext("upper") or "").strip():
            spec["mem_high"] = int(mem.findtext("upper").strip(), 16)

        waste = el.find("waste")
        if waste is not None:
            reset_el = waste.find("reset")
            if reset_el is not None:
                raw = reset_el.text or ""
                closes = []
                for child in reset_el:
                    if child.tag == "close":
                        c = _parse_close(child, notes, name)
                        if c:
                            closes.append(c)
                    raw += " " + (child.tail or "")
                vals, bad = parse_hex_tokens(raw)
                if bad:
                    notes.append(name + ": unparsed reset tokens " + repr(bad))
                elif len(vals) % 2 == 1:
                    notes.append(name + ": odd reset token count, ignored")
                else:
                    pairs = {}
                    clash = False
                    for a, v in zip(vals[0::2], vals[1::2]):
                        if a in pairs and pairs[a] != v:
                            notes.append(name + ": conflicting reset values for addr " + hex(a))
                            clash = True
                            break
                        pairs[a] = v
                    if pairs and not clash:
                        spec["reset"] = [[a, pairs[a]] for a in sorted(pairs)]
                if closes:
                    spec["close"] = closes
            query = waste.find("query")
            if query is not None:
                counters = []
                for c_el in query.findall("counter"):
                    c = _parse_counter(c_el, notes, name)
                    if c:
                        counters.append(c)
                if counters:
                    spec["counters"] = counters

        if spec:
            specs[name] = spec

    # Firmware recovery ('RCMODE') channels live in <EPSON-IPL><firmware>,
    # separate from the per-model service specs above. Each <rcmode> block
    # names one or more models via <model><label>, and carries the enter
    # (<start>) and leave (<close>) commands plus the expected reply token.
    recovery = []
    for rc in root.iter("rcmode"):
        model_el = rc.find("model")
        labels = (model_el.findtext("label") if model_el is not None else "") or ""
        labels = labels.split()
        start = rc.find("start/raw")
        if not labels or start is None:
            continue
        service = (start.findtext("group") or "").strip()
        enter, bad_e = parse_hex_tokens(start.findtext("query"))
        reply, _ = parse_hex_tokens(start.findtext("reply"))
        close_raw = rc.find("close/raw")
        close_cmd = []
        if close_raw is not None:
            close_cmd, _ = parse_hex_tokens(close_raw.findtext("query"))
        if not service or not enter:
            notes.append("rcmode " + " ".join(labels) + ": missing service or enter command")
            continue
        if bad_e:
            notes.append("rcmode " + " ".join(labels) + ": unparsed enter tokens " + repr(bad_e))
        recovery.append({
            "labels": labels,
            "service": service,
            "enter": enter,
            "close": close_cmd,
            "reply": reply,
        })

    return {"specs": specs, "rows": rows, "recovery": recovery,
            "notes": sorted(set(notes))}, version


# ---------------------------------------------------------------------------
# reink adapter -- independent waste-address cross-check for the old
# R220-class models, and the only upstream carrying per-color cartridge
# ink-reset maps (Phase 7). GPLv3 project: we read facts, we take no code.
#
# Ink counters live on the cartridge chip; the printer mirrors them into its
# own EEPROM as used-ink units (0 == full). Resetting a color means writing
# zeros to that mirror with the same factory write command the waste reset
# already uses; the firmware pushes the values back to the cartridge chip
# over CSIC on the next power cycle (so: power-cycle after an ink reset).
# ---------------------------------------------------------------------------

_INK_COLORS = ("black", "cyan", "magenta", "yellow", "lightcyan", "lightmagenta")


def _parse_inkmap(block):
    """Extract the per-color ink-reset address map from one printer_t block.

    Colors come from the .mask bit list; a color contributes only when its
    four mirror addresses are present and non-zero. The reset value is zero
    on every byte -- zero used ink == full cartridge.
    """
    mask_m = re.search(r"\.mask\s*=\s*([^,]+),", block)
    if not mask_m:
        return []
    enabled = {c.lower() for c in re.findall(r"INK_(\w+)", mask_m.group(1))}
    groups = []
    for color in _INK_COLORS:
        if color not in enabled:
            continue
        cm = re.search(r"\.%s\s*=\s*\{([^}]*)\}" % color, block)
        if not cm:
            continue
        caddrs = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]+)", cm.group(1))]
        if not caddrs or not any(caddrs):
            continue
        groups.append({
            "color": color,
            "addresses": caddrs,
            "reset": [0] * len(caddrs),
        })
    return groups


def extract_reink(raw_bytes):
    text = raw_bytes.decode("utf-8", errors="replace")
    specs = {}
    for m in re.finditer(r"\[PM_(\w+)\]\s*=\s*\{(.*?)\n\t\},?", text, re.S):
        pm, block = m.group(1), m.group(2)
        name_m = re.search(r'\.name\s*=\s*"([^"]*)"', block)
        model_m = re.search(r'\.model_name\s*=\s*"([^"]*)"', block)
        two_m = re.search(r"\.twobyte_addresses\s*=\s*(\d+)", block)
        wm = re.search(r"\.wastemap\s*=\s*\{(.*)\}", block, re.S)
        if not (name_m and wm):
            continue
        len_m = re.search(r"\.len\s*=\s*(\d+)", wm.group(1))
        addr_m = re.search(r"\.addr\s*=\s*\{([^}]*)\}", wm.group(1))
        if not (len_m and addr_m):
            continue
        count = int(len_m.group(1))
        if count <= 0:
            continue
        addrs = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]+)", addr_m.group(1))][:count]
        names = [name_m.group(1)]
        if model_m and model_m.group(1) not in names:
            names.append(model_m.group(1))
        specs[pm] = {
            "names": names,
            "waste_addresses": sorted(addrs),
            "address_bytes": 2 if two_m and two_m.group(1) != "0" else 1,
        }
        ink_groups = _parse_inkmap(block)
        if ink_groups:
            specs[pm]["ink_groups"] = ink_groups
    if not specs:
        raise RuntimeError("no printer_t entries recognized")
    return {"specs": dict(sorted(specs.items()))}


# ---------------------------------------------------------------------------
# gutenprint adapter -- marketing name variants for device-ID matching.
# ---------------------------------------------------------------------------

def extract_gutenprint(raw_bytes):
    root = _load_xml(raw_bytes)
    names = set()
    for el in root.iter():
        if el.tag.rsplit("}", 1)[-1] != "printer":
            continue
        if (el.get("manufacturer") or "").upper() != "EPSON":
            continue
        name = (el.get("name") or "").strip()
        if name:
            names.add(name)
    if not names:
        raise RuntimeError("no Epson printer entries recognized")
    return {"names": sorted(names)}


# ---------------------------------------------------------------------------
# network fetch -- every source, fresh, on every run
# ---------------------------------------------------------------------------

def fetch_bytes(url):
    req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0 (EWR-Updater)"})
    with urllib.request.urlopen(req, timeout=60) as resp:
        return resp.read()


def extract_source(source):
    """Fetch one upstream over the network and parse it in memory.

    Raises on any network or parse failure; cmd_build treats that as
    "this source contributes nothing today" and moves on.
    """
    raw = fetch_bytes(SOURCE_URLS[source])
    if source == "reinkpy":
        return extract_reinkpy(raw)
    if source == "ezreset":
        payload, _version = extract_ezreset(raw)
        return payload
    if source == "reink":
        return extract_reink(raw)
    # gutenprint: a release tarball is also accepted -- pull escp2.xml out
    # of it in memory (magic-byte sniff, so a raw XML URL works too).
    if raw[:6] == b"\xfd7zXZ\x00":
        with tarfile.open(fileobj=io.BytesIO(raw), mode="r:xz") as tf:
            member = next(m for m in tf.getmembers()
                          if m.name.endswith("src/xml/printers/escp2.xml"))
            raw = tf.extractfile(member).read()
    return extract_gutenprint(raw)


# ---------------------------------------------------------------------------
# merge
# ---------------------------------------------------------------------------

def assemble_base(reinkpy):
    """Build per-model entries exactly the way update_db.py did."""
    db = {}
    for model, spec_id in reinkpy["models"].items():
        spec = reinkpy["specs"][spec_id]
        pad_groups = copy.deepcopy(spec["pad_groups"])
        db[model] = {
            "rkey": spec["rkey"],
            "wkey": spec["wkey"],
            "wkey1": spec["wkey1"],
            "rlen": spec["rlen"],
            "wlen": spec["wlen"],
            "mem_high": spec["mem_high"],
            # old clients read ONLY these
            "addresses": [a for pg in pad_groups for a in pg["addresses"]],
            "reset": [r for pg in pad_groups for r in pg["reset"]],
            # current clients prefer these
            "pad_groups": pad_groups,
        }
    return db


def build_name_index(db):
    idx = {}
    for model in db:
        key = norm_name(model)
        if key:
            idx.setdefault(key, set()).add(model)
    return idx


def match_models(idx, names):
    hit = set()
    for name in names:
        key = norm_name(name)
        if key:
            hit |= idx.get(key, set())
    return hit


def _counter_addrs(counter):
    out = set()
    for b in counter.get("bytes", []):
        out.add(b["addr"] if isinstance(b, dict) else b)
    return out


def _attach_counters(entry, counters):
    """Attach ez-reset counter specs to the pad group sharing their bytes.

    Richer than what reinkpy offers (masks, shared bytes, service limits),
    so an overlapping existing counter is replaced; its desc is kept.
    Display-only data -- never touches the write path.
    """
    groups = entry.get("pad_groups") or []
    attached = 0
    for c in counters:
        addrs = _counter_addrs(c)
        best = None
        best_n = 0
        for g in groups:
            n = len(addrs & set(g["addresses"]))
            if n > best_n:
                best, best_n = g, n
        if best is None:
            continue
        existing = best.setdefault("counters", [])
        new_c = {"bytes": copy.deepcopy(c["bytes"])}
        if "max" in c:
            new_c["max"] = c["max"]
        replaced = False
        for i, old in enumerate(existing):
            if _counter_addrs(old) & addrs:
                new_c["desc"] = old.get("desc", "Waste counter")
                if "max" not in new_c and "max" in old:
                    new_c["max"] = old["max"]
                existing[i] = new_c
                replaced = True
                break
        if not replaced:
            new_c["desc"] = best.get("desc") or "Waste counter"
            existing.append(new_c)
        attached += 1
    return attached


def _add_conflict(entry, p, field, shipped, theirs, source):
    entry["conflict"] = True
    p.setdefault("conflicts", []).append(
        {"field": field, "shipped": shipped, source: theirs})


def _apply_ezreset(entry, model, spec_name, spec, rows, p):
    p.setdefault("enriched_by", []).append("ezreset:" + spec_name)
    agree = p.setdefault("agrees_with_ezreset", [])

    # --- write-path comparison: records, never changes the entry ---
    if "rkey" in spec:
        if spec["rkey"] == entry["rkey"]:
            agree.append("rkey")
        else:
            _add_conflict(entry, p, "rkey", entry["rkey"], spec["rkey"], "ezreset")
    if "wkey" in spec:
        theirs = spec["wkey"].rstrip("\x00")
        if theirs == entry["wkey"].rstrip("\x00"):
            agree.append("wkey")
        elif theirs == (entry.get("wkey1") or "").rstrip("\x00"):
            agree.append("wkey1")
        else:
            _add_conflict(entry, p, "wkey", entry["wkey"], spec["wkey"], "ezreset")
    for f in ("rlen", "wlen"):
        if f in spec:
            if spec[f] == entry[f]:
                agree.append(f)
            else:
                _add_conflict(entry, p, f, entry[f], spec[f], "ezreset")
    if "mem_high" in spec and spec["mem_high"] != entry["mem_high"]:
        p.setdefault("notes", []).append(
            f"ezreset memory upper is {spec['mem_high']} (shipped {entry['mem_high']})")
    if "reset" in spec:
        ours = {}
        for a, v in zip(entry["addresses"], entry["reset"]):
            ours[a] = v
        theirs = {a: v for a, v in spec["reset"]}
        shared = sorted(set(ours) & set(theirs))
        mismatch = [a for a in shared if ours[a] != theirs[a]]
        if mismatch:
            _add_conflict(entry, p, "reset",
                          {f"{a:#x}": ours[a] for a in mismatch},
                          {f"{a:#x}": theirs[a] for a in mismatch}, "ezreset")
        elif shared:
            agree.append("reset(shared addresses)")
        extra = sorted(set(theirs) - set(ours))
        if extra:
            p.setdefault("notes", []).append(
                "ezreset also resets " + ", ".join(f"{a:#x}={theirs[a]}" for a in extra))

    # --- additive enrichment old clients ignore ---
    added = p.setdefault("added", [])
    if "close" in spec and not entry.get("close"):
        entry["close"] = copy.deepcopy(spec["close"])
        added.append("close")
    if "counters" in spec:
        n = _attach_counters(entry, spec["counters"])
        if n:
            added.append(f"counters({n})")
    aliases = set(entry.get("aliases", []))
    for r in rows:
        for nm in (r["title"], r["short"], r["model"]):
            if nm and nm != model:
                aliases.add(nm)
    if spec_name != model:
        aliases.add(spec_name)
    if aliases:
        entry["aliases"] = sorted(aliases)


def merge_sources(db, facts):
    prov_models = {}
    coverage = {}

    def prov(model):
        return prov_models.setdefault(model, {})

    idx = build_name_index(db)

    ez = facts.get("ezreset")
    if ez:
        rows_by_spec = {}
        for row in ez.get("rows", []):
            rows_by_spec.setdefault(row["spec"], []).append(row)
        unmatched = []
        ambiguous = []
        spec_by_model = {}
        for spec_name in sorted(ez["specs"]):
            spec = ez["specs"][spec_name]
            rows = rows_by_spec.get(spec_name, [])
            cands = {spec_name}
            for r in rows:
                cands |= {r["title"], r["short"], r["model"]}
            matched = match_models(idx, cands)
            if not matched:
                unmatched.append(spec_name)
                continue
            for model in sorted(matched):
                if model in spec_by_model and spec_by_model[model] != spec_name:
                    ambiguous.append(f"{model}: {spec_by_model[model]} vs {spec_name}")
                    continue
                spec_by_model[model] = spec_name
                _apply_ezreset(db[model], model, spec_name, spec, rows, prov(model))
        coverage["ezreset_specs_unmatched"] = unmatched
        coverage["ezreset_ambiguous_matches"] = sorted(ambiguous)

        # Firmware-recovery ('RCMODE') channels: match each block's labels to
        # model entries and attach the channel additively. Older clients ignore
        # the key; the new executor uses it to switch the printer into recovery
        # around the EEPROM writes (issue #16). First match wins so the result
        # is deterministic when families overlap.
        rec_unmatched = []
        for rec in ez.get("recovery", []):
            matched = match_models(idx, rec["labels"])
            if not matched:
                rec_unmatched.append(" ".join(rec["labels"]))
                continue
            for model in sorted(matched):
                if "recovery" in db[model]:
                    continue
                db[model]["recovery"] = {
                    "service": rec["service"],
                    "enter": list(rec["enter"]),
                    "close": list(rec["close"]),
                    "reply": list(rec["reply"]),
                }
                p = prov(model)
                p.setdefault("enriched_by", []).append("ezreset:rcmode")
                p.setdefault("added", []).append("recovery(" + rec["service"] + ")")
        coverage["ezreset_rcmode_unmatched"] = sorted(set(rec_unmatched))

        # A RCMODE <label> only names a representative of each family (e.g.
        # "ET-2800"), but reinkpy ships finer sibling entries (ET-2801/2803/
        # 2805) with a byte-identical write path. RCMODE is a firmware property
        # of that shared mainboard, so propagate the channel to every sibling
        # whose full write path matches a recovered model's. Only propagate
        # when a write path resolves to a single channel; identical write paths
        # that disagree are left untouched and reported instead.
        def _writepath_key(entry):
            return json.dumps([entry.get(k) for k in
                               ("rkey", "wkey", "wkey1", "rlen", "wlen",
                                "mem_high", "addresses", "reset")],
                              sort_keys=True)
        channel_by_writepath = {}
        ambiguous_writepaths = set()
        for entry in db.values():
            if not entry.get("recovery"):
                continue
            wp = _writepath_key(entry)
            chan = json.dumps(entry["recovery"], sort_keys=True)
            if wp in channel_by_writepath and channel_by_writepath[wp] != chan:
                ambiguous_writepaths.add(wp)
            else:
                channel_by_writepath[wp] = chan
        propagated = 0
        for name, entry in db.items():
            if entry.get("recovery"):
                continue
            wp = _writepath_key(entry)
            if wp in channel_by_writepath and wp not in ambiguous_writepaths:
                entry["recovery"] = json.loads(channel_by_writepath[wp])
                p = prov(name)
                p.setdefault("enriched_by", []).append("ezreset:rcmode(family)")
                p.setdefault("added", []).append("recovery(write-path sibling)")
                propagated += 1
        coverage["ezreset_rcmode_propagated"] = propagated

    rk = facts.get("reink")
    if rk:
        unmatched = []
        for pm in sorted(rk["specs"]):
            spec = rk["specs"][pm]
            matched = match_models(idx, spec["names"])
            if not matched:
                unmatched.append(spec["names"][0])
                continue
            for model in sorted(matched):
                entry = db[model]
                p = prov(model)
                p.setdefault("enriched_by", []).append("reink:" + pm)
                ours = set(entry["addresses"])
                theirs = set(spec["waste_addresses"])
                if theirs <= ours:
                    p.setdefault("agrees_with_reink", []).append("addresses")
                else:
                    p.setdefault("notes", []).append(
                        "reink lists extra waste addresses "
                        + ", ".join(f"{a:#x}" for a in sorted(theirs - ours)))
                if spec.get("address_bytes") and spec["address_bytes"] != entry["rlen"]:
                    p.setdefault("notes", []).append(
                        f"reink uses {spec['address_bytes']}-byte addressing (shipped rlen {entry['rlen']})")
                # Additive Phase 7 enrichment: per-color cartridge ink-reset
                # maps. Same factory write path as the waste reset; old
                # clients and the waste flow ignore the key entirely.
                if spec.get("ink_groups") and not entry.get("ink_groups"):
                    entry["ink_groups"] = copy.deepcopy(spec["ink_groups"])
                    p.setdefault("added", []).append(
                        "ink_groups(%d)" % len(spec["ink_groups"]))
        coverage["reink_models_unmatched"] = unmatched

    gp = facts.get("gutenprint")
    if gp:
        alias_hits = 0
        for name in gp["names"]:
            for model in sorted(match_models(idx, [name])):
                if name == model:
                    continue
                entry = db[model]
                aliases = set(entry.get("aliases", []))
                if name not in aliases:
                    aliases.add(name)
                    entry["aliases"] = sorted(aliases)
                    alias_hits += 1
        coverage["gutenprint_alias_matches"] = alias_hits

    return prov_models, coverage


def _cxx_norm(name):
    """Mirror of NormalizeModelName in src/deviceid.cpp: uppercase, collapse
    punctuation runs into single spaces, drop an 'EPSON ' prefix and a
    ' SERIES' suffix. Detection compares names in exactly this form, so
    alias collisions must be judged in it too."""
    out = []
    last_space = True
    for ch in name.upper():
        if ch.isascii() and ch.isalnum():
            out.append(ch)
            last_space = False
        elif not last_space:
            out.append(" ")
            last_space = True
    s = "".join(out).strip()
    if s.startswith("EPSON "):
        s = s[len("EPSON "):]
    if s.endswith(" SERIES") and len(s) > len(" SERIES"):
        s = s[: -len(" SERIES")]
    return s.strip()


def _expand_name_variants(name):
    """Expand an Epson slash family list into individual model names:
    'ET-2800/2801/2803/2805' -> all four full names. Digit-only segments are
    grafted onto the first segment's prefix; segments that already carry
    letters stand on their own. The original string is kept too."""
    out = {name}
    if "/" not in name:
        return out
    parts = [p.strip() for p in name.split("/") if p.strip()]
    if len(parts) < 2:
        return out
    first = parts[0]
    m = re.search(r"\d+$", first)
    out.add(first)
    for seg in parts[1:]:
        if seg.isdigit() and m:
            out.add(first[: m.start()] + seg)
        else:
            out.add(seg)
    return out


def finalize_aliases(db, coverage):
    """Last pass over the merged aliases before output.

    The merge pools spec-level names onto every model sharing the spec
    ('Stylus Photo R230' lands on R220 too), so ownership is decided here:

    1. An alias that normalizes to any model's own key is dropped - the
       detector already matches real keys directly.
    2. An alias claimed by several models goes to the one whose own key
       appears in it as whole words ('Stylus Photo R220' -> R220). If that
       does not single out exactly one owner, the alias is dropped.
    3. Surviving aliases expand their slash family lists on their owner
       ('ET-2800/2801/2803/2805' -> ET-2801, ET-2803, ...), and expanded
       variants are checked against the model keys and each other again.

    Detection stays dumb because the build resolves every collision here.
    """

    def contains_whole_words(hay, needle):
        return f" {hay} ".find(f" {needle} ") != -1

    key_norms = {}
    for model in sorted(db):
        key_norms.setdefault(_cxx_norm(model), model)

    raw = {}
    claims = {}
    for model in sorted(db):
        aliases = set(db[model].get("aliases") or [])
        raw[model] = aliases
        for alias in aliases:
            n = _cxx_norm(alias)
            if n:
                claims.setdefault(n, set()).add(model)

    dropped_keys = 0
    dropped_ambiguous = 0
    reassigned = 0

    resolved = {model: set() for model in raw}
    for n in sorted(claims):
        claimants = claims[n]
        if n in key_norms:
            dropped_keys += 1
            continue
        owners = sorted(claimants)
        if len(owners) > 1:
            picked = [m for m in owners
                      if contains_whole_words(n, _cxx_norm(m))]
            if len(picked) != 1:
                dropped_ambiguous += 1
                continue
            owners = picked
            reassigned += 1
        # One spelling per normalized name is enough for the detector.
        spelling = sorted(a for m in claimants for a in raw[m]
                          if _cxx_norm(a) == n)[0]
        resolved[owners[0]].add(spelling)

    expanded = {}
    expanded_claims = {}
    for model, aliases in resolved.items():
        names = set()
        for alias in aliases:
            names |= _expand_name_variants(alias)
        keep = set()
        for name in names:
            n = _cxx_norm(name)
            if not n:
                continue
            if n in key_norms:
                dropped_keys += 1  # a real model: it resolves as itself
                continue
            keep.add(name)
            expanded_claims.setdefault(n, set()).add(model)
        expanded[model] = keep

    dropped_cross = 0
    for model in sorted(db):
        kept = []
        seen = set()
        for name in sorted(expanded.get(model, ())):
            n = _cxx_norm(name)
            if n in seen:
                continue
            if len(expanded_claims.get(n, ())) > 1:
                dropped_cross += 1
                continue
            seen.add(n)
            kept.append(name)
        entry = db[model]
        if kept:
            entry["aliases"] = kept
        else:
            entry.pop("aliases", None)

    coverage["aliases_dropped_model_keys"] = dropped_keys
    coverage["aliases_dropped_ambiguous"] = dropped_ambiguous + dropped_cross
    coverage["aliases_reassigned_to_owner"] = reassigned


# ---------------------------------------------------------------------------
# output
# ---------------------------------------------------------------------------

def compact(db):
    """Fold identical model bodies into shared spec groups (schema 4)."""
    grouped = {}
    for name in sorted(db):
        body = {k: v for k, v in db[name].items() if k not in ("addresses", "reset")}
        grouped.setdefault(json.dumps(body, sort_keys=True), []).append(name)

    specs = {}
    models = {}
    for body_key, names in grouped.items():
        body = json.loads(body_key)
        if len(names) == 1:
            models[names[0]] = body
            continue
        spec_name = f"{names[0]}-family"
        specs[spec_name] = body
        for name in names:
            models[name] = {"spec": spec_name}

    return {"schema_version": SCHEMA_VERSION, "specs": specs, "models": models}


def diff_summary(old, new_db):
    if not isinstance(old, dict) or "schema_version" in old:
        return ["existing output is compact form; field diff skipped"]
    added = sorted(set(new_db) - set(old))
    removed = sorted(set(old) - set(new_db))
    changed = {}
    for m in set(new_db) & set(old):
        fields = sorted(k for k in set(new_db[m]) | set(old[m])
                        if new_db[m].get(k) != old[m].get(k))
        if fields:
            changed[m] = fields
    wp = sorted(m for m, fs in changed.items()
                if any(f in WRITE_PATH_FIELDS for f in fs))
    lines = [f"vs existing output: +{len(added)} / -{len(removed)} models, {len(changed)} entries changed"]
    if wp:
        shown = ", ".join(wp[:10]) + (" ..." if len(wp) > 10 else "")
        lines.append(f"write-path fields changed on {len(wp)} models: {shown}")
    else:
        lines.append("write-path fields changed on 0 models")
    return lines


def load_committed(out_path):
    """The committed database.json, loaded as the curated top layer."""
    if not out_path.exists():
        return {}
    try:
        data = json.loads(out_path.read_text(encoding="utf-8"))
    except Exception as e:
        print(f"[-] {out_path.name} unreadable ({e}); building from sources only")
        return {}
    if not isinstance(data, dict) or "schema_version" in data:
        print(f"[-] {out_path.name} is not flat form; curated overlay skipped")
        return {}
    return data


def _aligned_key(ours, theirs):
    """The key a list of dicts pairs up on, if both sides have one."""
    for key in ("addresses", "bytes"):
        if (ours and theirs
                and all(isinstance(x, dict) and key in x for x in ours)
                and all(isinstance(x, dict) and key in x for x in theirs)):
            return key
    return None


def _curated_merge(ours, theirs, diverged, path):
    """Committed value wins; the upstream merge only fills gaps, at any depth."""
    if isinstance(ours, dict) and isinstance(theirs, dict):
        out = {}
        for k, v in ours.items():
            if k in theirs:
                out[k] = _curated_merge(v, theirs[k], diverged, path + [k])
            else:
                out[k] = copy.deepcopy(v)
        for k, v in theirs.items():
            if k not in ours:
                out[k] = copy.deepcopy(v)
        return out
    key = (_aligned_key(ours, theirs)
           if isinstance(ours, list) and isinstance(theirs, list) else None)
    if key:
        theirs_by = {json.dumps(t[key]): t for t in theirs}
        seen = set()
        out = []
        for o in ours:
            kid = json.dumps(o[key])
            if kid in theirs_by:
                seen.add(kid)
                out.append(_curated_merge(o, theirs_by[kid], diverged,
                                          path + [kid]))
            else:
                out.append(copy.deepcopy(o))
        out.extend(copy.deepcopy(t) for t in theirs
                   if json.dumps(t[key]) not in seen)
        return out
    if ours != theirs:
        diverged.append(".".join(path))
    return copy.deepcopy(ours)


def overlay_curated(db, ours, prov_models, coverage):
    """database.json is the database: whatever is committed there, wins.

        value present in the committed file -> kept byte for byte
        value only in the upstream merge    -> filled in
        model only in the committed file    -> kept (hand-added via PR)
        model only in the upstream merge    -> appended

    The rule holds at every depth, not just per field: a hand-edited
    reset vector inside one pad group must not stop upstream counter
    specs from landing in the group next to it. Pad groups pair up on
    "addresses", counter specs on "bytes"; everything else that exists
    on both sides stays exactly as committed, and real disagreements
    are counted and reported, never silently flattened.

    The build can only ever ADD to what a human committed, never overwrite
    it, so editing database.json and opening a PR is the contribution
    workflow.
    """
    override_fields = 0
    override_models = set()
    added = []
    for model in sorted(ours):
        body = ours[model]
        if not isinstance(body, dict):
            print(f"[-] curated entry '{model}' is not an object; ignored")
            continue
        if model not in db:
            db[model] = copy.deepcopy(body)
            missing = sorted({"rkey", "wkey", "addresses", "reset"} - set(body))
            if missing:
                print(f"[-] curated model '{model}' is missing {missing}; kept, "
                      f"but clients will skip it until those fields exist")
            added.append(model)
            prov_models.setdefault(model, {})["curated"] = "hand-added via database.json"
            continue
        entry = db[model]
        diverged = []
        for field in sorted(body):
            value = body[field]
            if field in entry:
                entry[field] = _curated_merge(value, entry[field],
                                              diverged, [field])
            else:
                entry[field] = copy.deepcopy(value)
        if diverged:
            override_fields += len(diverged)
            override_models.add(model)
            prov_models.setdefault(model, {})["curated_overrides"] = diverged
    coverage["curated_overrides_fields"] = override_fields
    coverage["curated_override_models"] = len(override_models)
    coverage["curated_added_models"] = added


def cmd_build(args):
    facts = {}
    for source in SOURCE_ORDER:
        try:
            facts[source] = extract_source(source)
            print("[+] " + source + ": fetched " + SOURCE_URLS[source])
        except Exception as e:
            facts[source] = None
            print("[-] " + source + ": skipped (" + str(e) + "); everything it "
                  "contributed before is already committed in database.json")
    if not facts["reinkpy"]:
        print("[-] FATAL: could not fetch reinkpy, the write-path base. "
              "Check the network and retry.")
        return 1
    loaded = [s for s in SOURCE_ORDER if facts[s]]

    db = assemble_base(facts["reinkpy"])
    prov_models, coverage = merge_sources(db, facts)
    finalize_aliases(db, coverage)

    out_path = Path(args.output)
    overlay_curated(db, load_committed(out_path), prov_models, coverage)

    coverage["models_total"] = len(db)
    coverage["with_counters"] = sum(
        1 for e in db.values() if any("counters" in g for g in e["pad_groups"]))
    coverage["with_close"] = sum(1 for e in db.values() if e.get("close"))
    coverage["with_aliases"] = sum(1 for e in db.values() if e.get("aliases"))
    coverage["with_conflicts"] = sum(1 for e in db.values() if e.get("conflict"))
    coverage["with_recovery"] = sum(1 for e in db.values() if e.get("recovery"))
    coverage["with_ink"] = sum(1 for e in db.values() if e.get("ink_groups"))

    payload = compact(db) if args.compact else db
    new_text = json.dumps(payload, indent=4, sort_keys=True, ensure_ascii=True) + "\n"

    diff = []
    if out_path.exists() and not args.compact:
        try:
            diff = diff_summary(json.loads(out_path.read_text(encoding="utf-8")), db)
        except Exception:
            diff = ["existing output unreadable; field diff skipped"]

    out_path.write_text(new_text, encoding="utf-8", newline="\n")

    print(f"[+] Generated {out_path.name}: {len(db)} models from {', '.join(loaded)}")
    print(f"    {coverage['with_counters']} with counter specs, "
          f"{coverage['with_close']} with a commit step, "
          f"{coverage['with_aliases']} with aliases, "
          f"{coverage['with_conflicts']} flagged 'conflict'")
    if coverage.get("curated_override_models"):
        print(f"    curated: kept {coverage['curated_overrides_fields']} hand-edited "
              f"value(s) on {coverage['curated_override_models']} model(s) from the "
              f"committed database.json")
    if coverage.get("curated_added_models"):
        names = coverage["curated_added_models"]
        shown = ", ".join(names[:5]) + (" ..." if len(names) > 5 else "")
        print(f"    curated: {len(names)} hand-added model(s) not in any upstream "
              f"source: {shown}")
    for line in diff:
        print("    " + line)
    if coverage.get("ezreset_specs_unmatched"):
        n = len(coverage["ezreset_specs_unmatched"])
        print(f"    coverage: {n} ez-reset spec groups have no model entry yet "
              f"(candidates for Phase 5)")
    if coverage.get("with_recovery"):
        print(f"    {coverage['with_recovery']} models carry a firmware-recovery "
              f"(RCMODE) channel for issue-#16 writes")
    if coverage.get("ezreset_rcmode_propagated"):
        print(f"      (of those, {coverage['ezreset_rcmode_propagated']} propagated "
              f"to write-path-identical siblings)")
    if coverage.get("ezreset_rcmode_unmatched"):
        print(f"    coverage: {len(coverage['ezreset_rcmode_unmatched'])} RCMODE "
              f"label groups have no model entry yet")
    if coverage.get("with_ink"):
        print(f"    {coverage['with_ink']} models carry per-color cartridge "
              f"ink-reset maps (Phase 7, seeded from reink)")
    if coverage.get("reink_models_unmatched"):
        print(f"    coverage: reink ink/waste data for "
              f"{len(coverage['reink_models_unmatched'])} printer(s) has no model "
              f"entry yet: " + ", ".join(coverage["reink_models_unmatched"]))
    if args.compact:
        print(f"    Compact form: {len(payload['specs'])} spec groups, "
              f"{len(payload['models'])} model entries.")
    else:
        print("    Flat form: still readable by old clients in the field.")
    return 0


def main():
    ap = argparse.ArgumentParser(
        description="Rebuild database.json: fetch every upstream source over "
                    "the network and merge it around the committed file.")
    sub = ap.add_subparsers(dest="cmd", required=True)

    b = sub.add_parser("build", help="fetch all upstream sources and merge them "
                       "around the committed database.json (which always wins)")
    b.add_argument("--output", default=str(REPO_ROOT / "database.json"))
    b.add_argument("--compact", action="store_true",
                   help="schema 4 envelope with shared spec groups "
                        "(not readable by clients older than 1.2.3)")
    b.set_defaults(func=cmd_build)

    args = ap.parse_args()
    sys.exit(args.func(args) or 0)


if __name__ == "__main__":
    main()
