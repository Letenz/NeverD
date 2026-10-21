"""A reading view of NeverD C: the same code in fewer tokens, nothing lost.

The engine writes C that compiles: every fixed-width type spelled out, a stack
frame as a struct with one member per line, and each access into an unnamed
data region as an offset into a byte array.  A model reads the code, so this
view spells the same program shorter, and every rewrite can be undone from the
text and its legend:

- intN_t/uintN_t/uintptr_t are written iN/uN/uptr;
- a frame struct is declared on one line, and the members the engine names
  by offset (var_mN, gapN) are written without the `stack_storage.` that
  every access repeats;
- consecutive declarations of one type become one declaration;
- a value read or written at a constant offset into a byte-array global is
  named by its address, g_ADDR, and a legend declares each such name's type.
  Only accesses through a typed dereference are named, and only where every
  access at that address has one type; address arithmetic is left as written.

String and character literals and comments are never rewritten, and a short
name is used only where the text does not already use it.
"""
from __future__ import annotations

import re

TYPE_NAMES = {"int64_t": "i64", "uint64_t": "u64", "int32_t": "i32", "uint32_t": "u32", "int16_t": "i16",
              "uint16_t": "u16", "int8_t": "i8", "uint8_t": "u8", "uintptr_t": "uptr"}

_LITERAL = re.compile(r'"(?:[^"\\\n]|\\.)*"|\'(?:[^\'\\\n]|\\.)*\'|/\*.*?\*/|//[^\n]*', re.S)
_TYPE = re.compile(r"\b(" + "|".join(TYPE_NAMES) + r")\b")
_FRAME = re.compile(r"(\n[ \t]*)(_Alignas\(\d+\) struct frame_\w+) \{\n(.*?)\n[ \t]*\} stack_storage;", re.S)
_DECLARATION = re.compile(r"^([ \t]+)((?:const )?[A-Za-z_]\w*(?: \*+|\*+)?)\s*([A-Za-z_]\w*);$")
_KEYWORDS = {"return", "goto", "else", "break", "continue", "case", "default", "do"}
# *(T *)(uintptr_t)(&g_BASE_bytes[OFFSET]): a typed access at BASE + OFFSET.
_ACCESS = re.compile(r"\*\(((?:const )?[A-Za-z_]\w*)\s*\*\)\s*\(uintptr_t\)\s*\(&g_([0-9A-F]+)_bytes\[(\d+)\]\)")
_FRAME_MEMBER = re.compile(r"\bstack_storage\.((?:var_m|gap)[0-9A-F]+)\b")
_IDENTIFIER = re.compile(r"\b[A-Za-z_]\w*\b")


def _outside_literals(text, rewrite):
    """text with rewrite applied to the code between literals and comments."""
    parts, last = [], 0
    for literal in _LITERAL.finditer(text):
        parts.append(rewrite(text[last:literal.start()]))
        parts.append(literal.group(0))
        last = literal.end()
    parts.append(rewrite(text[last:]))
    return "".join(parts)


def _fold_frame(text):
    def fold(match):
        members = [line.strip() for line in match.group(3).split("\n") if line.strip()]
        return f"{match.group(1)}{match.group(2)} {{ " + " ".join(members) + " } stack_storage;"
    return _FRAME.sub(fold, text)


def _merge_declarations(text):
    lines, merged, index = text.split("\n"), [], 0
    while index < len(lines):
        first = _DECLARATION.match(lines[index])
        if not first or first.group(2) in _KEYWORDS:
            merged.append(lines[index])
            index += 1
            continue
        indent, declared, names = first.group(1), first.group(2), [first.group(3)]
        index += 1
        while index < len(lines):
            following = _DECLARATION.match(lines[index])
            if not following or following.group(1) != indent or following.group(2) != declared:
                break
            names.append(following.group(3))
            index += 1
        merged.append(f"{indent}{declared} {', '.join(names)};")
    return "\n".join(merged)


def _identifiers(text):
    found = set()
    _outside_literals(text, lambda code: found.update(_IDENTIFIER.findall(code)) or code)
    return found


def _name_globals(text, taken):
    """Typed accesses into byte-array globals named by address, and their types."""
    types = {}
    def note(code):
        for access in _ACCESS.finditer(code):
            types.setdefault(int(access.group(2), 16) + int(access.group(3)), set()).add(access.group(1))
        return code
    _outside_literals(text, note)
    named = {address: next(iter(kinds)) for address, kinds in types.items()
             if len(kinds) == 1 and f"g_{address:X}" not in taken}
    def rename(code):
        def one(access):
            address = int(access.group(2), 16) + int(access.group(3))
            return f"g_{address:X}" if address in named else access.group(0)
        return _ACCESS.sub(one, code)
    return _outside_literals(text, rename), named


def reading_view(text):
    """(view, legend) for engine C text; the legend names what the view
    abbreviates, and is empty when the view is the text itself."""
    taken = _identifiers(text)
    short = {long: brief for long, brief in TYPE_NAMES.items() if brief not in taken}
    # A frame member's bare name must not also name something else.
    elsewhere = _identifiers(_FRAME_MEMBER.sub("", _FRAME.sub("", text)))
    frame_names_free = not any(name.startswith(("var_m", "gap")) for name in elsewhere)
    used = {"types": False, "frame": False}
    def abbreviate(code):
        def one(match):
            if match.group(1) not in short:
                return match.group(1)
            used["types"] = True
            return short[match.group(1)]
        return _TYPE.sub(one, code)
    def unprefix(code):
        if not frame_names_free:
            return code
        bare = _FRAME_MEMBER.sub(r"\1", code)
        used["frame"] = used["frame"] or bare != code
        return bare
    text, globals_ = _name_globals(text, taken)
    text = _merge_declarations(_fold_frame(text))
    text = _outside_literals(text, lambda code: abbreviate(unprefix(code)))
    notes = []
    if used["types"]:
        notes.append("iN/uN: intN_t/uintN_t" + (", uptr: uintptr_t" if "uintptr_t" in short else ""))
    if used["frame"]:
        notes.append("var_mN/gapN: stack_storage members")
    if globals_:
        by_type = {}
        for address, declared in sorted(globals_.items()):
            by_type.setdefault(abbreviate(declared), []).append(f"g_{address:X}")
        notes.append("globals: " + "; ".join(f"{declared} {', '.join(names)}" for declared, names in by_type.items()))
    return text, "; ".join(notes)
