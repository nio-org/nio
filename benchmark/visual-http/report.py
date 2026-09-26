#!/usr/bin/env python3
"""Build results/report.html from the results bench.py wrote.

Results live in results/<scenario>/<target>.json; the report gets one section
per scenario present. The template carries all the chart code; this script only
injects the data, so the report is one self-contained file that opens anywhere
with no server and no network.
"""

import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ORDER = ["nio", "go", "go1", "node", "bun", "java"]
SCENARIOS = ["plaintext", "json"]


def main():
    results_dir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "results")
    out_scenarios = []
    for scenario in SCENARIOS:
        langs = {}
        meta = None
        for key in ORDER:
            path = os.path.join(results_dir, scenario, f"{key}.json")
            if not os.path.exists(path):
                continue
            with open(path) as f:
                r = json.load(f)
            langs[key] = {"label": r["label"], "per_second": r["per_second"],
                          "stages": r["stages"], "idle": r.get("idle")}
            meta = meta or r["meta"]
        if langs:
            out_scenarios.append({"key": scenario, "label": meta["scenario_label"],
                                  "meta": meta, "langs": langs})
    if not out_scenarios:
        sys.exit(f"no results under {results_dir}; run bench.py first")

    with open(os.path.join(HERE, "report_template.html")) as f:
        template = f.read()
    payload = json.dumps({"scenarios": out_scenarios})
    html = template.replace("/*__DATA__*/null", payload, 1)

    out = os.path.join(results_dir, "report.html")
    with open(out, "w") as f:
        f.write(html)
    names = ", ".join(f"{s['key']}({', '.join(s['langs'])})" for s in out_scenarios)
    print(f"report: {out} — {names}")


if __name__ == "__main__":
    main()
