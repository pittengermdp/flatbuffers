package flatbuffers

import (
	"encoding/json"
	"errors"
	"math"
	"strings"
	"testing"
)

type testColor int8

type testFlags uint8

const (
	testFlagA testFlags = 1
	testFlagB testFlags = 2
	testFlagC testFlags = 4
)

var (
	testFlagNames  = map[testFlags]string{testFlagA: "A", testFlagB: "B", testFlagC: "C"}
	testFlagValues = map[string]testFlags{"A": testFlagA, "B": testFlagB, "C": testFlagC}
	testColorNames = map[testColor]string{1: "Red", 2: "Green", 8: "Blue"}
	testColorVals  = map[string]testColor{"Red": 1, "Green": 2, "Blue": 8}
)

func writeOrFail(t *testing.T, write func(w *JSONWriter)) string {
	t.Helper()

	w := NewJSONWriter()
	write(w)

	b, err := w.Bytes()
	if err != nil {
		t.Fatalf("Bytes: %v", err)
	}

	return string(b)
}

func TestJSONWriterSeparatorsAndNesting(t *testing.T) {
	got := writeOrFail(t, func(w *JSONWriter) {
		w.BeginObject()
		w.Key("a")
		w.Int(-1)
		w.Key("b")
		w.BeginArray()
		w.Uint(1)
		w.BeginObject()
		w.EndObject()
		w.BeginArray()
		w.EndArray()
		w.String("x")
		w.EndArray()
		w.Key("c")
		w.Null()
		w.Key("d")
		w.Bool(true)
		w.EndObject()
	})

	const want = `{"a":-1,"b":[1,{},[],"x"],"c":null,"d":true}`
	if got != want {
		t.Fatalf("got %s, want %s", got, want)
	}
}

func TestJSONWriterUnbalanced(t *testing.T) {
	w := NewJSONWriter()
	w.BeginObject()

	if _, err := w.Bytes(); !errors.Is(err, errJSONUnbalanced) {
		t.Fatalf("open object: got %v", err)
	}

	w = NewJSONWriter()
	w.EndArray()

	if _, err := w.Bytes(); !errors.Is(err, errJSONUnbalanced) {
		t.Fatalf("stray close: got %v", err)
	}
}

func TestJSONWriterStringEscaping(t *testing.T) {
	cases := map[string]string{
		"plain":            `"plain"`,
		`q"b\s`:            `"q\"b\\s"`,
		"nl\nrt\rtab\t":    `"nl\nrt\rtab\t"`,
		"ctl\x01\x1f":      `"ctl\u0001\u001f"`,
		"<html>&":          `"<html>&"`,
		"snow☃":            `"snow☃"`,
		"bad\xffutf8":      `"bad\ufffdutf8"`,
		"ls\u2028ps\u2029": `"ls\u2028ps\u2029"`,
	}
	for in, want := range cases {
		got := writeOrFail(t, func(w *JSONWriter) { w.String(in) })
		if got != want {
			t.Errorf("String(%q) = %s, want %s", in, got, want)
		}

		var back string
		if err := json.Unmarshal([]byte(got), &back); err != nil {
			t.Errorf("String(%q) wrote invalid JSON %s: %v", in, got, err)
		}
	}
}

func TestJSONWriterFloats(t *testing.T) {
	cases := []struct {
		write func(w *JSONWriter)
		want  string
	}{
		{func(w *JSONWriter) { w.Float64(0.5) }, "0.5"},
		{func(w *JSONWriter) { w.Float64(1) }, "1"},
		{func(w *JSONWriter) { w.Float64(1e21) }, "1e+21"},
		{func(w *JSONWriter) { w.Float64(1e-7) }, "1e-7"},
		{func(w *JSONWriter) { w.Float32(0.1) }, "0.1"},
		{func(w *JSONWriter) { w.Float64(-2.5) }, "-2.5"},
	}
	for _, c := range cases {
		if got := writeOrFail(t, c.write); got != c.want {
			t.Errorf("got %s, want %s", got, c.want)
		}
	}

	for _, v := range []float64{math.NaN(), math.Inf(1), math.Inf(-1)} {
		w := NewJSONWriter()
		w.Float64(v)

		if _, err := w.Bytes(); err == nil {
			t.Errorf("Float64(%v): expected an error, JSON cannot spell it", v)
		}
	}
}

