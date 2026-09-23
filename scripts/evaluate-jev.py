#!/usr/bin/env python3
"""Evaluate Jev five times per case using the production request builder.

Build: cmake --build --preset ninja --target cha_jev_eval_requests
Run: JEV_EVAL_API_KEY=... python3 scripts/evaluate-jev.py --output /tmp/jev-results.json
Use --requests-only to inspect the exact cases and requests without a key or network.
The output contains all returned recipient answers (including probabilities),
choice counts, matched labels, and the range of each numeric answer field.
"""
import argparse
from collections import Counter, defaultdict
import json
import os
from pathlib import Path
import subprocess
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parents[1]


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, fp, code, message, headers, new_url):
        return None


def numeric_fields(value, prefix=""):
    if isinstance(value, dict):
        for key, child in value.items():
            yield from numeric_fields(child, f"{prefix}.{key}" if prefix else key)
    elif isinstance(value, list):
        for index, child in enumerate(value):
            yield from numeric_fields(child, f"{prefix}[{index}]")
    elif isinstance(value, (int, float)) and not isinstance(value, bool):
        yield prefix, value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--requests-only", action="store_true")
    parser.add_argument("--generator", type=Path, default=ROOT / "build/ninja/cha_jev_eval_requests")
    parser.add_argument("--cases", type=Path, default=ROOT / "tests/fixtures/jev/cases.json")
    args = parser.parse_args()
    cases = json.loads(subprocess.check_output([str(args.generator), str(args.cases)], text=True))
    if args.requests_only:
        args.output.write_text(json.dumps(cases, ensure_ascii=False, indent=2) + "\n")
        return
    key = os.environ.get("JEV_EVAL_API_KEY")
    if not key:
        parser.error("Set JEV_EVAL_API_KEY or use --requests-only.")
    opener = urllib.request.build_opener(NoRedirect)
    results = []
    for case in cases:
        choices = Counter()
        numbers = defaultdict(list)
        runs = []
        for _ in range(5):
            request = urllib.request.Request(
                "https://openrouter.ai/api/alpha/decisions",
                data=json.dumps(case["request"], ensure_ascii=False).encode(),
                headers={"Content-Type": "application/json", "Authorization": f"Bearer {key}"},
            )
            try:
                with opener.open(request, timeout=5) as response:
                    answer = json.load(response)["answers"]["recipient"]
                runs.append(answer)
                choices[answer.get("choice", "invalid")] += 1
                for name, value in numeric_fields(answer):
                    numbers[name].append(value)
            except (OSError, ValueError, KeyError) as error:
                # Do not print credentials, request headers, or upstream error bodies.
                runs.append({"error": type(error).__name__})
                choices["failed"] += 1
        results.append({
            **case, "runs": runs, "choice_counts": dict(choices),
            "matched": choices[case["expected"]], "total": 5,
            "numeric_ranges": {name: {"min": min(values), "max": max(values),
                                      "changed": len(set(values)) > 1}
                               for name, values in numbers.items()},
        })
        args.output.write_text(json.dumps(results, ensure_ascii=False, indent=2) + "\n")
        print(f"{case['roster']}: {choices[case['expected']]}/5 expected {case['expected']}; {dict(choices)}")


if __name__ == "__main__":
    main()
