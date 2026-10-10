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

// Package go_canonical_json_test checks canonical JSON three ways: the Go
// methods flatc --go-json generates, schema reflection
// (ReflectToCanonicalJSON / ReflectFromCanonicalJSON), and flatc's own JSON
// parser and printer, which every value must survive in both directions.
// GoTest.sh generates the code and the .bfbs files these tests read.
package go_canonical_json_test

import (
	"bytes"
	"errors"
	"flag"
	"os"
	"os/exec"
	"path/filepath"
	"reflect"
	"strings"
	"testing"

	cj "CanonicalJson"
	inc "CanonicalJsonInc"
	mapset "github.com/google/flatbuffers/MapSetTest"
	flatbuffers "github.com/google/flatbuffers/go"
	optional "optional_scalars"
)

var (
	flatcPath  = flag.String("flatc", "", "flatc binary; flatc round trips are skipped without it")
	schemasDir = flag.String("schemas", "", "directory holding the .fbs files")
)

// normalize turns empty slices and maps into nil, recursively: the Object
// API cannot tell an absent vector from an empty one, and neither can JSON.
func normalize(v reflect.Value) {
	switch v.Kind() {
	case reflect.Pointer, reflect.Interface:
		if !v.IsNil() {
			normalize(v.Elem())
		}
	case reflect.Struct:
		for i := 0; i < v.NumField(); i++ {
			f := v.Field(i)
			if (f.Kind() == reflect.Slice || f.Kind() == reflect.Map) && f.Len() == 0 && f.CanSet() {
				f.Set(reflect.Zero(f.Type()))
				continue
			}
			normalize(f)
		}
	case reflect.Slice:
		for i := 0; i < v.Len(); i++ {
			normalize(v.Index(i))
		}
	case reflect.Map:
		for _, k := range v.MapKeys() {
			normalize(v.MapIndex(k))
		}
	}
}

func equalT(t *testing.T, what string, want, got any) {
	t.Helper()
	normalize(reflect.ValueOf(want))
	normalize(reflect.ValueOf(got))
	if !reflect.DeepEqual(want, got) {
		t.Fatalf("%s differs:\nwant %#v\ngot  %#v", what, want, got)
	}
}

func requireFlatc(t *testing.T) {
	t.Helper()
	if *flatcPath == "" || *schemasDir == "" {
		t.Skip("--flatc and --schemas not given")
	}
}

func runFlatc(t *testing.T, args ...string) {
	t.Helper()
	out, err := exec.Command(*flatcPath, args...).CombinedOutput()
	if err != nil {
		t.Fatalf("flatc %v: %v\n%s", args, err, out)
	}
}

// flatcBinary has flatc's JSON parser read js and returns the binary.
func flatcBinary(t *testing.T, fbs string, js []byte) []byte {
	t.Helper()
	dir := t.TempDir()
	in := filepath.Join(dir, "doc.json")
	if err := os.WriteFile(in, js, 0o600); err != nil {
		t.Fatal(err)
	}
	runFlatc(t, "-b", "-o", dir, filepath.Join(*schemasDir, fbs), in)
	matches, _ := filepath.Glob(filepath.Join(dir, "doc.*"))
	for _, m := range matches {
		if !strings.HasSuffix(m, ".json") {
			b, err := os.ReadFile(m)
			if err != nil {
				t.Fatal(err)
			}
			return b
		}
	}
	t.Fatal("flatc wrote no binary")
	return nil
}

// flatcJSON has flatc's printer write bin as JSON.
func flatcJSON(t *testing.T, fbs string, bin []byte, extra ...string) []byte {
	t.Helper()
	dir := t.TempDir()
	in := filepath.Join(dir, "doc.bin")
	if err := os.WriteFile(in, bin, 0o600); err != nil {
		t.Fatal(err)
	}
	args := append([]string{"--json", "--strict-json", "--raw-binary"}, extra...)
	args = append(args, "-o", dir, filepath.Join(*schemasDir, fbs), "--", in)
	runFlatc(t, args...)
	b, err := os.ReadFile(filepath.Join(dir, "doc.json"))
	if err != nil {
		t.Fatal(err)
	}
	return b
}

