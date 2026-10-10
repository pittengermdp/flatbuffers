package flatbuffers

// Canonical JSON driven by schema reflection.
//
// These functions read and write the canonical JSON described in
// json_canonical.go for any table a binary schema (.bfbs) describes, with no
// generated code: ReflectToCanonicalJSON writes exactly the bytes the
// generated MarshalFlatJSON writes for the same message (after UnPack), and
// ReflectFromCanonicalJSON / ReflectFromCanonicalMap read what the generated
// UnmarshalFlatJSON reads. They sit beside ReflectPack / ReflectUnpack, whose
// own map format they leave unchanged.
//
// Where a buffer holds something an Object API value cannot (two map entries
// with one key, a union type without its value, a union variant from a newer
// schema), the JSON is what the generated code writes after UnPack has
// dropped it.

import (
	"encoding/binary"
	"fmt"
	"math"
	"sort"
	"strings"
)

// jsonMaxTables bounds how many tables ReflectToCanonicalJSON visits, as
// FlatBuffers' verifier does by default. Offsets may share a table, so a
// small buffer can otherwise describe an exponentially large document.
const jsonMaxTables = 1_000_000

// CanonicalJSONOptions selects the spelling canonical JSON uses where the
// format allows a choice. Reading accepts every spelling regardless.
type CanonicalJSONOptions struct {
	// Int64AsString writes long and ulong values as quoted decimal strings,
	// matching code generated with flatc --json-int64-as-string.
	Int64AsString bool
}

// ReflectToCanonicalJSON writes the FlatBuffer buf, whose root table is
// typeName (fully qualified, e.g. "Ln2.Message"), as canonical JSON. A
// malformed buffer is an error: offsets out of range, nesting deeper than
// JSONMaxDepth, or more tables than a verifier allows.
func ReflectToCanonicalJSON(
	schema *ReflectionSchema, typeName string, buf []byte, opts CanonicalJSONOptions,
) (out []byte, err error) {
	obj := schema.ObjectByName(typeName)
	if obj == nil || obj.IsStruct() {
		return nil, fmt.Errorf("%w: %q", errTypeNotFound, typeName)
	}

	if len(buf) < SizeUOffsetT {
		return nil, errBufferTooShort
	}

	defer recoverMalformed(&err)

	c := newCanonicalCodec(schema, opts)
	w := NewJSONWriter()
	c.writeTable(w, obj, Table{Bytes: buf, Pos: GetUOffsetT(buf)}, "")

	return w.Bytes()
}

// ReflectToCanonicalMap is ReflectToCanonicalJSON as a generic value, the
// way ParseJSON returns one: objects as map[string]any, arrays as []any,
// numbers as json.Number.
func ReflectToCanonicalMap(
	schema *ReflectionSchema, typeName string, buf []byte, opts CanonicalJSONOptions,
) (map[string]any, error) {
	data, err := ReflectToCanonicalJSON(schema, typeName, buf, opts)
	if err != nil {
		return nil, err
	}

	v, err := ParseJSON(data)
	if err != nil {
		return nil, err
	}

	return JSONObject(v, "")
}

// ReflectFromCanonicalJSON builds a finished FlatBuffer whose root table is
// typeName from canonical JSON.
func ReflectFromCanonicalJSON(schema *ReflectionSchema, typeName string, data []byte) ([]byte, error) {
	v, err := ParseJSON(data)
	if err != nil {
		return nil, err
	}

	m, err := JSONObject(v, "")
	if err != nil {
		return nil, err
	}

	return ReflectFromCanonicalMap(schema, typeName, m)
}

// ReflectFromCanonicalMap builds a finished FlatBuffer whose root table is
// typeName from a generic value: one ParseJSON returned, or one built by
// hand, where a number may also be any Go integer or float type.
func ReflectFromCanonicalMap(schema *ReflectionSchema, typeName string, m map[string]any) ([]byte, error) {
	obj := schema.ObjectByName(typeName)
	if obj == nil || obj.IsStruct() {
		return nil, fmt.Errorf("%w: %q", errTypeNotFound, typeName)
	}

	c := newCanonicalCodec(schema, CanonicalJSONOptions{})
	b := NewBuilder(reflectBuilderInitialSize)

	root, err := c.buildTable(b, obj, m, "")
	if err != nil {
		return nil, err
	}

	// The schema's root table carries the schema's file identifier, as a
	// buffer from flatc or a generated Finish<Root>Buffer does.
	if ident := schema.FileIdent(); ident != "" && schema.RootTable() != nil && schema.RootTable().Name() == typeName {
		b.FinishWithFileIdentifier(root, []byte(ident))
	} else {
		b.Finish(root)
	}

	return b.FinishedBytes(), nil
}

func recoverMalformed(err *error) {
	if r := recover(); r != nil {
		*err = fmt.Errorf("flatbuffers: malformed buffer: %v", r)
	}
}

// canonicalEnum is what canonical JSON needs of an enum, worked out once:
// the schema's accessors build their slices anew on every call, and an enum
// vector looks its enum up once per element.
type canonicalEnum struct {
	shortName string // unqualified, as generated code names it in errors
	names     map[uint64]string
	values    map[string]uint64
	bitFlags  bool
}

type canonicalCodec struct {
	schema  *ReflectionSchema
	opts    CanonicalJSONOptions
	objects []*ReflectionObject
	enums   []*ReflectionEnum
	enumAt  map[int32]*canonicalEnum
	fields  map[*ReflectionObject][]*ReflectionField
	tables  int // tables written so far
	depth   int // tables being built, innermost included
}

func newCanonicalCodec(schema *ReflectionSchema, opts CanonicalJSONOptions) *canonicalCodec {
	return &canonicalCodec{
		schema:  schema,
		opts:    opts,
		objects: schema.Objects(),
		enums:   schema.Enums(),
		enumAt:  make(map[int32]*canonicalEnum),
		fields:  make(map[*ReflectionObject][]*ReflectionField),
	}
}

