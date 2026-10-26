#!/usr/bin/env python3
"""
Deterministic dataset generator for Tessera benchmarks and manual testing.

    python3 scripts/gen_data.py --rows 1000000 --out data/sales.csv
    python3 scripts/gen_data.py --rows 200000 --format jsonl --out data/events.jsonl

The generated data is intentionally awkward in the ways real exports are:
quoted commas, doubled quotes, embedded newlines, empty cells, ragged rows and
a few columns that look numeric but are not.
"""
from __future__ import annotations

import argparse
import json
import random
import sys
from datetime import date, timedelta

REGIONS = ["EU", "US", "APAC", "LATAM", "MEA"]
COUNTRIES = ["DE", "FR", "NL", "US", "CA", "JP", "AU", "BR", "ZA", "IN"]
CHANNELS = ["web", "mobile", "partner", "retail", "inside-sales"]
PRODUCTS = [
    ("Tessera Pro", 149.0),
    ("Tessera Team", 499.0),
    ("Tessera Enterprise", 2400.0),
    ("Data Connector", 79.0),
    ("Audit Add-on", 39.0),
    ("Priority Support", 199.0),
]
STATUSES = ["paid", "paid", "paid", "refunded", "pending", "failed"]
NOTES = [
    "",
    "expedited",
    "customer asked for invoice",
    'mentions "renewal" in the call',
    "address, second line",
    "multi-line note\nsecond line",
    "  padded  ",
]


def csv_field(value: str) -> str:
    if any(ch in value for ch in [",", '"', "\n", "\r"]):
        return '"' + value.replace('"', '""') + '"'
    return value


def generate(rows: int, fmt: str, seed: int, header: bool) -> None:
    rng = random.Random(seed)
    start = date(2023, 1, 1)
    out = sys.stdout

    if fmt in ("csv", "tsv"):
        delimiter = "," if fmt == "csv" else "\t"
        fields = [
            "order_id", "created_at", "region", "country", "channel", "product",
            "units", "unit_price", "revenue", "discount_pct", "customer_id",
            "status", "notes",
        ]
        if header:
            out.write(delimiter.join(fields) + "\n")
        for i in range(rows):
            product, price = PRODUCTS[rng.randrange(len(PRODUCTS))]
            units = rng.randint(1, 40)
            discount = rng.choice([0, 0, 0, 5, 10, 12.5, 20])
            revenue = round(units * price * (1 - discount / 100.0), 2)
            day = start + timedelta(days=rng.randrange(730))
            values = [
                f"ORD-{i:09d}",
                f"{day.isoformat()} {rng.randrange(24):02d}:{rng.randrange(60):02d}:{rng.randrange(60):02d}",
                REGIONS[rng.randrange(len(REGIONS))],
                COUNTRIES[rng.randrange(len(COUNTRIES))],
                CHANNELS[rng.randrange(len(CHANNELS))],
                product,
                str(units),
                f"{price:.2f}",
                f"{revenue:.2f}",
                f"{discount:g}",
                f"C{rng.randrange(90000):05d}",
                STATUSES[rng.randrange(len(STATUSES))],
                NOTES[rng.randrange(len(NOTES))],
            ]
            if rng.random() < 0.002:  # ragged rows happen in real exports
                values = values[: rng.randint(8, 12)]
            out.write(delimiter.join(csv_field(v) for v in values) + "\n")
        return

    if fmt == "jsonl":
        for i in range(rows):
            product, price = PRODUCTS[rng.randrange(len(PRODUCTS))]
            units = rng.randint(1, 40)
            discount = rng.choice([0, 0, 0, 5, 10, 12.5, 20])
            day = start + timedelta(days=rng.randrange(730))
            record = {
                "order_id": f"ORD-{i:09d}",
                "created_at": f"{day.isoformat()}T{rng.randrange(24):02d}:{rng.randrange(60):02d}:00Z",
                "region": REGIONS[rng.randrange(len(REGIONS))],
                "product": product,
                "units": units,
                "revenue": round(units * price * (1 - discount / 100.0), 2),
                "status": STATUSES[rng.randrange(len(STATUSES))],
                "notes": NOTES[rng.randrange(len(NOTES))],
                "meta": {"source": CHANNELS[rng.randrange(len(CHANNELS))], "v": 2},
            }
            out.write(json.dumps(record, ensure_ascii=False) + "\n")
        return

    raise SystemExit(f"unknown format {fmt}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rows", type=int, default=100_000)
    parser.add_argument("--format", choices=["csv", "tsv", "jsonl"], default="csv")
    parser.add_argument("--seed", type=int, default=7)
    parser.add_argument("--no-header", action="store_true")
    parser.add_argument("--out", default="-")
    args = parser.parse_args()

    if args.out != "-":
        with open(args.out, "w", encoding="utf-8", newline="") as handle:
            original = sys.stdout
            sys.stdout = handle
            try:
                generate(args.rows, args.format, args.seed, not args.no_header)
            finally:
                sys.stdout = original
    else:
        generate(args.rows, args.format, args.seed, not args.no_header)


if __name__ == "__main__":
    main()
