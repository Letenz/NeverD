#!/usr/bin/env python3
"""Generate the binary-file ISA model: byte statistics per instruction set.

NeverD reads a file no header describes as code of the processor the user
names -- by default, the processor its bytes look like.  This script builds
the statistics that lookup reads, from two corpora:

* Code compiled for the model: Capstone's portable C and NeverD's own test
  programs, cross-compiled with clang for every ISA in ISAS at three
  optimization levels and linked with ld.lld (object files keep relocation
  placeholders that would skew the statistics).
* Code distributions ship: the executable sections of the Debian binary
  packages isa_model_corpus.json pins by hash, built with GCC for every
  Debian architecture.  They download from the URL listed, or by hash from
  snapshot.debian.org once the mirrors drop that version.  Rows marked
  "test" never train; they measure the model on packages it has not seen.

For each ISA the script counts which byte follows which and which byte each
position of a 4-byte word holds, and writes log P(next byte | byte) and
log P(byte | position) quantized to 16 levels to lib/loader/Raw/ISAModel.bin.
ISAModel.json records how: the compiler, the targets, the packages and the
corpus files with their hashes, and how windows of held-out code score.

Identifying instruction sets by byte statistics follows L. Granboulan,
"cpu_rec.py, un outil statistique pour la reconnaissance d'architectures
binaires exotiques" (SSTIC 2017); the model here is trained from scratch.

Requires clang with every listed target, ld.lld, ar and tar.
"""

from __future__ import annotations

import argparse
import collections
import concurrent.futures
import hashlib
import json
import math
import random
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import urllib.request
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
OUTPUT = REPO / "lib" / "loader" / "Raw"
PACKAGES = Path(__file__).resolve().parent / "isa_model_corpus.json"
ISA_TABLE = REPO / "include" / "neverd" / "loader" / "Raw" / "RawISA.def"

# Each ISA's clang targets: (target, flags).  The names are the ones
# RawISA.def describes; Thumb pools ARMv7-A and ARMv7-M code, and big-endian
# AArch64 is left out because AArch64 fetches instructions little-endian in
# either data order.  Alpha, PA-RISC and SuperH have no clang target; their
# code comes from Debian alone.
ISAS = [
    ("x86", [("i386-linux-gnu", [])]),
    ("x86_64", [("x86_64-linux-gnu", [])]),
    ("arm", [("armv7a-linux-gnueabihf", ["-marm"])]),
    ("thumb", [("armv7a-linux-gnueabihf", ["-mthumb"]), ("thumbv7m-none-eabi", [])]),
    ("aarch64", [("aarch64-linux-gnu", [])]),
    ("armeb", [("armebv7a-linux-gnueabi", ["-marm"])]),
    ("mips", [("mips-linux-gnu", [])]),
    ("mipsel", [("mipsel-linux-gnu", [])]),
    ("mips64", [("mips64-linux-gnuabi64", [])]),
    ("mips64el", [("mips64el-linux-gnuabi64", [])]),
    ("ppc", [("powerpc-linux-gnu", [])]),
    ("ppc64", [("powerpc64-linux-gnu", [])]),
    ("ppc64le", [("powerpc64le-linux-gnu", [])]),
    ("riscv32", [("riscv32-unknown-elf", ["-march=rv32imac"])]),
    ("riscv64", [("riscv64-linux-gnu", [])]),
    ("sparc", [("sparc-linux-gnu", [])]),
    ("sparcv9", [("sparcv9-linux-gnu", [])]),
    ("s390x", [("s390x-linux-gnu", [])]),
    ("m68k", [("m68k-linux-gnu", [])]),
    ("hexagon", [("hexagon-unknown-elf", [])]),
    ("loongarch64", [("loongarch64-linux-gnu", [])]),
    ("msp430", [("msp430-unknown-elf", [])]),
    ("avr", [("avr-unknown-unknown", ["-mmcu=atmega328p"])]),
    ("xtensa", [("xtensa-unknown-elf", [])]),
    ("bpf", [("bpfel-unknown-none", [])]),
    ("alpha", []),
    ("hppa", []),
    ("sh4", []),
]