func loadSchema(t *testing.T, name string) *flatbuffers.ReflectionSchema {
	t.Helper()
	b, err := os.ReadFile(name)
	if err != nil {
		t.Fatal(err)
	}
	s, err := flatbuffers.LoadReflectionSchema(b)
	if err != nil {
		t.Fatal(err)
	}
	return s
}

func i64(v int64) *int64 { return &v }

func roots() map[string]*cj.RootT {
	return map[string]*cj.RootT{
		"full": {
			Name:     "abc",
			Flags:    inc.FlagsA | inc.FlagsB,
			Mode:     inc.ModeOff,
			Cars:     64,
			Ratio:    0.25,
			Big:      18446744073709551615,
			Neg:      -9223372036854775808,
			MaybeBig: i64(0),
			Bigs:     []int64{1, -2, 9007199254740993},
			Tags:     []string{"t1", "t\"2", "line\nbreak"},
			Scores:   map[string]int64{"b": -1, "a": 9223372036854775807},
			ByMode:   map[inc.Mode]string{inc.ModeOn: "on", inc.ModeOff: "off"},
			Seen:     map[uint64]struct{}{18446744073709551615: {}, 0: {}},
			Pt:       &inc.PtT{X: -1, Y: 2},
			Payload:  &cj.PayloadT{Type: cj.PayloadCanonicalJsonInc_Leaf, Value: &inc.LeafT{Id: 9007199254740993, Note: "n"}},
			Leaf:     &inc.LeafT{Id: 1},
			Leaves:   []*inc.LeafT{{Id: 2}, {Note: "x"}},
			Pts:      []*inc.PtT{{X: 1, Y: 2}, {X: -3, Y: 4}},
		},
		"defaults":     {Name: "x", Flags: inc.FlagsA | inc.FlagsC, Mode: inc.ModeOn, Cars: 3, Ratio: 0.5, Big: 7},
		"non-defaults": {Name: "x", Flags: 0, Mode: inc.ModeOff, Cars: 0, Ratio: 0, Big: 0},
	}
}

func packRoot(r *cj.RootT) []byte {
	b := flatbuffers.NewBuilder(0)
	b.Finish(r.Pack(b))
	return b.FinishedBytes()
}

// Generated JSON survives flatc in both directions, and writing is a fixed
// point.
func TestGeneratedAgainstFlatc(t *testing.T) {
	requireFlatc(t)
	for name, want := range roots() {
		t.Run(name, func(t *testing.T) {
			js, err := want.MarshalFlatJSON()
			if err != nil {
				t.Fatal(err)
			}

			fromFlatc := cj.GetRootAsRoot(flatcBinary(t, "canonical_json_test.fbs", js), 0).UnPack()
			equalT(t, "flatc's reading of generated JSON", want, fromFlatc)

			printed := flatcJSON(t, "canonical_json_test.fbs", packRoot(want), "--json-int64-as-string")
			var fromPrinted cj.RootT
			if err := fromPrinted.UnmarshalFlatJSON(printed); err != nil {
				t.Fatalf("reading flatc's JSON %s: %v", printed, err)
			}
			equalT(t, "generated reading of flatc's JSON", want, &fromPrinted)

			again, err := fromFlatc.MarshalFlatJSON()
			if err != nil {
				t.Fatal(err)
			}
			if !bytes.Equal(js, again) {
				t.Fatalf("not a fixed point:\n%s\n%s", js, again)
			}
		})
	}
}

// Generated code and schema reflection are two implementations of one
// format: same message, same bytes; each reads the other's output.
func TestReflectionMatchesGenerated(t *testing.T) {
	schema := loadSchema(t, "canonical_json_test.bfbs")
	opts := flatbuffers.CanonicalJSONOptions{Int64AsString: true}
	for name, want := range roots() {
		t.Run(name, func(t *testing.T) {
			generated, err := want.MarshalFlatJSON()
			if err != nil {
				t.Fatal(err)
			}
			reflected, err := flatbuffers.ReflectToCanonicalJSON(schema, "CanonicalJson.Root", packRoot(want), opts)
			if err != nil {
				t.Fatal(err)
			}
			if !bytes.Equal(generated, reflected) {
				t.Fatalf("outputs differ:\ngenerated %s\nreflected %s", generated, reflected)
			}
			bin, err := flatbuffers.ReflectFromCanonicalJSON(schema, "CanonicalJson.Root", generated)
			if err != nil {
				t.Fatal(err)
			}
			equalT(t, "reflection's reading of generated JSON", want, cj.GetRootAsRoot(bin, 0).UnPack())
		})
	}
}

