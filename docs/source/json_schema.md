# JSON and JSON Schema

`flatc` reads and writes FlatBuffers as JSON (`--binary` / `--json`) and
describes that JSON with JSON Schema (`--jsonschema`). The schema follows what
the JSON parser actually accepts, so a document a JSON Schema validator
accepts is one `flatc --binary` can read.

```sh
flatc --jsonschema myschema.fbs        # writes myschema.schema.json
```

## How each kind of field is spelled

| Field | JSON | JSON Schema |
| --- | --- | --- |
| integer | number | `integer` bounded to the type's width |
| 64-bit integer, with `--json-int64-as-string` | quoted decimal string (number also read) | number **or** string matching an exact-range pattern |
| float, double | number | `number` |
| bool | `true` / `false` | `boolean` |
| optional scalar (`= null`) | the value, `null`, or absent | the type or `null` |
| enum | variant name | string from the variant names |
| `bit_flags` enum | space-separated names, e.g. `"A C"`; number when zero or a bit has no name | either form |
| string | string | `string` |
| struct | object with **every** field | object, all fields required |
| table | object; unknown members are rejected | object, `additionalProperties: false` |
| union `x` | `x_type` names the variant, `x` holds its value | value needs a type; NONE forbids a value; the value must match the named variant |
| vector | array | array of the element schema |
| `map<K, V>` | object keyed by the map keys | object; property names match `K`, values match `V` |
| `set<K>` | array of the keys | array of unique keys |

Map keys are JSON property names, so they are always strings: an enum key is
its variant name, a bool `"true"` / `"false"`, an integer its decimal digits.

A union value given as `null` is read by the parser as absent; the schema does
not accept it, keeping `null` for the one place it means something, an optional
scalar.

## Options

| Option | Effect |
| --- | --- |
| `--jsonschema-2020-12` | Draft 2020-12 with `$defs` (default: 2019-09 with `definitions`). |
| `--jsonschema-cross-file-refs` | Each file holds only the types its `.fbs` declares and refers to an included file's types by that file's schema, e.g. `"$ref": "common.schema.json#/$defs/Api_Error"`. Each file gets a relative `$id` (`<name>.schema.json`), so loading every file into one validator resolves every reference. Without it, each file copies its whole include tree. |
| `--jsonschema-defaults` | A `default` keyword on every defaulted table scalar, spelled as the printer writes it (variant names for enums). |
| `--json-int64-as-string` | Text output quotes `long` / `ulong` values; JSON Schema accepts number or string. A reader that holds numbers as doubles (JavaScript's `JSON.parse`) rounds integers above 2^53, so the string form is the only one that survives it. The pattern is generated per type and accepts exactly the canonical decimal spellings of its range: no sign on positives, no leading zeros, nothing past the bound. |

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
each value; on a set, each key. A float bound must be quoted
(`json_minimum: "-0.5"`), because attribute values cannot be float literals; it
is written to the schema as a number.

Misuse stops generation rather than dropping the rule: an attribute on a field
of the wrong kind, a `json_` name that is not in the table above (a typo such as
`json_minimun`), and, under `--json-int64-as-string`, a numeric bound on a
64-bit field, which its string form would escape.

These attributes are enforced by JSON Schema validators only. The FlatBuffers
JSON parser and binary readers do not check them.
