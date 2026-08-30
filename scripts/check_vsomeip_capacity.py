#!/usr/bin/env python3
from pathlib import Path
import re, sys

if len(sys.argv) != 2:
    sys.exit("Usage: checker RESULTS")
lines = [x for x in Path(sys.argv[1]).read_text().splitlines() if x.startswith("RESULT ")]
errors = []
levels = [1, 4, 16, 32, 64, 128]
rows = []
for x in lines:
    rows.append(dict(re.findall(r"(\w+)=(\S+)", x)))
if len(rows) != 18:
    errors.append(f"need 18 RESULT lines, got {len(rows)}")
seen = []
for f in rows:
    key = (int(f["window"]), int(f["repeat"]))
    seen.append(key)
    for k, v in {
        "payload_bytes": "64",
        "warmup_s": "10",
        "duration_s": "60",
        "errors": "0",
        "bad": "0",
        "duplicates": "0",
        "drained": "1",
        "pass": "1",
        "timeouts": "0",
    }.items():
        if f.get(k) != v:
            errors.append(f"{key}: {k}!={v}")
    if f.get("submitted") != f.get("completed") or f.get("completed") != f.get("ok"):
        errors.append(f"{key}: count mismatch")
    if float(f.get("timeout_rate_pct", "100")) >= 0.1:
        errors.append(f"{key}: timeout threshold")
    if int(f.get("peak", "0")) != int(f.get("window", "-1")):
        errors.append(f"{key}: peak not reached")
expected = [(w, r) for w in levels for r in [1, 2, 3]]
if seen != expected:
    errors.append("level/repeat order or coverage mismatch")
print(f"CAPACITY_CHECK levels={len(levels)} repeats=3 runs={len(rows)} errors={len(errors)}")
for e in errors:
    print("CAPACITY_FAIL " + e)
sys.exit(1 if errors else 0)
