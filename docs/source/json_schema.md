# JSON and JSON Schema

`flatc` reads and writes FlatBuffers as JSON (`--binary` / `--json`) and
describes that JSON with JSON Schema (`--jsonschema`). The schema follows what
the JSON parser actually accepts, so a document a JSON Schema validator
accepts is one `flatc --binary` can read -- with the exceptions listed under
[What a validator's verdict means](#what-a-validators-verdict-means).

```sh
flatc --jsonschema myschema.fbs        # writes myschema.schema.json
```

## How each kind of field is spelled

| Field | JSON | JSON Schema |
| --- | --- | --- |
| integer | number | `integer` bounded to the type's width |
| 64-bit integer, with `--json-int64-as-string` | quoted decimal string (number also read) | number up to ±(2^53 − 1), **or** string matching an exact-range pattern |
| float, double | number; with `--strict-json`, a NaN or infinity prints quoted (`"nan"`, `"inf"`, `"-inf"`), which the parser reads back | `number` bounded to the type's largest finite value (so no NaN or infinity) |
| bool | `true` / `false` | `boolean` |
| optional scalar (`= null`) | the value, `null`, or absent | the type or `null` |
| enum | variant name | string from the variant names |
| `bit_flags` enum | space-separated names, e.g. `"A C"`; number when zero or a bit has no name | either form (a 64-bit number may be a string under `--json-int64-as-string`) |
| string | string | `string` |
| struct | object with **every** field | object, all fields required |
| table | object; unknown members are rejected | object, `additionalProperties: false` |
| union `x` | `x_type` names the variant, `x` holds its value | `x` is an object; on the enclosing table: a value needs a type, NONE forbids a value, and the value must match the variant `x_type` names |
| vector | array | array of the element schema |
| `map<K, V>` | object keyed by the map keys | object; property names match `K`, values match `V` (a string or table value may also be `null`) |
| `set<K>` | array of the keys | array of unique keys |

Map keys are JSON property names, so they are always strings: an enum key is
its variant name, a `bit_flags` key its flag names or, when they cannot spell
it, its decimal digits (`"0"`), a bool `"true"` / `"false"`, an integer its
decimal digits. An entry without its string or table value prints as `null`
and reads back as an entry without one.

A union's value is not described by an `anyOf` of its variants: the pairing
on the enclosing table already says which variant it must be, and an `anyOf`
would hide the variants' defaults from a validator that fills them in (Ajv's
`useDefaults` refuses to compile such a schema in strict mode).

A union value given as `null` is read by the parser as absent; the schema does
not accept it, keeping `null` for the one place it means something, an optional
scalar.

## Options

