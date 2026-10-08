# NeverD desktop workbench

The desktop workbench is a Qt Widgets application with a separate
`neverd-worker` process. It starts in the classic interactive disassembler
layout and keeps that layout's window names, menus and default shortcuts, so
existing muscle memory carries over; colors follow the Visual Studio Code Dark+
(default) and Light+ themes, and every icon is original NeverD artwork. The
worker links the same `libneverd` shared library as the CLI through its public C
ABI: analysis, function discovery and decompilation stay in that library, and Qt
owns presentation and request coordination. A crash or a long engine call in the
worker never blocks or takes down the window. The GUI executable does not link
LLVM or the CLI, and no model service is needed to browse binaries.

## Build

The normal engine/CLI configuration is unchanged. Qt is optional and is searched
only when `NEVERD_BUILD_GUI=ON`. To add the workbench to an engine build, use:

```sh
cmake -S . -B build -DNEVERD_BUILD_GUI=ON -DCMAKE_PREFIX_PATH=/path/to/Qt/6.8.3/platform
cmake --build build --target neverd-gui
```

For fast desktop development, build against a matching existing shared engine:

```sh
cmake -S tools/neverd-gui -B build-gui -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/path/to/Qt/6.8.3/platform \
  -DNEVERD_ENGINE_LIBRARY=/path/to/libneverd.so
cmake --build build-gui
ctest --test-dir build-gui --output-on-failure
build-gui/bin/neverd-gui /absolute/path/to/binary
```

Requires C++20, CMake 3.24+, Qt 6.8+ Core, Gui, Widgets, Network, Svg, Sql
(with its SQLite driver), Concurrent and LinguistTools, the GuiPrivate and
WidgetsPrivate headers for the pinned KDDockWidgets frontend (Qt Test for
tests), and Python 3.10+ for tests and the MCP adapter. The first configure
downloads nlohmann/json 3.11.3 and KDDockWidgets 2.4.1 using pinned SHA-256
digests. Ship the exact Qt build used to compile its private headers. On Windows
set `NEVERD_ENGINE_LIBRARY` to the runtime DLL and `NEVERD_ENGINE_IMPLIB` to its
matching import `.lib`; make runtime dependencies available alongside the
worker.

The worker can also be built without Qt, either with `NEVERD_BUILD_WORKER=ON` in
the root build or by configuring `tools/neverd-worker` standalone. The shipped
worker never links the test engine.

## Layout

The first start shows the quick-start dialog (New, Go, Previous and the recent
files) over the default desktop:

- the navigation band across the top: the whole address space colored by
  library functions, regular functions, instructions, data, unexplored bytes
  and external symbols, with the current position; click or drag to navigate;
- **Functions** on the left, paged from the worker so a million functions cost
  only their visible rows; import thunks and recognized library functions are
  tinted;
- **NeverD View-A** in the center with **Hex View-1**, **Imports** and
  **Exports** as tabs, and the status line under the listing;
- **Output** across the bottom, with a command line for expressions and
  workbench commands;
- the status bar with the background analysis indicator (`AU: idle` or busy
  with progress), the search direction and free disk space.

Every window is a dock: drag tabs to split, stack or float them. **Windows →
Save desktop** remembers an arrangement and **Reset desktop** returns to the
default. **Graph overview** appears under the function list in graph view, and
the pseudocode and IR windows open beside the disassembly.

## Views

**Disassembly** is the whole image as one address-ordered listing in the classic
format: segment headers, function headers with their attributes and stack
variables, unwind frame markers, `proc`/`endp`, `loc_` and `locret_` labels,
data items, alignment and cross-reference comments, and the classic instruction
spellings (`jz`, `retn`, `[rbp+var_30]`, sizes only where no operand implies
them), with `segment:address` prefixes and optional opcode bytes (**Options →
General**). Stack variables are named through the frame pointer and, in 64-bit
code, through the stack pointer wherever every path agrees on its distance
from the frame (`[rsp+48h+var_30]`); each is as wide as its widest access.
A named object is as large as its symbol says (`stderr dq ?`), and data the
code reads or writes is laid out as wide as those accesses, under the name its
operands and the pseudocode use (`qword_A410 dq ?`, or `unk_` where the widths
differ); other unwritten bytes run as `db N dup(?)` up to the next item. Only a window of lines around the viewport is held; the scroll bar
maps to the linear address space. The arrow gutter draws branches, and clicking
an identifier highlights every occurrence. Names the engine leaves generic take
their classic forms in the listing, function list and jumps: import thunks after
their import, the entry point `start`, and `main` as passed to the C runtime's
start routine.

