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
	"bytes"
	"errors"
	"math"
	"strings"
	"testing"

	cj "CanonicalJson"
	edge "CanonicalJsonEdge"
	inc "CanonicalJsonInc"
	flatbuffers "github.com/google/flatbuffers/go"
	value "value"
)

const edgeType = "CanonicalJsonEdge.Edge"

func packEdge(e *edge.EdgeT) []byte {
	b := flatbuffers.NewBuilder(0)
	b.Finish(e.Pack(b))
	return b.FinishedBytes()
}

// withoutNaN returns e with its NaN-defaulted fields zeroed, after checking
// they hold NaN: reflect.DeepEqual never finds NaN equal to itself.
func withoutNaN(t *testing.T, e *edge.EdgeT) *edge.EdgeT {
	t.Helper()
	if !math.IsNaN(float64(e.NanF)) || !math.IsNaN(e.NanD) {
		return e
	}
	c := *e
	c.NanF, c.NanD = 0, 0
	return &c
}

func edgeValues() map[string]*edge.EdgeT {
	return map[string]*edge.EdgeT{
		// Every scalar at its default: two NaN defaults, a ulong default
		// past INT64_MAX, a true bool.
		"defaults": {
			Must: &value.LeafT{}, NanF: float32(math.NaN()), NanD: math.NaN(),
			Top: math.MaxUint64, Flag: true, Req: []int32{},
		},
		"full": {
			Must: &value.LeafT{Id: 1}, NanF: 1.5, NanD: -2.5, Z: 3, Top: 0, Flag: false,
			Wide: 9223372036854775810, // no variant: a quoted number
			Pair: &edge.PairT{A: 1, B: -1},
			// Keys flatc spells as "0", "P", "P Q" and "8".
			ByBits:  map[edge.Bits]int32{0: 1, edge.BitsP: 4, edge.BitsP | edge.BitsQ: 2, 8: 3},
			Pairs:   map[string]*edge.PairT{"a": {A: 0, B: 0}, "b": {A: 49, B: -7}},
			ByBool:  map[bool]string{true: "t", false: "f"},
			Bools:   map[bool]struct{}{true: {}, false: {}},
			Leaves:  map[string]*value.LeafT{"n": nil, "l": {Id: 2}},
			Req:     []int32{1, 2},
			Node:    &edge.NodeT{Left: &edge.NodeT{}},
			Nothing: nil,
		},
	}
}

// Generated code, reflection and flatc agree on every edge case: the same
// bytes from both writers, and every reader recovers the value.
func TestEdgeThreeWays(t *testing.T) {
	schema := loadSchema(t, "canonical_json_edge.bfbs")
	opts := flatbuffers.CanonicalJSONOptions{Int64AsString: true}
	for name, want := range edgeValues() {
		t.Run(name, func(t *testing.T) {
			generated, err := want.MarshalFlatJSON()
			if err != nil {
				t.Fatal(err)
			}
			reflected, err := flatbuffers.ReflectToCanonicalJSON(schema, edgeType, packEdge(want), opts)
			if err != nil {
				t.Fatal(err)
			}
			if !bytes.Equal(generated, reflected) {
				t.Fatalf("outputs differ:\ngenerated %s\nreflected %s", generated, reflected)
			}

			var back edge.EdgeT
			if err := back.UnmarshalFlatJSON(generated); err != nil {
				t.Fatalf("reading %s: %v", generated, err)
			}
			equalT(t, "generated reading", withoutNaN(t, want), withoutNaN(t, &back))

			bin, err := flatbuffers.ReflectFromCanonicalJSON(schema, edgeType, generated)
			if err != nil {
				t.Fatal(err)
			}
			equalT(t, "reflection's reading", withoutNaN(t, want), withoutNaN(t, edge.GetRootAsEdge(bin, 0).UnPack()))

			if *flatcPath == "" || *schemasDir == "" {
				return
			}
			fromFlatc := edge.GetRootAsEdge(flatcBinary(t, "canonical_json_edge.fbs", generated), 0).UnPack()
			equalT(t, "flatc's reading", withoutNaN(t, want), withoutNaN(t, fromFlatc))

			// flatc prints a NaN that Pack stored as "nan", which is not
			// JSON; a value without one prints as JSON the generated code
			// reads back.
			if math.IsNaN(float64(want.NanF)) {
				return
			}
			printed := flatcJSON(t, "canonical_json_edge.fbs", packEdge(want), "--json-int64-as-string")
			var fromPrinted edge.EdgeT
			if err := fromPrinted.UnmarshalFlatJSON(printed); err != nil {
				t.Fatalf("reading flatc's JSON %s: %v", printed, err)
			}
			equalT(t, "generated reading of flatc's JSON", want, &fromPrinted)
		})
	}

	full := edgeValues()["full"]
	js, err := full.MarshalFlatJSON()
	if err != nil {
		t.Fatal(err)
	}
	for _, part := range []string{
		`"top":"0"`, `"wide":"9223372036854775810"`,
		`"by_bits":{"0":1,"P":4,"P Q":2,"8":3}`,
		`"by_bool":{"false":"f","true":"t"}`, `"bools":[false,true]`,
		`"leaves":{"l":{"id":2},"n":null}`,
	} {
		if !strings.Contains(string(js), part) {
			t.Errorf("%s does not contain %s", js, part)
		}
	}
}

