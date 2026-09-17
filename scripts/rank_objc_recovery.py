#!/usr/bin/env python3
"""Rank Objective-C recovery work by complete blockers and dependencies.

This is an offline scheduling aid.  It consumes the production Objective-C
batch JSON without changing recovery decisions.  Candidate counts are estimates
from the reported evidence; only a new full export can establish real gains.
"""

from __future__ import annotations

import argparse
from collections import Counter, defaultdict, deque
import json
from pathlib import Path
import re
from typing import Any, Iterable, Iterator


IDENTITY_FIELDS = (
    "class_name",
    "selector",
    "class_method",
    "category_name",
    "category_address",
    "metadata_address",
    "implementation",
    "type_encoding",
)
ADDRESS_RE = re.compile(r"^0x[0-9a-fA-F]+$")


def address(value: Any) -> int | None:
    if not isinstance(value, str) or not ADDRESS_RE.fullmatch(value):
        return None
    return int(value, 16)


def address_text(value: int) -> str:
    return f"0x{value:x}"


def method_identity(method: dict[str, Any]) -> dict[str, Any]:
    return {field: method.get(field) for field in IDENTITY_FIELDS}


def identity_key(method: dict[str, Any]) -> tuple[Any, ...]:
    return tuple(method.get(field) for field in IDENTITY_FIELDS)


def strongly_connected_components(
    nodes: Iterable[int], children: dict[int, set[int]]
) -> tuple[dict[int, int], list[list[int]]]:
    """Return deterministic SCCs without depending on Python recursion depth."""
    all_nodes = sorted(set(nodes))
    reverse: dict[int, set[int]] = defaultdict(set)
    for parent, targets in children.items():
        for child in targets:
            reverse[child].add(parent)

    visited: set[int] = set()
    finish_order: list[int] = []
    for root in all_nodes:
        if root in visited:
            continue
        visited.add(root)
        # Suspend each parent's iterator while visiting one child. Marking all
        # siblings up front lets cross edges skip unfinished descendants and
        # produces an invalid finish order even for an acyclic graph.
        frames: list[tuple[int, Iterator[int]]] = [
            (root, iter(sorted(children.get(root, ()))))
        ]
        while frames:
            node, targets = frames[-1]
            child = next(targets, None)
            if child is None:
                finish_order.append(node)
                frames.pop()
                continue
            if child not in visited:
                visited.add(child)
                frames.append((child, iter(sorted(children.get(child, ())))))

    components: list[list[int]] = []
    assigned: set[int] = set()
    for root in reversed(finish_order):
        if root in assigned:
            continue
        component: list[int] = []
        pending = [root]
        assigned.add(root)
        while pending:
            node = pending.pop()
            component.append(node)
            for parent in sorted(reverse.get(node, ()), reverse=True):
                if parent not in assigned:
                    assigned.add(parent)
                    pending.append(parent)
        components.append(sorted(component))
    components.sort(key=lambda component: component[0])
    component_by_node = {
        node: component_index
        for component_index, component in enumerate(components)
        for node in component
    }
    return component_by_node, components


def blocker_key(
    item: dict[str, Any],
    component_by_node: dict[int, int],
    components: list[list[int]],
) -> tuple[str, dict[str, Any]]:
    code = str(item.get("code") or "unknown")
    reason = str(item.get("reason") or "")
    call = item.get("call") if isinstance(item.get("call"), dict) else {}
    target = address(call.get("target_address"))
    related = address(item.get("related_address"))
    detail: dict[str, Any] = {"code": code, "reason": reason}

    if target is not None and not call.get("indirect", False):
        detail["target_address"] = address_text(target)
        detail["target_name"] = str(call.get("target_name") or "")
        component_index = component_by_node.get(target)
        if component_index is not None:
            members = components[component_index]
            detail["dependency_scc"] = [address_text(member) for member in members]
            return f"{code}:scc:{address_text(members[0])}", detail
        return f"{code}:target:{address_text(target)}", detail

    if related is not None:
        detail["related_address"] = address_text(related)
        component_index = component_by_node.get(related)
        if code == "dependency" and component_index is not None:
            members = components[component_index]
            detail["dependency_scc"] = [address_text(member) for member in members]
            return f"{code}:scc:{address_text(members[0])}", detail
        return f"{code}:related:{address_text(related)}:{reason}", detail

    if call:
        name = str(call.get("target_name") or "")
        detail["target_name"] = name
        detail["indirect"] = bool(call.get("indirect", False))
        return f"{code}:indirect:{name}:{reason}", detail
    return f"{code}:reason:{reason}", detail