func TestJSONWriterIntegers(t *testing.T) {
	got := writeOrFail(t, func(w *JSONWriter) {
		w.BeginArray()
		w.IntString(math.MinInt64)
		w.UintString(math.MaxUint64)
		WriteJSONInteger(w, int8(-128))
		WriteJSONInteger(w, uint64(math.MaxUint64))
		w.EndArray()
	})

	const want = `["-9223372036854775808","18446744073709551615",-128,18446744073709551615]`
	if got != want {
		t.Fatalf("got %s, want %s", got, want)
	}
}

func TestWriteJSONEnumAndBitFlags(t *testing.T) {
	got := writeOrFail(t, func(w *JSONWriter) {
		w.BeginArray()
		WriteJSONEnum(w, testColor(8), testColorNames)
		WriteJSONEnum(w, testColor(5), testColorNames) // unnamed: number
		WriteJSONBitFlags(w, testFlagA|testFlagC, testFlagNames)
		WriteJSONBitFlags(w, testFlagB, testFlagNames)
		WriteJSONBitFlags(w, testFlags(0), testFlagNames)   // zero: number
		WriteJSONBitFlags(w, testFlags(8|1), testFlagNames) // uncovered bit: number
		w.EndArray()
	})

	const want = `["Blue",5,"A C","B",0,9]`
	if got != want {
		t.Fatalf("got %s, want %s", got, want)
	}
}

func TestParseJSON(t *testing.T) {
	v, err := ParseJSON([]byte(`{"n": 18446744073709551615}`))
	if err != nil {
		t.Fatal(err)
	}

	m, err := JSONObject(v, "")
	if err != nil {
		t.Fatal(err)
	}

	if got, want := m["n"], json.Number("18446744073709551615"); got != want {
		t.Fatalf("number kept as %v (%T), want exact %v", got, got, want)
	}

	if _, err := ParseJSON([]byte(`{} {}`)); err == nil {
		t.Fatal("trailing document accepted")
	}

	if _, err := ParseJSON([]byte(`{`)); err == nil {
		t.Fatal("truncated document accepted")
	}
}

func TestJSONInt(t *testing.T) {
	ok := []struct {
		in   any
		want int64
	}{
		{json.Number("-128"), -128},
		{json.Number("127"), 127},
		{"-5", -5},
		{json.Number("5.0"), 5},
		{json.Number("1e2"), 100},
	}
	for _, c := range ok {
		got, err := JSONInt[int8](c.in, "/x")
		if err != nil || int64(got) != c.want {
			t.Errorf("JSONInt[int8](%v) = %d, %v; want %d", c.in, got, err, c.want)
		}
	}

	for _, in := range []any{json.Number("128"), json.Number("-129"), json.Number("1.5"), "x", true, nil} {
		if _, err := JSONInt[int8](in, "/x"); err == nil {
			t.Errorf("JSONInt[int8](%v): expected an error", in)
		}
	}

	if got, err := JSONInt[uint64]("18446744073709551615", ""); err != nil || got != math.MaxUint64 {
		t.Errorf("uint64 max: %d, %v", got, err)
	}

	if _, err := JSONInt[uint64](json.Number("18446744073709551616"), ""); err == nil {
		t.Error("uint64 overflow accepted")
	}

	if _, err := JSONInt[uint32](json.Number("-1"), ""); err == nil {
		t.Error("negative uint32 accepted")
	}

	if got, err := JSONInt[int64](json.Number("-9223372036854775808"), ""); err != nil || got != math.MinInt64 {
		t.Errorf("int64 min: %d, %v", got, err)
	}

	// A named integer type is measured, not assumed.
	if _, err := JSONInt[testColor](json.Number("200"), ""); err == nil {
		t.Error("200 accepted for an int8-based type")
	}
}