// fieldsByID returns an object's fields in declaration (id) order: a binary
// schema stores them sorted by name.
func (c *canonicalCodec) fieldsByID(obj *ReflectionObject) []*ReflectionField {
	if fields, ok := c.fields[obj]; ok {
		return fields
	}

	fields := obj.Fields()
	sort.Slice(fields, func(i, j int) bool { return fields[i].ID() < fields[j].ID() })
	c.fields[obj] = fields

	return fields
}

func hasAttribute(attrs []*ReflectionKeyValue, key string) bool {
	for _, kv := range attrs {
		if kv.Key() == key {
			return true
		}
	}

	return false
}

func (c *canonicalCodec) object(t *ReflectionType) *ReflectionObject {
	return c.objects[t.Index()]
}

// enumOf returns the enum a scalar type draws its values from, or nil.
func (c *canonicalCodec) enumOf(t *ReflectionType) *canonicalEnum {
	index := t.Index()
	if index < 0 {
		return nil
	}

	if e, ok := c.enumAt[index]; ok {
		return e
	}

	re := c.enums[index]
	e := &canonicalEnum{
		shortName: re.Name()[strings.LastIndex(re.Name(), ".")+1:],
		names:     make(map[uint64]string),
		values:    make(map[string]uint64),
		bitFlags:  hasAttribute(re.Attributes(), "bit_flags"),
	}

	for _, v := range re.Values() {
		e.names[uint64(v.Value())] = v.Name()
		e.values[v.Name()] = uint64(v.Value())
	}

	c.enumAt[index] = e

	return e
}

// unionVariant returns the table a union's discriminant selects, or nil for
// NONE and for a value this schema does not know.
func (c *canonicalCodec) unionVariant(f *ReflectionField, discriminant uint64) *ReflectionObject {
	if discriminant == 0 {
		return nil
	}

	for _, v := range c.enums[f.Type().Index()].Values() {
		if uint64(v.Value()) == discriminant && v.UnionType() != nil {
			return c.object(v.UnionType())
		}
	}

	return nil
}

func isUnsignedBase(bt ReflectionBaseType) bool {
	switch bt {
	case ReflectionBaseTypeUType, ReflectionBaseTypeBool, ReflectionBaseTypeUByte,
		ReflectionBaseTypeUShort, ReflectionBaseTypeUInt, ReflectionBaseTypeULong:
		return true
	default:
		return false
	}
}

func isScalarBase(bt ReflectionBaseType) bool {
	return bt >= ReflectionBaseTypeUType && bt <= ReflectionBaseTypeDouble
}

// scalarSize returns a scalar base type's width in bytes.
func scalarSize(bt ReflectionBaseType) int {
	switch bt {
	case ReflectionBaseTypeUType, ReflectionBaseTypeBool, ReflectionBaseTypeByte, ReflectionBaseTypeUByte:
		return 1
	case ReflectionBaseTypeShort, ReflectionBaseTypeUShort:
		return 2
	case ReflectionBaseTypeInt, ReflectionBaseTypeUInt, ReflectionBaseTypeFloat:
		return 4
	default:
		return 8
	}
}

// readScalarBits reads a scalar at an absolute position as its raw bits,
// sign-extended for signed integers. A bool reads as 0 or 1, whatever
// nonzero byte holds it, as the generated accessors read one.
func readScalarBits(buf []byte, pos UOffsetT, bt ReflectionBaseType) uint64 {
	switch bt {
	case ReflectionBaseTypeBool:
		if buf[pos] != 0 {
			return 1
		}

		return 0
	case ReflectionBaseTypeByte:
		return uint64(int64(int8(buf[pos])))
	case ReflectionBaseTypeShort:
		return uint64(int64(int16(binary.LittleEndian.Uint16(buf[pos:]))))
	case ReflectionBaseTypeInt:
		return uint64(int64(int32(binary.LittleEndian.Uint32(buf[pos:]))))
	case ReflectionBaseTypeUShort:
		return uint64(binary.LittleEndian.Uint16(buf[pos:]))
	case ReflectionBaseTypeUInt, ReflectionBaseTypeFloat:
		return uint64(binary.LittleEndian.Uint32(buf[pos:]))
	case ReflectionBaseTypeLong, ReflectionBaseTypeULong, ReflectionBaseTypeDouble:
		return binary.LittleEndian.Uint64(buf[pos:])
	default:
		return uint64(buf[pos])
	}
}

// defaultBits returns a table scalar's default as raw bits. A ulong default
// past INT64_MAX is stored in default_integer as its two's-complement bits.
func defaultBits(f *ReflectionField, bt ReflectionBaseType) uint64 {
	switch bt {
	case ReflectionBaseTypeFloat:
		return uint64(math.Float32bits(float32(f.DefaultReal())))
	case ReflectionBaseTypeDouble:
		return math.Float64bits(f.DefaultReal())
	case ReflectionBaseTypeBool:
		if f.DefaultInteger() != 0 {
			return 1
		}

		return 0
	default:
		return uint64(f.DefaultInteger())
	}
}

// isDefault reports whether a table scalar's bits equal its default the way
// generated code compares them: floats by value, so -0 equals a 0 default,
// and a NaN default matches any NaN.
func isDefault(f *ReflectionField, bt ReflectionBaseType, bits uint64) bool {
	switch bt {
	case ReflectionBaseTypeFloat:
		v := math.Float32frombits(uint32(bits))
		d := float32(f.DefaultReal())

		return v == d || v != v && d != d
	case ReflectionBaseTypeDouble:
		v := math.Float64frombits(bits)
		d := f.DefaultReal()

		return v == d || math.IsNaN(v) && math.IsNaN(d)
	default:
		return bits == defaultBits(f, bt)
	}
}

