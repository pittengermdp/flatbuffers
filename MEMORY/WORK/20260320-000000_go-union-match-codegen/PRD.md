---
task: Implement Go union Match method codegen in flatc
slug: 20260320-000000_go-union-match-codegen
effort: standard
phase: complete
progress: 13/13
mode: interactive
started: 2026-03-20T00:00:00Z
updated: 2026-03-20T00:03:00Z
---

## Context

Add `GenUnionMatch` to `idl_gen_go.cpp` that generates a `Match()` method on Go union `*T` types. The method provides exhaustive typed callbacks — one per non-NONE variant — enabling type-safe dispatch on union values. This is a pure codegen addition to the flatc compiler, called from `GenNativeUnionCreator` after the existing 3 union codegen calls.

### Risks
- `namer_.Variant(ev)` returns kKeep cased name (e.g. `StatusMessage`), so callback param is `"on" + namer_.Variant(ev)` = `onStatusMessage`
- Type for params and assertions is `NativeType(ev.union_type)` which returns `*StatusMessageT` for struct union variants
- Must not break any existing test — only adds new generated output

## Criteria

- [x] ISC-1: `GenUnionMatch` method exists in `idl_gen_go.cpp`
- [x] ISC-2: `GenUnionMatch` is called from `GenNativeUnionCreator` after the 3 existing calls
- [x] ISC-3: Generated Match function has receiver `(u *MessageContentT)`
- [x] ISC-4: Generated Match function has one callback param per non-NONE variant
- [x] ISC-5: Each callback param type is the correct `*FooT` native type
- [x] ISC-6: Generated Match function has nil guard (`if u == nil { return }`)
- [x] ISC-7: Generated Match function uses type switch on `u.Type`
- [x] ISC-8: Each case uses type assertion `u.Value.(*FooT)` before calling callback
- [x] ISC-9: flatc rebuilds successfully with `make flatc`
- [x] ISC-10: Match method appears in generated `Any.go` for monster_test.fbs
- [x] ISC-11: `go vet` passes on generated Go code
- [x] ISC-A1: No existing generated code patterns changed
- [x] ISC-A2: No `#[allow]`, `//nolint`, or lint suppression attributes added

## Decisions

## Verification

- ISC-1: `grep -n "GenUnionMatch" idl_gen_go.cpp` → lines 206, 1335
- ISC-2: Line 206 is inside GenNativeUnionCreator, after GenNativeUnionUnPack (line 205)
- ISC-3-8: Generated output for `AnyT.Match` verified via grep on `/tmp/gopath_test/src/MyGame/Example/Any.go`
- ISC-9: `make flatc` completed `[100%] Built target flatc`
- ISC-10: grep shows Match method in Any.go, AnyUniqueAliases.go, AnyAmbiguousAliases.go
- ISC-11: `GO111MODULE=off GOPATH=/tmp/gopath_test go vet MyGame/...` → zero output (pass)
- ISC-A1: No modifications to GenNativeUnion, GenNativeUnionPack, GenNativeUnionUnPack
- ISC-A2: No allow/nolint attributes in any changed code