// A map or set can hold one key twice (flatc builds one from two spellings
// of it); written, it holds the last entry, as UnPack keeps it. Read, two
// spellings collapse the same way in both readers.
func TestEdgeRepeatedKeys(t *testing.T) {
	requireFlatc(t)
	schema := loadSchema(t, "canonical_json_edge.bfbs")
	doc := []byte(`{"must":{},"req":[],"by_bits":{"P":1,"1":2},"bools":[true,true,false]}`)

	bin := flatcBinary(t, "canonical_json_edge.fbs", doc)
	generated, err := edge.GetRootAsEdge(bin, 0).UnPack().MarshalFlatJSON()
	if err != nil {
		t.Fatal(err)
	}
	reflected, err := flatbuffers.ReflectToCanonicalJSON(schema, edgeType, bin, flatbuffers.CanonicalJSONOptions{Int64AsString: true})
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(generated, reflected) {
		t.Fatalf("outputs differ:\ngenerated %s\nreflected %s", generated, reflected)
	}

	var read edge.EdgeT
	if err := read.UnmarshalFlatJSON(doc); err != nil {
		t.Fatal(err)
	}
	built, err := flatbuffers.ReflectFromCanonicalJSON(schema, edgeType, doc)
	if err != nil {
		t.Fatal(err)
	}
	equalT(t, "reflection's reading", withoutNaN(t, &read), withoutNaN(t, edge.GetRootAsEdge(built, 0).UnPack()))
	if len(read.ByBits) != 1 || read.ByBits[edge.BitsP] != 1 || len(read.Bools) != 2 {
		t.Fatalf("collapsed to %v %v", read.ByBits, read.Bools)
	}
}

