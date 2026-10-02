"""External bytecode rules routed through the shared NeverD recovery API."""

from __future__ import annotations

import ctypes
from dataclasses import dataclass
import json
from typing import Any, Literal, Mapping, Sequence, cast

from .api import NeverDError
from .ffi import HostAPI


@dataclass(frozen=True)
class BytecodeRecoveryResult:
    """Reachable coverage and optional state-ABI C, not original source types."""

    functions: int
    blocks: int
    decoded_instructions: int
    decoded_bytes: int
    input_bytes: int
    scope: Literal["cfg", "state-c"]
    source: str


def recover_bytecode(
    code: bytes,
    profile: Mapping[str, Any],
    functions: Sequence[Mapping[str, Any]],
    *,
    bindings: Sequence[Mapping[str, Any]] = (),
    base: int = 0,
    output: Literal["check", "highc", "llvmc"] = "highc",
    optimize: bool = False,
    unaligned_pointers: bool = False,
    api: HostAPI | None = None,
) -> BytecodeRecoveryResult:
    """Check or emit caller-defined bytecode through the public C ABI.

    A trusted plugin owns container reading and rule construction. The engine
    validates encoding, CFG and state lowering; it does not execute the input.
    Dynamic decoder contexts and original source ABIs are not inferred by this
    profile API. ``api`` can name an explicitly loaded library outside the host.
    """
    if not isinstance(code, bytes):
        raise TypeError("code must be bytes")
    if len(code) > 64 * 1024 * 1024:
        raise ValueError("code exceeds the 64 MiB limit")
    request = json.dumps(
        {"schemaVersion": 1, "profile": dict(profile),
         "functions": [dict(item) for item in functions],
         "bindings": [dict(item) for item in bindings], "base": base, "output": output,
         "optimize": optimize, "unaligned_pointers": unaligned_pointers},
        allow_nan=False, separators=(",", ":"),
    ).encode("utf-8")
    if len(request) > 64 * 1024 * 1024:
        raise ValueError("request exceeds the 64 MiB limit")
    buffer = (ctypes.c_ubyte * len(code)).from_buffer_copy(code)
    host = api if api is not None else HostAPI()
    text = host.owned_string(
        "neverd_bytecode_recover_json_v1", buffer, len(code), request, len(request)
    )
    if text is None:
        raise NeverDError("bytecode recovery could not allocate its result")
    try:
        result = json.loads(text)
    except (ValueError, TypeError) as error:
        raise NeverDError("invalid bytecode recovery response") from error
    if (not isinstance(result, dict) or
            type(result.get("schemaVersion")) is not int or
            result.get("schemaVersion") != 1):
        raise NeverDError("unsupported bytecode recovery response")
    if result.get("ok") is not True:
        raise NeverDError(str(result.get("error", "bytecode recovery failed")))
    counts = ("functions", "blocks", "decoded_instructions", "decoded_bytes",
              "input_bytes")
    if any(type(result.get(key)) is not int or result[key] < 0 for key in counts):
        raise NeverDError("invalid bytecode recovery coverage")
    if (result.get("scope") not in ("cfg", "state-c") or
            not isinstance(result.get("source"), str) or
            result["input_bytes"] != len(code) or
            result["decoded_bytes"] > len(code) or
            result["scope"] != ("cfg" if output == "check" else "state-c") or
            (output == "check" and result["source"] != "")):
        raise NeverDError("invalid bytecode recovery source contract")
    return BytecodeRecoveryResult(
        result["functions"], result["blocks"], result["decoded_instructions"],
        result["decoded_bytes"], result["input_bytes"],
        cast(Literal["cfg", "state-c"], result["scope"]), result["source"],
    )


__all__ = ["BytecodeRecoveryResult", "recover_bytecode"]
