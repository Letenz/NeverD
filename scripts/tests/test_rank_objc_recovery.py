from __future__ import annotations

import unittest

from scripts.rank_objc_recovery import analyze, strongly_connected_components


def identity(name, selector, implementation, metadata, *, recovered=False):
    return {
        "class_name": name,
        "selector": selector,
        "class_method": False,
        "category_name": "",
        "category_address": "0x0",
        "metadata_address": metadata,
        "implementation": implementation,
        "type_encoding": "v16@0:8",
        "status": "recovered" if recovered else "unrecovered",
    }


def item(code, *, target=None, related=None, reason="blocked"):
    result = {"code": code, "reason": reason}
    if target is not None:
        result["call"] = {
            "target_address": target,
            "target_name": f"fn_{target}",
            "indirect": False,
        }
    if related is not None:
        result["related_address"] = related
    return result


class ObjCRecoveryRankingTests(unittest.TestCase):
    def report(self):
        first = identity("C", "first", "0x10", "0x100")
        first["projection_diagnostics"] = {
            "checks_complete": True,
            "items": [item("call_binding", target="0x30")],
        }
        alias = identity("C", "alias", "0x10", "0x108")
        alias["projection_diagnostics"] = {
            "checks_complete": True,
            "items": [
                item("call_binding", target="0x30"),
                item("data_binding", related="0x80", reason="mutable data"),
            ],
        }
        incomplete = identity("C", "unknown", "0x20", "0x110")
        incomplete["projection_diagnostics"] = {
            "checks_complete": False,
            "items": [item("call_binding", target="0x40")],
        }
        recovered = identity("C", "done", "0x50", "0x118", recovered=True)
        return {
            "method_count": 4,
            "recovered_method_count": 1,
            "methods": [first, alias, incomplete, recovered],
            "native_dependency_graph": {
                "inventory_complete": True,
                "targets_complete": False,
                "roots": ["0x10", "0x20", "0x50"],
                "missing_functions": [],
                "calls": [
                    {"caller": "0x10", "target_address": "0x30", "indirect": False},
                    {"caller": "0x20", "target_address": "0x30", "indirect": False},
                    {"caller": "0x30", "target_address": "0x40", "indirect": False},
                    {"caller": "0x40", "target_address": "0x30", "indirect": False},
                    {"caller": "0x50", "target_address": None, "indirect": True},
                ],
            },
        }

    def test_preserves_method_identities_and_ranks_complete_blockers(self):
        result = analyze(self.report())
        summary = result["summary"]
        self.assertEqual(summary["method_count"], 4)
        self.assertEqual(summary["unrecovered_method_count"], 3)
        self.assertEqual(summary["incomplete_check_method_count"], 1)
        self.assertEqual(summary["unique_implementation_count"], 3)
        self.assertEqual(summary["indirect_call_record_count"], 1)
        self.assertEqual(summary["recursive_scc_count"], 1)

        call = next(row for row in result["candidate_batches"]
                    if row["code"] == "call_binding" and row.get("target_address") == "0x30")
        self.assertEqual(call["reported_methods"], 3)
        self.assertEqual(call["known_ready_if_resolved"], 1)
        self.assertEqual(call["incomplete_check_methods"], 1)
        self.assertEqual(len(call["sample_methods"]), 3)
        self.assertEqual({row["metadata_address"] for row in call["sample_methods"]},
                         {"0x100", "0x108", "0x110"})

    def test_incomplete_checks_never_claim_a_ready_method(self):
        result = analyze(self.report())
        row = next(row for row in result["candidate_batches"]
                   if row.get("target_address") == "0x30")
        self.assertEqual(row["reported_methods"], 3)
        self.assertEqual(row["known_ready_if_resolved"], 1)
        self.assertEqual(row["incomplete_check_methods"], 1)
        self.assertEqual(row["dependency_scc"], ["0x30", "0x40"])

    def test_reports_full_blocker_combinations(self):
        result = analyze(self.report())
        combinations = result["complete_blocker_combinations"]
        self.assertEqual(sum(row["complete_methods"] for row in combinations), 2)
        self.assertEqual(sorted(len(row["blockers"]) for row in combinations), [1, 2])

    def test_rejects_duplicate_full_method_identity(self):
        report = self.report()
        report["methods"].append(dict(report["methods"][0]))
        report["method_count"] += 1
        with self.assertRaisesRegex(ValueError, "identities are not unique"):
            analyze(report)

    def test_sccs_are_deterministic(self):
        mapping, components = strongly_connected_components(
            [1, 2, 3, 4], {1: {2}, 2: {1, 3}, 3: {4}, 4: set()}
        )
        self.assertEqual(components, [[1, 2], [3], [4]])
        self.assertEqual(mapping[1], mapping[2])
        self.assertNotEqual(mapping[2], mapping[3])


if __name__ == "__main__":
    unittest.main()