// A union is written only when UnPack would read one: not for a type
// without its value, nor for a variant from a newer schema.
func TestEdgeUnionPresence(t *testing.T) {
	schema := loadSchema(t, "canonical_json_test.bfbs")
	opts := flatbuffers.CanonicalJSONOptions{Int64AsString: true}

	b := flatbuffers.NewBuilder(0)
	leaf := (&inc.LeafT{Id: 1}).Pack(b)
	name := b.CreateString("x")
	cj.RootStart(b)
	cj.RootAddName(b, name)
	cj.RootAddPayloadType(b, cj.Payload(9))
	cj.RootAddPayload(b, leaf)
	b.Finish(cj.RootEnd(b))
	newer := b.FinishedBytes()

	buffers := map[string][]byte{"newer variant": newer}
	if *flatcPath != "" && *schemasDir != "" {
		buffers["type without value"] = flatcBinary(t, "canonical_json_test.fbs",
			[]byte(`{"name":"x","payload_type":"CanonicalJsonInc_Leaf"}`))
	}
	for what, bin := range buffers {
		generated, err := cj.GetRootAsRoot(bin, 0).UnPack().MarshalFlatJSON()
		if err != nil {
			t.Fatal(err)
		}
		reflected, err := flatbuffers.ReflectToCanonicalJSON(schema, "CanonicalJson.Root", bin, opts)
		if err != nil {
			t.Fatalf("%s: %v", what, err)
		}
		if !bytes.Equal(generated, reflected) || strings.Contains(string(reflected), "payload") {
			t.Errorf("%s:\ngenerated %s\nreflected %s", what, generated, reflected)
		}
	}

	doc := []byte(`{"name":"x","payload_type":"CanonicalJsonInc_Leaf"}`)
	var r cj.RootT
	if err := r.UnmarshalFlatJSON(doc); err != nil || r.Payload != nil {
		t.Fatalf("generated: %v %+v", err, r.Payload)
	}
	built, err := flatbuffers.ReflectFromCanonicalJSON(schema, "CanonicalJson.Root", doc)
	if err != nil || cj.GetRootAsRoot(built, 0).PayloadType() != cj.PayloadNONE {
		t.Fatalf("reflection: %v", err)
	}
}

// fieldPos locates a present table field in buf, through the schema.
func fieldPos(t *testing.T, schema *flatbuffers.ReflectionSchema, typeName, field string, buf []byte) flatbuffers.UOffsetT {
	t.Helper()
	f := schema.ObjectByName(typeName).FieldByName(field)
	tab := flatbuffers.Table{Bytes: buf, Pos: flatbuffers.GetUOffsetT(buf)}
	o := tab.Offset(flatbuffers.VOffsetT(f.Offset()))
	if o == 0 {
		t.Fatalf("%s.%s is not in the buffer", typeName, field)
	}
	return tab.Pos + flatbuffers.UOffsetT(o)
}

// A buffer is untrusted input: one whose offsets loop, or share children
// so that it unfolds into an exponentially large document, is an error, not
// a crash or an endless loop.
func TestEdgeMalformedBuffers(t *testing.T) {
	schema := loadSchema(t, "canonical_json_edge.bfbs")
	opts := flatbuffers.CanonicalJSONOptions{Int64AsString: true}

	loop := packNode(&edge.NodeT{Left: &edge.NodeT{}})
	at := fieldPos(t, schema, "CanonicalJsonEdge.Node", "left", loop)
	root := flatbuffers.GetUOffsetT(loop)
	// The left child's offset now leads back to the root table itself.
	flatbuffers.WriteUOffsetT(loop[at:], root-at)
	if _, err := flatbuffers.ReflectToCanonicalJSON(schema, "CanonicalJsonEdge.Node", loop, opts); err == nil {
		t.Error("a looping buffer was written")
	}

	b := flatbuffers.NewBuilder(0)
	edge.NodeStart(b)
	shared := edge.NodeEnd(b)
	for range 40 {
		edge.NodeStart(b)
		edge.NodeAddLeft(b, shared)
		edge.NodeAddRight(b, shared)
		shared = edge.NodeEnd(b)
	}
	b.Finish(shared)
	if _, err := flatbuffers.ReflectToCanonicalJSON(schema, "CanonicalJsonEdge.Node", b.FinishedBytes(), opts); err == nil {
		t.Error("a 2^40-table document was written")
	}

	// A bool stored as 2 is true, as the generated accessor reads it.
	flagged := packEdge(&edge.EdgeT{Must: &value.LeafT{}, Flag: false, Req: []int32{}})
	flagged[fieldPos(t, schema, edgeType, "flag", flagged)] = 2
	generated, err := edge.GetRootAsEdge(flagged, 0).UnPack().MarshalFlatJSON()
	if err != nil {
		t.Fatal(err)
	}
	reflected, err := flatbuffers.ReflectToCanonicalJSON(schema, edgeType, flagged, opts)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(generated, reflected) || strings.Contains(string(reflected), "flag") {
		t.Errorf("bool stored as 2:\ngenerated %s\nreflected %s", generated, reflected)
	}

	// A required table that is missing is an error both ways.
	if _, err := (&edge.EdgeT{Req: []int32{}}).MarshalFlatJSON(); err == nil {
		t.Error("generated code wrote a value without its required table")
	}
	if _, err := flatbuffers.ReflectToCanonicalJSON(schema, edgeType, packEdge(&edge.EdgeT{Req: []int32{}}), opts); err == nil {
		t.Error("reflection wrote a buffer without its required table")
	}
}

