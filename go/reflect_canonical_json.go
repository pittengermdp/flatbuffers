package flatbuffers

// Canonical JSON driven by schema reflection.
//
// These functions read and write the canonical JSON described in
// json_canonical.go for any table a binary schema (.bfbs) describes, with no
// generated code: ReflectToCanonicalJSON writes exactly the bytes the
// generated MarshalFlatJSON writes for the same message, and
// ReflectFromCanonicalJSON / ReflectFromCanonicalMap read what the generated
// UnmarshalFlatJSON reads. They sit beside ReflectPack / ReflectUnpack, whose
// own map format they leave unchanged.

import (
	"encoding/binary"
	"fmt"
	"math"
	"sort"
	"strings"
)

// CanonicalJSONOptions selects the spelling canonical JSON uses where the
// format allows a choice. Reading accepts every spelling regardless.
type CanonicalJSONOptions struct {
	// Int64AsString writes long and ulong values as quoted decimal strings,
	// matching code generated with flatc --json-int64-as-string.
	Int64AsString bool
}

// ReflectToCanonicalJSON writes the FlatBuffer buf, whose root table is
// typeName (fully qualified, e.g. "Ln2.Message"), as canonical JSON.
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

	c := canonicalCodec{schema: schema, opts: opts}
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

	c := canonicalCodec{schema: schema}
	b := NewBuilder(reflectBuilderInitialSize)

	root, err := c.buildTable(b, obj, m, "")
	if err != nil {
		return nil, err
	}

	b.Finish(root)

	return b.FinishedBytes(), nil
}

func recoverMalformed(err *error) {
	if r := recover(); r != nil {
		*err = fmt.Errorf("flatbuffers: malformed buffer: %v", r)
	}
}

type canonicalCodec struct {
	schema *ReflectionSchema
	opts   CanonicalJSONOptions
}

// fieldsByID returns an object's fields in declaration (id) order: a binary
// schema stores them sorted by name.
func fieldsByID(obj *ReflectionObject) []*ReflectionField {
	fields := obj.Fields()
	sort.Slice(fields, func(i, j int) bool { return fields[i].ID() < fields[j].ID() })

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
	return c.schema.Objects()[t.Index()]
}

