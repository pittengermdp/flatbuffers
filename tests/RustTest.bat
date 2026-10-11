@echo off
rem Copyright 2018 Google Inc. All rights reserved.
rem
rem Licensed under the Apache License, Version 2.0 (the "License");
rem you may not use this file except in compliance with the License.
rem You may obtain a copy of the License at
rem
rem     http://www.apache.org/licenses/LICENSE-2.0
rem
rem Unless required by applicable law or agreed to in writing, software
rem distributed under the License is distributed on an "AS IS" BASIS,
rem WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
rem See the License for the specific language governing permissions and
rem limitations under the License.

rem Compile then run the Rust test.
setlocal
cd /d "%~dp0"

rem Test every crate committed to the repository, so a new crate is covered as
rem soon as it lands. A crate opts out only with a "skip" key in its own
rem [package.metadata.rust-test] table (see docs/source/languages/rust.md).
rem Clippy runs in RustTest.sh.
set CRATES=0
for /f "delims=" %%r in ('git rev-parse --show-toplevel') do set "REPO_ROOT=%%r"
for /f "delims=" %%m in ('git -C "%REPO_ROOT%" ls-files "*Cargo.toml"') do (
  call :test_crate "%%m" || exit /b 1
)
if %CRATES%==0 (
  echo KO: git lists no Cargo.toml in this repository.
  exit /b 1
)

cd rust_serialize_test
cargo run -- --quiet || exit /b 1

cd ../rust_no_std_compilation_test
rustup install nightly
rustup component add rust-src --toolchain nightly
rustup target add thumbv7m-none-eabi
cargo +nightly build || exit /b 1

cd ../rust_usage_test
cargo run --bin=flatbuffers_alloc_check || exit /b 1
cargo run --bin=flexbuffers_alloc_check || exit /b 1
cargo run --bin=monster_example || exit /b 1
cd ..
exit /b 0

rem Runs the tests of the crate whose Cargo.toml is %1, a path relative to the
rem repository root, unless the crate opts out.
:test_crate
set /a CRATES+=1
set "CRATE=%~1"
set "CRATE=%CRATE:/Cargo.toml=%"
set "CRATE_DIR=%REPO_ROOT%/%CRATE%"
set "CRATE_DIR=%CRATE_DIR:/=\%"
set "MANIFEST=%CRATE_DIR%\Cargo.toml"
findstr /l /b /c:"[package.metadata.rust-test]" "%MANIFEST%" >nul || goto :run_crate_tests
findstr /r /b /c:"skip *=" "%MANIFEST%" >nul || goto :run_crate_tests
echo SKIP: %CRATE%
exit /b 0

:run_crate_tests
pushd "%CRATE_DIR%" || exit /b 1
cargo test -- --quiet || (echo KO: Rust tests in %CRATE% failed.& exit /b 1)
popd
echo OK: Rust tests in %CRATE% passed.
exit /b 0
