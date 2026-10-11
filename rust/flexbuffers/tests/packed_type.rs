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

//! A packed type byte that names no `FlexBufferType` or `BitWidth` must be
//! rejected. Deriving `Default` with `#[default]` would break this: num_enum
//! 0.5 reads that attribute too and turns `TryFromPrimitive` into a catch-all
//! that decodes every unknown value as the default variant.

use flexbuffers::{BitWidth, FlexBufferType, Reader, ReaderError};
use std::convert::TryFrom;

#[test]
fn unknown_flexbuffer_type_is_rejected() {
    // The valid types are 0 through 26 (Bool) and 36 (VectorBool).
    assert_eq!(
        FlexBufferType::try_from(26u8).ok(),
        Some(FlexBufferType::Bool)
    );
    assert_eq!(
        FlexBufferType::try_from(36u8).ok(),
        Some(FlexBufferType::VectorBool)
    );
    for unknown in (27u8..36).chain(37..64) {
        assert!(FlexBufferType::try_from(unknown).is_err(), "{}", unknown);
    }
}

#[test]
fn unknown_bit_width_is_rejected() {
    assert_eq!(BitWidth::try_from(3u8).ok(), Some(BitWidth::W64));
    assert!(BitWidth::try_from(4u8).is_err());
}

#[test]
fn root_with_unknown_type_is_rejected() {
    // One root byte, then the packed type (type 27, width W8), then the root
    // width in bytes.
    let buffer = [0u8, 27 << 2, 1];
    assert_eq!(
        Reader::get_root(&buffer[..]).err(),
        Some(ReaderError::InvalidPackedType)
    );
}
