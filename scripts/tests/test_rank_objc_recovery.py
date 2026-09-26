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

    def source_report(self):
        report = self.report()
        def node(entry, dependencies=(), items=(), *, complete=True, closed=False):
            return {"address": entry, "dependencies": list(dependencies),
                    "has_typed_body": True, "local_gate_passed": not items,
                    "closure_closed": closed,
                    "local_diagnostics": {"checks_complete": complete,
                                          "items": list(items)}}
        report["source_projection_graph"] = {
            "schema_version": 1,
            "scope": "final_native_and_block_source_projections",
            "closure_stage": "before_method_emission_and_text_checks",
            "native_inventory_complete": True,
            "nodes": [node("0x10", ["0x30"]), node("0x20", ["0x40"]),
                      node("0x30", ["0x40"], [item("data_binding", related="0x90")]),
                      node("0x40", ["0x30"], [item("data_binding", related="0xa0")]),
                      node("0x50", closed=True)],
        }
        return report

    def test_source_bundles_include_downstream_blockers_and_keep_aliases(self):
        result = analyze(self.source_report())["source_closure"]
        self.assertTrue(result["available"])
        self.assertEqual(result["recursive_scc_count"], 1)
        self.assertEqual(result["complete_observed_method_count"], 2)
        self.assertEqual(result["incomplete_observed_method_count"], 1)
        self.assertEqual(len(result["repair_bundles"]), 2)
        for bundle in result["repair_bundles"]:
            details = [result["blocker_details"][key] for key in bundle["blockers"]]
            self.assertTrue({"0x90", "0xa0"} <=
                            {item.get("related_address") for item in details})
            self.assertTrue(bundle["can_reveal_new_dependencies"])
        self.assertEqual({method["metadata_address"]
                          for bundle in result["repair_bundles"]
                          for method in bundle["methods"]}, {"0x100", "0x108"})

    def test_unknown_downstream_evidence_excludes_every_affected_method(self):
        report = self.source_report()
        report["source_projection_graph"]["nodes"][3]["local_diagnostics"]["checks_complete"] = False
        result = analyze(report)["source_closure"]
        self.assertEqual(result["complete_observed_method_count"], 0)
        self.assertEqual(result["incomplete_observed_method_count"], 3)
        self.assertTrue(all("incomplete_projection:0x40" in row["unknown_conditions"]
                            for row in result["incomplete_methods"]))

    def test_closed_native_projection_does_not_imply_method_publication(self):
        report = self.source_report()
        graph = report["source_projection_graph"]
        for node in graph["nodes"]:
            node.update(closure_closed=True, local_gate_passed=True,
                        local_diagnostics={"checks_complete": True, "items": []})
        result = analyze(report)["source_closure"]
        self.assertEqual(result["closed_but_unpublished_method_count"], 3)
        self.assertEqual(result["complete_observed_method_count"], 2)
        # Native closure cannot erase alias-specific method diagnostics.
        self.assertEqual(sorted(len(row["blockers"]) for row in result["repair_bundles"]), [1, 2])

    def test_source_graph_rejects_missing_nodes_and_false_closure(self):
        for mutation in range(3):
            report = self.source_report()
            graph = report["source_projection_graph"]
            if mutation == 0:
                graph["nodes"].pop(3)
            elif mutation == 1:
                graph["nodes"][0]["closure_closed"] = True
            else:
                graph["nodes"].append(dict(graph["nodes"][0]))
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                analyze(report)

    def test_legacy_graph_never_claims_source_closure_evidence(self):
        self.assertFalse(analyze(self.report())["source_closure"]["available"])

    def test_unrelated_dynamic_calls_do_not_become_one_repair(self):
        report = self.source_report()
        for method, node in zip(report["methods"][:2],
                                report["source_projection_graph"]["nodes"][:2]):
            method["implementation"] = node["address"]
            diagnostic = {"checks_complete": True, "items": [{
                "code": "call_binding", "reason": "unknown dynamic ABI",
                "call": {"indirect": True, "target_name": "indirect"},
                "statement_address": node["address"],
            }]}
            method["projection_diagnostics"] = diagnostic
            node.update(dependencies=[], local_gate_passed=False,
                        local_diagnostics=diagnostic)
        result = analyze(report)["source_closure"]
        self.assertEqual(len(result["repair_bundles"]), 2)
        self.assertTrue(all(row["observed_complete_methods"] == 1
                            for row in result["repair_bundles"]))

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

    def test_cross_edges_do_not_create_cycles(self):
        for graph in (
            {1: {2, 3}, 2: {3}, 3: set()},
            {1: {2, 3}, 2: {4}, 3: {4}, 4: set()},
        ):
            with self.subTest(graph=graph):
                _, components = strongly_connected_components(graph, graph)
                self.assertEqual(components, [[node] for node in sorted(graph)])

    def test_all_three_node_graphs_match_mutual_reachability(self):
        # Exhaustive directed graphs, including self edges and disconnected
        # nodes. Reachability is an independent oracle, not a second DFS SCC.
        nodes = range(3)
        for mask in range(1 << 9):
            graph = {a: {b for b in nodes if mask & (1 << (a * 3 + b))}
                     for a in nodes}
            reachable = {a: {a} | graph[a] for a in nodes}
            for middle in nodes:
                for source in nodes:
                    if middle in reachable[source]:
                        reachable[source].update(reachable[middle])
            mapping, components = strongly_connected_components(nodes, graph)
            self.assertEqual(sorted(node for group in components for node in group),
                             list(nodes))
            for a in nodes:
                for b in nodes:
                    self.assertEqual(mapping[a] == mapping[b],
                                     b in reachable[a] and a in reachable[b],
                                     (mask, a, b))

    def test_deep_graph_does_not_use_the_python_call_stack(self):
        graph = {node: {node + 1} for node in range(5000)}
        graph[5000] = {2500}
        _, components = strongly_connected_components(range(5002), graph)
        self.assertEqual(components[:2500], [[node] for node in range(2500)])
        self.assertEqual(components[2500:], [list(range(2500, 5001)), [5001]])


if __name__ == "__main__":
    unittest.main()