// unsupportedField reports a field canonical JSON cannot spell, as the
// generated code's flatc --go-json refuses to generate one.
func unsupportedField(f *ReflectionField) bool {
	bt := f.Type().BaseType()

	return bt == ReflectionBaseTypeArray || bt == ReflectionBaseTypeVector64 || f.Offset64() ||
		hasAttribute(f.Attributes(), "nested_flatbuffer") || hasAttribute(f.Attributes(), "flexbuffer")
}

// ── Writing ──────────────────────────────────────────────────────────────

func (c *canonicalCodec) writeScalar(w *JSONWriter, bits uint64, t *ReflectionType, bt ReflectionBaseType) {
	if e := c.enumOf(t); e != nil && bt != ReflectionBaseTypeUType {
		c.writeEnum(w, bits, e, bt)

		return
	}

	switch bt {
	case ReflectionBaseTypeBool:
		w.Bool(bits != 0)
	case ReflectionBaseTypeFloat:
		w.Float32(math.Float32frombits(uint32(bits)))
	case ReflectionBaseTypeDouble:
		w.Float64(math.Float64frombits(bits))
	default:
		c.writeInteger(w, bits, bt)
	}
}

// writeInteger writes integer bits as a number, or as a quoted decimal
// string for a 64-bit integer under the int64-as-string option.
func (c *canonicalCodec) writeInteger(w *JSONWriter, bits uint64, bt ReflectionBaseType) {
	quoted := c.opts.Int64AsString && (bt == ReflectionBaseTypeLong || bt == ReflectionBaseTypeULong)

	switch {
	case isUnsignedBase(bt) && quoted:
		w.UintString(bits)
	case isUnsignedBase(bt):
		w.Uint(bits)
	case quoted:
		w.IntString(int64(bits))
	default:
		w.Int(int64(bits))
	}
}

// writeEnum writes an enum value by name, or as its number when no name
// spells it -- a 64-bit number quoted under the int64-as-string option.
func (c *canonicalCodec) writeEnum(w *JSONWriter, bits uint64, e *canonicalEnum, bt ReflectionBaseType) {
	if e.bitFlags {
		if text, ok := BitFlagsJSONText(bits, e.names); ok {
			w.String(text)

			return
		}
	} else if name, ok := e.names[bits]; ok {
		w.String(name)

		return
	}

	c.writeInteger(w, bits, bt)
}

// keyText spells a map key, given its raw bits, as a property name.
func (c *canonicalCodec) keyText(bits uint64, f *ReflectionField) string {
	t := f.Type()
	bt := t.BaseType()

	if e := c.enumOf(t); e != nil {
		if e.bitFlags {
			if text, ok := BitFlagsJSONText(bits, e.names); ok {
				return text
			}
		} else if name, ok := e.names[bits]; ok {
			return name
		}
	}

	if bt == ReflectionBaseTypeBool {
		return JSONBoolKeyText(bits != 0)
	}

	if isUnsignedBase(bt) {
		return JSONIntegerKey(bits)
	}

	return JSONIntegerKey(int64(bits))
}

func (c *canonicalCodec) writeTable(w *JSONWriter, obj *ReflectionObject, tab Table, pointer string) {
	// After the first error nothing more is written, so a buffer whose
	// offsets loop stops at JSONMaxDepth instead of recursing on.
	if w.err != nil {
		return
	}

	c.tables++
	if c.tables > jsonMaxTables {
		w.Fail(JSONErrorf(pointer, "more than %d tables", jsonMaxTables))

		return
	}

	w.BeginObject()

	for _, f := range c.fieldsByID(obj) {
		if f.Deprecated() {
			continue
		}

		c.writeTableField(w, obj, f, tab, pointer)
	}

	w.EndObject()
}

func (c *canonicalCodec) writeTableField(w *JSONWriter, obj *ReflectionObject, f *ReflectionField, tab Table, pointer string) {
	t := f.Type()
	bt := t.BaseType()

	if bt == ReflectionBaseTypeUType {
		return // written with its union
	}

	if unsupportedField(f) {
		w.Fail(JSONErrorf(JSONPointer(pointer, f.Name()), "canonical JSON does not support this field"))

		return
	}

	o := tab.Offset(VOffsetT(f.Offset()))
	pos := tab.Pos + UOffsetT(o)

	switch {
	case isScalarBase(bt):
		if f.Optional() {
			if o == 0 {
				return
			}

			w.Key(f.Name())
			c.writeScalar(w, readScalarBits(tab.Bytes, pos, bt), t, bt)

			return
		}

		bits := defaultBits(f, bt)
		if o != 0 {
			bits = readScalarBits(tab.Bytes, pos, bt)
		}

		if isDefault(f, bt, bits) {
			return
		}

		w.Key(f.Name())
		c.writeScalar(w, bits, t, bt)
	case bt == ReflectionBaseTypeString:
		s := ""
		if o != 0 {
			s = tab.String(pos)
		}

		if s == "" && !f.Required() {
			return
		}

		w.Key(f.Name())
		w.String(s)
	case bt == ReflectionBaseTypeObj:
		child := c.object(t)

		if o == 0 {
			if f.Required() || child.IsStruct() && obj.IsStruct() {
				w.Fail(JSONErrorf(JSONPointer(pointer, f.Name()), "required member is missing"))
			}

			return
		}

		w.Key(f.Name())

		if child.IsStruct() {
			c.writeStruct(w, child, tab.Bytes, pos)

			return
		}

		c.writeTable(w, child, Table{Bytes: tab.Bytes, Pos: pos + GetUOffsetT(tab.Bytes[pos:])}, JSONPointer(pointer, f.Name()))
	case bt == ReflectionBaseTypeUnion:
		c.writeUnion(w, obj, f, tab, pointer)
	case bt == ReflectionBaseTypeVector:
		n := 0
		vec := UOffsetT(0)

		if o != 0 {
			vec = pos + GetUOffsetT(tab.Bytes[pos:])
			n = int(GetUOffsetT(tab.Bytes[vec:]))
		}

		// A required vector is always written, as the generated code
		// writes the empty slice UnPack leaves for a missing one.
		if n == 0 && !f.Required() {
			return
		}

		w.Key(f.Name())
		c.writeVector(w, f, tab.Bytes, vec+UOffsetT(SizeUOffsetT), n, JSONPointer(pointer, f.Name()))
	}
}