// enumOf returns the enum a scalar type draws its values from, or nil.
func (c *canonicalCodec) enumOf(t *ReflectionType) *ReflectionEnum {
	if t.Index() < 0 {
		return nil
	}

	return c.schema.Enums()[t.Index()]
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
// sign-extended for signed integers.
func readScalarBits(buf []byte, pos UOffsetT, bt ReflectionBaseType) uint64 {
	switch bt {
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

// defaultBits returns a table scalar's default as raw bits.
func defaultBits(f *ReflectionField, bt ReflectionBaseType) uint64 {
	switch bt {
	case ReflectionBaseTypeFloat:
		return uint64(math.Float32bits(float32(f.DefaultReal())))
	case ReflectionBaseTypeDouble:
		return math.Float64bits(f.DefaultReal())
	default:
		return uint64(f.DefaultInteger())
	}
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
	case ReflectionBaseTypeLong:
		if c.opts.Int64AsString {
			w.IntString(int64(bits))

			return
		}

		w.Int(int64(bits))
	case ReflectionBaseTypeULong:
		if c.opts.Int64AsString {
			w.UintString(bits)

			return
		}

		w.Uint(bits)
	default:
		if isUnsignedBase(bt) {
			w.Uint(bits)

			return
		}

		w.Int(int64(bits))
	}
}

func enumNames(e *ReflectionEnum) map[uint64]string {
	names := make(map[uint64]string)
	for _, v := range e.Values() {
		names[uint64(v.Value())] = v.Name()
	}

	return names
}

func (c *canonicalCodec) writeEnum(w *JSONWriter, bits uint64, e *ReflectionEnum, bt ReflectionBaseType) {
	names := enumNames(e)
	if hasAttribute(e.Attributes(), "bit_flags") {
		if text, ok := BitFlagsJSONText(bits, names); ok {
			w.String(text)

			return
		}
	} else if name, ok := names[bits]; ok {
		w.String(name)

		return
	}

	if isUnsignedBase(bt) {
		w.Uint(bits)

		return
	}

	w.Int(int64(bits))
}

// keyText spells a map key read from an entry table as a property name.
func (c *canonicalCodec) keyText(buf []byte, pos UOffsetT, f *ReflectionField) string {
	t := f.Type()

	bt := t.BaseType()
	if bt == ReflectionBaseTypeString {
		return (&Table{Bytes: buf}).String(pos)
	}

	bits := readScalarBits(buf, pos, bt)

	if e := c.enumOf(t); e != nil {
		if name, ok := enumNames(e)[bits]; ok {
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
	w.BeginObject()

	for _, f := range fieldsByID(obj) {
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

	if bt == ReflectionBaseTypeArray || hasAttribute(f.Attributes(), "nested_flatbuffer") ||
		hasAttribute(f.Attributes(), "flexbuffer") {
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

		if bits == defaultBits(f, bt) {
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
		if o == 0 {
			return
		}

		vec := pos + GetUOffsetT(tab.Bytes[pos:])
		n := int(GetUOffsetT(tab.Bytes[vec:]))

		if n == 0 && !f.Required() {
			return
		}

		w.Key(f.Name())
		c.writeVector(w, f, tab.Bytes, vec+UOffsetT(SizeUOffsetT), n, JSONPointer(pointer, f.Name()))
	}
}

func (c *canonicalCodec) writeUnion(w *JSONWriter, obj *ReflectionObject, f *ReflectionField, tab Table, pointer string) {
	typeField := obj.FieldByName(f.Name() + "_type")
	if typeField == nil {
		w.Fail(JSONErrorf(JSONPointer(pointer, f.Name()), "union has no type field"))

		return
	}

	to := tab.Offset(VOffsetT(typeField.Offset()))
	if to == 0 {
		return
	}

	discriminant := uint64(tab.Bytes[tab.Pos+UOffsetT(to)])
	if discriminant == 0 {
		return
	}

	union := c.enumOf(f.Type())

	w.Key(typeField.Name())
	c.writeEnum(w, discriminant, union, ReflectionBaseTypeUType)

	o := tab.Offset(VOffsetT(f.Offset()))
	if o == 0 {
		return
	}

	var variant *ReflectionObject

	for _, v := range union.Values() {
		if uint64(v.Value()) == discriminant && v.UnionType() != nil {
			variant = c.object(v.UnionType())
		}
	}

	if variant == nil {
		w.Fail(JSONErrorf(JSONPointer(pointer, typeField.Name()), "%d is not a variant", discriminant))

		return
	}

	pos := tab.Pos + UOffsetT(o)

	w.Key(f.Name())
	c.writeTable(w, variant, Table{Bytes: tab.Bytes, Pos: pos + GetUOffsetT(tab.Bytes[pos:])}, JSONPointer(pointer, f.Name()))
}

func (c *canonicalCodec) writeStruct(w *JSONWriter, obj *ReflectionObject, buf []byte, pos UOffsetT) {
	w.BeginObject()

	for _, f := range fieldsByID(obj) {
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

type canonicalEntry struct {
	key   string
	order sortKey
	pos   UOffsetT
}

// sortKey orders map/set entries the way FlatBuffers sorts keyed vectors:
// strings bytewise, numbers by value.
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

		for i := range n {
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

	for i := range n {
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

func (c *canonicalCodec) writeMapOrSet(w *JSONWriter, entry *ReflectionObject, isMap bool, buf []byte, data UOffsetT, n int, pointer string) {
	var keyField, valueField *ReflectionField

	for _, f := range entry.Fields() {
		if f.Key() {
			keyField = f
		}

		if f.Name() == "value" {
			valueField = f
		}
	}

	if keyField == nil {
		w.Fail(JSONErrorf(pointer, "map/set entry has no key"))

		return
	}

	entries := make([]canonicalEntry, 0, n)

	for i := range n {
		at := data + UOffsetT(i*SizeUOffsetT)
		tab := Table{Bytes: buf, Pos: at + GetUOffsetT(buf[at:])}
		kt := keyField.Type().BaseType()
		ko := tab.Offset(VOffsetT(keyField.Offset()))

		e := canonicalEntry{pos: tab.Pos}

		if kt == ReflectionBaseTypeString {
			if ko != 0 {
				e.key = tab.String(tab.Pos + UOffsetT(ko))
			}

			e.order = sortKey{text: e.key, isStr: true}
		} else {
			bits := defaultBits(keyField, kt)
			if ko != 0 {
				bits = readScalarBits(buf, tab.Pos+UOffsetT(ko), kt)
			}

			e.order = sortKey{bits: bits, sign: !isUnsignedBase(kt)}
			if ko != 0 {
				e.key = c.keyText(buf, tab.Pos+UOffsetT(ko), keyField)
			} else {
				e.key = c.keyTextFromBits(bits, keyField)
			}
		}

		entries = append(entries, e)
	}

	sort.SliceStable(entries, func(i, j int) bool { return entries[i].order.less(entries[j].order) })

	if isMap {
		w.BeginObject()
	} else {
		w.BeginArray()
	}

	for _, e := range entries {
		tab := Table{Bytes: buf, Pos: e.pos}

		if !isMap {
			c.writeSetMember(w, keyField, tab, e)

			continue
		}

		w.Key(e.key)

		if valueField == nil {
			w.Null()

			continue
		}

		c.writeEntryValue(w, valueField, tab, JSONPointer(pointer, e.key))
	}

	if isMap {
		w.EndObject()
	} else {
		w.EndArray()
	}
}

func (c *canonicalCodec) keyTextFromBits(bits uint64, f *ReflectionField) string {
	t := f.Type()
	bt := t.BaseType()

	if e := c.enumOf(t); e != nil {
		if name, ok := enumNames(e)[bits]; ok {
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

func (c *canonicalCodec) writeSetMember(w *JSONWriter, keyField *ReflectionField, tab Table, e canonicalEntry) {
	t := keyField.Type()
	if t.BaseType() == ReflectionBaseTypeString {
		w.String(e.key)

		return
	}

	c.writeScalar(w, e.order.bits, t, t.BaseType())
}

// writeEntryValue writes a map value. Unlike a table member, a map value is
// written even when it equals its default: the entry exists.
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
		return c.enumFromJSON(v, e, bt, pointer)
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

func (c *canonicalCodec) enumFromJSON(v any, e *ReflectionEnum, bt ReflectionBaseType, pointer string) (uint64, error) {
	values := make(map[string]uint64)
	for _, ev := range e.Values() {
		values[ev.Name()] = uint64(ev.Value())
	}

	s, isString := v.(string)
	if !isString || looksNumeric(s) {
		return integerBitsFromJSON(v, bt, pointer)
	}

	if !hasAttribute(e.Attributes(), "bit_flags") {
		bits, ok := values[s]
		if !ok {
			return 0, JSONErrorf(pointer, "%q is not a variant of %s", s, e.Name())
		}

		return bits, nil
	}

	var bits uint64

	// Split on single spaces, as JSONBitFlags does: a doubled or trailing
	// space leaves an empty word, which names no variant.
	for _, word := range strings.Split(s, " ") {
		flag, ok := values[word]
		if !ok {
			return 0, JSONErrorf(pointer, "%q is not a variant of %s", word, e.Name())
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

	fields := fieldsByID(obj)
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

// placeStruct writes a struct inline at the builder's head, returning its
// offset.
func (c *canonicalCodec) placeStruct(b *Builder, obj *ReflectionObject, v any, pointer string) (UOffsetT, error) {
	img := make([]byte, obj.ByteSize())

	err := c.structImage(obj, v, pointer, img, 0)
	if err != nil {
		return 0, err
	}

	b.Prep(obj.MinAlign(), len(img))

	for i := len(img) - 1; i >= 0; i-- {
		b.PlaceByte(img[i])
	}

	return b.Offset(), nil
}

type pendingField struct {
	field  *ReflectionField
	offset UOffsetT
	bits   uint64
	value  any
}

func (c *canonicalCodec) buildTable(b *Builder, obj *ReflectionObject, m map[string]any, pointer string) (UOffsetT, error) {
	fields := fieldsByID(obj)
	names := make([]string, len(fields))
	slots := 0

	for i, f := range fields {
		names[i] = f.Name()
		if int(f.ID())+1 > slots {
			slots = int(f.ID()) + 1
		}
	}

	err := JSONCheckMembers(m, pointer, names...)
	if err != nil {
		return 0, err
	}

	// First everything stored out of line, which must precede the table.
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
			p, ok, uerr := c.buildUnion(b, obj, f, m, pointer)
			if uerr != nil {
				return 0, uerr
			}

			if ok {
				pending = append(pending, p...)
			}

			continue
		}

		x, present := JSONMember(m, f.Name())
		if !present {
			if f.Required() {
				return 0, JSONRequired(pointer, f.Name())
			}

			continue
		}

		p := pendingField{field: f, value: x}

		switch {
		case isScalarBase(bt):
			p.bits, err = c.scalarFromJSON(x, t, bt, fp)
		case bt == ReflectionBaseTypeString:
			var s string

			s, err = JSONString(x, fp)
			p.offset = b.CreateString(s)
		case bt == ReflectionBaseTypeObj && !c.object(t).IsStruct():
			var child map[string]any

			child, err = JSONObject(x, fp)
			if err == nil {
				p.offset, err = c.buildTable(b, c.object(t), child, fp)
			}
		case bt == ReflectionBaseTypeObj:
			// A struct is written inline, inside the table.
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

	b.StartObject(slots)

	for _, p := range pending {
		t := p.field.Type()
		bt := t.BaseType()
		slot := int(p.field.ID())

		switch {
		case isScalarBase(bt):
			if p.field.Optional() || p.bits != defaultBits(p.field, bt) {
				prependScalar(b, bt, p.bits)
				b.Slot(slot)
			}
		case bt == ReflectionBaseTypeObj && c.object(t).IsStruct():
			off, serr := c.placeStruct(b, c.object(t), p.value, JSONPointer(pointer, p.field.Name()))
			if serr != nil {
				return 0, serr
			}

			b.PrependStructSlot(slot, off, 0)
		default:
			b.PrependUOffsetTSlot(slot, p.offset, 0)
		}
	}

	return b.EndObject(), nil
}

// buildUnion builds a union's value table and returns it, with its type, as
// fields still to be added to the enclosing table.
func (c *canonicalCodec) buildUnion(
	b *Builder, obj *ReflectionObject, f *ReflectionField, m map[string]any, pointer string,
) ([]pendingField, bool, error) {
	typeField := obj.FieldByName(f.Name() + "_type")
	if typeField == nil {
		return nil, false, JSONErrorf(JSONPointer(pointer, f.Name()), "union has no type field")
	}

	tv, hasType := JSONMember(m, typeField.Name())
	value, hasValue := JSONMember(m, f.Name())

	if !hasType {
		if hasValue {
			return nil, false, JSONErrorf(JSONPointer(pointer, f.Name()), "union value without %s", typeField.Name())
		}

		return nil, false, nil
	}

	union := c.enumOf(f.Type())

	discriminant, err := c.enumFromJSON(tv, union, ReflectionBaseTypeUType, JSONPointer(pointer, typeField.Name()))
	if err != nil {
		return nil, false, err
	}

	if discriminant == 0 {
		if hasValue {
			return nil, false, JSONErrorf(JSONPointer(pointer, f.Name()), "union value under NONE")
		}

		return nil, false, nil
	}

	var variant *ReflectionObject

	for _, v := range union.Values() {
		if uint64(v.Value()) == discriminant && v.UnionType() != nil {
			variant = c.object(v.UnionType())
		}
	}

	if variant == nil {
		return nil, false, JSONErrorf(JSONPointer(pointer, typeField.Name()), "%d is not a variant of %s", discriminant, union.Name())
	}

	if !hasValue {
		return nil, false, nil // a type with no value reads as no union
	}

	fp := JSONPointer(pointer, f.Name())

	child, err := JSONObject(value, fp)
	if err != nil {
		return nil, false, err
	}

	off, err := c.buildTable(b, variant, child, fp)
	if err != nil {
		return nil, false, err
	}

	return []pendingField{
		{field: typeField, bits: discriminant},
		{field: f, offset: off},
	}, true, nil
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
			images[i] = make([]byte, entry.ByteSize())

			err = c.structImage(entry, e, JSONIndexPointer(pointer, i), images[i], 0)
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

type builtEntry struct {
	order  sortKey
	offset UOffsetT
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

// keyEntry reads one map key or set member into an entry's key field.
func (c *canonicalCodec) keyEntry(b *Builder, keyField *ReflectionField, v any, isPropertyName bool, pointer string) (pendingField, sortKey, error) {
	t := keyField.Type()
	bt := t.BaseType()

	if bt == ReflectionBaseTypeString {
		s, err := JSONString(v, pointer)

		return pendingField{field: keyField, offset: b.CreateString(s)}, sortKey{text: s, isStr: true}, err
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

func (c *canonicalCodec) finishEntries(b *Builder, entries []builtEntry) UOffsetT {
	sort.SliceStable(entries, func(i, j int) bool { return entries[i].order.less(entries[j].order) })

	b.StartVector(SizeUOffsetT, len(entries), SizeUOffsetT)

	for i := len(entries) - 1; i >= 0; i-- {
		b.PrependUOffsetT(entries[i].offset)
	}

	return b.EndVector(len(entries))
}

func (c *canonicalCodec) buildEntryTable(b *Builder, entry *ReflectionObject, fields []pendingField) UOffsetT {
	slots := 0

	for _, f := range entry.Fields() {
		if int(f.ID())+1 > slots {
			slots = int(f.ID()) + 1
		}
	}

	b.StartObject(slots)

	for _, p := range fields {
		bt := p.field.Type().BaseType()
		if isScalarBase(bt) {
			if p.bits != defaultBits(p.field, bt) {
				prependScalar(b, bt, p.bits)
				b.Slot(int(p.field.ID()))
			}

			continue
		}

		b.PrependUOffsetTSlot(int(p.field.ID()), p.offset, 0)
	}

	return b.EndObject()
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

	entries := make([]builtEntry, 0, len(o))

	for _, k := range JSONSortedKeys(o) {
		p := JSONPointer(pointer, k)

		key, order, kerr := c.keyEntry(b, keyField, k, true, p)
		if kerr != nil {
			return 0, kerr
		}

		fields := []pendingField{key}

		if v, ok := JSONMember(o, k); ok && valueField != nil {
			value := pendingField{field: valueField, value: v}
			vt := valueField.Type()
			vbt := vt.BaseType()

			switch {
			case isScalarBase(vbt):
				value.bits, err = c.scalarFromJSON(v, vt, vbt, p)
			case vbt == ReflectionBaseTypeString:
				var s string

				s, err = JSONString(v, p)
				value.offset = b.CreateString(s)
			case vbt == ReflectionBaseTypeObj && !c.object(vt).IsStruct():
				var child map[string]any

				child, err = JSONObject(v, p)
				if err == nil {
					value.offset, err = c.buildTable(b, c.object(vt), child, p)
				}
			default:
				err = JSONErrorf(p, "unsupported map value")
			}

			if err != nil {
				return 0, err
			}

			fields = append(fields, value)
		}

		entries = append(entries, builtEntry{order: order, offset: c.buildEntryTable(b, entry, fields)})
	}

	return c.finishEntries(b, entries), nil
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

	entries := make([]builtEntry, 0, len(a))

	for i, e := range a {
		key, order, kerr := c.keyEntry(b, keyField, e, false, JSONIndexPointer(pointer, i))
		if kerr != nil {
			return 0, kerr
		}

		entries = append(entries, builtEntry{order: order, offset: c.buildEntryTable(b, entry, []pendingField{key})})
	}

	return c.finishEntries(b, entries), nil
}
