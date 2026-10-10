// Copyright 2026 Google Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

package go_canonical_json_test

import (
	"encoding/json"
	"os"
	"os/exec"
	"path/filepath"
	"reflect"
	"regexp"
	"strings"
	"testing"
)

const jsonAttributes = `attribute "json_minimum"; attribute "json_maximum";
attribute "json_exclusive_minimum"; attribute "json_exclusive_maximum";
`

// genSchema has flatc --jsonschema generate the schema of one .fbs source,
// returning the parsed schema, or flatc's output when generation fails.
func genSchema(t *testing.T, fbs string, extra ...string) (map[string]any, string, bool) {
	t.Helper()
	requireFlatc(t)
	dir := t.TempDir()
	in := filepath.Join(dir, "s.fbs")
	if err := os.WriteFile(in, []byte(fbs), 0o600); err != nil {
		t.Fatal(err)
	}
	args := append([]string{"--jsonschema", "--jsonschema-2020-12", "-o", dir}, extra...)
	out, err := exec.Command(*flatcPath, append(args, in)...).CombinedOutput()
	if err != nil {
		return nil, string(out), false
	}
	data, err := os.ReadFile(filepath.Join(dir, "s.schema.json"))
	if err != nil {
		t.Fatal(err)
	}
	var schema map[string]any
	dec := json.NewDecoder(strings.NewReader(string(data)))
	dec.UseNumber()
	if err := dec.Decode(&schema); err != nil {
		t.Fatalf("flatc wrote invalid JSON: %v\n%s", err, data)
	}
	return schema, "", true
}

// property returns a definition's property schema.
func property(t *testing.T, schema map[string]any, def, prop string) map[string]any {
	t.Helper()
	d, _ := schema["$defs"].(map[string]any)[def].(map[string]any)
	p, _ := d["properties"].(map[string]any)[prop].(map[string]any)
	if p == nil {
		t.Fatalf("no %s.%s in %v", def, prop, schema)
	}
	return p
}

func expectKeywords(t *testing.T, got map[string]any, want map[string]string) {
	t.Helper()
	for k, v := range want {
		n, ok := got[k].(json.Number)
		if !ok || string(n) != v {
			t.Errorf("%s = %v, want %s (in %v)", k, got[k], v, got)
		}
	}
}

// A bound no value of the field's type can meet, two names for one
// definition, or contradictory bounds fail generation with a message saying
// why, rather than giving a schema nothing (or the wrong thing) passes.
func TestJSONSchemaGenerationErrors(t *testing.T) {
	cases := map[string]string{
		jsonAttributes + `table T { f: ubyte (json_minimum: 300); }`:                    "above the largest value",
		jsonAttributes + `table T { f: byte (json_maximum: -200); }`:                    "below the smallest value",
		jsonAttributes + `table T { f: int (json_minimum: 5, json_maximum: 4); }`:       "no value could pass",
		jsonAttributes + `table T { f: float (json_minimum: "1e39"); }`:                 "above the largest value",
		jsonAttributes + `table T { f: double (json_maximum: "-1e309"); }`:              "finite number",
		`namespace A.B; table C { x: int; } namespace A; table B_C { y: int; }`:         "would share the JSON Schema definition name `A_B_C`",
		`namespace A; enum B_C : byte { X } namespace A.B; table C { e: A.B_C; }`:       "would share the JSON Schema definition name `A_B_C`",
		jsonAttributes + `table T { f: long (json_maximum: 5); }`:                       "",
		jsonAttributes + `table T { f: ulong (json_maximum: "18446744073709551615"); }`: "",
	}
	for fbs, want := range cases {
		_, out, ok := genSchema(t, fbs)
		if want == "" {
			if !ok {
				t.Errorf("%s: generation failed: %s", fbs, out)
			}
			continue
		}
		if ok || !strings.Contains(out, want) {
			t.Errorf("%s: got ok=%v %q, want a failure saying %q", fbs, ok, out, want)
		}
	}
}