func TestDefaultsFilled(t *testing.T) {
	var r cj.RootT
	if err := r.UnmarshalFlatJSON([]byte(`{"name":"x"}`)); err != nil {
		t.Fatal(err)
	}
	if r.Cars != 3 || r.Mode != inc.ModeOn || r.Flags != inc.FlagsA|inc.FlagsC || r.Ratio != 0.5 || r.Big != 7 {
		t.Fatalf("defaults not filled: %+v", r)
	}
}

// Both readers reject the same documents at the same JSON Pointers.
func TestErrorPointers(t *testing.T) {
	schema := loadSchema(t, "canonical_json_test.bfbs")
	cases := map[string]string{
		`{"name":"x","zzz":1}`:                                                  "/zzz",
		`{"cars":3}`:                                                            "/name",
		`{"name":"x","cars":300}`:                                               "/cars",
		`{"name":"x","mode":"Half"}`:                                            "/mode",
		`{"name":"x","flags":"A  B"}`:                                           "/flags",
		`{"name":"x","by_mode":{"Half":"x"}}`:                                   "/by_mode/Half",
		`{"name":"x","bigs":[1,"x"]}`:                                           "/bigs/1",
		`{"name":"x","big":"18446744073709551616"}`:                             "/big",
		`{"name":"x","pt":{"x":1}}`:                                             "/pt/y",
		`{"name":"x","payload":{"id":1}}`:                                       "/payload",
		`{"name":"x","payload_type":"NONE","payload":{}}`:                       "/payload",
		`{"name":"x","payload_type":"CanonicalJsonInc_Leaf","payload":{"q":1}}`: "/payload/q",
		`{"name":"x","seen":[-1]}`:                                              "/seen/0",
		`{"name":"x","leaves":[{},{"id":"x"}]}`:                                 "/leaves/1/id",
	}
	for doc, pointer := range cases {
		var r cj.RootT
		gerr := r.UnmarshalFlatJSON([]byte(doc))
		_, rerr := flatbuffers.ReflectFromCanonicalJSON(schema, "CanonicalJson.Root", []byte(doc))
		var g, rf *flatbuffers.JSONError
		if !errors.As(gerr, &g) || g.Pointer != pointer {
			t.Errorf("%s: generated %v, want an error at %s", doc, gerr, pointer)
		}
		if !errors.As(rerr, &rf) || rf.Pointer != pointer {
			t.Errorf("%s: reflection %v, want an error at %s", doc, rerr, pointer)
		}
	}
}

func TestMapSet(t *testing.T) {
	schema := loadSchema(t, "map_set_test.bfbs")
	want := &mapset.ConfigT{
		Settings:    map[string]int32{"q\"uote": 3, "a": 1, "zero": 0},
		IdToName:    map[uint32]string{7: "seven", 4000000000: "big"},
		ColorScores: map[mapset.Color]float32{mapset.ColorBlue: 1.5, mapset.ColorRed: 0.5},
		Profiles:    map[string]*mapset.StatsT{"x": {Hp: 3, Strength: 2.5}},
		Tags:        map[string]struct{}{"z": {}, "a": {}},
		ActiveIds:   map[int32]struct{}{5: {}, -2: {}, 0: {}},
	}
	generated, err := want.MarshalFlatJSON()
	if err != nil {
		t.Fatal(err)
	}
	b := flatbuffers.NewBuilder(0)
	b.Finish(want.Pack(b))
	reflected, err := flatbuffers.ReflectToCanonicalJSON(schema, "MapSetTest.Config", b.FinishedBytes(), flatbuffers.CanonicalJSONOptions{})
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(generated, reflected) {
		t.Fatalf("outputs differ:\ngenerated %s\nreflected %s", generated, reflected)
	}
	var back mapset.ConfigT
	if err := back.UnmarshalFlatJSON(generated); err != nil {
		t.Fatal(err)
	}
	equalT(t, "map/set round trip", want, &back)

	if *flatcPath != "" && *schemasDir != "" {
		equalT(t, "flatc's reading of generated JSON", want,
			mapset.GetRootAsConfig(flatcBinary(t, "map_set_test.fbs", generated), 0).UnPack())
	}
}