# Debian architecture: (ISA, ELF machine, 64-bit class, big-endian).  Only
# ELF files of that machine, class and byte order count.
DEBIAN_ARCHES = {
    "amd64": ("x86_64", 62, True, False),
    "i386": ("x86", 3, False, False),
    "arm64": ("aarch64", 183, True, False),
    "armel": ("arm", 40, False, False),
    "armhf": ("thumb", 40, False, False),
    "mipsel": ("mipsel", 8, False, False),
    "mips64el": ("mips64el", 8, True, False),
    "mips": ("mips", 8, False, True),
    "ppc64el": ("ppc64le", 21, True, False),
    "powerpc": ("ppc", 20, False, True),
    "ppc64": ("ppc64", 21, True, True),
    "riscv64": ("riscv64", 243, True, False),
    "s390x": ("s390x", 22, True, True),
    "loong64": ("loongarch64", 258, True, False),
    "sparc64": ("sparcv9", 43, True, True),
    "m68k": ("m68k", 4, False, True),
    "alpha": ("alpha", 0x9026, True, False),
    "hppa": ("hppa", 15, False, True),
    "sh4": ("sh4", 42, False, False),
}
SNAPSHOT = "https://snapshot.debian.org/file/"

OPTIMIZATIONS = ["-O0", "-O2", "-Os"]
LEVELS = 16
POSITIONS = 4
SMOOTHING = 0.01
FLOOR = -20.0
WINDOW = 4096
# Windows of held-out code scored per input: evenly spread, enough to tell.
SCORED_WINDOWS = 64
MAGIC = b"NDISAMDL"
VERSION = 2

# Declarations the corpus includes; the code calls them, never runs them.
SHIM_HEADERS = {
    "stdio.h": """#include <stddef.h>
#include <stdarg.h>
typedef struct FILE FILE;
extern FILE *stdin, *stdout, *stderr;
#define EOF (-1)
int printf(const char *, ...); int fprintf(FILE *, const char *, ...);
int sprintf(char *, const char *, ...);
int snprintf(char *, size_t, const char *, ...);
int vsnprintf(char *, size_t, const char *, va_list);
int vfprintf(FILE *, const char *, va_list); int vprintf(const char *, va_list);
int puts(const char *); int putchar(int); int fputs(const char *, FILE *);
int fputc(int, FILE *); int getchar(void); int fgetc(FILE *);
char *fgets(char *, int, FILE *); FILE *fopen(const char *, const char *);
int fclose(FILE *); size_t fread(void *, size_t, size_t, FILE *);
size_t fwrite(const void *, size_t, size_t, FILE *); int fflush(FILE *);
int sscanf(const char *, const char *, ...); int scanf(const char *, ...);
int fseek(FILE *, long, int); long ftell(FILE *); void perror(const char *);
""",
    "string.h": """#include <stddef.h>
void *memcpy(void *, const void *, size_t); void *memmove(void *, const void *, size_t);
void *memset(void *, int, size_t); int memcmp(const void *, const void *, size_t);
void *memchr(const void *, int, size_t); size_t strlen(const char *);
size_t strnlen(const char *, size_t); char *strcpy(char *, const char *);
char *strncpy(char *, const char *, size_t); char *strcat(char *, const char *);
char *strncat(char *, const char *, size_t); int strcmp(const char *, const char *);
int strncmp(const char *, const char *, size_t);
int strcasecmp(const char *, const char *);
int strncasecmp(const char *, const char *, size_t); char *strchr(const char *, int);
char *strrchr(const char *, int); char *strstr(const char *, const char *);
char *strdup(const char *); char *strtok(char *, const char *);
size_t strspn(const char *, const char *); size_t strcspn(const char *, const char *);
char *strerror(int);
""",
    "stdlib.h": """#include <stddef.h>
void *malloc(size_t); void *calloc(size_t, size_t); void *realloc(void *, size_t);
void free(void *); void abort(void); void exit(int); int atoi(const char *);
long atol(const char *); long strtol(const char *, char **, int);
unsigned long strtoul(const char *, char **, int);
long long strtoll(const char *, char **, int);
unsigned long long strtoull(const char *, char **, int);
double strtod(const char *, char **); double atof(const char *); int abs(int);
long labs(long); int rand(void); void srand(unsigned);
void qsort(void *, size_t, size_t, int (*)(const void *, const void *));
void *bsearch(const void *, const void *, size_t, size_t,
              int (*)(const void *, const void *));
char *getenv(const char *);
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 2147483647
""",
    "ctype.h": """int isalpha(int); int isdigit(int); int isalnum(int); int isspace(int);
int isupper(int); int islower(int); int isxdigit(int); int isprint(int);
int ispunct(int); int iscntrl(int); int isgraph(int); int toupper(int); int tolower(int);
""",
    "assert.h": """void __corpus_assert(const char *, const char *, int);
#define assert(e) ((e) ? (void)0 : __corpus_assert(#e, __FILE__, __LINE__))
""",
    "errno.h": """extern int errno;
#define EINVAL 22
#define ENOMEM 12
#define ERANGE 34
""",
    "math.h": """double sqrt(double); double pow(double, double); double fabs(double);
double floor(double); double ceil(double); double sin(double); double cos(double);
double exp(double); double log(double); double fmod(double, double);
float sqrtf(float); float fabsf(float);
#define INFINITY (__builtin_inff())
#define NAN (__builtin_nanf(""))
""",
    "inttypes.h": """#include <stdint.h>
#define PRIx64 "llx"
#define PRIu64 "llu"
#define PRId64 "lld"
#define PRIx32 "x"
#define PRIu32 "u"
#define PRId32 "d"
#define PRIxPTR "lx"
""",
}