func TestJSONSchemaBounds(t *testing.T) {
	schema, out, ok := genSchema(t, jsonAttributes+`
table T {
  neg_max: byte (json_maximum: -5);
  big_max: ulong (json_maximum: "18446744073709551615");
  ratio: float (json_minimum: "-0.5");
  any_float: float;
  any_double: double;
}`)
	if !ok {
		t.Fatal(out)
	}
	expectKeywords(t, property(t, schema, "T", "neg_max"), map[string]string{"minimum": "-128", "maximum": "-5"})
	expectKeywords(t, property(t, schema, "T", "big_max"), map[string]string{"minimum": "0", "maximum": "18446744073709551615"})
	expectKeywords(t, property(t, schema, "T", "ratio"), map[string]string{"minimum": "-0.5", "maximum": "3.4028235e+38"})
	expectKeywords(t, property(t, schema, "T", "any_float"), map[string]string{"minimum": "-3.4028235e+38", "maximum": "3.4028235e+38"})
	expectKeywords(t, property(t, schema, "T", "any_double"),
		map[string]string{"minimum": "-1.7976931348623157e+308", "maximum": "1.7976931348623157e+308"})

	// Offered alongside the exact string form, a 64-bit number is bounded to
	// what a double holds exactly, so a validator that reads JSON numbers as
	// doubles judges it as an exact one does.
	schema, out, ok = genSchema(t, `table T { l: long; u: ulong; }`, "--json-int64-as-string")
	if !ok {
		t.Fatal(out)
	}
	for _, prop := range []string{"l", "u"} {
		number := property(t, schema, "T", prop)["anyOf"].([]any)[0].(map[string]any)
		expectKeywords(t, number, map[string]string{"maximum": "9007199254740991"})
	}
}

// Defaults are written exactly, and left out when the field's own bounds
// would reject them: a validator filling them in (Ajv's useDefaults) would
// otherwise turn a valid document invalid.
func TestJSONSchemaDefaults(t *testing.T) {
	schema, out, ok := genSchema(t, jsonAttributes+`
table T {
  tiny: double = 1e-20;
  tenth: float = 0.1;
  third: double = 0.333333333333333314829616256247390992939472198486328125;
  unset: short = 0 (json_minimum: 1, json_maximum: 64);
  set: short = 5 (json_minimum: 1, json_maximum: 64);
  edge: int = 3 (json_exclusive_maximum: 3);
}`, "--jsonschema-defaults")
	if !ok {
		t.Fatal(out)
	}
	expectKeywords(t, property(t, schema, "T", "tiny"), map[string]string{"default": "1e-20"})
	expectKeywords(t, property(t, schema, "T", "tenth"), map[string]string{"default": "0.1"})
	expectKeywords(t, property(t, schema, "T", "third"), map[string]string{"default": "0.3333333333333333"})
	expectKeywords(t, property(t, schema, "T", "set"), map[string]string{"default": "5"})
	for _, prop := range []string{"unset", "edge"} {
		if d, has := property(t, schema, "T", prop)["default"]; has {
			t.Errorf("%s has default %v outside its bounds", prop, d)
		}
	}
}

// A union value is an object; the variant it must be is the type field's
// pairing, so an empty union needs no anyOf (an empty one is not valid).
// A map's bit_flags key may be its names or digits, and a table map value
// may be null, as flatc prints a missing one.
func TestJSONSchemaShapes(t *testing.T) {
	schema, out, ok := genSchema(t, `
enum B : ubyte (bit_flags) { P, Q }
union Empty {}
table V { n: int; }
table T { e: Empty; by_bits: map<B, int>; tables: map<string, V>; }`)
	if !ok {
		t.Fatal(out)
	}
	if got := property(t, schema, "T", "e"); !reflect.DeepEqual(got, map[string]any{"type": "object"}) {
		t.Errorf("empty union value: %v", got)
	}

	keys := property(t, schema, "T", "by_bits")["propertyNames"].(map[string]any)["anyOf"].([]any)
	names := regexp.MustCompile(keys[0].(map[string]any)["pattern"].(string))
	digits := regexp.MustCompile(keys[1].(map[string]any)["pattern"].(string))
	for key, want := range map[string]bool{"P": true, "P Q": true, "0": true, "8": true, "255": true, "256": false, "R": false} {
		if got := names.MatchString(key) || digits.MatchString(key); got != want {
			t.Errorf("bit_flags key %q accepted=%v, want %v (%v)", key, got, want, keys)
		}
	}

	value := property(t, schema, "T", "tables")["additionalProperties"].(map[string]any)
	if value["else"] == nil || value["then"] == nil {
		t.Errorf("table map value: %v", value)
	}
}
