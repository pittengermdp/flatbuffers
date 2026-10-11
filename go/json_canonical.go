package flatbuffers

// Canonical FlatBuffers JSON.
//
// This is the JSON text format flatc itself reads and writes (field names as
// declared, enums by variant name, a union as "<field>_type" plus "<field>",
// map<K, V> as an object, set<K> as an array of keys), with three rules that
// make it a single canonical spelling:
//
//   - A value equal to its default is omitted, as are an empty string and an
//     empty vector, map or set, unless the field is required. An Object API
//     value cannot tell "absent" from "zero" or "empty", so neither does the
//     JSON.
//   - Members appear in schema declaration order, compactly, without spaces.
//   - Under the int64-as-string option a long or ulong is written as a quoted
//     decimal string: a reader that holds JSON numbers as doubles (JavaScript's
//     JSON.parse) silently rounds integers above 2^53.
//
// Generated Object API code (flatc --go --gen-object-api --go-json) and schema
// reflection (ReflectToCanonicalJSON) both write through JSONWriter, so the
// same message comes out byte for byte the same either way. Reading accepts
// everything the generated JSON Schema accepts and a little more (a 64-bit
// integer as either form, a numeric string for any integer, null for any
// absent member), so a document that validates decodes -- unless it repeats a
// member name, which JSON Schema cannot see and which reading refuses, as
// flatc does.

import (
	"bytes"
	"cmp"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"maps"
	"math"
	"math/big"
	"slices"
	"sort"
	"strconv"
	"strings"
	"unicode/utf8"
)

// JSONMaxDepth is how deeply canonical JSON may nest objects and arrays, on
// reading and on writing. It bounds the recursion a document (or a buffer
// whose offsets loop back on themselves) can drive; FlatBuffers' own verifier
// allows 64 levels of nested tables, which take at most twice as many.
const JSONMaxDepth = 256

// jsonClipLength bounds how much of an offending value an error repeats, so
// a short input cannot produce a long error.
const jsonClipLength = 64

// jsonClip shortens s for an error message.
func jsonClip(s string) string {
	if len(s) <= jsonClipLength {
		return s
	}

	cut := jsonClipLength
	for cut > 0 && !utf8.RuneStart(s[cut]) {
		cut--
	}

	return s[:cut] + "..."
}

// JSONError reports a canonical JSON value that cannot be read or written,
// located by an RFC 6901 JSON Pointer ("" is the document itself).
type JSONError struct {
	Pointer string
	Message string
}

func (e *JSONError) Error() string {
	if e.Pointer == "" {
		return "flatbuffers json: " + e.Message
	}

	return "flatbuffers json: " + e.Pointer + ": " + e.Message
}

// JSONErrorf returns a *JSONError at pointer.
func JSONErrorf(pointer, format string, args ...any) error {
	return &JSONError{Pointer: pointer, Message: fmt.Sprintf(format, args...)}
}

// JSONPointer appends one reference token to a JSON Pointer, escaping "~" and
// "/" as RFC 6901 requires.
func JSONPointer(parent, token string) string {
	token = strings.ReplaceAll(token, "~", "~0")
	token = strings.ReplaceAll(token, "/", "~1")

	return parent + "/" + token
}

// JSONIndexPointer appends an array index to a JSON Pointer.
func JSONIndexPointer(parent string, index int) string {
	return parent + "/" + strconv.Itoa(index)
}

// FlatJSONMessage is implemented by every Object API type generated with
// flatc --go-json.
type FlatJSONMessage interface {
	// MarshalFlatJSON returns the value as canonical JSON.
	MarshalFlatJSON() ([]byte, error)
	// UnmarshalFlatJSON replaces the value with the one data describes,
	// filling every absent scalar with its schema default.
	UnmarshalFlatJSON(data []byte) error
	// FlatJSONSchema names the JSON Schema definition describing the value's
	// JSON: the schema file flatc --jsonschema writes for the .fbs that
	// declares the type, and the definition's name inside it.
	FlatJSONSchema() (file, definition string)
}

// ── Writing ──────────────────────────────────────────────────────────────

var errJSONUnbalanced = errors.New("flatbuffers json: unbalanced object or array")

type jsonContainer struct {
	array bool
	count int
}

