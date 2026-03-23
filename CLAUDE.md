# flatbuffers (Google FlatBuffers Fork)

## Overview
Liftnet2 fork of Google's FlatBuffers serialization library. Multi-language (C++, Java, Go, TypeScript, Rust, etc.). Contains the flatc compiler and runtime libraries.

## Build
```bash
cmake -G "Unix Makefiles" && make
```

## Key Files
- `CMakeLists.txt` — Main build configuration
- `configure_*.py` — QuestDB integration scripts

## Notes
- This is an external dependency fork — be careful with modifications
- Used to compile .fbs schemas into language-specific code
- Changes should generally go in flatbuffer_schemas, not here