// writeUnion writes a union's type and value, or nothing when there is no
// value to write: NONE, a type whose value is absent, or a variant this
// schema does not know (one from a newer schema), each of which UnPack
// reads as no union.
func (c *canonicalCodec) writeUnion(w *JSONWriter, obj *ReflectionObject, f *ReflectionField, tab Table, pointer string) {
	typeField := obj.FieldByName(f.Name() + "_type")
	if typeField == nil {
		w.Fail(JSONErrorf(JSONPointer(pointer, f.Name()), "union has no type field"))

		return
	}

	to := tab.Offset(VOffsetT(typeField.Offset()))
	o := tab.Offset(VOffsetT(f.Offset()))

	if to == 0 || o == 0 {
		return
	}

	discriminant := uint64(tab.Bytes[tab.Pos+UOffsetT(to)])

	variant := c.unionVariant(f, discriminant)
	if variant == nil {
		return
	}

	w.Key(typeField.Name())
	c.writeEnum(w, discriminant, c.enumOf(f.Type()), ReflectionBaseTypeUType)

	pos := tab.Pos + UOffsetT(o)

	w.Key(f.Name())
	c.writeTable(w, variant, Table{Bytes: tab.Bytes, Pos: pos + GetUOffsetT(tab.Bytes[pos:])}, JSONPointer(pointer, f.Name()))
}

func (c *canonicalCodec) writeStruct(w *JSONWriter, obj *ReflectionObject, buf []byte, pos UOffsetT) {
	w.BeginObject()

	for _, f := range c.fieldsByID(obj) {
		t := f.Type()
		bt := t.BaseType()
		fpos := pos + UOffsetT(f.Offset())

		w.Key(f.Name())

		switch {
		case bt == ReflectionBaseTypeObj:
			c.writeStruct(w, c.object(t), buf, fpos)
		case isScalarBase(bt):
			c.writeScalar(w, readScalarBits(buf, fpos, bt), t, bt)
		default:
			w.Fail(JSONErrorf("", "canonical JSON does not support fixed-size arrays"))
		}
	}

	w.EndObject()
}

// sortKey orders map/set entries the way FlatBuffers sorts keyed vectors:
// strings bytewise, numbers by value. It is comparable, so it also tells
// two entries with the same key apart from two with different ones.
type sortKey struct {
	text  string
	bits  uint64
	isStr bool
	sign  bool
}

func (a sortKey) less(b sortKey) bool {
	if a.isStr {
		return a.text < b.text
	}

	if a.sign {
		return int64(a.bits) < int64(b.bits)
	}

	return a.bits < b.bits
}

type canonicalEntry struct {
	order sortKey
	pos   UOffsetT
}

func (c *canonicalCodec) writeVector(w *JSONWriter, f *ReflectionField, buf []byte, data UOffsetT, n int, pointer string) {
	t := f.Type()
	elem := t.Element()

	if elem == ReflectionBaseTypeObj {
		entry := c.object(t)
		isMap := hasAttribute(f.Attributes(), "map_entry")

		if isMap || hasAttribute(f.Attributes(), "set_entry") {
			c.writeMapOrSet(w, entry, isMap, buf, data, n, pointer)

			return
		}

		w.BeginArray()

		for i := 0; i < n && w.err == nil; i++ {
			if entry.IsStruct() {
				c.writeStruct(w, entry, buf, data+UOffsetT(i*entry.ByteSize()))

				continue
			}

			at := data + UOffsetT(i*SizeUOffsetT)
			c.writeTable(w, entry, Table{Bytes: buf, Pos: at + GetUOffsetT(buf[at:])}, JSONIndexPointer(pointer, i))
		}

		w.EndArray()

		return
	}

	w.BeginArray()

	for i := 0; i < n && w.err == nil; i++ {
		switch {
		case elem == ReflectionBaseTypeString:
			w.String((&Table{Bytes: buf}).String(data + UOffsetT(i*SizeUOffsetT)))
		case isScalarBase(elem):
			size := scalarSize(elem)
			c.writeScalar(w, readScalarBits(buf, data+UOffsetT(i*size), elem), t, elem)
		default:
			w.Fail(JSONErrorf(pointer, "canonical JSON does not support vectors of unions"))
		}
	}

	w.EndArray()
}

func entryFields(entry *ReflectionObject) (*ReflectionField, *ReflectionField) {
	var key, value *ReflectionField

	for _, f := range entry.Fields() {
		if f.Key() {
			key = f
		}

		if f.Name() == "value" {
			value = f
		}
	}

	return key, value
}