func TestOptionalScalars(t *testing.T) {
	schema := loadSchema(t, "optional_scalars.bfbs")
	var zero int8
	on := true
	want := &optional.ScalarStuffT{
		JustI8: 4, MaybeI8: &zero, MaybeBool: &on,
		DefaultI8: 42, DefaultU8: 42, DefaultI16: 42, DefaultU16: 42, DefaultI32: 42, DefaultU32: 42,
		DefaultI64: 42, DefaultU64: 42, DefaultF32: 42, DefaultF64: 42, DefaultBool: true,
		DefaultEnum: optional.OptionalByteTwo,
	}
	generated, err := want.MarshalFlatJSON()
	if err != nil {
		t.Fatal(err)
	}
	b := flatbuffers.NewBuilder(0)
	b.Finish(want.Pack(b))
	reflected, err := flatbuffers.ReflectToCanonicalJSON(schema, "optional_scalars.ScalarStuff", b.FinishedBytes(), flatbuffers.CanonicalJSONOptions{})
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(generated, reflected) {
		t.Fatalf("outputs differ:\ngenerated %s\nreflected %s", generated, reflected)
	}

	// The schema's root table gets the schema's file identifier.
	built, err := flatbuffers.ReflectFromCanonicalJSON(schema, "optional_scalars.ScalarStuff", generated)
	if err != nil {
		t.Fatal(err)
	}
	if !flatbuffers.BufferHasIdentifier(built, "NULL") {
		t.Fatalf("no NULL file identifier in %x", built[:8])
	}
	equalT(t, "reflection's reading of generated JSON", want, optional.GetRootAsScalarStuff(built, 0).UnPack())

	var back optional.ScalarStuffT
	if err := back.UnmarshalFlatJSON([]byte(`{"maybe_i8":null}`)); err != nil || back.MaybeI8 != nil || back.DefaultI8 != 42 {
		t.Fatalf("null optional scalar: %v %+v", err, back)
	}
}

// A generic value built by hand may hold plain Go numbers.
func TestReflectionFromHandBuiltMap(t *testing.T) {
	schema := loadSchema(t, "canonical_json_test.bfbs")
	m := map[string]any{
		"name":         "abc",
		"cars":         64,
		"big":          uint64(18446744073709551615),
		"neg":          int64(-5),
		"ratio":        0.25,
		"flags":        "A B",
		"bigs":         []any{1, int64(-2)},
		"scores":       map[string]any{"a": 1},
		"seen":         []any{uint64(3)},
		"pt":           map[string]any{"x": -1, "y": 2},
		"payload_type": "CanonicalJsonInc_Leaf",
		"payload":      map[string]any{"id": 7},
	}
	bin, err := flatbuffers.ReflectFromCanonicalMap(schema, "CanonicalJson.Root", m)
	if err != nil {
		t.Fatal(err)
	}
	got := cj.GetRootAsRoot(bin, 0).UnPack()
	leaf, _ := got.Payload.Value.(*inc.LeafT)
	if got.Cars != 64 || got.Big != 18446744073709551615 || got.Neg != -5 || got.Flags != inc.FlagsA|inc.FlagsB ||
		got.Pt.X != -1 || leaf == nil || leaf.Id != 7 || got.Scores["a"] != 1 {
		t.Fatalf("hand-built map read wrong: %+v", got)
	}
	back, err := flatbuffers.ReflectToCanonicalMap(schema, "CanonicalJson.Root", bin, flatbuffers.CanonicalJSONOptions{Int64AsString: true})
	if err != nil {
		t.Fatal(err)
	}
	if back["big"] != "18446744073709551615" || back["flags"] != "A B" {
		t.Fatalf("canonical map: %v", back)
	}
}
