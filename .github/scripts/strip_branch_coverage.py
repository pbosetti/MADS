#!/usr/bin/env python3
"""Strip branch/condition data from a gcovr Cobertura report before it is
uploaded to Codecov.

Codecov scores a line as "hit" only when its branches are *fully* covered;
a line that executed but took only one side of a branch is scored
"partial" and does not count toward the coverage percentage. gcovr's own
line-rate has no such notion -- any executed line counts, regardless of
branch completeness -- which is the number quoted everywhere in this repo
(README badge, the 80% Codecov gate, CHANGES.md). Removing the branch
detail makes Codecov compute the same plain line coverage gcovr reports.
"""
import sys
import xml.etree.ElementTree as ET


def strip(path: str) -> None:
    tree = ET.parse(path)
    root = tree.getroot()
    for line in root.iter("line"):
        if line.get("branch") == "true":
            line.set("branch", "false")
            line.attrib.pop("condition-coverage", None)
            for conditions in list(line.findall("conditions")):
                line.remove(conditions)
    for attr in ("branch-rate", "branches-covered", "branches-valid"):
        root.attrib.pop(attr, None)
    tree.write(path, xml_declaration=True, encoding="utf-8")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(f"usage: {sys.argv[0]} <coverage.xml>")
    strip(sys.argv[1])