// writeMapOrSet writes a map as an object and a set as an array, in key
// order. Entries sharing a key are written once, holding the last one's
// value, as UnPack keeps them.
func (c *canonicalCodec) writeMapOrSet(w *JSONWriter, entry *ReflectionObject, isMap bool, buf []byte, data UOffsetT, n int, pointer string) {
	keyField, valueField := entryFields(entry)
	if keyField == nil {
		w.Fail(JSONErrorf(pointer, "map/set entry has no key"))

		return
	}

	kt := keyField.Type().BaseType()
	byKey := make(map[sortKey]int, n)
	entries := make([]canonicalEntry, 0, n)

	for i := range n {
		at := data + UOffsetT(i*SizeUOffsetT)
		tab := Table{Bytes: buf, Pos: at + GetUOffsetT(buf[at:])}
		ko := tab.Offset(VOffsetT(keyField.Offset()))

		var order sortKey

		if kt == ReflectionBaseTypeString {
			text := ""
			if ko != 0 {
				text = tab.String(tab.Pos + UOffsetT(ko))
			}

			order = sortKey{text: text, isStr: true}
		} else {
			bits := defaultBits(keyField, kt)
			if ko != 0 {
				bits = readScalarBits(buf, tab.Pos+UOffsetT(ko), kt)
			}

			order = sortKey{bits: bits, sign: !isUnsignedBase(kt)}
		}

		if j, seen := byKey[order]; seen {
			entries[j].pos = tab.Pos

			continue
		}

		byKey[order] = len(entries)
		entries = append(entries, canonicalEntry{order: order, pos: tab.Pos})
	}

	sort.Slice(entries, func(i, j int) bool { return entries[i].order.less(entries[j].order) })

	if isMap {
		w.BeginObject()
	} else {
		w.BeginArray()
	}

	for _, e := range entries {
		if w.err != nil {
			break
		}

		key := e.order.text
		if !e.order.isStr {
			key = c.keyText(e.order.bits, keyField)
		}

		if !isMap {
			if e.order.isStr {
				w.SetMember(key)
			} else {
				c.writeScalar(w, e.order.bits, keyField.Type(), kt)
			}

			continue
		}

		w.MapKey(key)

		if valueField == nil {
			w.Null()

			continue
		}

		c.writeEntryValue(w, valueField, Table{Bytes: buf, Pos: e.pos}, JSONPointer(pointer, key))
	}

	if isMap {
		w.EndObject()
	} else {
		w.EndArray()
	}
}

// writeEntryValue writes a map value. Unlike a table member, a map value is
// written even when it equals its default: the entry exists. An absent
// string is written as "" and an absent table or struct as null, as the
// generated code writes what UnPack leaves for them.
func (c *canonicalCodec) writeEntryValue(w *JSONWriter, f *ReflectionField, tab Table, pointer string) {
	t := f.Type()
	bt := t.BaseType()
	o := tab.Offset(VOffsetT(f.Offset()))
	pos := tab.Pos + UOffsetT(o)

	switch {
	case isScalarBase(bt):
		bits := defaultBits(f, bt)
		if o != 0 {
			bits = readScalarBits(tab.Bytes, pos, bt)
		}

		c.writeScalar(w, bits, t, bt)
	case bt == ReflectionBaseTypeString:
		if o == 0 {
			w.String("")

			return
		}

		w.String(tab.String(pos))
	case bt == ReflectionBaseTypeObj:
		child := c.object(t)

		switch {
		case o == 0:
			w.Null()
		case child.IsStruct():
			c.writeStruct(w, child, tab.Bytes, pos)
		default:
			c.writeTable(w, child, Table{Bytes: tab.Bytes, Pos: pos + GetUOffsetT(tab.Bytes[pos:])}, pointer)
		}
	default:
		w.Fail(JSONErrorf(pointer, "unsupported map value"))
	}
}

// ── Building ─────────────────────────────────────────────────────────────

// scalarFromJSON reads a JSON value as a scalar's raw bits.
func (c *canonicalCodec) scalarFromJSON(v any, t *ReflectionType, bt ReflectionBaseType, pointer string) (uint64, error) {
	if e := c.enumOf(t); e != nil && bt != ReflectionBaseTypeUType {
		return enumFromJSON(v, e, bt, pointer)
	}

	switch bt {
	case ReflectionBaseTypeBool:
		b, err := JSONBool(v, pointer)
		if b {
			return 1, err
		}

		return 0, err
	case ReflectionBaseTypeFloat:
		f, err := JSONFloat[float32](v, pointer)

		return uint64(math.Float32bits(f)), err
	case ReflectionBaseTypeDouble:
		f, err := JSONFloat[float64](v, pointer)

		return math.Float64bits(f), err
	default:
		return integerBitsFromJSON(v, bt, pointer)
	}
}

func integerBitsFromJSON(v any, bt ReflectionBaseType, pointer string) (uint64, error) {
	switch bt {
	case ReflectionBaseTypeByte:
		x, err := JSONInt[int8](v, pointer)

		return uint64(int64(x)), err
	case ReflectionBaseTypeUByte, ReflectionBaseTypeUType:
		x, err := JSONInt[uint8](v, pointer)

		return uint64(x), err
	case ReflectionBaseTypeShort:
		x, err := JSONInt[int16](v, pointer)

		return uint64(int64(x)), err
	case ReflectionBaseTypeUShort:
		x, err := JSONInt[uint16](v, pointer)

		return uint64(x), err
	case ReflectionBaseTypeInt:
		x, err := JSONInt[int32](v, pointer)

		return uint64(int64(x)), err
	case ReflectionBaseTypeUInt:
		x, err := JSONInt[uint32](v, pointer)

		return uint64(x), err
	case ReflectionBaseTypeLong:
		x, err := JSONInt[int64](v, pointer)

		return uint64(x), err
	default:
		return JSONInt[uint64](v, pointer)
	}
}

// enumFromJSON reads an enum value as JSONEnum and JSONBitFlags do.
func enumFromJSON(v any, e *canonicalEnum, bt ReflectionBaseType, pointer string) (uint64, error) {
	s, isString := v.(string)
	if !isString || looksNumeric(s) {
		return integerBitsFromJSON(v, bt, pointer)
	}

	if !e.bitFlags {
		bits, ok := e.values[s]
		if !ok {
			return 0, JSONErrorf(pointer, "%q is not a variant of this enum", jsonClip(s))
		}

		return bits, nil
	}

	var bits uint64

	// Split on single spaces, as JSONBitFlags does: a doubled or trailing
	// space leaves an empty word, which names no variant.
	for _, word := range strings.Split(s, " ") {
		flag, ok := e.values[word]
		if !ok {
			return 0, JSONErrorf(pointer, "%q is not a variant of this enum", jsonClip(word))
		}

		bits |= flag
	}

	return bits, nil
}

