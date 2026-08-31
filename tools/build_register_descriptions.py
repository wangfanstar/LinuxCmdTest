#!/usr/bin/env python3
"""Extract local reference manuals and compile description-only sidecar packages.

No network calls. Original manuals and register files are opened read-only.

Two modes:
  --inspect          := Extract PDF catalogs to a temp inspection cache (needs pdfplumber).
  --publish          := Build the register-file description overlay JSON (needs pdfplumber).
  --coverage-report  := Also write an unmatched-coverage report JSON.

The key/name helpers (reg_key_of / field_key_of / compact / bit_range / address_values) are
importable without pdfplumber; the key-consistency test relies on this.

Requires pdfplumber for extraction/publishing. Run from any path; defaults are this repo.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import tempfile
from collections import Counter, defaultdict
from datetime import datetime, timezone
from pathlib import Path

try:
    import pdfplumber
    HAS_PDFPLUMBER = True
except Exception:  # pragma: no cover - import failure guards
    pdfplumber = None
    HAS_PDFPLUMBER = False


ROOT = Path(__file__).resolve().parents[1]
MANUALS = {
    "CEFEC": "CEFEC_reference_guide_v1.1.pdf",
    "CEMAC": "CEMAC_reference_guide_v1.7.pdf",
    "CEPCS": "CEPCS_reference_guide_v1.82.pdf",
    "CESOCX16_WRAP": "CESOCX16_WRAP_reference_guide_v1.0.pdf",
}
EXTRACTOR_VERSION = "1"
SEP = "\x1f"

# Register identities are read from the register file and referenced as a single
# immutable string. The browser (register-viewer.html) uses identical keys via
# regKeyOf()/fieldKeyOf(); keep the join order + separator in lockstep.
OVERLAY_FORMAT_VERSION = 1
OVERLAY_NAME = "register_descriptions.json"
DESC_SUFFIX = ".descriptions.json"



def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def clean(text):
    text = (text or "").replace("\u00ad", "")
    text = re.sub(r"(?<=\d)\ufffd+([bhd])(?=[0-9a-fA-F])", r"'\1", text)
    text = re.sub(r"(?<=\w)\ufffd+(?=(?:s|t)\b)", "'", text)
    return re.sub(r"[ \t]+", " ", text).strip()


def compact(text):
    return re.sub(r"[^A-Z0-9]", "", (text or "").upper())


def extract_manual(path, cache_dir):
    if not HAS_PDFPLUMBER:
        raise SystemExit("pdfplumber is required to extract manuals. pip install pdfplumber")
    sha = digest(path)
    cache_path = cache_dir / (sha + "-" + EXTRACTOR_VERSION + ".json")
    if cache_path.exists():
        return json.loads(cache_path.read_text(encoding="utf-8"))
    pages = []
    with pdfplumber.open(path) as pdf:
        for index, page in enumerate(pdf.pages):
            text = page.extract_text(layout=False) or ""
            tables = []
            for table in page.find_tables():
                raw_rows = table.extract()
                if not raw_rows:
                    continue
                columns = [i for i in range(len(raw_rows[0]))
                           if any(row[i] not in (None, "") for row in raw_rows)]
                rows = [[clean(row[i]) for i in columns] for row in raw_rows]
                above = page.crop((0, 0, page.width, max(1, table.bbox[1]))).extract_text() or ""
                captions = re.findall(r"Table\s+(\d+-\d+)\s+([^\n]+)", above)
                headings = re.findall(r"^(\d+(?:\.\d+)+)\s+([^\n]+)", above, re.M)
                tables.append({
                    "rows": rows,
                    "bbox": list(table.bbox),
                    "table": captions[-1][0] if captions else "",
                    "caption": re.sub(r"\s+\d+$", "", captions[-1][1]) if captions else "",
                    "section": headings[-1][0] if headings else "",
                })
            pages.append({"number": index + 1, "text": text, "tables": tables})
            if (index + 1) % 25 == 0:
                print(f"  extracted {path.name}: {index + 1}/{len(pdf.pages)}", flush=True)
    result = {"file": path.name, "sha256": sha, "pageCount": len(pages), "pages": pages}
    cache_dir.mkdir(parents=True, exist_ok=True)
    cache_path.write_text(json.dumps(result, ensure_ascii=False), encoding="utf-8")
    return result


def source(doc, page, table, section=None):
    return {"document": doc["file"], "sha256": doc["sha256"],
            "pdfPage": page, "pageLabel": str(page), "table": table.get("table", ""),
            "section": section or table.get("section", ""),
            "bbox": table.get("bbox")}


def append_continuation(record, text, src):
    record["text"] = clean(record["text"] + "\n" + text)
    if src not in record["sources"]:
        record["sources"].append(src)


def address_values(text):
    text = re.sub(r"\s+", "", text).replace("0X", "0x")
    # Do not guess malformed addresses, including the literal 'OF' in WRAP.
    if re.fullmatch(r"(?:0x)?[0-9a-fA-F]+", text):
        return [int(text.removeprefix("0x"), 16)]
    if re.fullmatch(r"(?:0x)?[0-9a-fA-F]+[~\-](?:0x)?[0-9a-fA-F]+", text):
        a, b = re.split(r"[~\-]", text)
        lo, hi = int(a.removeprefix("0x"), 16), int(b.removeprefix("0x"), 16)
        if 0 <= hi - lo < 512:
            return list(range(lo, hi + 1))
    return []


def bit_range(text):
    text = (text or "").strip().strip("[]").replace(" ", "")
    if not re.fullmatch(r"\d+(?:[:~\-]\d+)?", text):
        return None
    nums = [int(x) for x in re.split(r"[:~\-]", text)]
    return min(nums), max(nums)


def map_catalog(doc, ranges, ip):
    records = []
    for first, last, domain, section in ranges:
        previous = None
        last_table = ""
        for p in doc["pages"][first-1:last]:
            for table in p["tables"]:
                if table["table"]:
                    last_table = table["table"]
                table = dict(table, table=table["table"] or last_table)
                for row in table["rows"]:
                    if len(row) == 6:  # CEPCS: channel, address, name, access, description, reset
                        ch, addr, name, access, desc, reset = row
                    elif len(row) == 5 and ip == "CEPCS":
                        addr, name, access, desc, reset = row
                        ch = ""
                    elif len(row) == 5:
                        addr, name, desc, access, reset = row
                        ch = ""
                    else:
                        continue
                    src = source(doc, p["number"], table, section)
                    if not addr and not name and desc and previous is not None:
                        append_continuation(previous, desc, src)
                        continue
                    values = address_values(addr)
                    if not values or not name or not desc or "Register Name" == name:
                        continue
                    record = {"kind": "register", "ip": ip, "domain": domain,
                              "channel": ch, "addressText": addr, "addresses": values,
                              "name": clean(name.replace("\n", " ")), "text": desc,
                              "access": access, "reset": reset, "sources": [src]}
                    records.append(record)
                    previous = record
    return records


def field_catalog(doc, first, last, ip):
    records = []
    context = None
    previous = None
    for p in doc["pages"][first-1:last]:
        for table in p["tables"]:
            rows = table["rows"]
            title = ""
            if rows and len(rows[0]) >= 2 and rows[0][0] == "" and "register" in rows[0][1].lower():
                title = rows[0][1]
            # CEMAC only has field tables for MAC_CFG and MAC_STATUS.
            if ip == "CEMAC":
                title = "MAC_CFG" if p["number"] <= 123 else "MAC_STATUS"
            is_field = any(len(r) >= 3 and r[0].startswith("Bit") and r[1] == "Name" for r in rows)
            if title and (is_field or ip == "CEMAC"):
                context = {"title": title, "table": table["table"], "section": table["section"]}
                previous = None
            elif is_field:
                context = None
            if context is None:
                continue
            src_table = dict(table, table=context["table"], section=context["section"])
            for row in rows:
                if len(row) != 5:
                    continue
                bits, name, desc, access, reset = row
                src = source(doc, p["number"], src_table)
                if not bits and not name and desc and previous is not None:
                    append_continuation(previous, desc, src)
                    continue
                br = bit_range(bits)
                if br is None or not name or not desc:
                    continue
                record = {"kind": "field", "ip": ip, "registerTitle": context["title"],
                          "name": clean(name.replace("\n", " ")), "startBit": br[0],
                          "endBit": br[1], "text": desc, "access": access,
                          "reset": reset, "reserved": "RESERV" in name.upper(), "sources": [src]}
                records.append(record)
                previous = record
    return records


def build_catalogs(docs):
    return {
        "CEPCS": map_catalog(docs["CEPCS"], [(48,70,"PCS","5.2"),(82,104,"FEC","5.4"),
                                             (112,121,"CUSTOM","5.6")], "CEPCS")
                 + field_catalog(docs["CEPCS"],71,81,"CEPCS")
                 + field_catalog(docs["CEPCS"],105,111,"CEPCS")
                 + field_catalog(docs["CEPCS"],122,146,"CEPCS"),
        "CEMAC": map_catalog(docs["CEMAC"],[(112,120,"MAC","8.1")],"CEMAC")
                 + field_catalog(docs["CEMAC"],121,124,"CEMAC"),
        "CESOCX16_WRAP": map_catalog(docs["CESOCX16_WRAP"],[(44,45,"CESOC_REG","5.2"),
                                                               (47,47,"FLEXEX8_REG","5.2")],"CESOCX16_WRAP"),
        "CEFEC": [],
    }


# ── Description overlay construction ──────────────────────────
# Key helpers. These MUST stay byte-for-byte consistent with the browser
# regKeyOf()/fieldKeyOf() in html/register-viewer.html (join order + SEP).
def reg_key_of(r):
    return SEP.join([coerce(r.get("entryType") or "reg"),
                     coerce(r.get("blockName")),
                     coerce(r.get("subName")),
                     coerce(r.get("regName"))])


def field_key_of(reg_key, f):
    return SEP.join([reg_key,
                     coerce(f.get("name")),
                     coerce(f.get("startBit")),
                     coerce(f.get("endBit"))])


def coerce(v):
    """JS-Array.join coercion: undefined -> '', 0 -> '0' (do NOT treat 0 as falsy)."""
    if v is None:
        return ""
    return str(v)


BIT_LINE_RE = re.compile(r"\[(\d+)(?:\s*[:~-]\s*(\d+))?\]\s+(.+)")


def bit_lines(text):
    """Parse a register description text block like '[0] FEC bypass correction enable'.

    Returns {normName: (loBit, hiBit, rawText)} from each '[N]' / '[Hi:Lo]' line.
    """
    out = {}
    for m in BIT_LINE_RE.finditer(text or ""):
        lo = int(m.group(1))
        hi = int(m.group(2)) if m.group(2) else lo
        if hi < lo:
            lo, hi = hi, lo
        out.setdefault(compact(m.group(3)), (lo, hi, m.group(3).strip()))
    return out


def source_of(rec):
    srcs = rec.get("sources") or []
    if not srcs:
        return None
    s = srcs[0]
    return {"manual": s.get("document", ""), "pdfPage": s.get("pdfPage")}


def build_catalog_indexes(catalogs):
    reg_by_name = defaultdict(list)
    reg_by_addr = defaultdict(list)
    field_by_key = defaultdict(list)     # (nameNorm, startBit, endBit) -> [rec]
    field_by_reg = defaultdict(list)     # (titleNorm, nameNorm, startBit, endBit) -> [rec]
    titles = set()
    for ip, records in catalogs.items():
        for rec in records:
            kind = rec.get("kind")
            if kind == "register":
                name_norm = compact(rec.get("name"))
                reg_by_name[name_norm].append(rec)
                for a in rec.get("addresses") or []:
                    reg_by_addr[a].append(rec)
                titles.add(name_norm)
            elif kind == "field":
                t = compact(rec.get("registerTitle") or "")
                key = (compact(rec.get("name")), rec.get("startBit"), rec.get("endBit"))
                field_by_key[key].append(rec)
                field_by_reg[(t,) + key].append(rec)
                titles.add(t)
    return reg_by_name, reg_by_addr, field_by_key, field_by_reg, titles


def parse_addr(reg):
    a = reg.get("address") or ""
    return int(a.replace("0x", "").replace("0X", ""), 16) if a else None


def resolve_title(reg_name_norm, titles):
    best = ""
    for t in titles:
        if t and reg_name_norm.endswith(t) and len(t) > len(best):
            best = t
    return best


def match_register(reg, reg_by_name, reg_by_addr, titles):
    """Return a catalog register record for a register-file entry, or None."""
    a = parse_addr(reg)
    if a is not None:
        cands = reg_by_addr.get(a) or []
        if len(cands) == 1:
            return cands[0]
    rn = compact(reg.get("regName") or "")
    t = resolve_title(rn, titles)
    if t:
        cands = reg_by_name.get(t) or []
        if len(cands) == 1:
            return cands[0]
        if len(cands) > 1 and a is not None:
            for c in cands:
                if a in (c.get("addresses") or []):
                    return c
    # suffix fallback over all register names
    matches = []
    for name_norm, recs in reg_by_name.items():
        if name_norm and rn.endswith(name_norm):
            matches.extend(recs)
    if len(matches) == 1:
        return matches[0]
    if len(matches) > 1 and a is not None:
        for c in matches:
            if a in (c.get("addresses") or []):
                return c
    return None


def match_field(f, reg_name_norm, field_by_key, field_by_reg, titles):
    key = (compact(f.get("name")), f.get("startBit"), f.get("endBit"))
    t = resolve_title(reg_name_norm, titles)
    if t:
        scoped = field_by_reg.get((t,) + key)
        if scoped:
            return scoped[0]
    global_cands = field_by_key.get(key)
    if not global_cands:
        return None
    if len(global_cands) == 1:
        return global_cands[0]
    if t:
        for c in global_cands:
            if compact(c.get("registerTitle") or "") == t:
                return c
    return global_cands[0]  # ambiguous, first hit; reported as weak


def build_overlay(register_file, catalogs):
    """Scan a register file and fill empty descriptions from the PDF catalogs."""
    reg_by_name, reg_by_addr, field_by_key, field_by_reg, titles = build_catalog_indexes(catalogs)
    regs = json.loads(Path(register_file).read_text(encoding="utf-8-sig"))
    overlay = {"version": OVERLAY_FORMAT_VERSION, "registers": {}}
    reg_desc_empty = reg_desc_filled = 0
    field_empty = field_filled = 0
    regs_patched = 0
    for reg in regs:
        rk = reg_key_of(reg)
        short_empty = not (reg.get("shortDesc") or "").strip()
        full_empty = not (reg.get("fullDesc") or "").strip()
        patch = {"shortDesc": "", "fullDesc": "", "fields": {}, "source": None}
        reg_done = field_done = False
        if short_empty or full_empty:
            reg_desc_empty += 1
            m = match_register(reg, reg_by_name, reg_by_addr, titles)
            if m:
                text = (m.get("text") or "").strip()
                if text:
                    patch["fullDesc"] = text
                    patch["shortDesc"] = text.split("\n")[0].strip() or text
                    patch["source"] = patch["source"] or source_of(m)
                    reg_done = True
        rn = compact(reg.get("regName") or "")
        bl = bit_lines(patch["fullDesc"]) if patch["fullDesc"] else {}
        for f in reg.get("fields") or []:
            if (f.get("description") or "").strip():
                continue
            field_empty += 1
            fm = match_field(f, rn, field_by_key, field_by_reg, titles)
            if fm and (fm.get("text") or "").strip():
                patch["fields"][field_key_of(rk, f)] = fm["text"].strip()
                if not patch["source"]:
                    patch["source"] = source_of(fm)
                field_filled += 1
                field_done = True
                continue
            # Fallback: parse the register-level summary text into per-field desc
            fn = compact(f.get("name") or "")
            if fn and fn in bl:
                lo, hi, raw = bl[fn]
                patch["fields"][field_key_of(rk, f)] = raw
                field_filled += 1
                field_done = True
        if reg_done:
            reg_desc_filled += 1
        if reg_done or field_done:
            regs_patched += 1
            overlay["registers"][rk] = patch
    stats = {"registers": {"empty": reg_desc_empty, "filled": reg_desc_filled},
             "fields": {"empty": field_empty, "filled": field_filled},
             "registersPatched": regs_patched}
    return overlay, stats


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manual-dir", type=Path, default=ROOT / "html/register")
    parser.add_argument("--registers", type=Path,
                        default=ROOT / "html/register/latest/d10_trunk_registers_13285.json")
    parser.add_argument("--output-dir", type=Path, default=None,
                        help="Directory for the overlay (default: same folder as --registers)")
    parser.add_argument("--inspect", action="store_true",
                        help="Extract catalogs to a temporary inspection file without publishing")
    parser.add_argument("--publish", action="store_true",
                        help="Build the description overlay JSON (companion file) and write it")
    parser.add_argument("--coverage-report", type=Path, default=None,
                        help="Also write an unmatched-coverage report to this path")
    parser.add_argument("--output", type=Path, default=None,
                        help="Overlay filename; default <registers-stem>.descriptions.json")
    args = parser.parse_args()

    cache_dir = Path(tempfile.gettempdir()) / "wfwebserver-regdesc-cache"
    docs = {ip: extract_manual(args.manual_dir / name, cache_dir) for ip, name in MANUALS.items()}
    catalogs = build_catalogs(docs)
    inspect_path = cache_dir / "catalog-inspection.json"
    inspect_path.write_text(json.dumps(catalogs, ensure_ascii=False, indent=2), encoding="utf-8")
    print("Catalog inspection:", inspect_path)
    for ip, rows in catalogs.items():
        print(ip, dict(Counter(r["kind"] for r in rows)))

    if not args.publish:
        if args.inspect:
            return
        raise SystemExit("Publishing is not enabled; use --publish to write the overlay.")

    overlay, stats = build_overlay(args.registers, catalogs)
    overlay["generatedAt"] = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    overlay["sourceRegisterFile"] = Path(args.registers).name
    overlay["sourceManuals"] = [m["file"] for m in docs.values()]
    reg_path = Path(args.registers)
    if args.output_dir:
        out_dir = args.output_dir
        out_dir.mkdir(parents=True, exist_ok=True)
        out_file = out_dir / (args.output.name if args.output else reg_path.stem + DESC_SUFFIX)
    else:
        out_file = reg_path.with_name(args.output.name if args.output
                                      else reg_path.stem + DESC_SUFFIX)
    out_file.write_text(json.dumps(overlay, ensure_ascii=False, indent=2), encoding="utf-8")
    print("Wrote overlay:", out_file)
    print("Registers empty/filled: {empty}/{filled}".format(**stats["registers"]))
    print("Fields empty/filled: {empty}/{filled}".format(**stats["fields"]))

    if args.coverage_report:
        report = {"sourceRegisterFile": Path(args.registers).name, "stats": stats}
        args.coverage_report.parent.mkdir(parents=True, exist_ok=True)
        args.coverage_report.write_text(
            json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
        print("Wrote coverage report:", args.coverage_report)


if __name__ == "__main__":
    main()
