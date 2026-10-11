package flatbuffers

import (
	"math"
	"testing"
)

// A NaN value in a NaN-defaulted slot is the default and is not written, as
// for any other default; a NaN in a slot whose default is a number is.
func TestPrependFloatSlotNaNDefault(t *testing.T) {
	cases := []struct {
		name    string
		prepend func(b *Builder)
		present bool
	}{
		{"float32 NaN, NaN default", func(b *Builder) { b.PrependFloat32Slot(0, float32(math.NaN()), float32(math.NaN())) }, false},
		{"float64 NaN, NaN default", func(b *Builder) { b.PrependFloat64Slot(0, math.NaN(), math.NaN()) }, false},
		{"float32 NaN, zero default", func(b *Builder) { b.PrependFloat32Slot(0, float32(math.NaN()), 0) }, true},
		{"float64 1, NaN default", func(b *Builder) { b.PrependFloat64Slot(0, 1, math.NaN()) }, true},
		{"float64 0, 0 default", func(b *Builder) { b.PrependFloat64Slot(0, 0, 0) }, false},
	}
	for _, c := range cases {
		b := NewBuilder(0)
		b.StartObject(1)
		c.prepend(b)
		b.Finish(b.EndObject())

		buf := b.FinishedBytes()
		tab := Table{Bytes: buf, Pos: GetUOffsetT(buf)}

		if present := tab.Offset(VOffsetT(4)) != 0; present != c.present {
			t.Errorf("%s: slot written = %v, want %v", c.name, present, c.present)
		}
	}
}
