#!/usr/bin/env python3
"""Cross-check that Python (build_register_descriptions) and JS (register-viewer.html)
produce byte-identical description overlay keys.

This test does NOT require pdfplumber (optional-import guarded).

Checks:
  1. Python reg_key_of/field_key_of produce expected strings.
  2. If `node` is available, compute the JS keys live and compare.
  3. Otherwise (or in addition) statically confirm the HTML key functions use the
     same join order and the '\x1f' separator.
"""

from __future__ import annotations

import importlib.util
import json
import re
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools/build_register_descriptions.py"
HTML = ROOT / "html/register-viewer.html"
SEP = "\x1f"


def load_tool():
    spec = importlib.util.spec_from_file_location("brd", TOOL)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


REG = {"entryType": "reg", "blockName": "AQM", "subName": "ING_IPFIX", "regName": "IND_DATA"}
FIELD = {"name": "FEC_BYPASS_CORRECTION_ENABLE", "startBit": 0, "endBit": 0}


def py_keys(mod):
    rk = mod.reg_key_of(REG)
    return rk, mod.field_key_of(rk, FIELD)


def js_keys_live():
    """Echo the JS key computation via node and parse it out."""
    script = (
        "const REG=" + json.dumps(REG) + ";"
        "const F=" + json.dumps(FIELD) + ";"
        "const rk=()=>[REG.entryType||'reg',REG.blockName||'',REG.subName||'',REG.regName||''].join('\\x1f');"
        "const r=rk();"
        "const fk=(rk,f)=>[rk,f.name||'',f.startBit,f.endBit].join('\\x1f');"
        "process.stdout.write(JSON.stringify([r, fk(r,F)]));"
    )
    out = subprocess.check_output(["node", "-e", script], timeout=30).decode("utf-8")
    r, fk = json.loads(out)
    return r, fk


def js_keys_static(html):
    """Extract the JS function bodies and confirm join order + separator statically."""
    def body(name):
        m = re.search(r"function\s+" + name + r"\([^)]*\)\s*\{(.*?)\}", html, re.S)
        assert m, "missing JS function " + name
        return m.group(1)

    reg_body = body("regKeyOf")
    assert re.search(r"\.join\('\\x1f'\)" + r"|\.join\(\"\\x1f\"\)", reg_body), \
        "regKeyOf must join with '\\x1f'"
    # order: entryType, blockName, subName, regName
    order = [x.strip() for x in re.findall(r"\.(\w+)", reg_body)]
    joined = [x for x in order if x in ("entryType", "blockName", "subName", "regName")]
    assert joined == ["entryType", "blockName", "subName", "regName"], \
        "regKeyOf join order mismatch: %s" % joined

    fk_body = body("fieldKeyOf")
    assert re.search(r"\.join\('\\x1f'\)|\.join\(\"\\x1f\"\)", fk_body), \
        "fieldKeyOf must join with '\\x1f'"
    order_f = [x.strip() for x in re.findall(r"\.(\w+)", fk_body)]
    # first element is the regKey param (not a .property), name/startBit/endBit are .properties
    joined_f = [x for x in order_f if x in ("name", "startBit", "endBit")]
    assert joined_f == ["name", "startBit", "endBit"], \
        "fieldKeyOf join order mismatch: %s" % joined_f
    return True


def main():
    mod = load_tool()
    py_r, py_f = py_keys(mod)
    expected_r = REG.get("entryType") + SEP + REG.get("blockName") + SEP + \
        REG.get("subName") + SEP + REG.get("regName")
    expected_f = expected_r + SEP + FIELD.get("name") + SEP + "0" + SEP + "0"
    assert py_r == expected_r, "python reg_key mismatch: %r" % py_r
    assert py_f == expected_f, "python field_key mismatch: %r" % py_f

    html = HTML.read_text(encoding="utf-8")
    js_keys_static(html)

    if shutil.which("node"):
        js_r, js_f = js_keys_live()
        assert py_r == js_r, "py/js reg_key mismatch:\n py=%r\n js=%r" % (py_r, js_r)
        assert py_f == js_f, "py/js field_key mismatch:\n py=%r\n js=%r" % (py_f, js_f)

    print("OK: python/js description overlay keys are consistent")


if __name__ == "__main__":
    main()
