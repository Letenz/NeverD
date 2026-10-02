"""A tiny external language; replace the rules with your own decoder profile."""

from neverd_plugin import Plugin, PluginType, Session, recover_bytecode


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
        result = recover_bytecode(
            b"\x6d", profile, [{"entry": 0, "end": 1, "name": "plugin_return"}],
            output="llvmc" if arg else "highc", optimize=bool(arg),
        )
        print(result.source)
        return 0 if result.functions == 1 and result.decoded_bytes == 1 else 1
