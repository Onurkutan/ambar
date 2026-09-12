#!/usr/bin/env python3
"""Break the code on purpose, and see whether the tests notice.

A green test suite says the tests pass.  It does not say they would fail if the
code were wrong, and those are different claims.  This script checks the second
one: it applies a small, deliberate defect to a source file, rebuilds, runs the
suite, and reports which tests -- if any -- caught it.

Three outcomes matter, and they mean different things.

  CAUGHT / N failing   The tests do their job.  This is the goal.

  SURVIVED             Either a test is missing, or the mutation produced code
                       that behaves identically.  The second happens more often
                       than people expect: a comparison that only differs on
                       equal keys is unreachable when equal keys are excluded by
                       a precondition.  Those are worth writing down next to the
                       code rather than chasing, so the next person does not go
                       looking for a test that should not exist.

  DID NOT COMPILE      Not a result.  Fix the mutation and rerun.

A mutation that crashes the process is caught, but crashes name no test, so the
suite is run with -v: the last name printed is the test that died.

Usage:
    python3 tools/mutate.py mutations/merger.json

The JSON file maps a description to [source_path, find, replace].  `find` must
appear exactly once in the file; the script restores the original afterwards,
including when it fails partway.
"""

import json
import pathlib
import subprocess
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
BUILD = REPO / "build"


def run(command):
    return subprocess.run(command, shell=True, cwd=REPO, capture_output=True,
                          text=True)


def apply_and_test(path, original, find, replace):
    if original.count(find) == 0:
        return "PATTERN NOT FOUND"
    if original.count(find) > 1:
        return f"PATTERN NOT UNIQUE ({original.count(find)} matches)"

    path.write_text(original.replace(find, replace, 1))

    build = run("cmake --build build")
    if "error" in (build.stdout + build.stderr):
        return "DID NOT COMPILE"

    result = run("timeout 600 ./build/ambar_tests -v")
    failures = [line.split()[1] for line in result.stdout.splitlines()
                if line.startswith("FAIL")]

    if result.returncode not in (0, 1):
        started = [line.strip()[4:] for line in result.stdout.splitlines()
                   if line.startswith("  ... ")]
        where = started[-1] if started else "an unnamed test"
        return f"CAUGHT (exit {result.returncode}) while running {where}"

    if failures:
        shown = ", ".join(f.split(".", 1)[1] for f in failures[:3])
        more = "" if len(failures) <= 3 else f", +{len(failures) - 3} more"
        return f"CAUGHT by {len(failures)}: {shown}{more}"

    return "SURVIVED -- no test distinguishes this from correct code"


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2

    mutations = json.loads(pathlib.Path(sys.argv[1]).read_text())
    originals = {}
    try:
        for name, mutation in mutations.items():
            if name.startswith("_"):
                continue  # "_comment" and the like: notes, not mutations
            relative, find, replace = mutation
            path = REPO / relative
            if path not in originals:
                originals[path] = path.read_text()
            print(f"{name}\n    {apply_and_test(path, originals[path], find, replace)}")
            path.write_text(originals[path])
    finally:
        for path, text in originals.items():
            path.write_text(text)
        run("cmake --build build")

    return 0


if __name__ == "__main__":
    sys.exit(main())