func prependScalar(b *Builder, bt ReflectionBaseType, bits uint64) {
	switch scalarSize(bt) {
	case 1:
		b.PrependByte(byte(bits))
	case 2:
		b.PrependUint16(uint16(bits))
	case 4:
		b.PrependUint32(uint32(bits))
	default:
		b.PrependUint64(bits)
	}
}

func putScalar(img []byte, at int, bt ReflectionBaseType, bits uint64) {
	switch scalarSize(bt) {
	case 1:
		img[at] = byte(bits)
	case 2:
		binary.LittleEndian.PutUint16(img[at:], uint16(bits))
	case 4:
		binary.LittleEndian.PutUint32(img[at:], uint32(bits))
	default:
		binary.LittleEndian.PutUint64(img[at:], bits)
	}
}

// structImage lays out a struct's bytes, padding zeroed.
func (c *canonicalCodec) structImage(obj *ReflectionObject, v any, pointer string, img []byte, at int) error {
	m, err := JSONObject(v, pointer)
	if err != nil {
		return err
	}

	fields := c.fieldsByID(obj)
	names := make([]string, len(fields))

	for i, f := range fields {
		names[i] = f.Name()
	}

	err = JSONCheckMembers(m, pointer, names...)
	if err != nil {
		return err
	}

	for _, f := range fields {
		x, ok := JSONMember(m, f.Name())
		if !ok {
			return JSONRequired(pointer, f.Name())
		}

		t := f.Type()
		bt := t.BaseType()
		fp := JSONPointer(pointer, f.Name())

		switch {
		case bt == ReflectionBaseTypeObj:
			err = c.structImage(c.object(t), x, fp, img, at+int(f.Offset()))
		case isScalarBase(bt):
			var bits uint64

			bits, err = c.scalarFromJSON(x, t, bt, fp)
			putScalar(img, at+int(f.Offset()), bt, bits)
		default:
			err = JSONErrorf(fp, "canonical JSON does not support fixed-size arrays")
		}

		if err != nil {
			return err
		}
	}

	return nil
}

// newStructImage lays out a struct value read from JSON.
func (c *canonicalCodec) newStructImage(obj *ReflectionObject, v any, pointer string) ([]byte, error) {
	img := make([]byte, obj.ByteSize())

	err := c.structImage(obj, v, pointer, img, 0)
	if err != nil {
		return nil, err
	}

	return img, nil
}

// placeStruct writes a struct's bytes inline at the builder's head,
// returning its offset.
func placeStruct(b *Builder, obj *ReflectionObject, img []byte) UOffsetT {
	b.Prep(obj.MinAlign(), len(img))

	for i := len(img) - 1; i >= 0; i-- {
		b.PlaceByte(img[i])
	}

	return b.Offset()
}

// pendingField is one table field read from JSON, waiting to be added once
// everything stored out of line has been built.
type pendingField struct {
	field  *ReflectionField
	offset UOffsetT // a string, table or vector built out of line
	bits   uint64   // a scalar
	image  []byte   // a struct, written inline
}

// addPending adds fields to the table the builder has started.
func (c *canonicalCodec) addPending(b *Builder, pending []pendingField) {
	for _, p := range pending {
		t := p.field.Type()
		bt := t.BaseType()
		slot := int(p.field.ID())

		switch {
		case isScalarBase(bt):
			if p.field.Optional() || !isDefault(p.field, bt, p.bits) {
				prependScalar(b, bt, p.bits)
				b.Slot(slot)
			}
		case p.image != nil:
			b.PrependStructSlot(slot, placeStruct(b, c.object(t), p.image), 0)
		default:
			b.PrependUOffsetTSlot(slot, p.offset, 0)
		}
	}
}

func slotCount(fields []*ReflectionField) int {
	slots := 0

	for _, f := range fields {
		if int(f.ID())+1 > slots {
			slots = int(f.ID()) + 1
		}
	}

	return slots
}

func (c *canonicalCodec) buildTable(b *Builder, obj *ReflectionObject, m map[string]any, pointer string) (UOffsetT, error) {
	// ParseJSON bounds a parsed document's nesting; a value built by hand
	// may even contain itself.
	c.depth++
	defer func() { c.depth-- }()

	if c.depth > JSONMaxDepth {
		return 0, JSONErrorf(pointer, "nested more than %d deep", JSONMaxDepth)
	}

	fields := c.fieldsByID(obj)
	names := make([]string, len(fields))

	for i, f := range fields {
		names[i] = f.Name()
	}

	err := JSONCheckMembers(m, pointer, names...)
	if err != nil {
		return 0, err
	}

	// First everything stored out of line, which must precede the table,
	// field by field in declaration order, as the generated code reads them.
	pending := make([]pendingField, 0, len(fields))

	for _, f := range fields {
		if f.Deprecated() {
			continue
		}

		t := f.Type()
		bt := t.BaseType()
		fp := JSONPointer(pointer, f.Name())

		if bt == ReflectionBaseTypeUType {
			continue // built with its union
		}

		if bt == ReflectionBaseTypeUnion {
			p, uerr := c.buildUnion(b, obj, f, m, pointer)
			if uerr != nil {
				return 0, uerr
			}

			pending = append(pending, p...)

			continue
		}

		x, present := JSONMember(m, f.Name())
		if !present {
			if f.Required() {
				return 0, JSONRequired(pointer, f.Name())
			}

			continue
		}

		if unsupportedField(f) {
			return 0, JSONErrorf(fp, "canonical JSON does not support this field")
		}

		p := pendingField{field: f}

		switch {
		case isScalarBase(bt):
			p.bits, err = c.scalarFromJSON(x, t, bt, fp)
		case bt == ReflectionBaseTypeString:
			var s string

			s, err = JSONString(x, fp)
			p.offset = b.CreateString(s)
		case bt == ReflectionBaseTypeObj && c.object(t).IsStruct():
			p.image, err = c.newStructImage(c.object(t), x, fp)
		case bt == ReflectionBaseTypeObj:
			var child map[string]any

			child, err = JSONObject(x, fp)
			if err == nil {
				p.offset, err = c.buildTable(b, c.object(t), child, fp)
			}
		case bt == ReflectionBaseTypeVector:
			p.offset, err = c.buildVector(b, f, x, fp)
		default:
			err = JSONErrorf(fp, "canonical JSON does not support this field")
		}

		if err != nil {
			return 0, err
		}

		pending = append(pending, p)
	}

	b.StartObject(slotCount(fields))
	c.addPending(b, pending)

	return b.EndObject(), nil
}

