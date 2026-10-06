#!/bin/bash -eu
#
# Copyright 2014 Google Inc. All rights reserved.
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

pushd "$(dirname "$0")" > /dev/null
test_dir="$(pwd)"
go_path=${test_dir}/go_gen
go_src=${go_path}/src

# Emit Go code for the example schemas in the test dir:
../flatc -g --gen-object-api -I include_test -o "${go_src}" monster_test.fbs optional_scalars.fbs
../flatc -g --gen-object-api -I include_test/sub -o "${go_src}" include_test/order.fbs
../flatc -g --gen-object-api -o "${go_src}/Pizza" include_test/sub/no_namespace.fbs
../flatc -g --gen-object-api -o "${go_src}" required_strings.fbs
# go_map_set_test imports this package as github.com/google/flatbuffers/MapSetTest,
# so emit it under that import path.
../flatc -g --gen-object-api -o "${go_src}/github.com/google/flatbuffers" map_set_test.fbs

# Go requires a particular layout of files in order to link multiple packages.
# Copy flatbuffer Go files to their own package directories to compile the
# test binary:
mkdir -p "${go_src}/github.com/google/flatbuffers/go"
mkdir -p "${go_src}/flatbuffers_test"
mkdir -p "${go_src}/go_map_set_test"

cp -a ../go/* ./go_gen/src/github.com/google/flatbuffers/go
cp -a ./go_test.go ./go_gen/src/flatbuffers_test/
cp -a ./go_map_set_test/*.go ./go_gen/src/go_map_set_test/

# This layout needs GOPATH mode, so go modules are turned off for these go
# commands only (https://stackoverflow.com/a/63545857/7024978). `go env -w`
# would instead write the user's go env file and change every later Go build
# on the machine, and a failing run exited before the line that set it back.
#
# Run tests with necessary flags.
# Developers may wish to see more detail by appending the verbosity flag
# -test.v to arguments for this command, as in:
#   go -test -test.v ...
# Developers may also wish to run benchmarks, which may be achieved with the
# flag -test.bench and the wildcard regexp ".":
#   go -test -test.bench=. ...
GO_TEST_RESULT=0
GO111MODULE=off GOPATH=${go_path} go test flatbuffers_test \
    --coverpkg=github.com/google/flatbuffers/go \
    --cpp_data="${test_dir}/monsterdata_test.mon" \
    --out_data="${test_dir}/monsterdata_go_wire.mon" \
    --bench=. \
    --benchtime=3s \
    --fuzz=true \
    --fuzz_fields=4 \
    --fuzz_objects=10000 || GO_TEST_RESULT=$?

# The flags above are defined by go_test.go, so this package runs on its own.
GO111MODULE=off GOPATH=${go_path} go test go_map_set_test || GO_TEST_RESULT=$?

rm -rf "${go_path:?}"/{pkg,src}
if [[ $GO_TEST_RESULT == 0 ]]; then
    echo "OK: Go tests passed."
else
    echo "KO: Go tests failed."
    exit 1
fi

NOT_FMT_FILES=$(gofmt -l .)
if [[ ${NOT_FMT_FILES} != "" ]]; then
    echo "These files are not well gofmt'ed:"
    echo
    echo "${NOT_FMT_FILES}"
    # enable this when enums are properly formated
    # exit 1
fi
