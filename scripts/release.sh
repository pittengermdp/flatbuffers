#!/usr/bin/bash

# Cut a release of the LN2 flatbuffers fork at a given SEMANTIC version.
#
# This fork versions on the semver 0.x line (see CHANGELOG.md), NOT the upstream
# calendar-based scheme, so the target version is passed in explicitly rather
# than derived from today's date.
#
# Usage:  scripts/release.sh <major.minor.patch>      e.g.  scripts/release.sh 0.5.1
#
# Full release procedure:
#  1. Ensure the repo builds and ./flattests passes first.
#  2. Run this script -- it rewrites every version string in the tree.
#  3. rm -rf build && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
#       -DFLATBUFFERS_BUILD_TESTS=ON && cmake --build build --target flatc
#  4. Confirm the version is baked into flatc: ./build/flatc --version
#  5. scripts/generate_code.py --flatc "$(pwd)/build/flatc"
#  6. goldens/generate_code.py --flatc "$(pwd)/build/flatc"   # if present
#  7. cmake --build build && ./build/flattests
#  8. git grep the old version -- it should only remain in CHANGELOG.md files.
#  9. Update CHANGELOG.md.
# 10. git commit -m "FlatBuffers Version X.Y.Z"
# 11. git tag -a -m "FlatBuffers Version X.Y.Z" vX.Y.Z
# 12. git push origin master && git push origin vX.Y.Z

# Requires the xmlstarlet command (apt install xmlstarlet / brew install xmlstarlet)
# and GNU sed. On macOS run under `gsed`-backed PATH or in a Linux container.
if ! command -v xmlstarlet > /dev/null 2>&1; then
    echo "xmlstarlet could not be found (apt install xmlstarlet)"
    exit 1
fi

version="${1:-}"
if [[ ! $version =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "Usage: $0 <major.minor.patch>   (semver, e.g. 0.5.1)"
    exit 1
fi
IFS=. read -r major minor patch <<< "$version"
version_underscore="${major}_${minor}_${patch}"

echo "Setting FlatBuffers version to: $version"

echo "Updating include/flatbuffers/base.h..."
sed -i \
    -e "s/\(#define FLATBUFFERS_VERSION_MAJOR \).*/\1$major/" \
    -e "s/\(#define FLATBUFFERS_VERSION_MINOR \).*/\1$minor/" \
    -e "s/\(#define FLATBUFFERS_VERSION_REVISION \).*/\1$patch/" \
    include/flatbuffers/base.h

echo "Updating CMake/Version.cmake..."
sed -i \
    -e "s/\(set(VERSION_MAJOR \).*/\1$major)/" \
    -e "s/\(set(VERSION_MINOR \).*/\1$minor)/" \
    -e "s/\(set(VERSION_PATCH \).*/\1$patch)/" \
    CMake/Version.cmake

# Committed generated headers embed a version static_assert. Normalize any prior
# value to the new one; this covers the files that scripts/generate_code.py and
# goldens/generate_code.py do NOT regenerate (reflection, goldens, android,
# benchmarks, and the 64bit/key_field/etc. committed corpus).
echo "Normalizing static_assert version checks in committed generated code..."
for f in $(git grep -lE 'FLATBUFFERS_VERSION_(MAJOR|MINOR|REVISION) (==|>=) [0-9]+' \
    -- ':(exclude)CHANGELOG.md' ':(exclude)scripts/release.sh' || true); do
    sed -i -E \
        -e "s/(FLATBUFFERS_VERSION_MAJOR (==|>=) )[0-9]+/\1$major/" \
        -e "s/(FLATBUFFERS_VERSION_MINOR (==|>=) )[0-9]+/\1$minor/" \
        -e "s/(FLATBUFFERS_VERSION_REVISION (==|>=) )[0-9]+/\1$patch/" \
        "$f"
done

# The C#/Java/Kotlin/Swift version-check function name embeds the version and is
# HARDCODED in the generators (src/idl_gen_{csharp,java,kotlin,swift}.cpp), the
# language runtimes, and the generated corpus. Rename all of them together so the
# definition and every call stay in sync.
echo "Renaming FLATBUFFERS_x_y_z()/FlatBuffersVersion_x_y_z() version-check functions..."
for f in $(git grep -lE 'FLATBUFFERS_[0-9]+_[0-9]+_[0-9]+|FlatBuffersVersion_[0-9]+_[0-9]+_[0-9]+' \
    -- ':(exclude)scripts/release.sh' || true); do
    sed -i -E \
        -e "s/FLATBUFFERS_[0-9]+_[0-9]+_[0-9]+/FLATBUFFERS_$version_underscore/g" \
        -e "s/FlatBuffersVersion_[0-9]+_[0-9]+_[0-9]+/FlatBuffersVersion_$version_underscore/g" \
        "$f"
done

echo "Updating java/pom.xml..."
xmlstarlet edit --inplace -N s=http://maven.apache.org/POM/4.0.0 \
    --update '//s:project/s:version' --value "$version" \
    java/pom.xml

echo "Updating package.json..."
sed -i -e "s/\(\"version\": \).*/\1\"$version\",/" package.json

echo "Updating library.json..."
sed -i -e "s/\(\"version\": \).*/\1\"$version\",/" library.json

echo "Updating net/FlatBuffers/Google.FlatBuffers.csproj..."
sed -i -e "s/\(<PackageVersion>\).*\(<\/PackageVersion>\)/\1$version\2/" \
    net/FlatBuffers/Google.FlatBuffers.csproj

echo "Updating dart/pubspec.yaml..."
sed -i -e "s/\(version: \).*/\1$version/" dart/pubspec.yaml

echo "Updating python/flatbuffers/_version.py..."
sed -i -e "s/\(__version__ = \).*/\1\"$version\"/" python/flatbuffers/_version.py

echo "Updating python/setup.py..."
sed -i -e "s/\(version='\).*/\1$version',/" python/setup.py

echo "Updating rust/flatbuffers/Cargo.toml..."
sed -i "s/^version = \".*\"$/version = \"$version\"/" rust/flatbuffers/Cargo.toml

echo "Updating rust/reflection/Cargo.toml..."
sed -i "s/^version = \".*\"$/version = \"$version\"/" rust/reflection/Cargo.toml

echo "Updating rust/flexbuffers/Cargo.toml..."
sed -i "s/^version = \".*\"$/version = \"$version\"/" rust/flexbuffers/Cargo.toml

echo "Updating FlatBuffers.podspec..."
sed -i -e "s/\(s.version[[:space:]]*= \).*/\1'$version'/" FlatBuffers.podspec

echo "Updating MODULE.bazel..."
sed -i "3s/version = \".*\"/version = \"$version\"/" MODULE.bazel

echo
echo "Version strings updated to $version. Now rebuild flatc, regenerate the"
echo "corpus (scripts/generate_code.py + goldens/generate_code.py), run"
echo "./flattests, update CHANGELOG.md, then commit and tag v$version."