func TestJSONIntErrorPointer(t *testing.T) {
	_, err := JSONInt[uint8](json.Number("300"), "/a/b")

	var je *JSONError
	if !errors.As(err, &je) || je.Pointer != "/a/b" {
		t.Fatalf("got %v, want a *JSONError at /a/b", err)
	}
}

func TestJSONFloat(t *testing.T) {
	if got, err := JSONFloat[float32](json.Number("0.1"), ""); err != nil || got != 0.1 {
		t.Errorf("float32: %v, %v", got, err)
	}

	if got, err := JSONFloat[float64]("2.5", ""); err != nil || got != 2.5 {
		t.Errorf("numeric string: %v, %v", got, err)
	}

	for _, in := range []any{"nan", "inf", json.Number("1e999"), true} {
		if _, err := JSONFloat[float64](in, ""); err == nil {
			t.Errorf("JSONFloat(%v): expected an error", in)
		}
	}
}

func TestJSONEnumAndBitFlags(t *testing.T) {
	if got, err := JSONEnum("Blue", "", testColorVals); err != nil || got != 8 {
		t.Errorf("name: %v, %v", got, err)
	}

	if got, err := JSONEnum(json.Number("5"), "", testColorVals); err != nil || got != 5 {
		t.Errorf("number: %v, %v", got, err)
	}

	if _, err := JSONEnum("Purple", "", testColorVals); err == nil {
		t.Error("unknown name accepted")
	}

	if _, err := JSONEnum(json.Number("300"), "", testColorVals); err == nil {
		t.Error("out-of-range number accepted")
	}

	if got, err := JSONBitFlags("A C", "", testFlagValues); err != nil || got != testFlagA|testFlagC {
		t.Errorf("names: %v, %v", got, err)
	}

	if got, err := JSONBitFlags(json.Number("0"), "", testFlagValues); err != nil || got != 0 {
		t.Errorf("zero: %v, %v", got, err)
	}

	for _, in := range []string{"A  C", "A ", "D", ""} {
		if _, err := JSONBitFlags(in, "", testFlagValues); err == nil {
			t.Errorf("JSONBitFlags(%q): expected an error", in)
		}
	}
}

func TestJSONCheckMembers(t *testing.T) {
	m := map[string]any{"a": 1, "zz": 2, "b": 3, "c": 4}

	err := JSONCheckMembers(m, "/t", "a", "c")

	var je *JSONError
	if !errors.As(err, &je) || je.Pointer != "/t/b" {
		t.Fatalf("got %v, want the first unknown member in sorted order, /t/b", err)
	}

	if err := JSONCheckMembers(m, "", "a", "b", "c", "zz"); err != nil {
		t.Fatalf("all known: %v", err)
	}
}

func TestJSONMemberTreatsNullAsAbsent(t *testing.T) {
	m := map[string]any{"n": nil, "v": "x"}
	if _, ok := JSONMember(m, "n"); ok {
		t.Error("null member reported present")
	}

	if _, ok := JSONMember(m, "missing"); ok {
		t.Error("missing member reported present")
	}

	if v, ok := JSONMember(m, "v"); !ok || v != "x" {
		t.Errorf("present member: %v, %v", v, ok)
	}
}

func TestJSONPointerEscaping(t *testing.T) {
	if got, want := JSONPointer("/a", "b/c~d"), "/a/b~1c~0d"; got != want {
		t.Fatalf("got %s, want %s", got, want)
	}

	if got, want := JSONIndexPointer("/a", 3), "/a/3"; got != want {
		t.Fatalf("got %s, want %s", got, want)
	}
}

func TestJSONBoolKey(t *testing.T) {
	if v, err := JSONBoolKey("true", ""); err != nil || !v {
		t.Errorf("true: %v, %v", v, err)
	}

	if _, err := JSONBoolKey("yes", ""); err == nil || !strings.Contains(err.Error(), "yes") {
		t.Errorf("yes: %v", err)
	}
}
