# Go Echo Example

A simple example demonstrating how to send flatbuffers over the network in Go.

## Generate flatbuffer code

```
flatc -g --gen-object-api --go-module-name echo hero.fbs net.fbs
```

The generated code is committed. `scripts/generate_code.py` regenerates it with
this command and formats it with `gofmt`, so it always matches the `flatc` in
this repository.

## Running example

`go.mod` points `github.com/google/flatbuffers` at this repository (`replace
github.com/google/flatbuffers => ../..`), because the generated code calls
runtime APIs, such as the verifier, that no published release has. Run the
example from inside a checkout of this repository; there is nothing to download.

1. Start a server
```
go run server/server.go
```

2. Run the client in another terminal
```
go run client/client.go
```