def reverse_reachable(starts: Iterable[int], parents: dict[int, set[int]]) -> set[int]:
    seen = set(starts)
    pending = deque(sorted(seen))
    while pending:
        node = pending.popleft()
        for parent in sorted(parents.get(node, ())):
            if parent not in seen:
                seen.add(parent)
                pending.append(parent)
    return seen


def analyze_source_closure(report: dict[str, Any], limit: int) -> dict[str, Any]:
    """Group complete *observed* repair sets on the publication graph.

    Unbound calls are diagnostics, not publication edges. Repairing one can
    introduce new edges, so even a complete observed set is not a gain promise.
    """
    graph = report.get("source_projection_graph")
    if graph is None:
        return {"available": False, "reason": "source projection evidence absent"}
    if (not isinstance(graph, dict) or graph.get("schema_version") != 1
            or graph.get("scope") != "final_native_and_block_source_projections"
            or graph.get("closure_stage") != "before_method_emission_and_text_checks"):
        raise ValueError("unsupported source projection graph")
    raw_nodes = graph.get("nodes")
    if not isinstance(raw_nodes, list):
        raise ValueError("source projection nodes must be an array")
    nodes = {}
    children: dict[int, set[int]] = {}
    for node in raw_nodes:
        entry = address(node.get("address"))
        if entry is None or entry in nodes:
            raise ValueError("invalid or duplicate source projection address")
        dependencies = node.get("dependencies")
        if not isinstance(dependencies, list):
            raise ValueError("source projection dependencies must be an array")
        targets = {address(target) for target in dependencies}
        if None in targets:
            raise ValueError("invalid source projection dependency address")
        nodes[entry] = node
        children[entry] = targets
    for entry, targets in children.items():
        if not targets <= nodes.keys():
            raise ValueError("source projection dependency has no evidence node")
        if nodes[entry].get("closure_closed") and (
            not nodes[entry].get("local_gate_passed")
            or any(not nodes[target].get("closure_closed") for target in targets)
        ):
            raise ValueError("source projection closure contradicts its dependencies")
    mapping, components = strongly_connected_components(nodes, children)
    def source_blocker(item: dict[str, Any], entry: int | None):
        key, detail = blocker_key(item, mapping, components)
        call = item.get("call", {})
        if ((call and (call.get("indirect") or address(call.get("target_address")) is None))
                or (not call and address(item.get("related_address")) is None)):
            # Identical generic reasons do not prove identical repair work.
            # Especially, unrelated dynamic calls must not become one batch.
            key += f":at:{entry}:{item.get('statement_address', 'unknown')}"
            detail = {**detail, "source_address": address_text(entry) if entry is not None else None,
                      "statement_address": item.get("statement_address")}
        return key, detail
    details = {}
    local_blockers = {}
    complete = {}
    for entry, node in nodes.items():
        diagnostic = node.get("local_diagnostics", {})
        items = diagnostic.get("items", [])
        complete[entry] = bool(node.get("has_typed_body")
                               and diagnostic.get("checks_complete"))
        keys = set()
        for item in items:
            key, detail = source_blocker(item, entry)
            keys.add(key)
            details.setdefault(key, detail)
        if not node.get("local_gate_passed") and not keys:
            key = f"unclassified:source:{address_text(entry)}:{node.get('local_reason', '')}"
            keys.add(key)
            details[key] = {"code": "unclassified", "reason": node.get("local_reason", "")}
            complete[entry] = False
        local_blockers[entry] = keys

    bundles: dict[tuple[str, ...], list[dict[str, Any]]] = defaultdict(list)
    uncertain = []
    closed_but_unpublished = 0
    for method in report["methods"]:
        if method.get("status") == "recovered":
            continue
        entry = address(method.get("implementation"))
        reached = set()
        pending = [entry] if entry in nodes else []
        blockers = set()
        unknown = set()
        if entry not in nodes:
            unknown.add("missing_method_projection")
        if not graph.get("native_inventory_complete"):
            unknown.add("incomplete_native_inventory")
        while pending:
            current = pending.pop()
            if current in reached:
                continue
            reached.add(current)
            blockers.update(local_blockers[current])
            if not complete[current]:
                unknown.add(f"incomplete_projection:{address_text(current)}")
            pending.extend(children[current] - reached)
        diagnostic = method.get("projection_diagnostics", {})
        if not diagnostic.get("checks_complete"):
            unknown.add("incomplete_method_checks")
        # Per-method rendering/type checks can differ for aliases of one IMP.
        # Keep their identities and diagnostics even when the native body closes.
        for item in diagnostic.get("items", []):
            key, detail = source_blocker(item, entry)
            blockers.add(key)
            details.setdefault(key, detail)
        if entry in nodes and nodes[entry].get("closure_closed"):
            closed_but_unpublished += 1
        if not blockers:
            unknown.add("unexplained_publication_failure")
        if unknown:
            uncertain.append({"method": method_identity(method),
                              "unknown_conditions": sorted(unknown),
                              "observed_blockers": sorted(blockers)})
        else:
            bundles[tuple(sorted(blockers))].append(method_identity(method))
    rows = [{"blockers": list(keys), "observed_complete_methods": len(methods),
             "methods": sorted(methods, key=lambda m: tuple(str(v) for v in identity_key(m))),
             "can_reveal_new_dependencies": any(details[key]["code"] in
                                                {"call_binding", "dependency"}
                                                for key in keys)}
            for keys, methods in bundles.items()]
    rows.sort(key=lambda row: (-row["observed_complete_methods"],
                              len(row["blockers"]), row["blockers"]))
    return {
        "available": True,
        "scope": graph["scope"],
        "node_count": len(nodes),
        "edge_count": sum(map(len, children.values())),
        "recursive_scc_count": sum(len(component) > 1 for component in components),
        "complete_observed_method_count": sum(map(len, bundles.values())),
        "incomplete_observed_method_count": len(uncertain),
        "closed_but_unpublished_method_count": closed_but_unpublished,
        "blocker_details": {key: details[key] for row in rows[:limit] for key in row["blockers"]},
        "repair_bundles": rows[:limit],
        "incomplete_methods": uncertain,
    }


