"""Run the standalone Windows allocator fixture from a fresh install prefix."""

import argparse
import os
import pathlib
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True)
    parser.add_argument("--config", required=True)
    parser.add_argument("--executable", required=True)
    parser.add_argument("--plugin", required=True)
    parser.add_argument("--enabled", choices=("ON", "OFF"), required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="neverd-allocator-install-") as temp:
        prefix = pathlib.Path(temp)
        subprocess.run(
            ["cmake", "--install", args.build_dir, "--config", args.config,
             "--prefix", str(prefix)], check=True, timeout=60,
        )
        runtime = prefix / "bin"
        if args.enabled == "ON":
            dlls = list(runtime.glob("mimalloc*.dll"))
            assert len(dlls) == 2, dlls
            assert sum("redirect" in dll.name for dll in dlls) == 1, dlls
            assert (prefix / "share/neverd/licenses/mimalloc/LICENSE").is_file()
        else:
            assert not list(runtime.glob("mimalloc*.dll"))
        # No upstream development package should leak into NeverD's install.
        assert not (prefix / "include/mimalloc.h").exists()
        environment = os.environ.copy()
        environment.pop("MIMALLOC_DISABLE_REDIRECT", None)
        environment.pop("NEVERD_TEST_EXPECT_SYSTEM_ALLOCATOR", None)
        subprocess.run(
            [str(runtime / args.executable), str(runtime / args.plugin)],
            cwd=runtime, env=environment, check=True, timeout=30,
        )


if __name__ == "__main__":
    main()
