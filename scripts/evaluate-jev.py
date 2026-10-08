#!/usr/bin/env python3
"""Evaluate Jev five times per case using the production request builder.

Build: cmake --build --preset ninja --target cha_jev_eval_requests
Run: JEV_EVAL_API_KEY=... python3 scripts/evaluate-jev.py --output /tmp/jev-results.json
Use --requests-only to inspect the exact cases and requests without a key or network.
Each fixture's expected object maps question names to recipient labels or
research booleans. The output retains responses, including usage/input tokens,
and reports choice counts, matches, and numeric ranges for each question.
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


def summarize_question(runs, name, expected):
    choices = Counter()
    numbers = defaultdict(list)
    expected_choice = ("yes" if expected else "no") if isinstance(expected, bool) else expected
    for run in runs:
        answer = run.get("answers", {}).get(name)
        choice = "failed" if "error" in run else "invalid"
        if isinstance(answer, dict) and answer.get("type") == "choice":
            value = answer.get("choice")
            if isinstance(value, str) and (not isinstance(expected, bool) or value in ("yes", "no")):
                choice = value
        choices[choice] += 1
        for field, value in numeric_fields(answer):
            numbers[field].append(value)
    return {
        "choice_counts": dict(choices), "matched": choices[expected_choice], "total": len(runs),
        "numeric_ranges": {field: {"min": min(values), "max": max(values),
                                    "changed": len(set(values)) > 1}
                           for field, values in numbers.items()},
    }


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
    for case in cases:
        missing = case["expected"].keys() - case["request"]["questions"].keys()
        if missing:
            parser.error(f"Production request is missing expected questions: {', '.join(sorted(missing))}")
    key = os.environ.get("JEV_EVAL_API_KEY")
    if not key:
        parser.error("Set JEV_EVAL_API_KEY or use --requests-only.")
    opener = urllib.request.build_opener(NoRedirect)
    results = []
    for case in cases:
        runs = []
        for _ in range(5):
            request = urllib.request.Request(
                "https://openrouter.ai/api/alpha/decisions",
                data=json.dumps(case["request"], ensure_ascii=False).encode(),
                headers={"Content-Type": "application/json", "Authorization": f"Bearer {key}"},
            )
            try:
                with opener.open(request, timeout=5) as response:
                    result = json.load(response)
                if not isinstance(result, dict) or not isinstance(result.get("answers"), dict):
                    raise ValueError("Invalid answers object")
                runs.append(result)
            except (OSError, ValueError, KeyError) as error:
                # Do not print credentials, request headers, or upstream error bodies.
                runs.append({"error": type(error).__name__})
        questions = {name: summarize_question(runs, name, expected)
                     for name, expected in case["expected"].items()}
        results.append({**case, "runs": runs, "questions": questions})
        args.output.write_text(json.dumps(results, ensure_ascii=False, indent=2) + "\n")
        for name, summary in questions.items():
            print(f"{case.get('id', case['roster'])} {name}: "
                  f"{summary['matched']}/5 expected {case['expected'][name]}; {summary['choice_counts']}")


if __name__ == "__main__":
    main()