// JSONWriter builds canonical JSON. The first error any method meets (for
// example a NaN, which JSON cannot spell) is kept and returned by Bytes;
// later calls are harmless.
type JSONWriter struct {
	buf   []byte
	stack []jsonContainer
	err   error
}

// NewJSONWriter returns an empty writer.
func NewJSONWriter() *JSONWriter {
	return &JSONWriter{}
}

// Fail records err unless an earlier error is already recorded.
func (w *JSONWriter) Fail(err error) {
	if w.err == nil {
		w.err = err
	}
}

// Bytes returns the JSON written so far, or the first error recorded.
func (w *JSONWriter) Bytes() ([]byte, error) {
	if w.err != nil {
		return nil, w.err
	}

	if len(w.stack) != 0 {
		return nil, errJSONUnbalanced
	}

	return w.buf, nil
}

// beforeValue writes the separator an array element needs.
func (w *JSONWriter) beforeValue() {
	n := len(w.stack)
	if n == 0 || !w.stack[n-1].array {
		return
	}

	if w.stack[n-1].count > 0 {
		w.buf = append(w.buf, ',')
	}

	w.stack[n-1].count++
}

// BeginObject opens an object.
func (w *JSONWriter) BeginObject() {
	w.checkDepth()
	w.beforeValue()
	w.buf = append(w.buf, '{')
	w.stack = append(w.stack, jsonContainer{})
}

// checkDepth fails a container that would nest deeper than JSONMaxDepth.
func (w *JSONWriter) checkDepth() {
	if len(w.stack) >= JSONMaxDepth {
		w.Fail(JSONErrorf("", "nested more than %d deep", JSONMaxDepth))
	}
}

// EndObject closes the innermost object.
func (w *JSONWriter) EndObject() {
	w.end('}')
}

// BeginArray opens an array.
func (w *JSONWriter) BeginArray() {
	w.checkDepth()
	w.beforeValue()
	w.buf = append(w.buf, '[')
	w.stack = append(w.stack, jsonContainer{array: true})
}

// EndArray closes the innermost array.
func (w *JSONWriter) EndArray() {
	w.end(']')
}

func (w *JSONWriter) end(closer byte) {
	if len(w.stack) == 0 {
		w.Fail(errJSONUnbalanced)

		return
	}

	w.stack = w.stack[:len(w.stack)-1]
	w.buf = append(w.buf, closer)
}

// Key writes an object member's name; the member's value must follow.
func (w *JSONWriter) Key(name string) {
	n := len(w.stack)
	if n == 0 || w.stack[n-1].array {
		w.Fail(errJSONUnbalanced)

		return
	}

	if w.stack[n-1].count > 0 {
		w.buf = append(w.buf, ',')
	}

	w.stack[n-1].count++
	w.buf = appendJSONString(w.buf, name)
	w.buf = append(w.buf, ':')
}

// MapKey writes a map key as an object member's name. Unlike a field name, a
// key comes from the data, and one that is not valid UTF-8 is refused: JSON
// would spell its bad bytes as U+FFFD, and two keys differing only there
// would become one member twice.
func (w *JSONWriter) MapKey(key string) {
	if !utf8.ValidString(key) {
		w.Fail(JSONErrorf("", "map key %q is not valid UTF-8", jsonClip(key)))

		return
	}

	w.Key(key)
}

// SetMember writes a member of a set of strings, refused when it is not
// valid UTF-8 for the reason MapKey refuses such a key.
func (w *JSONWriter) SetMember(member string) {
	if !utf8.ValidString(member) {
		w.Fail(JSONErrorf("", "set member %q is not valid UTF-8", jsonClip(member)))

		return
	}

	w.String(member)
}

// String writes a string.
func (w *JSONWriter) String(s string) {
	w.beforeValue()
	w.buf = appendJSONString(w.buf, s)
}

// Bool writes true or false.
func (w *JSONWriter) Bool(v bool) {
	w.beforeValue()
	w.buf = strconv.AppendBool(w.buf, v)
}

// Null writes null.
func (w *JSONWriter) Null() {
	w.beforeValue()
	w.buf = append(w.buf, "null"...)
}

// Int writes a signed integer as a JSON number.
func (w *JSONWriter) Int(v int64) {
	w.beforeValue()
	w.buf = strconv.AppendInt(w.buf, v, 10)
}

