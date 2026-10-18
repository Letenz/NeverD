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

This is one immutable lifecycle snapshot per workload. It does not execute
`bindApplication`, custom Application constructors, framework class initializers,
split/delegate-last/custom class loaders, security-manager policies, framework
callbacks, a Java scheduler, `Class.forName` or native Java method bodies.
RegisterNatives metadata remains separate from Java-to-native execution. The
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
