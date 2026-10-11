//! Float defaults are generated as literals that denote exactly the value flatc
//! stores: a short decimal when one is exact, the IEEE-754 bit pattern
//! otherwise (see tests/rust_float_defaults_test.fbs). Every expectation here
//! is Rust's own parse of the schema text, compared bit for bit, so a default
//! that drifts from the schema, or loses the sign of -0.0, fails.

// `pub` so the generated items count as reachable, not dead code.
#[path = "../../rust_float_defaults_test_generated.rs"]
pub mod rust_float_defaults_test_generated;

use crate::rust_float_defaults_test_generated::{
    FloatDefaults, FloatDefaultsArgs, FloatDefaultsBuilder, FloatDefaultsT,
};
use flatbuffers::FlatBufferBuilder;

#[track_caller]
fn assert_f32(actual: f32, schema_text: &str) {
    let expected = schema_text.parse::<f32>().unwrap();
    assert_eq!(
        actual.to_bits(),
        expected.to_bits(),
        "default {schema_text}"
    );
}

#[track_caller]
fn assert_f64(actual: f64, schema_text: &str) {
    let expected = schema_text.parse::<f64>().unwrap();
    assert_eq!(
        actual.to_bits(),
        expected.to_bits(),
        "default {schema_text}"
    );
}

fn assert_table_defaults(t: &FloatDefaults) {
    assert_f32(t.pi_approx(), "3.14159");
    assert_f64(t.e_approx(), "2.718");
    assert_f64(t.tenth(), "0.1");
    assert_f32(t.milli(), "1e-3");
    assert_f32(t.half(), "0.5");
    assert_f64(t.neg_eighth(), "-0.125");
    assert_f32(t.neg_zero(), "-0.0");
    assert_f32(t.plus(), "+2.5");
    assert_f32(t.whole(), "3");
    assert_f64(t.big(), "1e19");
}

fn empty_table() -> Vec<u8> {
    let mut fbb = FlatBufferBuilder::new();
    let table = FloatDefaultsBuilder::new(&mut fbb).finish();
    fbb.finish(table, None);
    fbb.finished_data().to_vec()
}

#[test]
fn accessors_return_schema_defaults_for_absent_fields() {
    let buf = empty_table();
    assert_table_defaults(&flatbuffers::root::<FloatDefaults>(&buf).unwrap());
}

#[test]
fn builder_omits_args_defaults() {
    // The builder skips a value equal to its default, so a table built from
    // FloatDefaultsArgs::default() is byte-identical to an empty one only if
    // the Args, builder and accessor defaults all agree.
    let mut fbb = FlatBufferBuilder::new();
    let table = FloatDefaults::create(&mut fbb, &FloatDefaultsArgs::default());
    fbb.finish(table, None);
    assert_eq!(fbb.finished_data(), empty_table().as_slice());
}

#[test]
fn object_api_defaults_are_schema_defaults() {
    // PartialEq on floats cannot tell -0.0 from 0.0, so check that sign apart.
    let buf = empty_table();
    let absent = flatbuffers::root::<FloatDefaults>(&buf).unwrap().unpack();
    assert_eq!(FloatDefaultsT::default(), absent);
    assert!(FloatDefaultsT::default().neg_zero.is_sign_negative());
}

#[test]
fn object_api_defaults_pack_to_an_empty_table() {
    let mut fbb = FlatBufferBuilder::new();
    let table = FloatDefaultsT::default().pack(&mut fbb);
    fbb.finish(table, None);
    assert_eq!(fbb.finished_data(), empty_table().as_slice());
}
