# Android application and class loader snapshots

The API 28 native profile can project a declared process lifecycle into the
shared Java heap. JNI and DEX use the same objects and methods. This is an
explicit analysis input, not discovery of the installed application or execution
of the Android framework's startup code.

```json
{
  "android": {
    "jni": true,
    "application_snapshot": {
      "phase": "application",
      "package_name": "example.application"
    }
  }
}
```

The snapshot requires JNI. Its phase is mandatory in JSON:

| Phase | `ActivityThread.currentActivityThread()` | `currentApplication()` / `getApplication()` |
| --- | --- | --- |
| `unattached` | null | null (instance calls still require a receiver) |
| `attached` | retained ActivityThread | null |
| `application` | retained ActivityThread | retained platform Application |

An absent snapshot is unknown and stops at the first framework operation. It
does not select `unattached`. `package_name` is optional, at most 1024 UTF-8 bytes,
nonempty, and allowed only in the application phase. An absent name stops
`getPackageName`; properties and DEX names are not used to guess it.

`application_loader_established` optionally supplies the independent loader
stage. With `phase: attached`, `true` makes the application PathClassLoader
available through supplied classes' `Class.getClassLoader` while
`currentApplication`/`getApplication` still return null. The loader retains the
boot parent and exact supplied-declaration namespace across thread detach.
Explicit application-loader lookup failures are also admitted in this stage.
Absent means unknown in `attached`; `false` explicitly declares it unestablished.
Neither case permits application-class loader queries to return a guessed value.
`unattached` implies false and `application` implies true; contradictory explicit
values are rejected. This separates loader setup from Application construction,
as in API 28 [LoadedApk.makeApplication](https://android.googlesource.com/platform/frameworks/base/+/android-9.0.0_r1/core/java/android/app/LoadedApk.java).
It does not infer the path, APK identity, custom factory or constructor body.

The application phase represents a base `android.app.Application` already
attached to a distinct `ContextImpl`. `getBaseContext()` returns that context;
both objects' `getApplicationContext()` return the same Application and their
`getClassLoader()` return the same application PathClassLoader. Local-reference
deletion, local frames, native thread attachment and detachment do not reset
these workload-owned identities. Two native threads see the same objects.

Three loader identities are separate: the boot loader, the system PathClassLoader
and the application PathClassLoader. System and application loaders have the
boot loader as parent; the boot parent is null. On API 28 a platform class's
`Class.getClassLoader()` returns the BootClassLoader object. Consequently the
base Application object's class loader differs from its Context's application
loader. Supplied DEX classes use the application loader; array classes use
their component's loader, including the boot loader for primitive arrays.

`ClassLoader.loadClass(String)` accepts known ASCII binary names. The application
loader can resolve supplied DEX declarations and the admitted platform classes;
system and boot loaders resolve only the admitted platform classes. Loading
does not execute a supplied class initializer, including after that initializer
has failed. Null names throw NullPointerException. Unprovided names, array or
descriptor spellings, non-ASCII lookup and unknown loader instances stop
explicitly. They do not become ClassNotFoundException, empty classes, host
classpath searches or successful placeholder objects.

An optional `application_snapshot.class_lookup_failures` array declares exact
negative `loadClass` observations. Each entry requires `loader` (`boot`,
`system` or `application`), `binary_name` and `message` (a string or explicit
null). For example:

```json
{"phase":"attached","class_lookup_failures":[
  {"loader":"boot","binary_name":"example.Optional","message":null}
]}
```

This input is conditional evidence, not automatic inference from an incomplete
DEX inventory. Only the selected loader's exact lookup throws a fresh shared
`ClassNotFoundException` with the declared message. The guest can catch or clear
it and continue; known loads still avoid initialization. Failure in a parent
does not declare failure in a child, nor does it alter JNI `FindClass` outcomes.
Unknown names and foreign loader instances still stop explicitly. The array is
limited to 256 unique loader/name pairs, binary names to 1024 ASCII bytes with
nonempty Java identifier components, and messages to 4096 UTF-8 bytes. Empty
messages and null messages remain distinct. Application loader outcomes require
an established application loader: the application phase or an explicit `true`
loader input in the attached phase. A failure cannot shadow a declaration visible
to that loader. Custom delegation, exception causes and a closed complete
classpath are not supplied by these observations.

The lifecycle phase is fixed per workload; modeled mutable fields retain guest writes. It does not execute
`bindApplication`, custom Application constructors, framework class initializers,
split/delegate-last/custom class loaders, security-manager policies, framework
callbacks, a Java scheduler or `Class.forName`. DEX invoke instructions and JNI calls to registered native declarations
use the bounded guest-call contract below; registration alone executes nothing. The
model does not give unmodeled framework fields or methods inferred values.
Native evidence obtained under a snapshot remains conditional on that input.

`dalvik::Runtime` owns the projection and its retained identities; Library owns
common argument, receiver, type and exception checks. DEX and JNI share this
boundary and the workload budget. A non-Java stop poisons the Runtime, including
partial allocation; it cannot be retried as a fresh snapshot.

Behavior references are Android's
[ActivityThread](https://android.googlesource.com/platform/frameworks/base/+/android-9.0.0_r1/core/java/android/app/ActivityThread.java),
[ContextImpl](https://android.googlesource.com/platform/frameworks/base/+/android-9.0.0_r1/core/java/android/app/ContextImpl.java),
[ContextWrapper](https://android.googlesource.com/platform/frameworks/base/+/android-9.0.0_r1/core/java/android/content/ContextWrapper.java),
[ApplicationLoaders](https://android.googlesource.com/platform/frameworks/base/+/android-9.0.0_r1/core/java/android/app/ApplicationLoaders.java),
[Class](https://android.googlesource.com/platform/libcore/+/android-9.0.0_r1/ojluni/src/main/java/java/lang/Class.java)
and [ClassLoader](https://android.googlesource.com/platform/libcore/+/android-9.0.0_r1/ojluni/src/main/java/java/lang/ClassLoader.java)
at `android-9.0.0_r1`. The implementation and synthetic tests are independently
written; reference framework bodies are not embedded in NeverD.

## Instrumentation field state

`application_snapshot.instrumentation` explicitly selects `uninitialized`
(a null field) or `platform` (one retained `android.app.Instrumentation` object).
It is optional for `attached` and `application`, and invalid for `unattached`.
Application creation does not select an instrumentation value: API 28 binding
installs instrumentation before creating the Application and can use a custom
subclass. An omitted value therefore stops when read, rather than implying null
or the default implementation.

The declared package-private `ActivityThread.mInstrumentation` instance field and
public `getInstrumentation()` method share one Runtime-owned value. JNI resolves
its actual field ID and reads/writes through the same owner used by DEX method
execution. A guest write, including null, establishes the value without reading
an unknown initial value; compatible supplied subclass objects retain identity.
Deleted JNI local references and native detach/reattach do not reset the field.
An unrelated ActivityThread object cannot borrow the process snapshot's state.
Unknown fields, custom instrumentation startup and callbacks remain unsupported;
this projection does not execute framework constructors, `basicInit` or services.

## Existing application files directory

`application_snapshot.files_directory` is optional in the `application` phase.
It names an already initialized, existing private files directory; it does not
infer a package, create a directory, change permissions or run ContextImpl's
initialization code. The native profile requires `linux_files` and an exact
entry in `linux_files.directories` (root is intrinsic). Missing or conflicting
namespace inputs fail before execution. Standalone Dalvik validates an absolute,
NUL-free UTF-8 path of at most 4095 bytes and applies File's Unix lexical slash
normalization; it does not resolve dot components or consult the host.

`Context.getFilesDir()` on the retained application or base context returns the
same workload-owned `java.io.File`. Its `getPath`, `getAbsolutePath` and `toString`
return the retained path string; `getName`, `getParent` and `isAbsolute` provide
lexical queries over that absolute path. Root has an empty name and null parent.
Unknown paths and foreign File objects stop explicitly. Arbitrary File
constructors, relative Java paths, canonicalization, filesystem queries and
mutations are outside this existing-directory projection. Directory availability
is not evidence of the actual installed application.

## Read-only Java state directory

Set `android.observe_java_state: true` with `jni: true` to include
`android.java_state` at the final native workload stop. This reads the shared
Runtime, including after a Java/resource failure. It does not initialize a
class, look up a new Java object, consume guest instructions, or create loaders.
The default is off. The C++ equivalent is `Runtime::snapshot()`.

`classes` lists the admitted Program declarations in descriptor order, with
their superclass, `source_id`, `supplied` ownership, class-object identity and
initialization state (`not_started`, `prepared`, `initializing`, `initialized`,
or `erroneous`). `initialized` records Runtime state; it does not imply that
a `<clinit>` body existed. Native DEX source IDs are `supplied-dex-N`, indexing the explicit
input images. Source labels do not establish ownership: `supplied` is recorded
by Program independently. Library-only intrinsics and array types are outside
this declaration inventory; it is not a complete Android boot classpath.

`framework` separates `declared_phase` from the identities materialized by
actual model operations. Zero means not materialized, not proof of absence on a
device. `instrumentation: null` means unknown/unread, whereas `"0"` is a known
null value. Identities are workload heap tokens, **not JNI handles or guest
addresses**. `terminal` means Runtime has retained a non-Java stop; an ordinary
Java exception instead retains the class's erroneous initialization state.

`projected_loader` uses the same ownership decision as `Class.getClassLoader`.
It is null when no lifecycle snapshot establishes that ownership. It records
boot/application ownership in the current single-application-loader projection,
not the actual definer of an unavailable APK. The system loader is distinct
from the application loader; both have the boot parent in this projection.
Reading this directory does not turn a declared Application snapshot into
execution of `LoadedApk.makeApplication`, a custom constructor or `onCreate`.

## Calling registered native methods through JNI

`GetMethodID` and `GetStaticMethodID` resolve supplied declarations independently
of body support and current native registration. Their active class
initialization still runs through Runtime; a missing library-load operation or
throwing initializer cannot be skipped by registration.

The JNI `Call<Type>Method`, `CallNonvirtual<Type>Method` and
`CallStatic<Type>Method` families, including their `A` and `V` forms, can invoke
registered ordinary native methods. Virtual dispatch selects the supplied
receiver override; nonvirtual dispatch keeps the selected declaring method.
The registration is read at invocation, so rebinding affects existing method
IDs and unregistering removes their target. Unknown native symbol lookup stops
explicitly rather than guessing a library or throwing an inferred link error.

Android's existing guest callback scheduler executes the registered AArch64
address on the calling thread, with its JNIEnv and a fresh local reference to
the receiver or declaring class. Up to six integral/reference Java arguments
fit the admitted register ABI. The shared Java heap validates object types and
retains mutations. Native callbacks can make nested JNI calls and wait in
modeled Bionic services; other threads retain their own frames and environments.
The existing workload instruction/deadline limits include original native work.

Each native invocation has an implicit local frame. A returned object is
validated before the callee's local frames retire, then gets a caller-local
reference to the same heap object. Globals and caller locals survive. Pending
Java exceptions survive retirement and supersede an unspecified native return
carrier. `PopLocalFrame` cannot pop the implicit frame, and detach during a
native Java invocation returns JNI_ERR without destroying the attachment.
Terminal guest faults or unsupported operations remain terminal; they are not
reported as a successful Java return.

Floating arguments/results, stack-passed native arguments, synchronized native
methods, dynamic JNI symbol resolution and DEX-to-native continuations remain
unsupported. The DEX reader rejects unsupported ABI-changing annotations.
This is not a host JVM, `System.loadLibrary` implementation, automatic
`JNI_OnLoad` call or execution of the Android application bootstrap.
Contracts follow the [JNI function specification](https://docs.oracle.com/en/java/javase/17/docs/specs/jni/functions.html)
and [invocation API](https://docs.oracle.com/en/java/javase/17/docs/specs/jni/invocation.html).

## Guest working directory

`linux_files.working_directory` optionally supplies a linked, reachable guest
cwd, independently of the application's files directory. It must be `/` or a
canonical absolute entry in `linux_files.directories`. An implicit catalogue
parent alone is insufficient for this explicit input. No host cwd is inherited.

Raw x64/AArch64 `getcwd` returns the bytes copied including NUL. A short buffer
returns `ERANGE` before output access, wholly inaccessible output returns
`EFAULT`, and mixed writable/unwritable output stops without inventing an
architecture-specific partial copy. Android Bionic `getcwd` returns the original
buffer on success and null with thread-local errno on failure. A non-null buffer
with size zero returns `EINVAL`; a null buffer allocates guest memory, and size
zero follows API 28's page allocation, exact string duplication and temporary
release. Guest `free` owns returned allocations. Successful calls preserve errno.
An absent cwd remains unknown. These inputs do not implement `chdir`, deleted
or unreachable cwd, relative pathname traversal, mounts or filesystem mutation.

Behavior references are API 28 [File](https://android.googlesource.com/platform/libcore/+/android-9.0.0_r1/ojluni/src/main/java/java/io/File.java),
[UnixFileSystem](https://android.googlesource.com/platform/libcore/+/android-9.0.0_r1/ojluni/src/main/java/java/io/UnixFileSystem.java)
and [Bionic getcwd](https://android.googlesource.com/platform/bionic/+/android-9.0.0_r1/libc/bionic/getcwd.cpp).
Implementations and synthetic callers are independently written.

## Java library loading and DEX/native continuation

`android.java_library` explicitly binds the currently mapped, initialized ELF
image to one logical `System.loadLibrary` name in the supplied DEX namespace:

```json
{"android":{"jni":true,"initialize":true,"java_library":"example",
            "dex_images":["<explicit DEX bytes>"]}}
```

The name is a nonempty ASCII identifier of at most 255 bytes, using letters,
digits, `_`, `-` and `.`. The input requires JNI and ELF initialization. It is
an analysis binding, not evidence of an APK, a native search path, or the
actual application class loader. No host file or shared library is opened.
The existing `libraries` option remains the catalogue of modeled system APIs;
it cannot supply an original `JNI_OnLoad` body.

An actual DEX call to `System.loadLibrary(String)` suspends its Java activation.
For the explicitly bound name, NeverD calls the ELF's original exported
`JNI_OnLoad(JavaVM *, null)` after its ELF initializers. The callback uses the
existing guest scheduler, instruction/deadline budget and implicit JNI local
frame. `FindClass` sees the one supplied DEX namespace. A valid API 28 result
(1.2, 1.4 or 1.6) completes the load. An ELF without that export completes without
a callback. Repeated loads do not rerun it; same-thread recursive loads can
continue. Invalid returned versions retain the failed load and throw
`UnsatisfiedLinkError`; already performed native writes and registrations remain.
Pending Java exceptions retain their heap identity, and class-initializer
failure uses Runtime's ordinary erroneous-class rules.

DEX invocations of registered ordinary native methods now use the same JNI
argument transport as JNI method calls. After guest return, the retained Java
registers, original invoke position, constructor state, exception handler and
class-initialization stack continue. No instructions or prior effects are
replayed. Native-to-Java callbacks can themselves suspend into another native
call. JNI method/field lookup and allocation finish their original operation
after a suspended initializer; reference returns are converted to caller-local
JNI handles only when that operation completes. Native and on-load local frames
retire even if an explicit nested frame was left open.

The standalone `dalvik::Runtime` enables suspension with a nonzero owner ID.
`ExecutionStop::Suspended` carries a request and a single-use continuation token;
`resume` accepts shared-heap values or exceptions for that exact occurrence.
Tokens are owner-bound and nested invocations complete in stack order. The
instruction closure is retained, and nested entries share the remaining stack
allowance. Synchronous callers still refuse native execution. At most 64
invocations may be retained; a terminal non-Java stop cannot be retried.

This is one explicit guest ELF and one supplied DEX namespace. Unknown library
bindings, native symbol-name lookup, custom/delegating loaders, unloading,
boot-loader native libraries, floating/stack native arguments, monitors and
cross-thread class or library initialization waits remain explicit stops.
Direct JNI calls to `System.loadLibrary` lack a DEX caller and remain unsupported.
This contract does not execute Android framework Application creation.

Behavior was checked against API 28 ART
[`JavaVMExt::LoadNativeLibrary`](https://android.googlesource.com/platform/art/+/android-9.0.0_r1/runtime/java_vm_ext.cc)
and the [JNI invocation specification](https://docs.oracle.com/javase/8/docs/technotes/guides/jni/spec/invocation.html#JNI_OnLoad).
Implementation and synthetic programs are independently authored.
