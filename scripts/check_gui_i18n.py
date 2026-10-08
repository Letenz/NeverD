#!/usr/bin/env python3
"""Validate the GUI locale matrix, translation coverage, Qt placeholders, and
menu access keys."""
from __future__ import annotations

import argparse
from collections import Counter
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

from check_docs_i18n import LOCALES

REPOSITORY = Path(__file__).resolve().parents[1]
GUI = REPOSITORY / "tools" / "neverd-gui"
PLACEHOLDER = re.compile(r"%L?(?:[1-9][0-9]*|n)")
MENU = r'NEVERD_MENU\((\w+),\s*(\w+),\s*QT_TRANSLATE_NOOP\("Menus",\s*"([^"]*)"\)\)'
MENU_ITEM = r"NEVERD_MENU_ITEM\((\w+),\s*(\w+)\)"
ACTION = re.compile(r'NEVERD_(?:VIEW_)?ACTION\(\s*(\w+),\s*QT_TRANSLATE_NOOP\("Actions",\s*((?:"[^"]*"\s*)+)\)')
ACCESS_KEY = re.compile(r"&(.)")


def gui_sources():
    """Every file with translatable strings: C++ sources and the .def tables."""
    return [str(path) for pattern in ("*.cpp", "app/*.cpp", "app/*.def", "mcp/*.cpp")
            for path in sorted(GUI.glob(pattern))]


def menu_entries():
    """Each menu's entries as catalog keys (context, source), from Menus.def
    and the action texts of Actions.def."""
    actions = {match[1]: "".join(re.findall(r'"([^"]*)"', match[2]))
               for match in ACTION.finditer((GUI / "app" / "Actions.def").read_text(encoding="utf-8"))}
    menus = {}
    for match in re.finditer(f"{MENU}|{MENU_ITEM}", (GUI / "app" / "Menus.def").read_text(encoding="utf-8")):
        if match[1]:
            menus.setdefault(match[2], []).append(("Menus", match[3]))
        else:
            menus.setdefault(match[4], []).append(("Actions", actions.get(match[5], match[5])))
    return menus


def access_key(text):
    """The letter after a menu text's single &, case-folded; None without one."""
    match = ACCESS_KEY.search(text.replace("&&", ""))
    return match[1].casefold() if match else None


def messages(path):
    tree = ET.parse(path)
    entries = {}
    for context in tree.getroot().findall("context"):
        for message in context.findall("message"):
            translation = message.find("translation")
            if translation is not None and translation.get("type") in ("vanished", "obsolete"):
                continue
            key = (context.findtext("name"), message.findtext("source") or "",
                   message.findtext("comment") or "", message.get("numerus", "no"))
            if key in entries:
                raise ValueError(f"{path.name}: duplicate translation key {key!r}")
            entries[key] = translation
    return tree.getroot(), entries


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check-sources", action="store_true", help="Use lupdate to check the actual C++ source keys")
    parser.add_argument("--lupdate", help="Explicit lupdate executable; implies --check-sources")
    args = parser.parse_args()
    errors = []
    locales = ("en", *(locale.replace("-", "_") for locale in LOCALES))
    directory = GUI / "i18n"
    expected_files = {f"neverd_{locale}.ts" for locale in locales}
    actual_files = {path.name for path in directory.glob("*.ts")}
    if expected_files != actual_files:
        errors.append(f"locale files: missing {sorted(expected_files - actual_files)}, extra {sorted(actual_files - expected_files)}")
    try:
        _, reference = messages(directory / "neverd_en.ts")
    except (OSError, ET.ParseError, ValueError) as error:
        print(error, file=sys.stderr)
        return 1
    if not reference:
        errors.append("English source catalog is empty")
    menus = menu_entries()
    for locale in locales:
        path = directory / f"neverd_{locale}.ts"
        if not path.exists():
            continue
        try:
            root, entries = messages(path)
        except (OSError, ET.ParseError, ValueError) as error:
            errors.append(str(error)); continue
        if root.get("language") != locale:
            errors.append(f"{path.name}: expected language={locale}")
        if set(entries) != set(reference):
            missing = sorted(set(reference) - set(entries))
            extra = sorted(set(entries) - set(reference))
            errors.append(f"{path.name}: missing source keys {missing!r}; extra keys {extra!r}")
        for key, translation in entries.items():
            source = key[1]
            if translation is None or translation.get("type") == "unfinished":
                errors.append(f"{path.name}: unfinished {key[:2]!r}")
                continue
            forms = translation.findall("numerusform") if key[3] == "yes" else [translation]
            if not forms:
                errors.append(f"{path.name}: empty plural forms {key[:2]!r}")
            for form in forms:
                value = "".join(form.itertext())
                if not value.strip():
                    errors.append(f"{path.name}: empty translation {key[:2]!r}")
                if Counter(PLACEHOLDER.findall(value)) != Counter(PLACEHOLDER.findall(source)):
                    errors.append(f"{path.name}: placeholder mismatch {key[:2]!r}")
                if value.count("\n") != source.count("\n"):
                    errors.append(f"{path.name}: multiline layout mismatch {key[:2]!r}")
                if locale == "en" and value != source:
                    errors.append(f"{path.name}: English catalog differs from source {key[:2]!r}")
        # Within a menu, each entry has its own access key.
        texts = {key[:2]: "".join(translation.itertext()) for key, translation in entries.items()
                 if translation is not None}
        for menu, items in menus.items():
            keys = Counter(access_key(texts.get(item, item[1])) for item in items)
            keys.pop(None, None)
            for letter, count in sorted(keys.items()):
                if count > 1:
                    errors.append(f"{path.name}: menu {menu} gives access key {letter!r} to {count} entries")
    if args.check_sources or args.lupdate:
        executable = args.lupdate or shutil.which("lupdate") or shutil.which("lupdate6")
        if not executable:
            errors.append("Source validation requires lupdate; supply --lupdate PATH")
        else:
            with tempfile.TemporaryDirectory(prefix="neverd-i18n-") as temporary:
                catalog = Path(temporary) / "sources.ts"
                sources = gui_sources()
                result = subprocess.run([executable, *sources, "-no-obsolete", "-source-language", "en", "-ts", str(catalog)],
                                        capture_output=True, text=True, timeout=60)
                if result.returncode:
                    errors.append("lupdate failed: " + result.stderr)
                else:
                    _, source_entries = messages(catalog)
                    if set(source_entries) != set(reference):
                        errors.append(f"English catalog is stale: missing {sorted(set(source_entries) - set(reference))!r}; obsolete {sorted(set(reference) - set(source_entries))!r}")
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    print(f"GUI translations: {len(locales)} locales, {len(reference)} source keys, complete coverage, "
          f"matching placeholders and unique access keys in {len(menus)} menus.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
