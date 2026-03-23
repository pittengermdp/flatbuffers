---
task: FlatBuffers verifier and reflection research across languages
slug: 20260320-000000_flatbuffers-verifier-reflection-research
effort: extended
phase: complete
progress: 18/18
mode: interactive
started: 2026-03-20T00:00:00Z
updated: 2026-03-20T00:00:00Z
---

## Context

Research task investigating FlatBuffers verifier and reflection support across languages — specifically Go and TypeScript gaps in the upstream Google repo, community interest in those gaps, how peer serialization frameworks handle buffer verification, and any notable forks/libraries filling the gap. This is a focused research deliverable to inform potential implementation work in the LN2 codebase.

## Criteria

- [x] ISC-1: Upstream Go directory surveyed for verifier presence or absence — ABSENT
- [x] ISC-2: Upstream TypeScript directory surveyed for verifier presence or absence — ABSENT
- [x] ISC-3: Go reflection support in upstream repo confirmed or denied — ABSENT (C++ only)
- [x] ISC-4: TypeScript reflection support in upstream repo confirmed or denied — ABSENT
- [x] ISC-5: Open GitHub issues requesting Go verifier found — issues #7310, #5793 found, both closed
- [x] ISC-6: Open GitHub issues requesting TypeScript verifier found — none found specifically for TS verifier
- [x] ISC-7: Open GitHub PRs adding Go verifier found or absence noted — none found
- [x] ISC-8: Open GitHub PRs adding TypeScript verifier found or absence noted — none found
- [x] ISC-9: Protobuf Go validation pattern identified — protovalidate-go, `protovalidate.Validate(msg)` API
- [x] ISC-10: Protobuf TypeScript validation pattern — @bufbuild/protovalidate-es with type narrowing
- [x] ISC-11: Cap'n Proto buffer verification mechanism documented — traversal limit + depth limit, built-in
- [x] ISC-12: MessagePack verification patterns documented — no built-in; relies on decode-time panics
- [x] ISC-13: C++ FlatBuffers verifier API documented — `Verifier v(buf,len); VerifyMonsterBuffer(v);`
- [x] ISC-14: Rust FlatBuffers verifier API documented — `root::<T>(buf)` runs verifier; `root_unchecked` skips
- [x] ISC-15: Reflection support in C++ upstream confirmed — yes, flatbuffers/reflection.h + src/reflection.cpp
- [x] ISC-16: Reflection support in Rust upstream confirmed — yes, rust/reflection/src/lib.rs + flatbuffers-reflection crate
- [x] ISC-17: Notable forks adding Go verifier — none found; flatcc (pure C) has verifier, not Go
- [x] ISC-18: Notable third-party TS FlatBuffers verification — none found; mgit-at/typescript-flatbuffers-codegen exists but no verifier
- [x] ISC-A: No speculation — all claims sourced or confirmed absent via search

## Decisions

## Verification