def corpus():
    """The C files, split by file into training and held-out sets."""
    capstone = REPO / "third_party" / "capstone"
    files = sorted(capstone.glob("*.c"))
    for arch in ("X86", "ARM", "AArch64", "Mips", "PowerPC", "RISCV", "Sparc",
                 "SystemZ", "M68K", "BPF", "LoongArch", "Xtensa"):
        files += sorted((capstone / "arch" / arch).glob("*.c"))
    files += sorted((REPO / "unittests").rglob("*.c"))
    random.Random(3389).shuffle(files)
    return files[:60], files[60:100]


def compile_one(job):
    clang, target, flags, opt, source, obj, shim = job
    if obj.exists():
        return True
    command = [clang, f"--target={target}", *flags, opt, "-ffreestanding",
               "-nostdlibinc", "-fno-stack-protector", "-fno-pic", "-w",
               "-DCAPSTONE_HAS_X86", "-DCAPSTONE_USE_SYS_DYN_MEM",
               "-I", str(REPO / "third_party" / "capstone" / "include"),
               "-I", str(REPO / "third_party" / "capstone"),
               "-isystem", str(shim), "-c", str(source), "-o", str(obj)]
    try:
        return subprocess.run(command, capture_output=True, timeout=600).returncode == 0
    except subprocess.TimeoutExpired:
        return False


def elf_code(data, machine=None):
    """An ELF file's executable sections as (file offset, bytes), any class or
    order; none unless the file is for machine (number, 64-bit, big-endian)."""
    if data[:4] != b"\x7fELF":
        return []
    wide = data[4] == 2
    order = "<" if data[5] == 1 else ">"
    try:
        if machine is not None:
            number, = struct.unpack_from(order + "H", data, 0x12)
            if (number, wide, order == ">") != machine:
                return []
        if wide:
            shoff, = struct.unpack_from(order + "Q", data, 0x28)
            entsize, count = struct.unpack_from(order + "HH", data, 0x3A)
        else:
            shoff, = struct.unpack_from(order + "I", data, 0x20)
            entsize, count = struct.unpack_from(order + "HH", data, 0x2E)
        layout = order + ("IIQQQQ" if wide else "IIIIII")
        sections = []
        for index in range(count):
            _, kind, flags, _, offset, size = struct.unpack_from(
                layout, data, shoff + index * entsize)
            if kind == 1 and flags & 0x4 and size:
                sections.append((offset, data[offset:offset + size]))
        return sections
    except struct.error:
        return []


def linked_code(lld, objects, image):
    """Code of the objects linked; their own code when the linker cannot."""
    if not objects:
        return []
    linked = subprocess.run(
        [lld, "--unresolved-symbols=ignore-all", "--allow-multiple-definition",
         "-e", "0", "--no-pie", "-o", str(image), *map(str, objects)],
        capture_output=True)
    if linked.returncode == 0 and image.exists():
        return elf_code(image.read_bytes())
    return [section for obj in objects for section in elf_code(obj.read_bytes())]


def fetch_package(row, directory):
    """The package a row pins, checked against its hash; a Debian package
    the mirrors dropped comes from snapshot.debian.org by its SHA-1."""
    path = directory / row["url"].rsplit("/", 1)[1]
    if path.exists() and hashlib.sha256(path.read_bytes()).hexdigest() == row["sha256"]:
        return path
    for url in [row["url"]] + ([SNAPSHOT + row["sha1"]] if "sha1" in row else []):
        try:
            with urllib.request.urlopen(url, timeout=300) as reply:
                blob = reply.read()
        except OSError:
            continue
        if hashlib.sha256(blob).hexdigest() == row["sha256"]:
            path.write_bytes(blob)
            return path
    sys.exit(f"cannot fetch {row['url']} with its pinned hash")