func packNode(n *edge.NodeT) []byte {
	b := flatbuffers.NewBuilder(0)
	b.Finish(n.Pack(b))
	return b.FinishedBytes()
}

// A field canonical JSON cannot spell is refused both ways, never dropped.
func TestEdgeVector64Refused(t *testing.T) {
	requireFlatc(t)
	schema := loadSchema(t, "canonical_json_v64.bfbs")
	doc := []byte(`{"a":1,"big":[1,2,3],"b":2}`)
	bin := flatcBinary(t, "canonical_json_v64.fbs", doc)
	if js, err := flatbuffers.ReflectToCanonicalJSON(schema, "CanonicalJsonV64.Big", bin, flatbuffers.CanonicalJSONOptions{}); err == nil {
		t.Errorf("vector64 written as %s", js)
	}
	if _, err := flatbuffers.ReflectFromCanonicalJSON(schema, "CanonicalJsonV64.Big", doc); err == nil {
		t.Error("vector64 read")
	}
}

// Both readers reject the same documents at the same JSON Pointers, the
// first problem in field order first.
func TestEdgeErrorPointers(t *testing.T) {
	schema := loadSchema(t, "canonical_json_edge.bfbs")
	const base = `"must":{},"req":[]`
	cases := map[string]string{
		`{` + base + `,"pair":{"a":"bad","b":1},"by_bits":{"Z":1}}`: "/pair/a",
		`{` + base + `,"top":"0x10"}`:                               "/top",
		`{` + base + `,"top":"10/2"}`:                               "/top",
		`{` + base + `,"z":1e999999}`:                               "/z",
		`{` + base + `,"wide":"1e3"}`:                               "/wide",
		`{"must":{},"req":[],"req":[1]}`:                            "/req",
		`{` + base + `,"pairs":{"a":{"a":1}}}`:                      "/pairs/a/b",
		`{` + base + `,"by_bool":{"yes":"x"}}`:                      "/by_bool/yes",
		`{` + base + `,"by_bits":{"P  Q":1}}`:                       "/by_bits/P  Q",
		`{` + base + `,"nothing_type":"Some"}`:                      "/nothing_type",
		`{` + base + `,"nothing_type":"NONE","nothing":{}}`:         "/nothing",
		`{` + base + `,"leaves":{"a":{"id":1.5}}}`:                  "/leaves/a/id",
	}
	for doc, pointer := range cases {
		var e edge.EdgeT
		gerr := e.UnmarshalFlatJSON([]byte(doc))
		_, rerr := flatbuffers.ReflectFromCanonicalJSON(schema, edgeType, []byte(doc))
		var g, rf *flatbuffers.JSONError
		if !errors.As(gerr, &g) || g.Pointer != pointer {
			t.Errorf("%s: generated %v, want an error at %s", doc, gerr, pointer)
		}
		if !errors.As(rerr, &rf) || rf.Pointer != pointer {
			t.Errorf("%s: reflection %v, want an error at %s", doc, rerr, pointer)
		}
		if g != nil && rf != nil && g.Message != rf.Message {
			t.Errorf("%s: generated says %q, reflection %q", doc, g.Message, rf.Message)
		}
	}

	deep := []byte(`{` + base + `,"node":` + strings.Repeat(`{"left":`, 300) + `{}` + strings.Repeat(`}`, 300) + `}`)
	var e edge.EdgeT
	if err := e.UnmarshalFlatJSON(deep); err == nil {
		t.Error("generated code read a document nested 300 deep")
	}
	if _, err := flatbuffers.ReflectFromCanonicalJSON(schema, edgeType, deep); err == nil {
		t.Error("reflection read a document nested 300 deep")
	}
}