**Graph view** (Space) lays the current function out in layers with the
conditional (green/red) and unconditional edges routed around blocks; panning
and zooming are local, and text is dropped at low zoom. The graph overview
shows the whole function and the visible area.

**Pseudocode** (F5, Tab) and **IR** windows show C, C through LLVM, LowIR,
MedIR, HighIR or LLVM IR of the current function and follow the disassembly
unless their lock is set. The LLVM views translate the current function alone,
with the others declared, so a function the engine refuses to translate shows
its reason without affecting other functions. Rows mapped to instructions move
the disassembly cursor. C opens at the function: the includes, support types and
declarations before its definition fold into one line. Recognized library
operations in C can fold into one-line summaries too. Click a summary or press
Keypad + on it to expand it; Keypad - folds the declarations again, and the
context menu expands or collapses either kind. Hovering a type or macro the code
declares, such as an unaligned access type, shows its declaration, and
double-clicking it goes there. Double-clicking a function's name shows that
function's pseudocode in place; an import's name opens its thunk, or its slot
when it has none, in the disassembly, as do other names of data. Memory reads
and writes print through the standard scalar types, `*(uint64_t *)p`. That C
reads scalars at any address and through any type, so it is built for a target
that allows unaligned access and with `-fno-strict-aliasing`, as its prelude
notes; recovered bytecode and devirtualized sources, which are compiled to run,
declare one-byte-aligned, `may_alias` types instead. A call argument that points
to a string, directly or through a pointer the image holds, shows the string
beside it, after its encoding unless that is ASCII or UTF-8:
`puts(u8s /* "你好" */)`, `/* GBK "中文" */`. Copy and export always use the
complete code.

Global data is declared the way C source declares it, with the value the image
holds: a string the code reaches by its address is its array
(`const char gbk[] = "\xD6\xD0\xCE\xC4"; /* GBK "中文" */`), a pointer the code
only reads that holds a read-only string's address is that string's pointer
(`const char *u8s = "你好";`, `const char16_t *w = u"宽字";`), and a scalar
shows its initial value (`int32_t counter = 5;`). Every initializer spells the
image's bytes exactly: text another encoding than UTF-8 holds is escaped, with
the decoded text in a comment beside it. Unnamed data is named as the listing
names it, so a name in the pseudocode matches the disassembly operand and
double-clicking it goes there: `off_3FC0` for a slot holding a pointer or one
the code calls through, the size of its accesses otherwise (`qword_3FB8`,
`dword_4010`), and `unk_` for data reached by its address alone. The command
line's `neverd decompile` prints the same declarations.

A function's address reads as the function: `_start` passes `main`, not
`0x1169`. A function the output defines is declared before the code that
names it, and one it does not define is declared as a callee is. A routine no
standard header declares is declared with the prototype the C library tables
give it: the start-up and exit routines (`__libc_start_main`, `__cxa_atexit`,
`__cxa_finalize`), the errno and ctype accessors (`__errno_location`,
`__ctype_b_loc`), the Itanium C++ runtime and unwinder (`__cxa_throw`,
`__cxa_begin_catch`, `_Unwind_Resume`), glibc's fortified and ISO C routines
(`__printf_chk`, `__isoc23_sscanf`) and, in a PE image, the Windows C
runtime's (`__getmainargs`, `_initterm`, `__stdio_common_vfprintf`). Each
argument converts to its parameter type as a disassembler's decompiler shows
it: `__libc_start_main((int (*)(int, char **, char **))main, argc, (char
**)argv, 0, 0, (void (*)(void))rtld_fini, (void *)stack_end)`. A pointer such
a routine returns keeps the integer the machine reads,
`(uintptr_t)__errno_location()`. A function passed to a standard function
converts to the type its header declares: `qsort(base, n, 4, (int (*)(const
void *, const void *))compare)`.

C++ names read as a classic disassembler shows them: the listing keeps the
linkage name an instruction uses and adds its demangled form as a comment
(`call _ZN8QDomNodeC1Ev ; QDomNode::QDomNode()`), a function with a mangled
name has the demangled one above its header, and the Functions window lists
every function demangled, PLT entries included, with the filter matching
either spelling. Itanium, Microsoft, Rust and D names are demangled.