// buildUnion builds a union's value table and returns it, with its type, as
// fields still to be added to the enclosing table. Its checks run in the
// order the generated Unmarshal<Union>FlatJSON makes them.
func (c *canonicalCodec) buildUnion(
	b *Builder, obj *ReflectionObject, f *ReflectionField, m map[string]any, pointer string,
) ([]pendingField, error) {
	typeField := obj.FieldByName(f.Name() + "_type")
	if typeField == nil {
		return nil, JSONErrorf(JSONPointer(pointer, f.Name()), "union has no type field")
	}

	tv, hasType := JSONMember(m, typeField.Name())
	value, hasValue := JSONMember(m, f.Name())
	tp := JSONPointer(pointer, typeField.Name())
	fp := JSONPointer(pointer, f.Name())

	if !hasType {
		if hasValue {
			return nil, JSONErrorf(fp, "union value without %s", typeField.Name())
		}

		return nil, nil
	}

	union := c.enumOf(f.Type())

	discriminant, err := enumFromJSON(tv, union, ReflectionBaseTypeUType, tp)
	if err != nil {
		return nil, err
	}

	if !hasValue {
		return nil, nil // a type with no value reads as no union
	}

	if discriminant == 0 {
		return nil, JSONErrorf(fp, "union value under NONE")
	}

	variant := c.unionVariant(f, discriminant)
	if variant == nil {
		return nil, JSONErrorf(tp, "%d is not a variant of %s", discriminant, union.shortName)
	}

	child, err := JSONObject(value, fp)
	if err != nil {
		return nil, err
	}

	off, err := c.buildTable(b, variant, child, fp)
	if err != nil {
		return nil, err
	}

	return []pendingField{
		{field: typeField, bits: discriminant},
		{field: f, offset: off},
	}, nil
}

func (c *canonicalCodec) buildVector(b *Builder, f *ReflectionField, x any, pointer string) (UOffsetT, error) {
	t := f.Type()
	elem := t.Element()

	if elem == ReflectionBaseTypeObj {
		entry := c.object(t)
		if hasAttribute(f.Attributes(), "map_entry") {
			return c.buildMap(b, entry, x, pointer)
		}

		if hasAttribute(f.Attributes(), "set_entry") {
			return c.buildSet(b, entry, x, pointer)
		}
	}

	a, err := JSONArray(x, pointer)
	if err != nil {
		return 0, err
	}

	n := len(a)

	switch {
	case elem == ReflectionBaseTypeString || elem == ReflectionBaseTypeObj && !c.object(t).IsStruct():
		offsets := make([]UOffsetT, n)

		for i, e := range a {
			ep := JSONIndexPointer(pointer, i)
			if elem == ReflectionBaseTypeString {
				s, serr := JSONString(e, ep)
				if serr != nil {
					return 0, serr
				}

				offsets[i] = b.CreateString(s)

				continue
			}

			child, oerr := JSONObject(e, ep)
			if oerr != nil {
				return 0, oerr
			}

			offsets[i], err = c.buildTable(b, c.object(t), child, ep)
			if err != nil {
				return 0, err
			}
		}

		b.StartVector(SizeUOffsetT, n, SizeUOffsetT)

		for i := n - 1; i >= 0; i-- {
			b.PrependUOffsetT(offsets[i])
		}

		return b.EndVector(n), nil
	case elem == ReflectionBaseTypeObj:
		entry := c.object(t)
		images := make([][]byte, n)

		for i, e := range a {
			images[i], err = c.newStructImage(entry, e, JSONIndexPointer(pointer, i))
			if err != nil {
				return 0, err
			}
		}

		b.StartVector(entry.ByteSize(), n, entry.MinAlign())

		for i := n - 1; i >= 0; i-- {
			b.Prep(entry.MinAlign(), entry.ByteSize())

			for j := len(images[i]) - 1; j >= 0; j-- {
				b.PlaceByte(images[i][j])
			}
		}

		return b.EndVector(n), nil
	case isScalarBase(elem):
		values := make([]uint64, n)

		for i, e := range a {
			values[i], err = c.scalarFromJSON(e, t, elem, JSONIndexPointer(pointer, i))
			if err != nil {
				return 0, err
			}
		}

		size := scalarSize(elem)
		b.StartVector(size, n, size)

		for i := n - 1; i >= 0; i-- {
			prependScalar(b, elem, values[i])
		}

		return b.EndVector(n), nil
	default:
		return 0, JSONErrorf(pointer, "canonical JSON does not support vectors of unions")
	}
}

// keyEntry reads one map key or set member into an entry's key field.
func (c *canonicalCodec) keyEntry(keyField *ReflectionField, v any, isPropertyName bool, pointer string) (pendingField, sortKey, error) {
	t := keyField.Type()
	bt := t.BaseType()

	if bt == ReflectionBaseTypeString {
		s, err := JSONString(v, pointer)

		return pendingField{field: keyField}, sortKey{text: s, isStr: true}, err
	}

	if isPropertyName && bt == ReflectionBaseTypeBool {
		text, isString := v.(string)
		if !isString {
			return pendingField{}, sortKey{}, JSONErrorf(pointer, "map key is not a string")
		}

		k, err := JSONBoolKey(text, pointer)
		if k {
			return pendingField{field: keyField, bits: 1}, sortKey{bits: 1}, err
		}

		return pendingField{field: keyField}, sortKey{}, err
	}

	bits, err := c.scalarFromJSON(v, t, bt, pointer)

	return pendingField{field: keyField, bits: bits}, sortKey{bits: bits, sign: !isUnsignedBase(bt)}, err
}

