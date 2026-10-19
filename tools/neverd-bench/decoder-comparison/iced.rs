// NeverD Decompiler: benchmark-only iced public API adapter.
use iced_x86::{
    Decoder, DecoderOptions, Formatter, Instruction, InstructionInfoFactory, IntelFormatter,
};

#[repr(C)]
pub struct Result {
    instructions: u64,
    rejected: u64,
    boundaries: u64,
    values: u64,
}

struct State {
    bits: u32,
    mode: u32,
    formatter: IntelFormatter,
    factory: InstructionInfoFactory,
    text: String,
}

#[no_mangle]
pub extern "C" fn iced_create(bits: u32, mode: u32) -> *mut std::ffi::c_void {
    Box::into_raw(Box::new(State {
        bits,
        mode,
        formatter: IntelFormatter::new(),
        factory: InstructionInfoFactory::new(),
        text: String::with_capacity(512),
    }))
    .cast()
}

#[no_mangle]
pub unsafe extern "C" fn iced_destroy(state: *mut std::ffi::c_void) {
    drop(Box::from_raw(state.cast::<State>()));
}

#[no_mangle]
pub unsafe extern "C" fn iced_run(
    state: *mut std::ffi::c_void,
    input: *const u8,
    length: usize,
    repeats: u32,
    output: *mut Result,
) {
    let state = &mut *state.cast::<State>();
    let bytes = std::slice::from_raw_parts(input, length);
    let mut result = Result {
        instructions: 0,
        rejected: 0,
        boundaries: 0xcbf29ce484222325,
        values: 0,
    };
    for _ in 0..repeats {
        let mut decoder = Decoder::with_ip(state.bits, bytes, 0x100000, DecoderOptions::NONE);
        let mut instruction = Instruction::default();
        while decoder.can_decode() {
            let offset = decoder.position();
            decoder.decode_out(&mut instruction);
            let ok = !instruction.is_invalid();
            let size = if ok { instruction.len() } else { 1 };
            result.boundaries = (result.boundaries
                ^ (((offset as u64) << 5) | ((size as u64) << 1) | ok as u64))
                .wrapping_mul(1099511628211);
            if !ok {
                result.rejected += 1;
                decoder.set_position(offset + 1).unwrap();
                decoder.set_ip(0x100000 + offset as u64 + 1);
                continue;
            }
            result.instructions += 1;
            result.values = result.values.wrapping_add(instruction.code() as u64);
            if state.mode != 0 {
                state.text.clear();
                state.formatter.format(&instruction, &mut state.text);
                result.values = result.values.wrapping_add(
                    state.text.len() as u64
                        + state.text.as_bytes().last().copied().unwrap_or(0) as u64,
                );
            }
            if state.mode == 2 {
                let info = state.factory.info(&instruction);
                for r in info.used_registers() {
                    result.values = result
                        .values
                        .wrapping_add(r.register() as u64 + r.access() as u64);
                }
                for m in info.used_memory() {
                    result.values = result.values.wrapping_add(
                        m.segment() as u64 + m.base() as u64 + m.index() as u64 + m.scale() as u64,
                    );
                    result.values = result
                        .values
                        .wrapping_add(m.displacement())
                        .wrapping_add(m.access() as u64 + m.memory_size() as u64);
                }
                for i in 0..instruction.op_count() {
                    result.values = result.values.wrapping_add(
                        instruction.op_kind(i) as u64
                            + instruction.op_register(i) as u64
                            + info.op_access(i) as u64,
                    );
                }
                result.values = result
                    .values
                    .wrapping_add(instruction.immediate64())
                    .wrapping_add(instruction.near_branch_target());
                result.values = result.values.wrapping_add(
                    instruction.rflags_read() as u64
                        + instruction.rflags_modified() as u64
                        + instruction.rflags_undefined() as u64
                        + instruction.flow_control() as u64,
                );
            }
        }
    }
    *output = result;
}