def analyze(report: dict[str, Any], *, limit: int = 50) -> dict[str, Any]:
    methods = report.get("methods")
    graph = report.get("native_dependency_graph")
    if not isinstance(methods, list) or not isinstance(graph, dict):
        raise ValueError("input must contain methods and native_dependency_graph")
    if report.get("method_count") != len(methods):
        raise ValueError("method_count does not match methods")
    recovered_count = sum(method.get("status") == "recovered" for method in methods)
    if report.get("recovered_method_count") != recovered_count:
        raise ValueError("recovered_method_count does not match methods")
    if len({identity_key(method) for method in methods}) != len(methods):
        raise ValueError("method identities are not unique")

    children: dict[int, set[int]] = defaultdict(set)
    parents: dict[int, set[int]] = defaultdict(set)
    nodes: set[int] = set()
    calls = graph.get("calls")
    if not isinstance(calls, list):
        raise ValueError("native_dependency_graph.calls must be an array")
    direct_call_records = 0
    indirect_call_records = 0
    for call in calls:
        if not isinstance(call, dict):
            raise ValueError("native_dependency_graph call must be an object")
        caller = address(call.get("caller"))
        target = address(call.get("target_address"))
        if caller is not None:
            nodes.add(caller)
        if call.get("indirect", False) or target is None:
            indirect_call_records += 1
            continue
        if caller is None:
            raise ValueError("direct dependency call has no caller address")
        direct_call_records += 1
        nodes.add(target)
        children[caller].add(target)
        parents[target].add(caller)
    roots = {value for raw in graph.get("roots", []) if (value := address(raw)) is not None}
    nodes.update(roots)
    component_by_node, components = strongly_connected_components(nodes, children)

    identities_by_impl: dict[int, list[dict[str, Any]]] = defaultdict(list)
    recovered_by_impl: dict[int, list[dict[str, Any]]] = defaultdict(list)
    unrecovered: list[dict[str, Any]] = []
    for method in methods:
        implementation = address(method.get("implementation"))
        if implementation is not None:
            identities_by_impl[implementation].append(method)
            if method.get("status") == "recovered":
                recovered_by_impl[implementation].append(method)
        if method.get("status") != "recovered":
            unrecovered.append(method)

    blocker_methods: dict[str, set[tuple[Any, ...]]] = defaultdict(set)
    blocker_details: dict[str, dict[str, Any]] = {}
    blocker_targets: dict[str, set[int]] = defaultdict(set)
    method_blockers: dict[tuple[Any, ...], set[str]] = {}
    method_complete: dict[tuple[Any, ...], bool] = {}
    method_by_key = {identity_key(method): method for method in methods}
    code_events: Counter[str] = Counter()

    for method in unrecovered:
        key = identity_key(method)
        diagnostics = method.get("projection_diagnostics")
        if not isinstance(diagnostics, dict):
            diagnostics = {"checks_complete": False, "items": []}
        items = diagnostics.get("items")
        if not isinstance(items, list):
            items = []
        blockers: set[str] = set()
        for item in items:
            if not isinstance(item, dict):
                continue
            code_events[str(item.get("code") or "unknown")] += 1
            blocker, detail = blocker_key(item, component_by_node, components)
            blockers.add(blocker)
            blocker_methods[blocker].add(key)
            blocker_details.setdefault(blocker, detail)
            call = item.get("call") if isinstance(item.get("call"), dict) else {}
            target = address(call.get("target_address"))
            related = address(item.get("related_address"))
            if target is not None and not call.get("indirect", False):
                blocker_targets[blocker].add(target)
            if detail.get("code") == "dependency" and related is not None:
                blocker_targets[blocker].add(related)
        if not blockers:
            fallback = f"unclassified:reason:{method.get('reason', '')}"
            blockers.add(fallback)
            blocker_methods[fallback].add(key)
            blocker_details.setdefault(
                fallback,
                {"code": "unclassified", "reason": str(method.get("reason") or "")},
            )
        method_blockers[key] = blockers
        method_complete[key] = bool(diagnostics.get("checks_complete", False))

    candidate_rows: list[dict[str, Any]] = []
    for blocker, affected_keys in blocker_methods.items():
        targets = blocker_targets.get(blocker, set())
        reverse_nodes = reverse_reachable(targets, parents) if targets else set()
        reverse_unrecovered = sum(
            len([m for m in identities_by_impl.get(node, ()) if m.get("status") != "recovered"])
            for node in reverse_nodes
        )
        reverse_recovered = sum(
            len(recovered_by_impl.get(node, ())) for node in reverse_nodes
        )
        ready_keys = {
            key
            for key in affected_keys
            if method_complete[key] and method_blockers[key] == {blocker}
        }
        incomplete_keys = {key for key in affected_keys if not method_complete[key]}
        samples = [
            method_identity(method_by_key[key])
            for key in sorted(affected_keys, key=lambda value: tuple(str(x) for x in value))[:5]
        ]
        candidate_rows.append(
            {
                "blocker": blocker,
                **blocker_details[blocker],
                "reported_methods": len(affected_keys),
                "known_ready_if_resolved": len(ready_keys),
                "incomplete_check_methods": len(incomplete_keys),
                "reverse_unrecovered_methods": reverse_unrecovered,
                "reverse_recovered_methods_at_risk": reverse_recovered,
                "reverse_functions": len(reverse_nodes),
                "sample_methods": samples,
            }
        )
    candidate_rows.sort(
        key=lambda row: (
            -row["known_ready_if_resolved"],
            -row["reported_methods"],
            -row["reverse_unrecovered_methods"],
            row["blocker"],
        )
    )

    combinations: Counter[tuple[str, ...]] = Counter()
    incomplete_methods = 0
    for key, blockers in method_blockers.items():
        if method_complete[key]:
            combinations[tuple(sorted(blockers))] += 1
        else:
            incomplete_methods += 1
    combination_rows = [
        {"blockers": list(blockers), "complete_methods": count}
        for blockers, count in combinations.items()
    ]
    combination_rows.sort(
        key=lambda row: (-row["complete_methods"], len(row["blockers"]), row["blockers"])
    )

    direct_edges = sum(len(targets) for targets in children.values())
    recursive_components = [component for component in components if len(component) > 1]
    return {
        "schema_version": 1,
        "scope": "offline_objc_recovery_scheduling",
        "limitations": [
            "Candidate counts are scheduling estimates, not recovered methods.",
            "Unknown indirect call targets are not traversed.",
            "Methods with incomplete checks may have additional blockers.",
            "A full export and source validation establish actual gains and regressions.",
            "Legacy known_ready_if_resolved only counts method-local reported blockers.",
            "Source repair bundles include current projection dependencies; binding repairs can reveal new ones.",
        ],
        "summary": {
            "method_count": len(methods),
            "recovered_method_count": recovered_count,
            "unrecovered_method_count": len(unrecovered),
            "incomplete_check_method_count": incomplete_methods,
            "unique_implementation_count": len(identities_by_impl),
            "graph_root_count": len(roots),
            "call_record_count": len(calls),
            "direct_call_record_count": direct_call_records,
            "indirect_call_record_count": indirect_call_records,
            "unique_direct_edge_count": direct_edges,
            "known_graph_node_count": len(nodes),
            "recursive_scc_count": len(recursive_components),
            "inventory_complete": bool(graph.get("inventory_complete", False)),
            "targets_complete": bool(graph.get("targets_complete", False)),
            "diagnostic_event_counts": dict(sorted(code_events.items())),
        },
        "candidate_batches": candidate_rows[:limit],
        "complete_blocker_combinations": combination_rows[:limit],
        "source_closure": analyze_source_closure(report, limit),
    }