C has no `::`, so in C pseudocode a C++ function reads by its scopes joined
with underscores: `QDomNode_nodeType`, constructors and destructors as
`QDomNode_ctor` and `QDomNode_dtor`, operators by name (`QString_assign`).
Its complete demangled signature is a comment above its definition, overloads
sharing a name are numbered (`QDomNodeList_ctor_2`), and an imported C++
function keeps its mangled symbol in an `__asm__` label so that the code still
links. Other names keep every byte of their symbol (`__libc_start_main`), apart
from the underscore Mach-O and 32-bit Windows add to C names and the start-up
functions the C runtime defines itself (`_start` reads `start`).

Strings are found by default in ASCII, UTF-8, UTF-16LE and UTF-32LE (the
`wchar_t` of Linux and macOS), and C strings that are not UTF-8 in the common
code pages: windows-1252, GBK, Big5, Shift-JIS and EUC-KR, all in one pass.
UTF-8 text such as Chinese, Japanese or Korean prints as itself
(`db '中文字符串',0`), each wide character two columns wide so the comments
after it line up, a string in another encoding under its label as
`text "Shift-JIS", '日本語',0`, and an instruction that refers to a string
quotes it in a comment (`; "Usage: %s"`). A code page's string is shown when
one of them reads it as text in a script it is made for and no code page for
another script reads it too: the Big5 bytes of 中文檔, which GBK reads as kana,
stay Big5, and KOI8-R text that GBK would read as ideographs is left alone.
**Options → String literals** chooses the encodings searched, including
UTF-16BE and UTF-32BE; whether the common code pages are detected; a preferred
code page (GBK, Big5, Shift-JIS, EUC-KR, Windows-1250 to 1258, ISO-8859, KOI8
or IBM866), which reads C strings first and wins where other code pages read
them too; and the minimum length in display columns, a wide East Asian
character counting two. Text mostly beyond ASCII also needs three characters,
since two random code points read as text too often. The Strings window shows
each string's encoding in its Type column. Code pages decode by the WHATWG
Encoding Standard, and a string reads as text only when its characters keep to
one script. Settings from versions that searched one code page keep it as the
preferred one.

**Search → String references** (Ctrl+Shift+F12, or `strref [text]` on the
command line) lists every instruction that refers to a string: directly, into
the middle of one (the text from that character on), or by reading a pointer
slot that holds its address. The list opens with its filter ready; the filter
matches the text, the function or the address, and every column but
Disassembly sorts. Enter goes to the instruction. In this list and in
Strings, Ctrl+X lists the references to the selected string.

**Hex View-1** follows the disassembly cursor; while it is the active view, a
jump (G or `g` on the command line) moves it and keeps it in front. Its
context menu's **Text encoding** reads the text column as ASCII, UTF-8,
UTF-16, UTF-32 or a code page; a character shows at its first byte, and wide
characters take two columns. The choice is kept for later sessions.
**Imports**, **Exports**, **Names**, **Strings**, **Segments** and
**Bookmarks** are choosers with a quick filter and sortable columns. **Jump
anywhere** (G) takes an address, a name or an expression such as `main+0x10`
and suggests names as you type. Cross references (X, Ctrl+X, Ctrl+J) list call
(`p`), jump (`j`), read (`r`), write (`w`) and offset (`o`) references from the
worker's reference index.

The output window's command line evaluates expressions in hexadecimal by
default (`#10` is decimal) and runs `g`, `x`, `n`, `c`, `d`, `f`, `graph`,
`hex`, `strref`, `analyze` and `save`; `help` lists them. **Options → Show command palette**
(Ctrl+Shift+P) searches every command.

## Keyboard

