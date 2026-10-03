"""Static rules or a contextual callback for an independently constructed language."""

from neverd_plugin import (
    Plugin, PluginType, Session, recover_bytecode, recover_bytecode_with_decoder,
)


@Plugin(
    name="External Bytecode Python", version="1.0.0",
    author="NeverD contributors", type=PluginType.PROCESSOR,
    description="Recovers an externally specified instruction language",
)
class ExternalBytecode:
    def on_run(self, session: Session, arg: int) -> int:
        profile = {
            "version": 1, "register_bytes": 8, "byte_order": "little",
            "encodings": [{"size": 1, "match": [{"offset": 0, "value": 109}],
                           "operations": [{"op": "RETURN", "inputs": []}]}],
        }
        functions = [{"entry": 0, "end": 1, "name": "plugin_return"}]
        if arg & 2:
            key = 37

            def decode(data: bytes, pc: int) -> dict:
                if data[0] ^ ((pc + key) & 255) != 0x6d:
                    raise ValueError("unrecognized example instruction")
                return {"size": 1, "operations": [{"op": "RETURN", "inputs": []}]}

            layout = {name: value for name, value in profile.items() if name != "encodings"}
            result = recover_bytecode_with_decoder(
                bytes([0x6d ^ key]), layout, functions, decode,
                output="llvmc" if arg & 1 else "highc", optimize=bool(arg & 1),
            )
        else:
            result = recover_bytecode(
                b"\x6d", profile, functions,
                output="llvmc" if arg & 1 else "highc", optimize=bool(arg & 1),
            )
        print(result.source)
        return 0 if result.functions == 1 and result.decoded_bytes == 1 else 1
