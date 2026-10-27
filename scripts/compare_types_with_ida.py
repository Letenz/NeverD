#!/usr/bin/env python3
"""Compare a program's local types in NeverD with IDA's.

Reads NeverD's types through `neverd types --layout --json` and IDA's from a
JSON dump of its Local Types (a list of types whose records give each
member's name, bit offset, size in bits and type, with IDA's anonymous
helper types `$...` listed as types of their own).  For every record both
have it compares the size, the leaves (every named member with its bit
offset and width, anonymous unions and structures looked through) and the
nesting (where the anonymous unions and structures are and how large), and
for every enumeration its values.  It reports what agrees and each
difference, so that a layout the two read differently can be checked
against the PDB.
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys

BATCH = 200
GAP = re.compile(r"gap[0-9A-F]+")


def neverd(arguments, *options):
    command = [arguments.neverd, "types", arguments.binary]
    if arguments.pdb:
        command += ["--pdb", arguments.pdb]
    result = subprocess.run(command + list(options), capture_output=True,
                            text=True)
    if result.returncode:
        sys.exit(f"neverd types failed: {result.stderr.strip()}")
    return json.loads(result.stdout)


def neverd_types(arguments):
    listing = neverd(arguments, "--json")
    names = [row["name"] for row in listing["types"]]
    types = {}
    for start in range(0, len(names), BATCH):
        options = ["--layout", "--json"]
        for name in names[start:start + BATCH]:
            options += ["--type", name]
        for entry in neverd(arguments, *options):
            types[entry["name"]] = entry
    return listing, types


def neverd_leaves(members, path="", out=None):
    """Named members as (path, bit offset, bits), records in place looked
    through; arrays of them count as one member."""
    out = [] if out is None else out
    for member in members:
        kind = member["kind"]
        if kind == "padding":
            continue
        name = member["name"]
        if kind in ("struct", "union") and "[" not in member.get("type", ""):
            neverd_leaves(member.get("members", []),
                          path + (name + "." if name else ""), out)
            continue
        bits = member["bitWidth"] if kind == "bitfield" else member["size"] * 8
        out.append((path + name, member["bitOffset"], bits))
    return out


def neverd_shape(members, out=None):
    """Where anonymous records are: (bit offset, bits, kind)."""
    out = [] if out is None else out
    for member in members:
        if member["kind"] in ("struct", "union"):
            if not member["name"]:
                out.append((member["bitOffset"], member["size"] * 8,
                            member["kind"]))
            neverd_shape(member.get("members", []), out)
    return out


def anonymous_type(name):
    return name.startswith("$") or "::$" in name


class Ida:
    def __init__(self, path):
        dump = json.load(open(path, encoding="utf-8"))
        self.types = {entry["name"]: entry for entry in dump["types"]}
        self.gaps = 0

    def record(self, name):
        entry = self.types.get(name.strip())
        if entry and entry.get("layout"):
            return entry
        return None

    def helper(self, member):
        """The anonymous helper record a member's type names, if any."""
        type_name = member.get("type_name") or member.get("type", "")
        for prefix in ("struct ", "union "):
            if type_name.startswith(prefix):
                type_name = type_name[len(prefix):]
        if not anonymous_type(type_name) or "[" in type_name:
            return None
        return self.record(type_name)

    def leaves(self, members, base=0, path="", out=None):
        out = [] if out is None else out
        for member in members:
            offset = base + member["bit_offset"]
            helper = self.helper(member)
            if helper is not None:
                self.leaves(helper["layout"]["members"], offset,
                            path + ("" if self.anonymous(member)
                                    else member["name"] + "."),
                            out)
                continue
            # IDA names bytes it leaves undefined gapN; they hold no member.
            if GAP.fullmatch(member["name"]):
                self.gaps += 1
                continue
            out.append((path + member["name"], offset, member["size_bits"]))
        return out

    @staticmethod
    def anonymous(member):
        return not member["name"] or "anonymous_udm" in (member.get("flags")
                                                         or [])

    def shape(self, members, base=0, out=None):
        out = [] if out is None else out
        for member in members:
            offset = base + member["bit_offset"]
            helper = self.helper(member)
            if helper is None:
                continue
            if self.anonymous(member):
                out.append((offset, member["size_bits"], helper["kind"]))
            self.shape(helper["layout"]["members"], offset, out)
        return out


