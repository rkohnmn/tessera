#!/usr/bin/env python3
"""
Generate the awkward files that real exports actually produce.

Every case here is something that has appeared in the wild: a BOM from Excel, a
UTF-16 export from a bank, a semicolon-separated European export, a pipe dump
from a database tool, CRLF from Windows, an embedded CRLF inside a quoted field,
a header-only file, a single-column log, a file with no trailing newline, a
ragged row in the middle, a field containing a NUL, and so on.

Each case ships with the expected number of data rows (or None when the file is
intentionally unparseable), so a test can check Tessera's answer rather than
just "it did not crash".

    python3 scripts/gen_edge_cases.py --out /tmp/edge

Writes one file per case plus `manifest.json` describing them.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path

# (name, bytes, expected_rows_or_None, note)
CASES: list[tuple[str, bytes, int | None, str]] = []


def add(name: str, data: bytes, rows: int | None, note: str) -> None:
    CASES.append((name, data, rows, note))


def main() -> None:
    # ── Encodings ────────────────────────────────────────────────────────────
    plain = b"id,name,amount\n1,alpha,10.5\n2,beta,20\n3,gamma,30\n"
    add("utf8_plain.csv", plain, 3, "baseline")
    add("utf8_bom.csv", b"\xef\xbb\xbf" + plain, 3, "Excel writes a UTF-8 BOM")
    add("utf16le_bom.csv", b"\xff\xfe" + plain.decode().encode("utf-16-le"), 3,
        "BOM + UTF-16LE, as exported by some banks")
    add("utf16be_bom.csv", b"\xfe\xff" + plain.decode().encode("utf-16-be"), 3, "BOM + UTF-16BE")
    add("latin1.csv", "id,city,amount\n1,Zürich,10\n2,São Paulo,20\n3,Malmö,30\n".encode("latin-1"), 3,
        "Latin-1 bytes that are not valid UTF-8")
    add("utf8_cjk.csv", "id,city,amount\n1,東京,10\n2,北京,20\n".encode(), 2, "CJK content")
    add("utf8_emoji.csv", "id,note,amount\n1,ship it 🚀,10\n2,ok ✅,20\n".encode(), 2, "emoji in a field")

    # ── Delimiters and quoting ───────────────────────────────────────────────
    add("semicolon.csv", b"id;name;amount\n1;alpha;10,5\n2;beta;20,0\n", 2, "European semicolons with comma decimals")
    add("pipe.csv", b"id|name|amount\n1|alpha|10\n2|beta|20\n", 2, "pipe-delimited database dump")
    add("tab.tsv", b"id\tname\tamount\n1\talpha\t10\n2\tbeta\t20\n", 2, ".tsv extension")
    add("quoted_all.csv", b'"id","name","amount"\n"1","alpha, inc.","10"\n"2","say ""hi""","20"\n', 2,
        "everything quoted, doubled quotes")
    add("quoted_newlines.csv", b'id,note\n1,"line one\nline two"\n2,"crlf\r\ninside"\n3,plain\n', 3,
        "embedded LF and CRLF inside quoted fields")
    add("crlf.csv", plain.replace(b"\n", b"\r\n"), 3, "Windows line endings")
    add("no_trailing_newline.csv", b"id,name\n1,alpha\n2,beta", 2, "last record not terminated")
    add("single_column.csv", b"value\n1\n2\n3\n4\n", 4, "one column only")
    add("header_only.csv", b"id,name,amount\n", 0, "header but no rows")
    add("empty.csv", b"", 0, "zero bytes")
    add("bom_only.csv", b"\xef\xbb\xbf", 0, "BOM and nothing else")
    add("ragged.csv", b"id,name,amount\n1,alpha,10\n2,beta\n3,gamma,30,extra\n", 3,
        "rows with too few and too many fields")
    add("blank_lines.csv", b"id,name\n1,alpha\n\n2,beta\n\n\n", 2, "empty lines between records")
    add("spaces.csv", b"id, name , amount\n1, alpha , 10\n2, beta , 20\n", 2, "padding around headers and cells")
    add("dup_headers.csv", b"id,id,amount\n1,2,10\n3,4,20\n", 2, "duplicate column names")
    add("quoted_header.csv", b'"weird, header","second ""quoted"""\n1,2\n', 1, "quotes in the header row")
    add("nul_byte.csv", b"id,name\n1,a\x00b\n2,beta\n", 2, "a NUL inside a field")
    add("large_field.csv", b"id,blob\n1," + b"x" * 200000 + b"\n2,small\n", 2, "a 200 KB single field")
    add("numeric_text.csv", b"zip,code,amount\n01234,0001,10\n99999,0002,20\n", 2, "leading zeros must survive")
    add("ragged_quotes.csv", b'id,note\n1,"unbalanced\n2,beta\n', None, "a quote never closed (malformed)")
    add("one_row.csv", b"a,b\n1,2\n", 1, "single data row")
    add("one_col_one_row.csv", b"a\n1\n", 1, "single cell plus header")

    # ── JSON Lines ───────────────────────────────────────────────────────────
    add("jsonl_union.jsonl",
        b'{"id":1,"name":"alpha"}\n{"id":2,"extra":true}\n{"id":3,"name":"gamma","n":3.5}\n',
        3, "union of keys across records")
    add("jsonl_nested.jsonl",
        b'{"id":1,"user":{"name":"a","id":9}}\n{"id":2,"user":{"name":"b"}}\n',
        2, "one level of nesting")
    add("jsonl_no_trailing.jsonl", b'{"a":1}\n{"a":2}', 2, "last line unterminated")
    add("jsonl_bad.jsonl", b'{"a":1}\nnot json at all\n{"a":3}\n', None, "a corrupt line (malformed)")

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", default="edge-cases", help="output directory")
    args = parser.parse_args()

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    manifest = []
    for name, data, rows, note in CASES:
        path = out / name
        path.write_bytes(data)
        manifest.append({"file": name, "bytes": len(data), "rows": rows, "note": note})
        print(f"  {name:28s} {len(data):>8,} B  rows={rows}  {note}")

    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"\n{len(manifest)} cases written to {out}/manifest.json")


if __name__ == "__main__":
    main()