// Uint writes an unsigned integer as a JSON number.
func (w *JSONWriter) Uint(v uint64) {
	w.beforeValue()
	w.buf = strconv.AppendUint(w.buf, v, 10)
}

// IntString writes a signed integer as a quoted decimal string.
func (w *JSONWriter) IntString(v int64) {
	w.beforeValue()
	w.buf = append(w.buf, '"')
	w.buf = strconv.AppendInt(w.buf, v, 10)
	w.buf = append(w.buf, '"')
}

// UintString writes an unsigned integer as a quoted decimal string.
func (w *JSONWriter) UintString(v uint64) {
	w.beforeValue()
	w.buf = append(w.buf, '"')
	w.buf = strconv.AppendUint(w.buf, v, 10)
	w.buf = append(w.buf, '"')
}

// Float32 writes a float in its shortest round-tripping form.
func (w *JSONWriter) Float32(v float32) {
	w.float(float64(v), 32)
}

// Float64 writes a double in its shortest round-tripping form.
func (w *JSONWriter) Float64(v float64) {
	w.float(v, 64)
}

func (w *JSONWriter) float(v float64, bits int) {
	if math.IsNaN(v) || math.IsInf(v, 0) {
		w.Fail(JSONErrorf("", "%v has no JSON spelling", v))

		return
	}

	w.beforeValue()
	w.buf = appendJSONFloat(w.buf, v, bits)
}

// appendJSONFloat follows encoding/json: plain notation for magnitudes in
// [1e-6, 1e21), exponent notation outside it, with "e-07" shortened to "e-7".
func appendJSONFloat(b []byte, v float64, bits int) []byte {
	const (
		smallest = 1e-6
		largest  = 1e21
	)

	format := byte('f')

	if abs := math.Abs(v); abs != 0 {
		if bits == 64 && (abs < smallest || abs >= largest) ||
			bits == 32 && (float32(abs) < float32(smallest) || float32(abs) >= float32(largest)) {
			format = 'e'
		}
	}

	b = strconv.AppendFloat(b, v, format, -1, bits)

	if format == 'e' {
		n := len(b)
		if n >= 4 && b[n-4] == 'e' && b[n-3] == '-' && b[n-2] == '0' {
			b[n-2] = b[n-1]
			b = b[:n-1]
		}
	}

	return b
}

const jsonHex = "0123456789abcdef"

// appendJSONString writes s as a JSON string: quotes, backslashes and control
// characters escaped, U+2028/U+2029 escaped for JavaScript, invalid UTF-8
// replaced by U+FFFD -- written as the character itself, as a real U+FFFD is,
// so the JSON reads back to a string that writes the same JSON again. Nothing
// else (no HTML escaping) is touched.
func appendJSONString(b []byte, s string) []byte {
	b = append(b, '"')

	start := 0

	for i := 0; i < len(s); {
		c := s[i]
		if c < utf8.RuneSelf {
			if c >= 0x20 && c != '"' && c != '\\' {
				i++

				continue
			}

			b = append(b, s[start:i]...)

			switch c {
			case '"', '\\':
				b = append(b, '\\', c)
			case '\n':
				b = append(b, '\\', 'n')
			case '\r':
				b = append(b, '\\', 'r')
			case '\t':
				b = append(b, '\\', 't')
			default:
				b = append(b, '\\', 'u', '0', '0', jsonHex[c>>4], jsonHex[c&0xF])
			}

			i++
			start = i

			continue
		}

		r, size := utf8.DecodeRuneInString(s[i:])
		if r == utf8.RuneError && size == 1 {
			b = append(b, s[start:i]...)
			b = utf8.AppendRune(b, utf8.RuneError)
			i += size
			start = i

			continue
		}

		if r == '\u2028' || r == '\u2029' {
			b = append(b, s[start:i]...)
			b = append(b, '\\', 'u', '2', '0', '2', jsonHex[r&0xF])
			i += size
			start = i

			continue
		}

		i += size
	}

	b = append(b, s[start:]...)

	return append(b, '"')
}

// JSONInteger is every integer kind a FlatBuffers scalar or enum can have.
type JSONInteger interface {
	~int8 | ~int16 | ~int32 | ~int64 | ~uint8 | ~uint16 | ~uint32 | ~uint64
}