def package_code(row, work):
    """Code of the package's ELF files for its architecture."""
    directory = work / "debian" / row["arch"] / row["url"].rsplit("/", 1)[1].split("_")[0]
    directory.mkdir(parents=True, exist_ok=True)
    deb = fetch_package(row, directory)
    tree = directory / "tree"
    if not tree.exists():
        subprocess.run(["ar", "x", str(deb)], cwd=directory, check=True)
        tree.mkdir()
        subprocess.run(["tar", "-xf", str(next(directory.glob("data.tar.*"))), "-C", str(tree)],
                       check=True)
    _, number, wide, big = DEBIAN_ARCHES[row["arch"]]
    sections, seen = [], set()
    for path in sorted(tree.rglob("*")):
        if path.is_symlink() or not path.is_file():
            continue
        with path.open("rb") as handle:
            if handle.read(4) != b"\x7fELF":
                continue
        data = path.read_bytes()
        digest = hashlib.sha256(data).digest()
        if digest not in seen:
            seen.add(digest)
            sections += elf_code(data, (number, wide, big))
    return sections


def pair_counts(sections):
    """How often each byte follows each in the sections."""
    counts = [0] * 65536
    little = sys.byteorder == "little"
    for _, code in sections:
        for start in (0, 1):
            end = start + (len(code) - start) // 2 * 2
            for value, count in collections.Counter(memoryview(code[start:end]).cast("H")).items():
                first, second = (value & 0xFF, value >> 8) if little else (value >> 8, value & 0xFF)
                counts[first << 8 | second] += count
    return counts


def position_counts(sections):
    """How often each byte sits at each position of an aligned 4-byte word."""
    counts = [0] * (POSITIONS * 256)
    for offset, code in sections:
        for start in range(POSITIONS):
            row = (offset + start) % POSITIONS * 256
            for value, count in collections.Counter(code[start::POSITIONS]).items():
                counts[row + value] += count
    return counts


def quantized(counts, rows):
    """log P(column | row) for rows of 256 columns, quantized: (levels, low, step)."""
    table = []
    for row in range(rows):
        cells = counts[row * 256:row * 256 + 256]
        total = sum(cells) + SMOOTHING * 256
        table += [math.log((c + SMOOTHING) / total) for c in cells]
    low = max(min(table), FLOOR)
    step = -low / (LEVELS - 1)
    return [min(LEVELS - 1, max(0, round((v - low) / step))) for v in table], low, step


def families():
    """ISA name -> family, as RawISA.def groups them."""
    text = ISA_TABLE.read_text(encoding="utf-8")
    return {m.group(1): m.group(2) for m in re.finditer(
        r'NEVERD_RAW_ISA\(\s*"([^"]+)",\s*"[^"]*",\s*"[^"]*",\s*"([^"]+)"\s*\)', text)}


