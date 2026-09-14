#!/usr/bin/env python3
"""Extract source-call ABI facts from Foundation with the SDK's libclang.

The catalog contains declarations, never framework implementations. Each
architecture must agree between the macOS and iOS preprocessing environments.
An empty encoding records negative evidence (variadic or differing declarations).
A missing platform declaration supplies no evidence and cannot veto a signature
recovered from the binary itself.
"""

import argparse
import ctypes
import json
from pathlib import Path
import tempfile


class CXString(ctypes.Structure):
    _fields_ = [("data", ctypes.c_void_p), ("flags", ctypes.c_uint)]


class CXCursor(ctypes.Structure):
    _fields_ = [
        ("kind", ctypes.c_uint),
        ("xdata", ctypes.c_int),
        ("data", ctypes.c_void_p * 3),
    ]


VISITOR = ctypes.CFUNCTYPE(
    ctypes.c_uint, CXCursor, CXCursor, ctypes.c_void_p
)
TARGETS = (
    "arm64-apple-macos15.0",
    "arm64-apple-ios18.0",
    "x86_64-apple-macos15.0",
    "x86_64-apple-ios18.0-simulator",
)


class Clang:
    def __init__(self, library):
        self.library = ctypes.CDLL(str(library))
        void = ctypes.c_void_p
        uint = ctypes.c_uint
        char = ctypes.c_char_p
        self.bind("clang_createIndex", void, ctypes.c_int, ctypes.c_int)
        self.bind("clang_disposeIndex", None, void)
        self.bind("clang_parseTranslationUnit", void, void, char,
                  ctypes.POINTER(char), ctypes.c_int, void, uint, uint)
        self.bind("clang_disposeTranslationUnit", None, void)
        self.bind("clang_getTranslationUnitCursor", CXCursor, void)
        self.bind("clang_visitChildren", uint, CXCursor, VISITOR, void)
        self.bind("clang_getCursorSpelling", CXString, CXCursor)
        self.bind("clang_getDeclObjCTypeEncoding", CXString, CXCursor)
        self.bind("clang_Cursor_isVariadic", uint, CXCursor)
        self.bind("clang_getCString", char, CXString)
        self.bind("clang_disposeString", None, CXString)
        self.bind("clang_getNumDiagnostics", uint, void)
        self.bind("clang_getDiagnostic", void, void, uint)
        self.bind("clang_getDiagnosticSeverity", uint, void)
        self.bind("clang_getDiagnosticSpelling", CXString, void)
        self.bind("clang_disposeDiagnostic", None, void)
        self.bind("clang_getClangVersion", CXString)

    def bind(self, name, result, *arguments):
        function = getattr(self.library, name)
        function.restype = result
        function.argtypes = list(arguments)
        setattr(self, name, function)

    def string(self, value):
        try:
            return (self.clang_getCString(value) or b"").decode("utf-8")
        finally:
            self.clang_disposeString(value)

    def extract(self, source, sdk, target):
        arguments = ["-x", "objective-c", "-fblocks", "-target", target,
                     "-isysroot", str(sdk)]
        argv = (ctypes.c_char_p * len(arguments))(
            *(argument.encode() for argument in arguments)
        )
        index = self.clang_createIndex(0, 0)
        unit = None
        try:
            unit = self.clang_parseTranslationUnit(
                index, str(source).encode(), argv, len(argv), None, 0, 0
            )
            if not unit:
                raise RuntimeError(f"cannot parse Foundation for {target}")
            errors = []
            for number in range(self.clang_getNumDiagnostics(unit)):
                diagnostic = self.clang_getDiagnostic(unit, number)
                try:
                    if self.clang_getDiagnosticSeverity(diagnostic) >= 3:
                        errors.append(self.string(
                            self.clang_getDiagnosticSpelling(diagnostic)))
                finally:
                    self.clang_disposeDiagnostic(diagnostic)
            if errors:
                raise RuntimeError(f"{target}: " + "\n".join(errors))
            declarations = {}
            failures = []

            @VISITOR
            def visit(cursor, parent, data):
                try:
                    # CXCursor_ObjCInstanceMethodDecl / ObjCClassMethodDecl.
                    # libclang includes synthesized property accessors here.
                    if cursor.kind in (16, 17):
                        selector = self.string(
                            self.clang_getCursorSpelling(cursor))
                        encoding = self.string(
                            self.clang_getDeclObjCTypeEncoding(cursor))
                        if self.clang_Cursor_isVariadic(cursor):
                            encoding = ""
                        declarations.setdefault(selector, set()).add(encoding)
                    return 2  # CXChildVisit_Recurse
                except Exception as error:
                    failures.append(error)
                    return 0

            self.clang_visitChildren(
                self.clang_getTranslationUnitCursor(unit), visit, None
            )
            if failures:
                raise failures[0]
            if not declarations:
                raise RuntimeError(f"no Objective-C declarations for {target}")
            return declarations
        finally:
            if unit:
                self.clang_disposeTranslationUnit(unit)
            self.clang_disposeIndex(index)


def common_encodings(first, second, selector):
    left, right = first.get(selector), second.get(selector)
    if not left or not right:
        return [None]
    if left != right or "" in left:
        return [""]
    return sorted(left)


def catalog_rows(profiles):
    if len(profiles) != len(TARGETS):
        raise ValueError("all four target profiles are required")
    selectors = sorted(set().union(*(profile.keys() for profile in profiles)))
    for selector in selectors:
        arm = common_encodings(profiles[0], profiles[1], selector)
        x64 = common_encodings(profiles[2], profiles[3], selector)
        if arm == x64 == [None]:
            continue
        for index in range(max(len(arm), len(x64))):
            # Repeat the last alternative instead of inventing a declaration.
            # The consumer merges every alternative for the same selector.
            yield selector, arm[min(index, len(arm) - 1)], x64[min(index, len(x64) - 1)]


def render(profiles, version, compiler):
    lines = [
        "// clang-format off",
        "// Generated by scripts/generate_objc_declarations.py.",
        f"// Compiler-derived Foundation API facts: MacOSX SDK {version}.",
        f"// {compiler}",
        "// Target profiles: " + ", ".join(TARGETS),
        "// No framework source implementation is included.",
        "// Empty encodings are negative evidence; nullptr means no declaration.",
    ]
    for row in catalog_rows(profiles):
        lines.append("{" + ", ".join(
            "nullptr" if value is None else json.dumps(value) for value in row
        ) + "},")
    lines.append("    // clang-format on")
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", type=Path, required=True)
    parser.add_argument("--libclang", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    sdk = args.sdk.resolve(strict=True)
    version = json.loads((sdk / "SDKSettings.json").read_text())["Version"]
    clang = Clang(args.libclang)
    with tempfile.TemporaryDirectory(prefix="neverd-objc-declarations-") as work:
        source = Path(work) / "declarations.m"
        source.write_text("#import <Foundation/Foundation.h>\n")
        profiles = [clang.extract(source, sdk, target) for target in TARGETS]
    output = render(profiles, version,
                    clang.string(clang.clang_getClangVersion()))
    if args.check:
        if args.output.read_text() != output:
            parser.error("generated catalog differs; regenerate with this SDK")
    else:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.NamedTemporaryFile(mode="w", dir=args.output.parent,
                                         delete=False) as temporary:
            temporary.write(output)
            staging = Path(temporary.name)
        try:
            staging.replace(args.output)
        finally:
            staging.unlink(missing_ok=True)
    print(f"verified {len({row[0] for row in catalog_rows(profiles)})} common selectors")


if __name__ == "__main__":
    main()