// WriteJSONInteger writes any integer as a JSON number.
func WriteJSONInteger[T JSONInteger](w *JSONWriter, v T) {
	if v < 0 {
		w.Int(int64(v))

		return
	}

	w.Uint(uint64(v))
}

// writeJSONIntegerString writes any integer as a quoted decimal string.
func writeJSONIntegerString[T JSONInteger](w *JSONWriter, v T) {
	if v < 0 {
		w.IntString(int64(v))

		return
	}

	w.UintString(uint64(v))
}

// WriteJSONEnum writes an enum value by its variant name, or as its number
// when no variant has that value (a value from a newer schema, say).
func WriteJSONEnum[E JSONInteger](w *JSONWriter, v E, names map[E]string) {
	if name, ok := names[v]; ok {
		w.String(name)

		return
	}

	WriteJSONInteger(w, v)
}

// WriteJSONEnumQuoted is WriteJSONEnum for a 64-bit enum under the
// int64-as-string option: a value no variant names is a 64-bit number, so it
// is written as a quoted decimal string like any other.
func WriteJSONEnumQuoted[E JSONInteger](w *JSONWriter, v E, names map[E]string) {
	if name, ok := names[v]; ok {
		w.String(name)

		return
	}

	writeJSONIntegerString(w, v)
}

// WriteJSONBitFlags writes a bit_flags value as flatc prints one: the name of
// the variant equal to it if there is one, otherwise the space-separated
// names of every variant sharing a bit with it (in ascending value order)
// when together they make up exactly the value, otherwise its number.
func WriteJSONBitFlags[E JSONInteger](w *JSONWriter, v E, names map[E]string) {
	if text, ok := BitFlagsJSONText(v, names); ok {
		w.String(text)

		return
	}

	WriteJSONInteger(w, v)
}

// WriteJSONBitFlagsQuoted is WriteJSONBitFlags for a 64-bit bit_flags enum
// under the int64-as-string option, as WriteJSONEnumQuoted is for an enum.
func WriteJSONBitFlagsQuoted[E JSONInteger](w *JSONWriter, v E, names map[E]string) {
	if text, ok := BitFlagsJSONText(v, names); ok {
		w.String(text)

		return
	}

	writeJSONIntegerString(w, v)
}

// BitFlagsJSONText spells a bit_flags value as WriteJSONBitFlags does,
// reporting false when only its number can.
func BitFlagsJSONText[E JSONInteger](v E, names map[E]string) (string, bool) {
	if name, ok := names[v]; ok {
		return name, true
	}

	if v == 0 {
		return "", false
	}

	flags := make([]E, 0, len(names))
	for flag := range names {
		flags = append(flags, flag)
	}

	sort.Slice(flags, func(i, j int) bool { return flags[i] < flags[j] })

	var (
		parts []string
		mask  E
	)

	for _, flag := range flags {
		if flag&v != 0 {
			mask |= flag
			parts = append(parts, names[flag])
		}
	}

	if mask != v {
		return "", false
	}

	return strings.Join(parts, " "), true
}

// ── Reading ──────────────────────────────────────────────────────────────

// ParseJSON parses one JSON document, keeping every number exact as a
// json.Number. Objects come back as map[string]any, arrays as []any.
//
// It is stricter than encoding/json, as flatc's own parser is: an object may
// not repeat a member name (encoding/json keeps the last, so two readers of
// one document could disagree on its value), nothing but whitespace may
// follow the document, and nesting is limited to JSONMaxDepth.
func ParseJSON(data []byte) (any, error) {
	dec := json.NewDecoder(bytes.NewReader(data))
	dec.UseNumber()

	v, err := parseJSONValue(dec, "", 0)
	if err != nil {
		return nil, err
	}

	_, err = dec.Token()
	if !errors.Is(err, io.EOF) {
		return nil, JSONErrorf("", "unexpected data after the document")
	}

	return v, nil
}

// jsonSyntaxError reports a tokenizer error at pointer; running out of input
// mid-document is io.EOF there, which says nothing useful on its own.
func jsonSyntaxError(pointer string, err error) error {
	if errors.Is(err, io.EOF) {
		return JSONErrorf(pointer, "unexpected end of JSON input")
	}

	return JSONErrorf(pointer, "%v", err)
}

