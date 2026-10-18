// Types as Rust lays them out, which NeverD's DWARF type import reads.
// Regenerate rust_dwarf.o with:
//   rustc --crate-type=lib --emit=obj -g -C opt-level=0 -C panic=abort \
//     rust_dwarf.rs -o rust_dwarf.o

#[repr(C)]
pub struct Point {
    pub x: i32,
    pub y: i64,
}

/// Rust orders these fields as it likes.
pub struct Plain {
    pub a: u8,
    pub b: u32,
    pub c: u16,
}

/// Variants, which local types hold as the enumeration's bytes.
pub enum Shape {
    Circle(f64),
    Square { side: u32 },
}

/// An enumeration with data held by value, and members of no bytes.
pub struct Holder {
    pub tag: u16,
    pub shape: Shape,
    pub unit: (),
    pub marker: core::marker::PhantomData<u64>,
}

#[repr(u8)]
pub enum Kind {
    A = 1,
    B = 2,
}

#[no_mangle]
pub extern "C" fn use_types(p: &Point, q: &Plain, s: &Shape, k: Kind, h: &Holder) -> i64 {
    let shape = match s {
        Shape::Circle(r) => *r as i64,
        Shape::Square { side } => *side as i64,
    };
    p.y + q.b as i64 + q.a as i64 + q.c as i64 + shape + k as i64 + h.tag as i64
}