| Key | Action |
| --- | --- |
| G | Jump to an address, name or expression |
| Esc / Ctrl+Enter | Previous / next position (mouse Back/Forward work too) |
| Enter / Alt+Enter | Follow the operand / follow it in a new view |
| Space | Toggle graph and text view |
| F5 / Tab | Pseudocode / switch between disassembly and pseudocode |
| X / Ctrl+X / Ctrl+J | References to the operand / to the item / from the item |
| N | Rename the name under the cursor, or the address: a function at its entry, data, a label in code |
| P | Create a function at the address (**Edit → Functions** also deletes the current one) |
| D / A / U | Make data (again for the next size) / a string / bytes of the item |
| : or ; | Comment the address |
| Alt+M / Ctrl+M | Mark a position / jump to a marked position |
| Ctrl+P / Ctrl+L / Ctrl+S / Ctrl+E | Choose a function / name / segment / entry point |
| Alt+T, Ctrl+T / Alt+B, Ctrl+B | Search text / bytes, and repeat |
| Alt+Up / Alt+Down | Previous / next occurrence of the highlighted identifier |
| Ctrl+Shift+Up / Ctrl+Shift+Down | Previous / next function |
| Shift+F3, Shift+F4, Shift+F7, Shift+F12 | Functions, Names, Segments, Strings |
| Ctrl+Shift+F12 | String references |
| F6 / Shift+F6 | Next / previous window |
| Ctrl+W | Save the database |
| Ctrl+Shift+P | Command palette |

Single-key shortcuts apply only while an analysis view has the keyboard focus,
so typing in a field or dialog keeps normal text editing. **Options →
Shortcuts** lists every command with its key.

**Edit → Copy** (Ctrl+C) copies from the window that has the keyboard focus:
the selected lines of a disassembly, pseudocode or IR window (the current line
when nothing is selected), the selected rows of a list with their columns
separated by tabs, or the text selected in a field.

GNOME attaches a modal dialog to its parent window, so dragging the dialog
would drag the whole workbench. On GNOME the workbench keeps dialogs
free-standing: under X11 a modal dialog takes the utility window type, and
under Wayland the compositor is not told which dialogs are modal
(`xdg-dialog-v1`). A modal dialog still blocks the workbench until it closes.

## Databases

**File → Save** (Ctrl+W) packs the project into a NeverD database next to the
input: `ls` saves to `ls.nddb`. A database is one SQLite file holding the input
itself, its comments, renames, function edits and edit history, and the
workbench state (location, graph mode, bookmarks and desktop). Each save is a
single transaction, so a database is never left half written; the input is
stored in independently compressed chunks that compress and expand in parallel,
and an unchanged input is not rewritten.

Open a `.nddb` file directly to continue a project anywhere, even without the
original binary: the input is unpacked into a per-database working directory
and checked against its SHA-256 digest, and a damaged database is reported
instead of loaded. Opening a binary whose `.nddb` sits beside it restores the
saved location, bookmarks and desktop; when its comment files are missing they
are restored from the database, and a database that describes a different
version of the file is reported and left unused until the next save replaces
it. Closing the window updates the saved location of an existing database.

Every database carries the SQLite application id `NDDB` (`0x4E444442`) in its
header, so a renamed database still opens as a project and `file` tells it from
other SQLite files (`application id 1313096770`). Another application's SQLite
file is never read or written, even when it is named `.nddb`; it is reported as
not a NeverD database.

## Edits, history and analysis

Comments are staged and saved explicitly; renames, function edits and data
items commit at once (staged comments are saved first). Opening another file, restarting the
worker or quitting with unsaved comments offers Save, Discard and Cancel. Edits
prepared for a session that has since closed are refused, never applied to the
new one. Annotation, rename, function edit and data item commands have bounded undo/redo
history bound to the input hash and sidecar contents; a write-ahead journal
recovers an interrupted save, and foreign edits disable replay instead of
silently applying commands to another state. One worker owns a writable input
through an operating-system advisory lock.

**Edit → Functions → Create function** (P) starts a function at the cursor,
inside another function or in code nothing reaches; **Delete function** stops
treating the current function as one. The last edit at an address decides over
symbols, the function detector and analysis, and the edits are kept in
`<input>.neverd-functions.json`, which the command line reads too (`neverd
function-edits <input> --create <address>`, `--delete <address>`, `--list`).
A function edit drops whole-program analysis results: analysis continues
function by function until **Analyze** runs again.

**Edit → Rename** (N) names any address, as the listing and the pseudocode
show it: a name under the cursor renames what it denotes, otherwise the item
the cursor is on. A data name replaces `qword_A410` in its label, every operand
(`mov rdx, cs:pname`) and the C (`fprintf(stderr, "%s: %s\n", pname, msg)`). A
name has no spaces, leads to one address and is never an automatic name such
as `sub_1234`. Names are kept in `<input>.neverd-renames.json`, which the
command line reads and writes too (`neverd rename <input> --addr <address>
--to <name>`, `--clear`).

