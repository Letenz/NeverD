# PHF compatibility projection

`lib/web/desktop/tauri/PHF.cpp` adapts the SipHash13-128 round/init/final equations
and PHF 0.10 displacement projection from these fixed source releases:

- [rust-siphash 0.3.10 sip128.rs](https://github.com/jedisct1/rust-siphash/blob/0.3.10/src/sip128.rs),
  copyright 2012–2015 The Rust Project Developers, Apache-2.0 OR MIT.
- [rust-phf v0.10.1 phf_shared/src/lib.rs](https://github.com/sfackler/rust-phf/blob/v0.10.1/phf_shared/src/lib.rs),
  copyright 2014–2016 Steven Fackler, MIT.

`COPYING` preserves upstream rust-siphash's author/license notice. `LICENSE-MIT`
contains the MIT grant with those authors; `LICENSE-APACHE` contains the
unmodified Apache 2.0 text (from the Rust 1.66.0 license distribution).
`PHF-LICENSE` preserves rust-phf's notice. Modified 2026-10-12: a one-shot C++
byte-string projection, without the generic Rust hash traits, stateful public
hasher, table generation or runtime code. UTF-8 string keys have no terminator.
This compatibility hash validates table lookups; it is not content integrity
or publisher authentication. No Rust crate or runtime is a product dependency.