def markdown(result: dict[str, Any]) -> str:
    summary = result["summary"]
    lines = [
        "# Objective-C recovery batch ranking",
        "",
        f"Recovered: {summary['recovered_method_count']} / {summary['method_count']}; "
        f"unrecovered: {summary['unrecovered_method_count']}; "
        f"incomplete checks: {summary['incomplete_check_method_count']}.",
        "",
        "Method-local estimates on the LowIR graph (not source closure):",
        "",
        "| Single reported blocker | Reported | Incomplete | Reverse roots | Recovered at risk | Blocker |",
        "| ---: | ---: | ---: | ---: | ---: | --- |",
    ]
    for row in result["candidate_batches"]:
        lines.append(
            f"| {row['known_ready_if_resolved']} | {row['reported_methods']} | "
            f"{row['incomplete_check_methods']} | {row['reverse_unrecovered_methods']} | "
            f"{row['reverse_recovered_methods_at_risk']} | `{row['blocker']}` |"
        )
    lines.extend(["", "Counts are estimates; actual gains require a complete export.", ""])
    closure = result["source_closure"]
    if closure["available"]:
        lines.extend([
            "## Current source projection repair sets", "",
            f"Complete observed methods: {closure['complete_observed_method_count']}; "
            f"incomplete: {closure['incomplete_observed_method_count']}; "
            f"closed but unpublished: {closure['closed_but_unpublished_method_count']}.",
            "",
            "Binding repairs can reveal additional dependencies. These counts are not guaranteed gains.",
            "", "| Observed complete methods | Repair set |", "| ---: | --- |",
        ])
        for row in closure["repair_bundles"]:
            lines.append(f"| {row['observed_complete_methods']} | "
                         + "; ".join(f"`{key}`" for key in row["blockers"]) + " |")
        lines.append("")
    return "\n".join(lines)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path, help="Objective-C batch JSON")
    parser.add_argument("--json-output", type=Path)
    parser.add_argument("--markdown-output", type=Path)
    parser.add_argument("--limit", type=int, default=50)
    args = parser.parse_args()
    if args.limit <= 0:
        parser.error("--limit must be positive")
    result = analyze(json.loads(args.report.read_text()), limit=args.limit)
    encoded = json.dumps(result, indent=2, sort_keys=True) + "\n"
    if args.json_output:
        args.json_output.write_text(encoded)
    else:
        print(encoded, end="")
    if args.markdown_output:
        args.markdown_output.write_text(markdown(result))


if __name__ == "__main__":
    main()
