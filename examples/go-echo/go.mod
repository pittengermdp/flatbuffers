module echo

go 1.24

require github.com/google/flatbuffers v0.0.0-00010101000000-000000000000

// Build against the runtime in this repository: the code flatc generates here
// calls verifier APIs that no published release of the runtime has.
replace github.com/google/flatbuffers => ../..