def compare(arguments):
    listing, ours = neverd_types(arguments)
    ida = Ida(arguments.ida)
    report = {"neverd_types": len(ours), "ida_types": len(ida.types),
              "records": {"both": 0, "same_size": 0, "same_leaves": 0,
                          "same_nesting": 0},
              "enums": {"both": 0, "same_values": 0},
              "differences": [], "only_neverd": [], "only_ida": {}}
    for name, entry in sorted(ours.items()):
        theirs = ida.types.get(name)
        if theirs is None:
            report["only_neverd"].append(name)
            continue
        kind = entry["kind"]
        if kind in ("struct", "union") and entry.get("defined", True):
            if not theirs.get("layout"):
                continue
            records = report["records"]
            records["both"] += 1
            problems = []
            if entry["size"] == theirs["size"]:
                records["same_size"] += 1
            else:
                problems.append(f"size {entry['size']:#x} against IDA's "
                                f"{theirs['size']:#x}")
            # Where each member is, not the order union alternatives list in.
            our_leaves = sorted(neverd_leaves(entry["members"]),
                                key=lambda leaf: (leaf[1], leaf[0]))
            gaps = ida.gaps
            their_leaves = sorted(ida.leaves(theirs["layout"]["members"]),
                                  key=lambda leaf: (leaf[1], leaf[0]))
            if ida.gaps != gaps:
                records["ida_gaps"] = records.get("ida_gaps", 0) + 1
                problems.append(f"IDA leaves {ida.gaps - gaps} span(s) "
                                "undefined (gapN)")
            if our_leaves == their_leaves:
                records["same_leaves"] += 1
            else:
                mine, others = set(our_leaves), set(their_leaves)
                for leaf in sorted(mine - others, key=lambda x: x[1]):
                    problems.append(f"NeverD has {leaf[0]} at bit {leaf[1]} "
                                    f"({leaf[2]} bits)")
                for leaf in sorted(others - mine, key=lambda x: x[1]):
                    problems.append(f"IDA has {leaf[0]} at bit {leaf[1]} "
                                    f"({leaf[2]} bits)")

            our_shape = sorted(neverd_shape(entry["members"]))
            their_shape = sorted(ida.shape(theirs["layout"]["members"]))
            if our_shape == their_shape:
                records["same_nesting"] += 1
            elif our_leaves == their_leaves:
                mine, others = set(our_shape), set(their_shape)
                for where in sorted(mine - others):
                    problems.append(f"NeverD nests a {where[2]} of {where[1]} "
                                    f"bits at bit {where[0]}")
                for where in sorted(others - mine):
                    problems.append(f"IDA nests a {where[2]} of {where[1]} "
                                    f"bits at bit {where[0]}")
            if problems:
                report["differences"].append({"type": name,
                                              "problems": problems})
        elif kind == "enum" and theirs.get("kind") == "enum":
            report["enums"]["both"] += 1
            values = {v["name"]: v["value"] for v in entry.get("values", [])}
            members = theirs.get("members") or theirs.get("values") or []
            if isinstance(members, list) and members and isinstance(
                    members[0], dict):
                their_values = {m["name"]: m.get("value") for m in members}
                if all(their_values.get(k) in (v, v & 0xFFFFFFFF)
                       for k, v in values.items()):
                    report["enums"]["same_values"] += 1
                else:
                    report["differences"].append(
                        {"type": name, "problems": ["enumeration values "
                                                     "differ"]})
            else:
                report["enums"]["same_values"] += 1
    for name, entry in ida.types.items():
        if name not in ours and not anonymous_type(name):
            report["only_ida"].setdefault(entry["kind"], []).append(name)
    return report


def print_report(report, details):
    records, enums = report["records"], report["enums"]
    print(f"NeverD {report['neverd_types']} types, IDA {report['ida_types']}")
    print(f"records in both: {records['both']}; same size "
          f"{records['same_size']}, same members {records['same_leaves']}, "
          f"same nesting {records['same_nesting']}, IDA leaving spans "
          f"undefined {records.get('ida_gaps', 0)}")
    print(f"enumerations in both: {enums['both']}; same values "
          f"{enums['same_values']}")
    print(f"only in NeverD: {len(report['only_neverd'])}")
    print("only in IDA: " + ", ".join(f"{len(names)} {kind}"
                                      for kind, names in sorted(
                                          report["only_ida"].items())))
    print(f"differences: {len(report['differences'])}")
    for difference in report["differences"][:details]:
        print(f"  {difference['type']}:")
        for problem in difference["problems"][:8]:
            print(f"    {problem}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", required=True)
    parser.add_argument("--binary", required=True)
    parser.add_argument("--pdb")
    parser.add_argument("--ida", required=True,
                        help="IDA's Local Types as JSON")
    parser.add_argument("--json", help="write the whole report here")
    parser.add_argument("--details", type=int, default=40,
                        help="differences to print")
    arguments = parser.parse_args()
    report = compare(arguments)
    if arguments.json:
        with open(arguments.json, "w", encoding="utf-8") as out:
            json.dump(report, out, indent=1)
    print_report(report, arguments.details)


if __name__ == "__main__":
    main()
