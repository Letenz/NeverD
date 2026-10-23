"""Exercise the Linux deployment collector with small real ELF dependencies."""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


COLLECTOR = Path(__file__).resolve().parents[3] / "cmake/NeverDDesktopRuntime.cmake"


@unittest.skipUnless(sys.platform.startswith("linux") and all(
    shutil.which(tool) for tool in ("cc", "cmake", "patchelf")), "Linux deployment tools required")
class DesktopRuntimeTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="neverd-deployment-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.prefix = self.root / "package"
        self.external = self.root / "external"
        self.sql = self.prefix / "plugins/sqldrivers"
        for directory in (self.prefix / "bin", self.prefix / "lib", self.external, self.sql):
            directory.mkdir(parents=True, exist_ok=True)
        for name in ("neverd-gui", "neverd-worker", "neverd-mcp"):
            shutil.copy2(shutil.which("true"), self.prefix / "bin" / name)
        self.compile("int desktopDependency(void) { return 42; }",
                     self.external / "libdesktopdependency.so")
        self.compile("extern int desktopDependency(void); "
                     "int desktopPlugin(void) { return desktopDependency(); }",
                     self.sql / "libqsqlite.so", "-L" + str(self.external),
                     "-ldesktopdependency", "-Wl,-rpath," + str(self.external))
        self.optional = self.sql / "libqsqlpsql.so"
        self.optional.write_text("An unused SQL connector is removed before dependency discovery.")
        self.original = (self.external / "libdesktopdependency.so").read_bytes()

    def compile(self, source, output, *arguments):
        subprocess.run(["cc", "-shared", "-fPIC", "-x", "c", "-", "-o", str(output),
                        *arguments], input=source, text=True, check=True, capture_output=True)

    def collect(self, runtime_library=None):
        script = self.root / "deploy.cmake"
        script.write_text(f'''cmake_minimum_required(VERSION 3.24)
set(QT_DEPLOY_PREFIX "{self.prefix}")
set(QT_DEPLOY_LIB_DIR lib)
set(QT_DEPLOY_PLUGINS_DIR plugins)
set(NEVERD_DEPLOY_SYSTEM Linux)
set(NEVERD_DEPLOY_CONFIG Release)
set(NEVERD_DEPLOY_SEARCH_DIRS "{self.external}")
set(NEVERD_DEPLOY_RUNTIME_LIBRARIES "{runtime_library or ''}")
include("{COLLECTOR}")
''')
        environment = {key: value for key, value in os.environ.items()
                       if key != "LD_LIBRARY_PATH"}
        return subprocess.run(["cmake", "-P", str(script)], env=environment,
                              text=True, capture_output=True, timeout=30)

    def test_transitive_dependency_survives_relocation(self):
        result = self.collect()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse(self.optional.exists())
        self.assertEqual((self.external / "libdesktopdependency.so").read_bytes(), self.original)
        moved = self.root / "relocated"
        self.prefix.rename(moved)
        shutil.rmtree(self.external)
        plugin = moved / "plugins/sqldrivers/libqsqlite.so"
        subprocess.run([sys.executable, "-c",
                        "import ctypes, sys; assert ctypes.CDLL(sys.argv[1]).desktopPlugin() == 42",
                        str(plugin)], check=True)
        audit = (moved / "share/neverd/dependency-audit.txt").read_text()
        digest = hashlib.sha256(plugin.read_bytes()).hexdigest()
        self.assertIn(digest + "  plugins/sqldrivers/libqsqlite.so", audit)

    def test_missing_sqlite_preserves_optional_drivers(self):
        (self.sql / "libqsqlite.so").unlink()
        self.assertNotEqual(self.collect().returncode, 0)
        self.assertTrue(self.optional.exists())

    def test_explicit_runtime_library_is_copied_and_audited(self):
        self.assertEqual(self.collect().returncode, 0)
        runtime = self.external / "libdesktopoptional.so"
        self.compile("extern int desktopDependency(void); "
                     "int optionalFeature(void) { return desktopDependency(); }",
                     runtime, "-L" + str(self.external), "-ldesktopdependency",
                     "-Wl,-rpath," + str(self.external))
        result = self.collect(runtime)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue((self.prefix / "lib" / runtime.name).is_file())
        audit = (self.prefix / "share/neverd/dependency-audit.txt").read_text()
        self.assertIn("lib/libdesktopoptional.so", audit)

    def test_unknown_driver_preserves_optional_drivers(self):
        (self.sql / "unknown.so").write_text("unknown")
        self.assertNotEqual(self.collect().returncode, 0)
        self.assertTrue(self.optional.exists())

    def test_sql_symlink_is_rejected_before_removal(self):
        self.optional.unlink()
        self.optional.symlink_to(self.external / "libdesktopdependency.so")
        self.assertNotEqual(self.collect().returncode, 0)
        self.assertTrue(self.optional.is_symlink())

    def test_escaping_library_is_never_patched(self):
        (self.prefix / "lib/libescape.so").symlink_to(self.external / "libdesktopdependency.so")
        result = self.collect()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("outside the package", result.stderr)
        self.assertEqual((self.external / "libdesktopdependency.so").read_bytes(), self.original)

    def test_unresolved_dependency_stops_packaging(self):
        (self.external / "libdesktopdependency.so").unlink()
        result = self.collect()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("libdesktopdependency.so", result.stderr)


if __name__ == "__main__":
    unittest.main()