func parseJSONValue(dec *json.Decoder, pointer string, depth int) (any, error) {
	tok, err := dec.Token()
	if err != nil {
		return nil, jsonSyntaxError(pointer, err)
	}

	delim, isDelim := tok.(json.Delim)
	if !isDelim {
		return tok, nil // a string, json.Number, bool or nil
	}

	if depth >= JSONMaxDepth {
		return nil, JSONErrorf(pointer, "nested more than %d deep", JSONMaxDepth)
	}

	if delim == '[' {
		return parseJSONArray(dec, pointer, depth)
	}

	return parseJSONObject(dec, pointer, depth)
}

func parseJSONObject(dec *json.Decoder, pointer string, depth int) (any, error) {
	m := make(map[string]any)

	for dec.More() {
		tok, err := dec.Token()
		if err != nil {
			return nil, jsonSyntaxError(pointer, err)
		}

		key, _ := tok.(string) // the tokenizer allows only a string here
		p := JSONPointer(pointer, key)

		if _, repeated := m[key]; repeated {
			return nil, JSONErrorf(p, "member appears more than once")
		}

		m[key], err = parseJSONValue(dec, p, depth+1)
		if err != nil {
			return nil, err
		}
	}

	_, err := dec.Token() // '}'
	if err != nil {
		return nil, jsonSyntaxError(pointer, err)
	}

	return m, nil
}

func parseJSONArray(dec *json.Decoder, pointer string, depth int) (any, error) {
	a := []any{}

	for i := 0; dec.More(); i++ {
		v, err := parseJSONValue(dec, JSONIndexPointer(pointer, i), depth+1)
		if err != nil {
			return nil, err
		}

		a = append(a, v)
	}

	_, err := dec.Token() // ']'
	if err != nil {
		return nil, jsonSyntaxError(pointer, err)
	}

	return a, nil
}

// JSONObject asserts that v is an object.
func JSONObject(v any, pointer string) (map[string]any, error) {
	m, ok := v.(map[string]any)
	if !ok {
		return nil, JSONErrorf(pointer, "expected an object, got %s", jsonKind(v))
	}

	return m, nil
}

// JSONArray asserts that v is an array.
func JSONArray(v any, pointer string) ([]any, error) {
	a, ok := v.([]any)
	if !ok {
		return nil, JSONErrorf(pointer, "expected an array, got %s", jsonKind(v))
	}

	return a, nil
}

// JSONMember returns an object member that is present and not null: a null
// member reads exactly as an absent one, as it does to flatc.
func JSONMember(m map[string]any, key string) (any, bool) {
	v, ok := m[key]
	if !ok || v == nil {
		return nil, false
	}

	return v, true
}

// JSONCheckMembers rejects any member of m not in known, naming the first in
// sorted order so the error does not depend on map iteration.
func JSONCheckMembers(m map[string]any, pointer string, known ...string) error {
	var unknown []string

	for key := range m {
		found := false

		for _, k := range known {
			if k == key {
				found = true

				break
			}
		}

		if !found {
			unknown = append(unknown, key)
		}
	}

	if len(unknown) == 0 {
		return nil
	}

	sort.Strings(unknown)

	return JSONErrorf(JSONPointer(pointer, unknown[0]), "unknown member")
}

// JSONSortedKeys returns an object's member names in sorted order, so code
// that walks an object reports the same error for the same input every time.
func JSONSortedKeys(m map[string]any) []string {
	keys := make([]string, 0, len(m))
	for key := range m {
		keys = append(keys, key)
	}

	sort.Strings(keys)

	return keys
}

// JSONRequired reports a required member that is absent or null.
func JSONRequired(pointer, key string) error {
	return JSONErrorf(JSONPointer(pointer, key), "required member is missing")
}

// JSONString reads a string.
func JSONString(v any, pointer string) (string, error) {
	s, ok := v.(string)
	if !ok {
		return "", JSONErrorf(pointer, "expected a string, got %s", jsonKind(v))
	}

	return s, nil
}

// JSONBool reads true or false.
func JSONBool(v any, pointer string) (bool, error) {
	b, ok := v.(bool)
	if !ok {
		return false, JSONErrorf(pointer, "expected true or false, got %s", jsonKind(v))
	}

	return b, nil
}

// JSONBoolKey reads a map key spelled "true" or "false".
func JSONBoolKey(key, pointer string) (bool, error) {
	switch key {
	case "true":
		return true, nil
	case "false":
		return false, nil
	default:
		return false, JSONErrorf(pointer, "map key %q is not true or false", key)
	}
}