def window_accuracy(models, family, inputs):
    """Top-1 ISA and family of evenly spread windows of each held-out input."""
    names = list(models)
    tables = [[low + level * step for level in levels]
              for (levels, low, step), _ in models.values()]
    accuracy = {}
    for label, (name, code) in inputs.items():
        windows = len(code) // WINDOW
        scored = min(windows, SCORED_WINDOWS)
        right = {"isa": 0, "family": 0}
        for index in sorted({i * windows // scored for i in range(scored)}):
            window = code[index * WINDOW:(index + 1) * WINDOW]
            pairs = collections.Counter(zip(window, window[1:])).items()
            scores = [sum(table[a << 8 | b] * c for (a, b), c in pairs) for table in tables]
            best = names[scores.index(max(scores))]
            right["isa"] += best == name
            right["family"] += family[best] == family[name]
        accuracy[label] = {"windows": scored, **right}
    return accuracy


def packed(levels):
    return bytes(levels[i] | levels[i + 1] << 4 for i in range(0, len(levels), 2))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--clang", default=shutil.which("clang"))
    parser.add_argument("--lld", default=shutil.which("ld.lld"))
    parser.add_argument("--jobs", type=int, default=8)
    parser.add_argument("--work", type=Path,
                        help="Keep and reuse the corpus objects and packages here")
    args = parser.parse_args()
    if not args.clang or not args.lld:
        sys.exit("needs clang and ld.lld")
    family = families()
    if missing := [name for name, _ in ISAS if name not in family]:
        sys.exit(f"RawISA.def does not describe {', '.join(missing)}")
    work = (args.work or Path(tempfile.mkdtemp(prefix="neverd-isa-model-"))).resolve()
    shim = work / "shim"
    shim.mkdir(parents=True, exist_ok=True)
    for name, text in SHIM_HEADERS.items():
        (shim / name).write_text(text, encoding="ascii")

    train, held_out = corpus()
    jobs, objects = [], {}
    for split, files, opts in (("train", train, OPTIMIZATIONS), ("held-out", held_out, ["-O2"])):
        for name, targets in ISAS:
            for target, flags in targets:
                for opt in opts:
                    directory = work / split / name / target / opt.lstrip("-")
                    directory.mkdir(parents=True, exist_ok=True)
                    for source in files:
                        obj = directory / f"{source.parent.name}_{source.stem}.o"
                        jobs.append((args.clang, target, flags, opt, source, obj, shim))
                        objects.setdefault((split, name, directory), []).append(obj)
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        built = list(pool.map(compile_one, jobs))
    print(f"compiled {sum(built)} of {len(built)} corpus objects", flush=True)

    sections = collections.defaultdict(list)
    for (split, name, directory), objs in objects.items():
        present = [obj for obj in objs if obj.exists()]
        sections[(split, name)] += linked_code(args.lld, present, directory / "linked.elf")

    packages = json.loads(PACKAGES.read_text(encoding="utf-8"))
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        found = list(pool.map(lambda row: package_code(row, work), packages))
    tests = {}
    for row, code in zip(packages, found):
        name = DEBIAN_ARCHES[row["arch"]][0]
        if row["role"] == "train":
            sections[("debian", name)] += code
        else:
            package = row["url"].rsplit("/", 1)[1].split("_")[0]
            tests[f"debian {row['arch']} {package}"] = (name, b"".join(c for _, c in code))
    print(f"read {len(packages)} Debian packages", flush=True)

    models, trained = {}, {}
    for name, _ in ISAS:
        training = sections[("train", name)] + sections[("debian", name)]
        trained[name] = {source: sum(len(code) for _, code in sections[(source, name)])
                         for source in ("train", "debian")}
        if sum(trained[name].values()) < WINDOW * 64:
            sys.exit(f"{name}: too little training code")
        models[name] = (quantized(pair_counts(training), 256),
                        quantized(position_counts(training), POSITIONS))
        print(f"{name}: {trained[name]['train']} bytes compiled, "
              f"{trained[name]['debian']} bytes from Debian", flush=True)

    held = {f"clang {name}": (name, b"".join(code for _, code in sections[("held-out", name)]))
            for name, _ in ISAS if sections[("held-out", name)]}
    held.update(tests)
    accuracy = window_accuracy(models, family, held)
    for label, row in accuracy.items():
        print(f"{label}: {row['isa']}/{row['windows']} windows, family {row['family']}", flush=True)

    blob = bytearray(MAGIC)
    blob += struct.pack("<IIII", VERSION, len(models), LEVELS, POSITIONS)
    for name, ((_, low, step), (_, position_low, position_step)) in models.items():
        blob += name.encode("ascii").ljust(16, b"\0")
        blob += struct.pack("<ffff", low, step, position_low, position_step)
    for (levels, _, _), (positions, _, _) in models.values():
        blob += packed(levels) + packed(positions)
    (OUTPUT / "ISAModel.bin").write_bytes(bytes(blob))

    version = subprocess.run([args.clang, "--version"], capture_output=True,
                             text=True).stdout.splitlines()[0]
    provenance = {
        "generator": "scripts/generate_isa_model.py",
        "compiler": version,
        "optimizations": OPTIMIZATIONS,
        "isas": {name: [target for target, _ in targets] for name, targets in ISAS},
        "training_bytes": {name: {"compiled": t["train"], "debian": t["debian"]}
                           for name, t in trained.items()},
        "corpus": {
            split: {str(f.relative_to(REPO)): hashlib.sha256(f.read_bytes()).hexdigest()
                    for f in files}
            for split, files in (("train", train), ("held_out", held_out))},
        "packages": {"list": str(PACKAGES.relative_to(REPO)),
                     "sha256": hashlib.sha256(PACKAGES.read_bytes()).hexdigest()},
        "held_out_window_accuracy": accuracy,
    }
    (OUTPUT / "ISAModel.json").write_text(json.dumps(provenance, indent=1, sort_keys=True) + "\n",
                                          encoding="utf-8")
    print(f"wrote {len(blob)} bytes for {len(models)} ISAs", flush=True)


if __name__ == "__main__":
    main()