| Option | Effect |
| --- | --- |
| `--jsonschema-2020-12` | Draft 2020-12 with `$defs` (default: 2019-09 with `definitions`). |
| `--jsonschema-cross-file-refs` | Each file holds only the types its `.fbs` declares and refers to an included file's types by that file's schema, e.g. `"$ref": "common.schema.json#/$defs/Api_Error"`. Each file gets a relative `$id` (`<name>.schema.json`), so loading every file into one validator resolves every reference. Without it, each file copies its whole include tree. |
| `--jsonschema-defaults` | A `default` keyword on every defaulted table scalar, spelled as the printer writes it (variant names for enums; floats in their shortest exact form). A default outside the field's own `json_*` bounds gets none: a validator that fills defaults in would otherwise turn a valid document invalid. |
| `--json-int64-as-string` | Text output quotes `long` / `ulong` values, including a 64-bit enum value no variant names; JSON Schema accepts number or string. A reader that holds numbers as doubles (JavaScript's `JSON.parse`) rounds integers above 2^53, so the string form is the only one that survives it -- and the number form is bounded to ±(2^53 − 1), so such a validator and an exact one agree on every number. The pattern is generated per type and accepts exactly the canonical decimal spellings of its range: no sign on positives, no leading zeros, nothing past the bound. |

A schema without a `root_type` still gets every definition; it simply has no
top-level `$ref`, so validate against a definition by name:
`myschema.schema.json#/definitions/MyNamespace_MyTable`.

## Validation attributes

Rules a FlatBuffer cannot express, such as a range or a pattern, can be attached
to fields as attributes and become JSON Schema keywords. Declare each attribute
you use; `flatc` requires user attributes to be declared.

```
attribute "json_minimum";
attribute "json_maximum";
attribute "json_pattern";

table Adapter {
  max_cars: ubyte = 8 (json_minimum: 1, json_maximum: 64);
  name: string (json_pattern: "^[a-z0-9-]+$");
}
```

| Attribute | Keyword | Applies to |
| --- | --- | --- |
| `json_minimum`, `json_maximum` | `minimum`, `maximum` | integers and floats |
| `json_exclusive_minimum`, `json_exclusive_maximum` | `exclusiveMinimum`, `exclusiveMaximum` | integers and floats |
| `json_min_length`, `json_max_length` | `minLength`, `maxLength` | strings |
| `json_pattern`, `json_format` | `pattern`, `format` | strings |
| `json_min_items`, `json_max_items` | `minItems`, `maxItems` (`minProperties`, `maxProperties` on a map) | vectors, maps, sets |

On a vector, number and string attributes constrain each element; on a map,
each value; on a set, each key. On a number, `json_minimum` and `json_maximum`
replace the range the type contributes rather than adding a second `minimum` /
`maximum` to the same object, clamped so they never widen it:
`ubyte (json_minimum: -9, json_maximum: 900)` is written as `0..255`. A float
bound, or an integer one outside `int`'s range, must be quoted
(`json_minimum: "-0.5"`), because attribute values cannot be float or wide
literals; it is written to the schema as a number.

Misuse stops generation rather than dropping the rule or writing a schema
nothing passes: an attribute on a field of the wrong kind; a `json_` name that
is not in the table above (a typo such as `json_minimun`); a bound no value of
the type can meet (`ubyte (json_minimum: 300)`), or a maximum below the
minimum; and, under `--json-int64-as-string`, a numeric bound on a 64-bit
field, which its string form would escape.

Two types whose names map to the same definition name also stop generation:
`A.B.C` and `A.B_C` would both be `A_B_C`, and a reference to it would be
ambiguous.

These attributes are enforced by JSON Schema validators only. The FlatBuffers
JSON parser and binary readers do not check them.

## What a validator's verdict means

A document the schema accepts is one flatc and the Go readers below read,
except for what JSON Schema cannot see or says differently:

- **An integer spelled with a fraction or exponent** (`5.0`, `1e2`): JSON
  Schema counts it as an integer; flatc's parser does not (the Go readers do).
- **A member repeated in one object** (`{"a":1,"a":2}`): JSON Schema sees only
  one of them; flatc and the Go readers refuse the document.

Validators also differ among themselves where JSON numbers exceed a double. Ajv
reads numbers with JavaScript's `JSON.parse`, so it judges the double nearest
the number written, not the number itself:

- **64-bit integers written as numbers** without `--json-int64-as-string`:
  past 2^53 Ajv sees a rounded value (2^64, one past `ulong`'s largest, passes).
  With the option the number form stops at 2^53 − 1, where every number is
  exact, and the string form is exact everywhere.
- **A float literal within one rounding step of the type's largest value**:
  an exact validator compares the literal, Ajv the double it rounds to.

Outside those, Ajv and an exact validator give the same verdict.

## Canonical JSON in Go

flatc's JSON allows more than one spelling of the same value. Canonical JSON
picks one, so the same message is always the same bytes:

- a value equal to its default is omitted, as are an empty string and an empty
  vector, map or set, unless the field is required (an Object API value cannot
  tell absent from zero or empty either);
- members appear in declaration order, with no whitespace;
- with `--json-int64-as-string`, `long` and `ulong` are quoted, and so is a
  64-bit enum value no variant names;
- a string's invalid UTF-8 is written as U+FFFD; a map key or set member that
  is not valid UTF-8 is an error, since two such keys could become one.

Reading accepts every spelling flatc's parser does for these values, plus a
number for any enum, so any document the JSON Schema accepts can be read
(see [What a validator's verdict means](#what-a-validators-verdict-means)). Like
flatc's parser it refuses an object that repeats a member and anything but
whitespace after the document. A number is a JSON number, or for an integer a
string of an optional `-` and digits: hexadecimal, `1_0`, `inf` and the like
are refused, and a huge exponent is out of range at once, without building the
number. Documents may nest objects and arrays `flatbuffers.JSONMaxDepth` (256)
deep, reading and writing.

A map read from two spellings of one key (`"1"` and `"01"`), or a buffer whose
map or set holds one key twice, keeps the last entry, as `UnPack` does.

### Generated methods: `--go-json`

```sh
flatc --go --gen-object-api --go-json --json-int64-as-string myschema.fbs
```

Every table and struct Object API type (`FooT`) gets:

| Method | Does |
| --- | --- |
| `MarshalFlatJSON() ([]byte, error)` | canonical JSON for the value |
| `WriteFlatJSON(w *flatbuffers.JSONWriter)` | the same, into a writer shared with enclosing values |
| `UnmarshalFlatJSON(data []byte) error` | replaces the value; absent scalars take their schema defaults |
| `UnmarshalFlatJSONValue(v any, pointer string) error` | the same, for a value `flatbuffers.ParseJSON` already parsed |
| `FlatJSONSchema() (file, definition string)` | the JSON Schema file and definition `--jsonschema` writes for the type |

They satisfy `flatbuffers.FlatJSONMessage`. The names deliberately are not
`MarshalJSON` / `UnmarshalJSON`, so `encoding/json` keeps encoding Object API
values exactly as before.

A union field `x` is read and written through methods on its `XT` type and an
`UnmarshalXFlatJSON` function, so a union declared in another package needs
nothing extra. `map<K, V>` fields write their keys in FlatBuffers key order
(strings bytewise, numbers and enums by value), the order the binary holds
them in.

Errors are `*flatbuffers.JSONError`, located by an RFC 6901 JSON Pointer:
`/payload/id: "x" is not an integer`.

Not supported, and rejected at generation time: fixed-size arrays,
`nested_flatbuffer`, `flexbuffer`, vectors of unions, and 64-bit offsets
(`offset64`, `vector64`).

### Without generated code: schema reflection

The same format is available for any table a binary schema (`.bfbs`)
describes:

```go
schema, _ := flatbuffers.LoadReflectionSchema(bfbs)
opts := flatbuffers.CanonicalJSONOptions{Int64AsString: true}

js, err := flatbuffers.ReflectToCanonicalJSON(schema, "MyGame.Monster", buf, opts)
m, err := flatbuffers.ReflectToCanonicalMap(schema, "MyGame.Monster", buf, opts)
buf, err := flatbuffers.ReflectFromCanonicalJSON(schema, "MyGame.Monster", js)
buf, err := flatbuffers.ReflectFromCanonicalMap(schema, "MyGame.Monster", m)
```

`ReflectToCanonicalJSON` writes exactly the bytes `MarshalFlatJSON` writes for
the same message after `UnPack`; each side reads the other's output back to the
same value. Where a buffer holds what an Object API value cannot -- a union
type without its value, a union variant from a newer schema, a map key twice
-- it writes what the generated code writes once `UnPack` has dropped it.

A buffer is untrusted input. One whose offsets loop, or that shares tables so
it unfolds into an enormous document, is an error rather than a crash: writing
stops at `JSONMaxDepth` and after a million tables, as FlatBuffers' verifier
does by default. A field canonical JSON cannot spell (`vector64`, a fixed-size
array) is an error both ways, never silently dropped.
A map passed to `ReflectFromCanonicalMap` may hold plain Go numbers (`int`,
`uint64`, `float64`, …) as well as `json.Number` and strings.

Generate the `.bfbs` with `--bfbs-builtins`: `bit_flags`, `map<K, V>` and
`set<K>` are recorded as built-in attributes, which a binary schema otherwise
leaves out. Without them, bit flags read and write as plain numbers and maps
and sets as arrays of entry tables.

`ReflectPack` and `ReflectUnpack` are unchanged and keep their own map format.