// JSONFloating is both float kinds.
type JSONFloating interface {
	~float32 | ~float64
}

// JSONInt reads an integer of T's width and signedness from a number or a
// decimal string. A number in another spelling of an integer (5.0, 1e3) is
// accepted: JSON Schema counts those as integers too.
func JSONInt[T JSONInteger](v any, pointer string) (T, error) {
	n, text, err := jsonBigInt(v, pointer)
	if err != nil {
		return 0, err
	}

	return integerFromBig[T](n, text, pointer)
}

// integerFromBig narrows n, read from the JSON text, to T.
func integerFromBig[T JSONInteger](n *big.Int, text, pointer string) (T, error) {
	lo, hi := integerRange[T]()
	if n.Cmp(lo) < 0 || n.Cmp(hi) > 0 {
		return 0, JSONErrorf(pointer, "%s does not fit a %d-bit %s integer",
			jsonClip(text), integerBits[T](), map[bool]string{true: "signed", false: "unsigned"}[isSigned[T]()])
	}

	if n.Sign() < 0 {
		return T(n.Int64()), nil
	}

	return T(n.Uint64()), nil
}

// JSONFloat reads a float or double from a number, or from a string holding
// one spelled as a JSON number.
func JSONFloat[T JSONFloating](v any, pointer string) (T, error) {
	var text string

	switch x := v.(type) {
	case json.Number:
		text = string(x)
	case string:
		text = x
	case float64:
		text = strconv.FormatFloat(x, 'g', -1, 64)
	case float32:
		text = strconv.FormatFloat(float64(x), 'g', -1, 32)
	default:
		n, ok := goNumberToBig(v)
		if !ok || n == nil {
			return 0, JSONErrorf(pointer, "expected a number, got %s", jsonKind(v))
		}

		text = n.String()
	}

	var zero T

	bits := 64
	if _, isFloat32 := any(zero).(float32); isFloat32 {
		bits = 32
	}

	// strconv also reads hexadecimal floats, underscores, "inf" and "nan",
	// none of which JSON or flatc's parser accepts.
	if _, ok := parseJSONNumber(text); !ok {
		return zero, JSONErrorf(pointer, "%q is not a number", jsonClip(text))
	}

	f, err := strconv.ParseFloat(text, bits)
	if err != nil || math.IsNaN(f) || math.IsInf(f, 0) {
		return zero, JSONErrorf(pointer, "%s is not a finite %d-bit float", jsonClip(text), bits)
	}

	return T(f), nil
}

// JSONEnum reads an enum value: a variant name, or a number of the enum's
// width (one from a newer schema, say).
func JSONEnum[E JSONInteger](v any, pointer string, values map[string]E) (E, error) {
	if s, ok := v.(string); ok {
		if value, found := values[s]; found {
			return value, nil
		}

		if !looksNumeric(s) {
			return 0, JSONErrorf(pointer, "%q is not a variant of this enum", jsonClip(s))
		}
	}

	return jsonEnumNumber[E](v, pointer)
}

// JSONBitFlags reads a bit_flags value: space-separated variant names, or a
// number of the enum's width.
func JSONBitFlags[E JSONInteger](v any, pointer string, values map[string]E) (E, error) {
	s, ok := v.(string)
	if !ok || looksNumeric(s) {
		return jsonEnumNumber[E](v, pointer)
	}

	var flags E

	for _, word := range strings.Split(s, " ") {
		value, found := values[word]
		if !found {
			return 0, JSONErrorf(pointer, "%q is not a variant of this enum", jsonClip(word))
		}

		flags |= value
	}

	return flags, nil
}

func jsonEnumNumber[E JSONInteger](v any, pointer string) (E, error) {
	n, text, err := jsonBigInt(v, pointer)
	if err != nil {
		return 0, err
	}

	return integerFromBig[E](n, text, pointer)
}

// maxIntegerDigits is more digits than any 64-bit integer has: a value with
// more is out of every FlatBuffers integer's range, and saying so needs no
// arithmetic on it.
const maxIntegerDigits = 21

// jsonNumber is a decimal number split into sign, significant digits (no
// leading or trailing zeros; empty for zero) and a power of ten:
// [-]digits * 10^exp.
type jsonNumber struct {
	negative bool
	digits   string
	exp      int
}

