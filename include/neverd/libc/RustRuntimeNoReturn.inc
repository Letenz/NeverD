// Routines of Rust's standard library whose declarations return `!`: they
// panic, abort or exit, and never return to their caller.  Included by
// LibCCallTraits.cpp with RUST_NO_RETURN_PATH and RUST_NO_RETURN_PATH_PREFIX
// defined.  A path is matched against a symbol's demangled path without its
// generic arguments (`core::panicking::assert_failed::<u64, u64>` is
// `core::panicking::assert_failed`), and a path without `::` against an
// unmangled symbol's name.  A routine that only sometimes returns, such as
// `__rust_start_panic` (it returns when no unwinder takes the panic), and
// one that inspects a panic, such as `std::thread::panicking`, are not here.

// core::panicking: the panic entry points the compiler and `panic!` call.
RUST_NO_RETURN_PATH("core::panicking::panic")
RUST_NO_RETURN_PATH("core::panicking::panic_fmt")
RUST_NO_RETURN_PATH("core::panicking::const_panic_fmt")
RUST_NO_RETURN_PATH("core::panicking::panic_nounwind")
RUST_NO_RETURN_PATH("core::panicking::panic_nounwind_fmt")
RUST_NO_RETURN_PATH("core::panicking::panic_nounwind_nobacktrace")
RUST_NO_RETURN_PATH("core::panicking::panic_explicit")
RUST_NO_RETURN_PATH("core::panicking::panic_str_2015")
RUST_NO_RETURN_PATH("core::panicking::panic_display")
RUST_NO_RETURN_PATH("core::panicking::unreachable_display")
RUST_NO_RETURN_PATH("core::panicking::panic_bounds_check")
RUST_NO_RETURN_PATH("core::panicking::panic_misaligned_pointer_dereference")
RUST_NO_RETURN_PATH("core::panicking::panic_null_pointer_dereference")
RUST_NO_RETURN_PATH("core::panicking::panic_invalid_enum_construction")
RUST_NO_RETURN_PATH("core::panicking::panic_cannot_unwind")
RUST_NO_RETURN_PATH("core::panicking::panic_in_cleanup")
RUST_NO_RETURN_PATH("core::panicking::assert_failed")
RUST_NO_RETURN_PATH("core::panicking::assert_failed_inner")
// Overflow, division and shift checks since Rust 1.79.
RUST_NO_RETURN_PATH_PREFIX("core::panicking::panic_const::panic_const_")

// `unwrap`, `expect`, slicing and `RefCell` failures.
RUST_NO_RETURN_PATH("core::option::unwrap_failed")
RUST_NO_RETURN_PATH("core::option::expect_failed")
RUST_NO_RETURN_PATH("core::result::unwrap_failed")
RUST_NO_RETURN_PATH("core::slice::index::slice_index_fail")
RUST_NO_RETURN_PATH("core::slice::index::slice_start_index_len_fail")
RUST_NO_RETURN_PATH("core::slice::index::slice_end_index_len_fail")
RUST_NO_RETURN_PATH("core::slice::index::slice_index_order_fail")
RUST_NO_RETURN_PATH("core::slice::index::slice_start_index_overflow_fail")
RUST_NO_RETURN_PATH("core::slice::index::slice_end_index_overflow_fail")
RUST_NO_RETURN_PATH(
    "core::slice::sort::shared::smallsort::panic_on_ord_violation")
RUST_NO_RETURN_PATH("core::str::slice_error_fail")
RUST_NO_RETURN_PATH("core::str::slice_error_fail_rt")
RUST_NO_RETURN_PATH("core::cell::panic_already_borrowed")
RUST_NO_RETURN_PATH("core::cell::panic_already_mutably_borrowed")

// Allocation failure.
RUST_NO_RETURN_PATH("alloc::raw_vec::capacity_overflow")
RUST_NO_RETURN_PATH("alloc::raw_vec::handle_error")
RUST_NO_RETURN_PATH("alloc::alloc::handle_alloc_error")
RUST_NO_RETURN_PATH("alloc::alloc::handle_alloc_error::rt_error")
RUST_NO_RETURN_PATH("std::alloc::rust_oom")

// std's panic runtime and process exit.
RUST_NO_RETURN_PATH("std::panicking::begin_panic")
RUST_NO_RETURN_PATH("std::panicking::begin_panic_handler")
RUST_NO_RETURN_PATH("std::panicking::rust_panic_with_hook")
RUST_NO_RETURN_PATH("std::panicking::rust_panic_without_hook")
RUST_NO_RETURN_PATH("std::panicking::rust_panic")
RUST_NO_RETURN_PATH("std::panic::panic_any")
RUST_NO_RETURN_PATH("std::panic::resume_unwind")
RUST_NO_RETURN_PATH("std::process::abort")
RUST_NO_RETURN_PATH("std::process::exit")
RUST_NO_RETURN_PATH("std::sys::pal::unix::abort_internal")
RUST_NO_RETURN_PATH("std::sys::unix::abort_internal")
RUST_NO_RETURN_PATH("std::sys::pal::windows::abort_internal")
RUST_NO_RETURN_PATH("std::sys::windows::abort_internal")
// The `#[panic_handler]` and the panic's last step, which std exports under
// these names: plain C symbols in some builds, members of the synthetic
// `__rustc` crate in v0-mangled ones.
RUST_NO_RETURN_PATH("rust_begin_unwind")
RUST_NO_RETURN_PATH("__rustc::rust_begin_unwind")
RUST_NO_RETURN_PATH("rust_panic")
RUST_NO_RETURN_PATH("__rustc::rust_panic")