**Edit → Data** (D) makes the item under the cursor a value, and pressing it
again cycles the value through byte, word, dword and qword; **Edit → String**
(A) makes the string that starts there an item, read as the string scan reads
one; **Edit → Undefine** (U) shows the item's bytes as bytes, whatever
analysis reads in them. D or A inside undefined bytes takes just the bytes the
new item needs and leaves the rest undefined, and each press is one step of
undo history. Code belongs to its function and is never made data. The items
are kept in `<input>.neverd-items.json`, which the command line reads and
writes too (`neverd items <input> --data <address> --size 4`, `--string
<address>`, `--undefine <address> --size <n>`, `--clear <address>`).

Opening a file never starts whole-program analysis. The listing, function list,
references and graph come from the loader and from per-function work: a
decompile, graph or IR request analyzes only the function it names. While the
window is idle the worker first lets the engine add the functions its detector
finds from the image alone (call targets, prologues and format tables), so
binaries without unwind tables list their functions too, and then builds the
reference index in parallel across all cores; references and labels appear when
it finishes, and an explicit cross-reference request completes it at once.
**Options → Analysis → Whole-program analysis** runs the full pipeline when
wanted, and the listing then shows the switch jump tables it recovered as a
classic disassembler does: the table under its `jpt_` name with one slot per
line (`dd offset loc_164C0 - 27444h`, or `dq offset`/`dd rva` for absolute and
image-relative tables), `switch 54 cases` on the instruction that loads it,
`switch jump` on the dispatch, and on each target a code reference from the
dispatch and a data reference from the table. A table is laid out only when
the engine has checked that every slot holds its target. Cancel removes
queued work; a running engine call finishes unless the worker is restarted.

## Languages, extensions and MCP

The UI starts in English and bundles all 11 project languages; **Options →
Language** switches immediately without reloading the analysis, and Arabic
mirrors the window chrome while code and addresses stay left to right.

**View → Open subviews → Extensions** imports versioned JSON manifests with namespaced
read-only query contributions and runs them on the current address. Manifests
cannot run scripts or register arbitrary engine operations. See the manifest
schema in the [worker protocol](../tools/neverd-worker/PROTOCOL.md).

**View → Open subviews → MCP connections** starts connections only on
request. It supports local
stdio programs with an explicit argument list and Streamable HTTP with TLS
verification, optional bearer credentials and a custom CA file. Tool schemas,
arguments and results are inspectable, and the call history keeps parameters,
results and cancellation states. MCP is an interoperability client, not a model
provider. The standalone, Qt-free adapter uses MCP **2025-11-25** newline
JSON-RPC:

```sh
tools/neverd-mcp/neverd-mcp --worker /absolute/path/to/neverd-worker \
  --file /absolute/path/to/binary
```

To share the open GUI project, enable session sharing in MCP connections and
copy its credential-file path for:

```sh
tools/neverd-mcp/neverd-mcp --attach /absolute/path/to/credentials.json
```

Attachment queries the same worker and revision, never starts another project
writer, and ends when sharing or the GUI closes or the project changes. See
[MCP details](../tools/neverd-mcp/README.md).

## Packaging and qualification

`cmake --install build-gui --prefix dist` invokes Qt's deployment script for
the Widgets, Svg and Sql plugins. The matching engine and its dependencies must
be included in a distributable package. macOS can create an ad-hoc signed
development bundle with:

```sh
python3 tools/neverd-gui/package_macos.py --build-dir build-gui \
  --engine /absolute/path/to/libneverd.dylib \
  --qt-dir /path/to/Qt/6.8.3/macos --output dist/NeverD.app
```

This produces an ad-hoc signed development application, not a notarized
release. Test native dialogs, IME, accessibility, mixed-DPI screens and platform
packaging on each target platform before a public release; automated offscreen
tests do not prove those properties. `--startup-benchmark` writes startup
milestones from main entry to the first painted listing of the opened file; see
the [benchmark harness](../tools/neverd-gui/benchmarks/README.md).

See the [packaging guide](../tools/neverd-gui/PACKAGING.md) for dependency and
license inputs, and the [qualification record](gui-qualification.md) for
measured evidence and remaining release criteria.