// mapEntry is one map or set entry read from JSON. Entries sharing a key
// collapse to the last one, in the order the generated code reads them.
type mapEntry struct {
	order  sortKey
	fields []pendingField // the key, and a scalar or struct value
	value  *deferredValue // a string or table value
}

// deferredValue is a map value stored out of line. Generated Pack builds
// each entry's key, value and table together, in key order; the values are
// built then too, so the buffer matches byte for byte.
type deferredValue struct {
	field   *ReflectionField
	text    string            // a string value
	obj     *ReflectionObject // a table value's type, nil for a string
	child   map[string]any
	pointer string
}

func collapseEntry(entries []mapEntry, byKey map[sortKey]int, e mapEntry) []mapEntry {
	if j, seen := byKey[e.order]; seen {
		entries[j] = e

		return entries
	}

	byKey[e.order] = len(entries)

	return append(entries, e)
}

// finishEntries builds each entry table, then the vector holding them in key
// order.
func (c *canonicalCodec) finishEntries(b *Builder, entry *ReflectionObject, entries []mapEntry) (UOffsetT, error) {
	sort.Slice(entries, func(i, j int) bool { return entries[i].order.less(entries[j].order) })

	slots := slotCount(c.fieldsByID(entry))
	offsets := make([]UOffsetT, len(entries))

	for i, e := range entries {
		fields := e.fields

		if fields[0].field.Type().BaseType() == ReflectionBaseTypeString {
			fields[0].offset = b.CreateString(e.order.text)
		}

		if d := e.value; d != nil {
			p := pendingField{field: d.field}

			if d.obj == nil {
				p.offset = b.CreateString(d.text)
			} else {
				off, err := c.buildTable(b, d.obj, d.child, d.pointer)
				if err != nil {
					return 0, err
				}

				p.offset = off
			}

			fields = append(fields, p)
		}

		b.StartObject(slots)
		c.addPending(b, fields)
		offsets[i] = b.EndObject()
	}

	b.StartVector(SizeUOffsetT, len(offsets), SizeUOffsetT)

	for i := len(offsets) - 1; i >= 0; i-- {
		b.PrependUOffsetT(offsets[i])
	}

	return b.EndVector(len(offsets)), nil
}

func (c *canonicalCodec) buildMap(b *Builder, entry *ReflectionObject, x any, pointer string) (UOffsetT, error) {
	o, err := JSONObject(x, pointer)
	if err != nil {
		return 0, err
	}

	keyField, valueField := entryFields(entry)
	if keyField == nil {
		return 0, JSONErrorf(pointer, "map entry has no key")
	}

	entries := make([]mapEntry, 0, len(o))
	byKey := make(map[sortKey]int, len(o))

	for _, k := range JSONSortedKeys(o) {
		p := JSONPointer(pointer, k)

		key, order, kerr := c.keyEntry(keyField, k, true, p)
		if kerr != nil {
			return 0, kerr
		}

		e := mapEntry{order: order, fields: []pendingField{key}}

		if valueField != nil {
			v, present := JSONMember(o, k)

			err = c.entryValue(valueField, v, present, p, &e)
			if err != nil {
				return 0, err
			}
		}

		entries = collapseEntry(entries, byKey, e)
	}

	return c.finishEntries(b, entry, entries)
}

// entryValue reads a map value into e. A null value (as flatc prints a
// missing string or table) reads as the generated code reads it: the zero
// value, which Pack then writes -- an empty string, a zero scalar, and no
// table or struct at all.
func (c *canonicalCodec) entryValue(f *ReflectionField, v any, present bool, pointer string, e *mapEntry) error {
	t := f.Type()
	bt := t.BaseType()

	switch {
	case bt == ReflectionBaseTypeString:
		d := &deferredValue{field: f}

		if present {
			var err error

			d.text, err = JSONString(v, pointer)
			if err != nil {
				return err
			}
		}

		e.value = d
	case !present && isScalarBase(bt):
		e.fields = append(e.fields, pendingField{field: f})
	case !present:
	case isScalarBase(bt):
		bits, err := c.scalarFromJSON(v, t, bt, pointer)
		if err != nil {
			return err
		}

		e.fields = append(e.fields, pendingField{field: f, bits: bits})
	case bt == ReflectionBaseTypeObj && c.object(t).IsStruct():
		img, err := c.newStructImage(c.object(t), v, pointer)
		if err != nil {
			return err
		}

		e.fields = append(e.fields, pendingField{field: f, image: img})
	case bt == ReflectionBaseTypeObj:
		child, err := JSONObject(v, pointer)
		if err != nil {
			return err
		}

		e.value = &deferredValue{field: f, obj: c.object(t), child: child, pointer: pointer}
	default:
		return JSONErrorf(pointer, "unsupported map value")
	}

	return nil
}

func (c *canonicalCodec) buildSet(b *Builder, entry *ReflectionObject, x any, pointer string) (UOffsetT, error) {
	a, err := JSONArray(x, pointer)
	if err != nil {
		return 0, err
	}

	keyField, _ := entryFields(entry)
	if keyField == nil {
		return 0, JSONErrorf(pointer, "set entry has no key")
	}

	entries := make([]mapEntry, 0, len(a))
	byKey := make(map[sortKey]int, len(a))

	for i, e := range a {
		key, order, kerr := c.keyEntry(keyField, e, false, JSONIndexPointer(pointer, i))
		if kerr != nil {
			return 0, kerr
		}

		entries = collapseEntry(entries, byKey, mapEntry{order: order, fields: []pendingField{key}})
	}

	return c.finishEntries(b, entry, entries)
}
