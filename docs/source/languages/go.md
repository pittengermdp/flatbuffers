Use in Go    {#flatbuffers_guide_use_go}
=========

## Before you get started

Before diving into the FlatBuffers usage in Go, it should be noted that
the [Tutorial](../tutorial.md) page has a complete guide
to general FlatBuffers usage in all of the supported languages (including Go).
This page is designed to cover the nuances of FlatBuffers usage, specific to
Go.

You should also have read the [Building](../building.md)
documentation to build `flatc` and should be familiar with
[Using the schema compiler](../flatc.md) and
[Writing a schema](../schema.md).

## FlatBuffers Go library code location

The code for the FlatBuffers Go library can be found at
`flatbuffers/go`. You can browse the library code on the [FlatBuffers
GitHub page](https://github.com/google/flatbuffers/tree/master/go).

## Testing the FlatBuffers Go library

The code to test the Go library can be found at `flatbuffers/tests`.
The test code itself is located in [go_test.go](https://github.com/google/
flatbuffers/blob/master/tests/go_test.go).

To run the tests, use the [GoTest.sh](https://github.com/google/flatbuffers/
blob/master/tests/GoTest.sh) shell script.

*Note: The shell script requires [Go](https://golang.org/doc/install) to
be installed.*

## Using the FlatBuffers Go library

*Note: See [Tutorial](../tutorial.md) for a more in-depth
example of how to use FlatBuffers in Go.*

FlatBuffers supports reading and writing binary FlatBuffers in Go.

To use FlatBuffers in your own code, first generate Go classes from your
schema with the `--go` option to `flatc`. Then you can include both FlatBuffers
and the generated code to read or write a FlatBuffer.

For example, here is how you would read a FlatBuffer binary file in Go: First,
include the library and generated code. Then read a FlatBuffer binary file into
a `[]byte`, which you pass to the `GetRootAsMonster` function:

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~{.go}
    import (
       example "MyGame/Example"
       flatbuffers "github.com/google/flatbuffers/go"

       "os"
    )

    buf, err := os.ReadFile("monster.dat")
    // handle err
    monster := example.GetRootAsMonster(buf, 0)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Now you can access values like this:

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~{.go}
    hp := monster.Hp()
    pos := monster.Pos(nil)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~


In some cases it's necessary to modify values in an existing FlatBuffer in place (without creating a copy). For this reason, scalar fields of a Flatbuffer table or struct can be mutated.

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~{.go}
    monster := example.GetRootAsMonster(buf, 0)

    // Set table field.
    if ok := monster.MutateHp(10); !ok {
      panic("failed to mutate Hp")
    }

    // Set struct field.
    monster.Pos().MutateZ(4)

    // This mutation will fail because the mana field is not available in
    // the buffer. It should be set when creating the buffer.
    if ok := monster.MutateMana(20); !ok {
      panic("failed to mutate Hp")
    }
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The term `mutate` is used instead of `set` to indicate that this is a special use case. All mutate functions return a boolean value which is false if the field we're trying to mutate is not available in the buffer.

## Names in generated code

Generated code imports each referenced namespace as a package named after the
namespace, which may be a single word such as `x`, `o` or `t`, and refers to
the schema's own types, which may be named the same way. So that no name the
generator chooses can shadow one of those -- or `flatbuffers` and `math`,
which a field may also be called -- every receiver, parameter and local the
generated code declares ends in an underscore:

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~{.go}
    func (rcv_ *Monster) Pos(obj_ *Vec3) *Vec3
    func (t_ *MonsterT) Pack(builder_ *flatbuffers.Builder) flatbuffers.UOffsetT
    func MonsterAddHp(builder_ *flatbuffers.Builder, hpArg_ int16)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

A name derived from a field also carries a role before the underscore --
`hpArg_` in `MonsterAddHp`, `xArg_` in `CreateVec3`, `nameOffset_` in
`MonsterT.Pack` -- so a field named `builder` cannot collide with the
`builder_` parameter beside it.

Parameter names are not part of a Go function's type, so callers are
unaffected. A schema name that itself ends in an underscore can still collide
with a generated name. Separately, a namespace cannot be named after a Go
keyword (`func`, `range`), a predeclared identifier (`nil`, `len`, `error`) or
a package the generated code imports (`flatbuffers`, `bytes`, `math`, `sort`,
`strconv`): its package name itself conflicts.

## Text Parsing

There currently is no support for parsing text (Schema's and JSON) directly
from Go, though you could use the C++ parser through cgo. Please see the
C++ documentation for more on text parsing.

<br>