// parseJSONNumber splits text, which must be a JSON number, into a
// jsonNumber. The exponent saturates rather than overflow, so no spelling,
// however long, costs more than reading it.
func parseJSONNumber(text string) (jsonNumber, bool) {
	var n jsonNumber

	i := 0
	if i < len(text) && text[i] == '-' {
		n.negative = true
		i++
	}

	intStart := i
	for i < len(text) && isDigit(text[i]) {
		i++
	}

	intPart := text[intStart:i]
	if intPart == "" || len(intPart) > 1 && intPart[0] == '0' {
		return n, false
	}

	var fracPart string

	if i < len(text) && text[i] == '.' {
		i++
		fracStart := i

		for i < len(text) && isDigit(text[i]) {
			i++
		}

		fracPart = text[fracStart:i]
		if fracPart == "" {
			return n, false
		}
	}

	exp := 0

	if i < len(text) && (text[i] == 'e' || text[i] == 'E') {
		i++

		expNegative := false
		if i < len(text) && (text[i] == '+' || text[i] == '-') {
			expNegative = text[i] == '-'
			i++
		}

		// Saturate far past any meaningful exponent, low enough that the
		// next step cannot overflow even a 32-bit int.
		const expLimit = 100_000_000

		expStart := i
		for i < len(text) && isDigit(text[i]) {
			if exp < expLimit {
				exp = exp*10 + int(text[i]-'0')
			}

			i++
		}

		if i == expStart {
			return n, false
		}

		if expNegative {
			exp = -exp
		}
	}

	if i != len(text) {
		return n, false
	}

	digits := strings.TrimLeft(intPart+fracPart, "0")
	trimmed := strings.TrimRight(digits, "0")
	n.exp = exp - len(fracPart) + len(digits) - len(trimmed)
	n.digits = trimmed

	return n, true
}

func isDigit(c byte) bool {
	return c >= '0' && c <= '9'
}

// jsonBigInt reads an integer from a JSON number or a decimal string,
// exactly, returning the text it was read from for error messages. A string
// holds only an optional minus sign and digits; a number may be spelled as a
// decimal (5.0, 1e3) when its value is whole.
func jsonBigInt(v any, pointer string) (*big.Int, string, error) {
	var (
		text  string
		isStr bool
	)

	switch x := v.(type) {
	case json.Number:
		text = string(x)
	case string:
		text, isStr = x, true
	default:
		// A generic value built by hand may hold any Go number.
		n, ok := goNumberToBig(v)
		if !ok {
			return nil, "", JSONErrorf(pointer, "expected an integer, got %s", jsonKind(v))
		}

		if n == nil {
			return nil, "", JSONErrorf(pointer, "%v is not an integer", v)
		}

		return n, n.String(), nil
	}

	num, ok := parseJSONNumber(text)
	if !ok || isStr && !isDecimalInteger(text) {
		return nil, text, JSONErrorf(pointer, "%q is not an integer", jsonClip(text))
	}

	if num.digits == "" {
		return new(big.Int), text, nil
	}

	if num.exp < 0 {
		return nil, text, JSONErrorf(pointer, "%s is not an integer", jsonClip(text))
	}

	// Too large for any integer type: report it with a stand-in of the right
	// sign rather than build it.
	if len(num.digits)+num.exp > maxIntegerDigits {
		n := new(big.Int).Exp(big.NewInt(10), big.NewInt(maxIntegerDigits), nil)
		if num.negative {
			n.Neg(n)
		}

		return n, text, nil
	}

	n, _ := new(big.Int).SetString(num.digits+strings.Repeat("0", num.exp), 10)
	if num.negative {
		n.Neg(n)
	}

	return n, text, nil
}

// isDecimalInteger reports whether s is an optional minus sign and digits.
func isDecimalInteger(s string) bool {
	s = strings.TrimPrefix(s, "-")
	if s == "" {
		return false
	}

	for i := range len(s) {
		if !isDigit(s[i]) {
			return false
		}
	}

	return true
}

