// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

//! `BitWidth::from(f64)` is exact; see the comment on that impl. A margin, as
//! clippy's pedantic `float_cmp` suggests, would store `1.0 + f64::EPSILON` as
//! the f32 `1.0`.

use flexbuffers::BitWidth;

#[test]
fn f64_that_f32_holds_exactly_is_32_bits() {
    assert_eq!(BitWidth::from(0.5f64), BitWidth::W32);
    assert_eq!(BitWidth::from(-0.0f64), BitWidth::W32);
    assert_eq!(BitWidth::from(f64::from(f32::MAX)), BitWidth::W32);
    assert_eq!(BitWidth::from(f64::INFINITY), BitWidth::W32);
}

#[test]
fn f64_that_f32_rounds_is_64_bits() {
    assert_eq!(BitWidth::from(0.1f64), BitWidth::W64);
    assert_eq!(BitWidth::from(1.0 + f64::EPSILON), BitWidth::W64);
    assert_eq!(BitWidth::from(f64::MAX), BitWidth::W64);
}

#[test]
fn nan_is_64_bits() {
    assert_eq!(BitWidth::from(f64::NAN), BitWidth::W64);
}
