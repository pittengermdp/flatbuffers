#!/bin/bash
set -e
#
# Copyright 2018 Google Inc. All rights reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

if [[ "$1" == "mips-unknown-linux-gnu" ]]; then
    TARGET_FLAG=(--target mips-unknown-linux-gnu)
    export CARGO_TARGET_MIPS_UNKNOWN_LINUX_GNU_LINKER=mips-linux-gnu-gcc
    export CARGO_TARGET_MIPS_UNKNOWN_LINUX_GNU_RUNNER="qemu-mips -L /usr/mips-linux-gnu"
fi

# Runs the command that follows $1 and reports it under the name $1. A failure
# stops the run.
function check() {
    local name=$1
    shift
    if "$@"; then
        echo "OK: $name passed."
    else
        echo "KO: $name failed."
        exit 1
    fi
}

# Prints the value of key $2 in the [package.metadata.rust-test] table of the
# Cargo.toml at $1, or nothing when the crate does not set it. That table is
# the only way a crate opts out of the checks every crate gets below. Its keys
# (skip, clippy-debt, clippy-debt-limit) are documented in
# docs/source/languages/rust.md.
function rust_test_metadata() {
    awk -v key="$2" '
        /^[ \t]*\[/ {
            in_table = ($0 ~ /^[ \t]*\[package\.metadata\.rust-test\][ \t]*$/)
            next
        }
        in_table && $0 ~ ("^[ \t]*" key "[ \t]*=") {
            sub(/^[^=]*=[ \t]*/, "")
            gsub(/^"|"[ \t]*$/, "")
            print
            exit
        }' "$1"
}

# Lints the crate in the current directory, named $1, whose Cargo.toml is $2.
# Any warning fails the run unless the crate declares clippy debt. A crate in
# debt must report exactly its clippy-debt-limit diagnostics: more means new
# warnings, fewer means the limit has to come down with them, and none means
# the debt entry has to go. Debt can only shrink.
function check_clippy() {
    local crate=$1 manifest=$2 debt limit log status=0 diagnostics count
    local clippy=(cargo clippy "${TARGET_FLAG[@]}" --all-targets)
    debt=$(rust_test_metadata "$manifest" clippy-debt)
    if [[ -z $debt ]]; then
        check "Clippy on $crate" "${clippy[@]}" -- -D warnings
        return
    fi

    limit=$(rust_test_metadata "$manifest" clippy-debt-limit)
    if ! [[ $limit =~ ^[0-9]+$ ]]; then
        echo "KO: $crate declares clippy-debt without a numeric clippy-debt-limit."
        exit 1
    fi

    # Without -D warnings, and with --keep-going, cargo checks every target
    # instead of stopping at the first one that fails, so the count covers the
    # whole crate. --color never keeps CARGO_TERM_COLOR=always (which CI's
    # toolchain action sets) from hiding every line from the grep below.
    log=$("${clippy[@]}" --keep-going --color never --message-format=short 2>&1) || status=$?
    diagnostics=$(grep -E '^[^ ]+:[0-9]+:[0-9]+: (warning|error)' <<< "$log" | sort -u)
    count=$(grep -c . <<< "$diagnostics" || true)
    if [[ $count == 0 && $status != 0 ]]; then
        printf '%s\n' "$log"
        echo "KO: clippy failed on $crate without reporting a lint."
        exit 1
    elif [[ $count == 0 ]]; then
        echo "KO: $crate is clippy-clean. Delete clippy-debt and clippy-debt-limit from its Cargo.toml."
        exit 1
    elif ((count > limit)); then
        printf '%s\n' "$diagnostics"
        echo "KO: $crate has $count clippy diagnostics, $((count - limit)) over its clippy-debt-limit of $limit."
        exit 1
    elif ((count < limit)); then
        echo "KO: $crate is down to $count clippy diagnostics. Lower its clippy-debt-limit from $limit to $count."
        exit 1
    fi
    echo "DEBT: $crate is at its clippy-debt-limit of $count diagnostics: $debt"
}

repo_root=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
manifests=$(git -C "$repo_root" ls-files '*Cargo.toml')
if [[ -z $manifests ]]; then
    echo "KO: git lists no Cargo.toml under $repo_root."
    exit 1
fi

rustup component add clippy

# Every crate committed to the repository is linted and tested here, so a new
# crate is covered as soon as it lands. A crate opts out only through its own
# Cargo.toml (see rust_test_metadata), never through a list in this script.
while IFS= read -r manifest <&3; do
    crate=$(dirname "$manifest")
    skip=$(rust_test_metadata "$repo_root/$manifest" skip)
    if [[ -n $skip ]]; then
        echo "SKIP: $crate: $skip"
        continue
    fi

    cd "$repo_root/$crate"
    check_clippy "$crate" "$repo_root/$manifest"
    check "Rust tests in $crate" cargo test "${TARGET_FLAG[@]}" -- --quiet
done 3<<< "$manifests"

cd "$repo_root/tests/rust_serialize_test"
check "Rust serde tests" cargo run "${TARGET_FLAG[@]}" -- --quiet

cd ../rust_no_std_compilation_test
rustup install nightly
rustup component add rust-src --toolchain nightly
rustup target add thumbv7m-none-eabi
check "Rust flatbuffers test no_std compilation" cargo +nightly build

# Stable cargo ignores the build-std setting in this crate's .cargo/config.toml
# and checks it against the prebuilt core and alloc of the target added above.
check "Clippy on the no_std compilation test" cargo clippy -- -D warnings

cd ../rust_usage_test
check "Rust tests (no_std)" cargo test "${TARGET_FLAG[@]}" --no-default-features -- --quiet
check "Rust flatbuffers heap alloc test" cargo run "${TARGET_FLAG[@]}" --bin=flatbuffers_alloc_check
check "Rust flexbuffers heap alloc test" cargo run "${TARGET_FLAG[@]}" --bin=flexbuffers_alloc_check

cargo bench "${TARGET_FLAG[@]}"

# This test is dependent on flatc.
if [[ -f ../../flatc ]]; then
    cd outdir
    check "Rust generated file in \$OUT_DIR" cargo test
    cd ..
fi

# RUST_NIGHTLY environment variable set in dockerfile.
if [[ $RUST_NIGHTLY == 1 ]]; then
    rustup +nightly component add miri
    MIRIFLAGS="-Zmiri-disable-isolation" cargo +nightly miri test
fi