// goNumberToBig converts a Go number in a hand-built generic value to an
// integer. ok is false when v is not a number at all; n is nil when it is a
// float with a fractional part (or not finite).
func goNumberToBig(v any) (n *big.Int, ok bool) {
	switch x := v.(type) {
	case int:
		return big.NewInt(int64(x)), true
	case int8:
		return big.NewInt(int64(x)), true
	case int16:
		return big.NewInt(int64(x)), true
	case int32:
		return big.NewInt(int64(x)), true
	case int64:
		return big.NewInt(x), true
	case uint:
		return new(big.Int).SetUint64(uint64(x)), true
	case uint8:
		return new(big.Int).SetUint64(uint64(x)), true
	case uint16:
		return new(big.Int).SetUint64(uint64(x)), true
	case uint32:
		return new(big.Int).SetUint64(uint64(x)), true
	case uint64:
		return new(big.Int).SetUint64(x), true
	case float32:
		return floatToBig(float64(x)), true
	case float64:
		return floatToBig(x), true
	default:
		return nil, false
	}
}

func floatToBig(f float64) *big.Int {
	if math.IsNaN(f) || math.IsInf(f, 0) || f != math.Trunc(f) {
		return nil
	}

	n, _ := big.NewFloat(f).Int(nil)

	return n
}

// looksNumeric reports whether an enum's string value is a number rather
// than a name: names start with a letter or underscore.
func looksNumeric(s string) bool {
	if s == "" {
		return false
	}

	c := s[0]

	return c == '-' || c == '+' || isDigit(c)
}

// integerBits counts T's bits by shifting a one bit out of it.
func integerBits[T JSONInteger]() int {
	bits := 0
	for probe := T(1); probe != 0; probe <<= 1 {
		bits++
	}

	return bits
}

func isSigned[T JSONInteger]() bool {
	return ^T(0) < 0
}

// integerRange returns the smallest and largest values of T.
func integerRange[T JSONInteger]() (*big.Int, *big.Int) {
	bits := uint(integerBits[T]())
	if !isSigned[T]() {
		hi := new(big.Int).Lsh(big.NewInt(1), bits)

		return big.NewInt(0), hi.Sub(hi, big.NewInt(1))
	}

	hi := new(big.Int).Lsh(big.NewInt(1), bits-1)
	lo := new(big.Int).Neg(hi)

	return lo, hi.Sub(hi, big.NewInt(1))
}

func jsonKind(v any) string {
	switch v.(type) {
	case nil:
		return "null"
	case bool:
		return "a boolean"
	case json.Number, float64:
		return "a number"
	case string:
		return "a string"
	case []any:
		return "an array"
	case map[string]any:
		return "an object"
	default:
		return fmt.Sprintf("%T", v)
	}
}

// ── Map and set keys ─────────────────────────────────────────────────────

// JSONMapKeys returns a map's keys in FlatBuffers key order (strings
// bytewise, numbers and enums by value), the order the binary's sorted entry
// vector holds them in, so a map always writes the same JSON.
func JSONMapKeys[K cmp.Ordered, V any](m map[K]V) []K {
	return slices.Sorted(maps.Keys(m))
}

// JSONBoolMapKeys returns a bool-keyed map's keys, false first.
func JSONBoolMapKeys[V any](m map[bool]V) []bool {
	keys := make([]bool, 0, len(m))
	for _, k := range []bool{false, true} {
		if _, ok := m[k]; ok {
			keys = append(keys, k)
		}
	}

	return keys
}

// JSONIntegerKey spells an integer map key as a JSON property name.
func JSONIntegerKey[T JSONInteger](k T) string {
	if k < 0 {
		return strconv.FormatInt(int64(k), 10)
	}

	return strconv.FormatUint(uint64(k), 10)
}

// JSONEnumKey spells an enum map key as a JSON property name: its variant
// name, or its number when no variant has that value.
func JSONEnumKey[E JSONInteger](k E, names map[E]string) string {
	if name, ok := names[k]; ok {
		return name
	}

	return JSONIntegerKey(k)
}

// JSONBitFlagsKey spells a bit_flags map key as a JSON property name, as
// flatc prints one: its flag names, or its number when they cannot spell it.
func JSONBitFlagsKey[E JSONInteger](k E, names map[E]string) string {
	if text, ok := BitFlagsJSONText(k, names); ok {
		return text
	}

	return JSONIntegerKey(k)
}

// JSONBoolKeyText spells a bool map key as a JSON property name.
func JSONBoolKeyText(k bool) string {
	return strconv.FormatBool(k)
}
