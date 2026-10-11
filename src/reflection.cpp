/*
 * Copyright 2015 Google Inc. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "flatbuffers/reflection.h"

#include "flatbuffers/util.h"

// Helper functionality for reflection.

namespace flatbuffers {

namespace {

static void CopyInline(FlatBufferBuilder& fbb,
                       const reflection::Field& fielddef, const Table& table,
                       size_t align, size_t size) {
  fbb.Align(align);
  fbb.PushBytes(table.GetStruct<const uint8_t*>(fielddef.offset()), size);
  fbb.TrackField(fielddef.offset(), fbb.GetSize());
}

// Whether a field must be present. A deprecated field need not be, even if it
// is marked required: the generated builder does not require it, and the
// generated verifier skips it. One that is present is still verified, as
// reflection can still read it.
static bool IsRequired(const reflection::Field& field) {
  return field.required() && !field.deprecated();
}

static bool VerifyStruct(flatbuffers::Verifier& v,
                         const flatbuffers::Table& parent_table,
                         voffset_t field_offset, const reflection::Object& obj,
                         bool required) {
  auto offset = parent_table.GetOptionalFieldOffset(field_offset);
  if (required && !offset) {
    return false;
  }

  return !offset ||
         v.VerifyFieldStruct(reinterpret_cast<const uint8_t*>(&parent_table),
                             offset, obj.bytesize(), obj.minalign());
}

template <typename OffsetT = uoffset_t, typename LenT = uoffset_t>
static bool VerifyVectorOfStructs(flatbuffers::Verifier& v,
                                  const flatbuffers::Table& parent_table,
                                  voffset_t field_offset,
                                  const reflection::Object& obj,
                                  bool required) {
  auto p = parent_table.GetPointer<const uint8_t*, OffsetT>(field_offset);
  if (required && !p) {
    return false;
  }

  return !p || v.VerifyVectorOrString<LenT>(p, obj.bytesize());
}

// Verifies the 32-bit offset in a field's slot, which must be present if
// IsRequired(), as the generated VerifyOffset() and
// VerifyOffsetRequired() do. Call it before reading the slot: only this bounds
// the slot by the buffer, since a vtable can put a field anywhere in the 64KiB
// past its table. It also rejects an offset of 0, which points the field at
// its own slot, one that is negative read as signed, and one that leads out of
// the buffer.
static bool VerifyOffset32Field(flatbuffers::Verifier& v,
                                const flatbuffers::Table& table,
                                const reflection::Field& field) {
  return IsRequired(field) ? table.VerifyOffsetRequired(v, field.offset())
                           : table.VerifyOffset(v, field.offset());
}

// Verifies the 64-bit offset in a field's slot, which must be present if
// IsRequired(), as the generated VerifyOffset64Required() does.
static bool VerifyOffset64Field(flatbuffers::Verifier& v,
                                const flatbuffers::Table& table,
                                const reflection::Field& field) {
  return IsRequired(field) ? table.VerifyOffset64Required(v, field.offset())
                           : table.VerifyOffset64(v, field.offset());
}

// Verifies a vector of T reached through a 64-bit offset, with the same
// Verifier::VerifyVector() call -- and so the same element alignment -- as
// the generated verifier.
template <typename T, typename LenT>
static bool VerifyOffset64VectorOf(flatbuffers::Verifier& v,
                                   const flatbuffers::Table& table,
                                   const reflection::Field& vec_field) {
  return v.VerifyVector(
      table.GetPointer64<const Vector<T, LenT>*>(vec_field.offset()));
}

// Verifies a vector reached through a 64-bit offset. A `(vector64)` field also
// has a 64-bit length (LenT = uoffset64_t); an `(offset64)` one keeps a 32-bit
// length. The parser only allows scalar and struct elements in either.
template <typename LenT>
static bool VerifyOffset64Vector(flatbuffers::Verifier& v,
                                 const reflection::Schema& schema,
                                 const flatbuffers::Table& table,
                                 const reflection::Field& vec_field) {
  if (!VerifyOffset64Field(v, table, vec_field)) return false;

  switch (vec_field.type()->element()) {
    case reflection::UType:
    case reflection::Bool:
    case reflection::Byte:
    case reflection::UByte:
      return VerifyOffset64VectorOf<uint8_t, LenT>(v, table, vec_field);
    case reflection::Short:
    case reflection::UShort:
      return VerifyOffset64VectorOf<int16_t, LenT>(v, table, vec_field);
    case reflection::Int:
    case reflection::UInt:
      return VerifyOffset64VectorOf<int32_t, LenT>(v, table, vec_field);
    case reflection::Long:
    case reflection::ULong:
      return VerifyOffset64VectorOf<int64_t, LenT>(v, table, vec_field);
    case reflection::Float:
      return VerifyOffset64VectorOf<float, LenT>(v, table, vec_field);
    case reflection::Double:
      return VerifyOffset64VectorOf<double, LenT>(v, table, vec_field);
    case reflection::Obj: {
      auto obj = schema.objects()->Get(vec_field.type()->index());
      return obj->is_struct() &&
             VerifyVectorOfStructs<uoffset64_t, LenT>(
                 v, table, vec_field.offset(), *obj, IsRequired(vec_field));
    }
    default:
      return false;
  }
}

// Verifies the `_type` field of a union, which is as wide as the union's
// underlying type, `underlying`: 1 byte by default, but 4 for `union U: int`.
static bool VerifyUnionTypeField(flatbuffers::Verifier& v,
                                 const flatbuffers::Table& table,
                                 voffset_t field_offset,
                                 reflection::BaseType underlying) {
  auto offset = table.GetOptionalFieldOffset(field_offset);
  auto size = GetTypeSize(underlying);
  return !offset ||
         v.VerifyFieldStruct(reinterpret_cast<const uint8_t*>(&table), offset,
                             size, size);
}

// Verifies the `_type` vector of a vector of unions, whose elements are as
// wide as the union's underlying type, `underlying`.
static bool VerifyUnionTypeVector(flatbuffers::Verifier& v,
                                  const VectorOfAny* types,
                                  reflection::BaseType underlying) {
  auto size = GetTypeSize(underlying);
  return !types ||
         v.VerifyVectorOrString(reinterpret_cast<const uint8_t*>(types), size,
                                nullptr, size);
}

// forward declare to resolve cyclic deps between VerifyObject and VerifyVector
static bool VerifyObject(flatbuffers::Verifier& v,
                         const reflection::Schema& schema,
                         const reflection::Object& obj,
                         const flatbuffers::Table* table, bool required);

static bool VerifyUnion(flatbuffers::Verifier& v,
                        const reflection::Schema& schema, int64_t utype,
                        const uint8_t* elem,
                        const reflection::Field& union_field) {
  if (!utype) return true;  // Not present.
  auto fb_enum = schema.enums()->Get(union_field.type()->index());
  // Members are keyed by value, which need not be their position in values():
  // in `union U { A = 1, B = 3 }`, B is values()[2]. A type naming no member is
  // rejected, unlike the generated verifiers, which accept it for forward
  // compatibility: its value cannot be checked against this schema, and
  // GetUnionType() would dereference the missing member.
  auto enum_val = fb_enum->values()->LookupByKey(utype);
  if (!enum_val) return false;
  auto elem_type = enum_val->union_type();
  switch (elem_type->base_type()) {
    case reflection::Obj: {
      auto elem_obj = schema.objects()->Get(elem_type->index());
      if (elem_obj->is_struct()) {
        return v.VerifyFromPointer(elem, elem_obj->bytesize());
      } else {
        return VerifyObject(v, schema, *elem_obj,
                            reinterpret_cast<const flatbuffers::Table*>(elem),
                            true);
      }
    }
    case reflection::String:
      return v.VerifyString(reinterpret_cast<const flatbuffers::String*>(elem));
    default:
      return false;
  }
}

static bool VerifyString(flatbuffers::Verifier& v,
                         const flatbuffers::Table& table,
                         const reflection::Field& str_field) {
  if (str_field.offset64()) {
    return VerifyOffset64Field(v, table, str_field) &&
           v.VerifyString(
               table.GetPointer64<const String*>(str_field.offset()));
  }
  return VerifyOffset32Field(v, table, str_field) &&
         v.VerifyString(flatbuffers::GetFieldS(table, str_field));
}

static bool VerifyVector(flatbuffers::Verifier& v,
                         const reflection::Schema& schema,
                         const flatbuffers::Table& table,
                         const reflection::Field& vec_field) {
  if (vec_field.type()->base_type() == reflection::Vector64) {
    return VerifyOffset64Vector<uoffset64_t>(v, schema, table, vec_field);
  }
  FLATBUFFERS_ASSERT(vec_field.type()->base_type() == reflection::Vector);
  if (vec_field.offset64()) {
    return VerifyOffset64Vector<uoffset_t>(v, schema, table, vec_field);
  }
  if (!VerifyOffset32Field(v, table, vec_field)) return false;

  switch (vec_field.type()->element()) {
    case reflection::UType:
      return VerifyUnionTypeVector(v, GetFieldAnyV(table, vec_field),
                                   GetUnionUnderlyingType(schema, vec_field));
    case reflection::Bool:
    case reflection::Byte:
    case reflection::UByte:
      return v.VerifyVector(flatbuffers::GetFieldV<int8_t>(table, vec_field));
    case reflection::Short:
    case reflection::UShort:
      return v.VerifyVector(flatbuffers::GetFieldV<int16_t>(table, vec_field));
    case reflection::Int:
    case reflection::UInt:
      return v.VerifyVector(flatbuffers::GetFieldV<int32_t>(table, vec_field));
    case reflection::Long:
    case reflection::ULong:
      return v.VerifyVector(flatbuffers::GetFieldV<int64_t>(table, vec_field));
    case reflection::Float:
      return v.VerifyVector(flatbuffers::GetFieldV<float>(table, vec_field));
    case reflection::Double:
      return v.VerifyVector(flatbuffers::GetFieldV<double>(table, vec_field));
    case reflection::String: {
      auto vec_string =
          flatbuffers::GetFieldV<flatbuffers::Offset<flatbuffers::String>>(
              table, vec_field);
      if (v.VerifyVector(vec_string) && v.VerifyVectorOfStrings(vec_string)) {
        return true;
      } else {
        return false;
      }
    }
    case reflection::Obj: {
      auto obj = schema.objects()->Get(vec_field.type()->index());
      if (obj->is_struct()) {
        return VerifyVectorOfStructs(v, table, vec_field.offset(), *obj,
                                     IsRequired(vec_field));
      } else {
        auto vec =
            flatbuffers::GetFieldV<flatbuffers::Offset<flatbuffers::Table>>(
                table, vec_field);
        if (!v.VerifyVector(vec)) return false;
        if (!vec) return true;
        for (uoffset_t j = 0; j < vec->size(); j++) {
          if (!VerifyObject(v, schema, *obj, vec->Get(j), true)) {
            return false;
          }
        }
        return true;
      }
    }
    case reflection::Union: {
      // The types are in the `<name>_type` field before this one, which sorts
      // after it by name and so is not verified yet.
      if (!table.VerifyField<uoffset_t>(
              v, vec_field.offset() - sizeof(voffset_t), sizeof(uoffset_t)))
        return false;
      auto vec = flatbuffers::GetFieldV<flatbuffers::Offset<uint8_t>>(
          table, vec_field);
      if (!v.VerifyVector(vec)) return false;
      if (!vec) return true;
      // The types are in the field before this one, `<name>_type`, which
      // sorts after it by name and so is not verified yet.
      if (!table.VerifyOffset(v, vec_field.offset() - sizeof(voffset_t)))
        return false;
      auto underlying = GetUnionUnderlyingType(schema, vec_field);
      auto type_vec = table.GetPointer<const VectorOfAny*>(vec_field.offset() -
                                                           sizeof(voffset_t));
      // VerifyUnionTypeVector(nullptr) succeeds, but values without types do
      // not.
      if (!type_vec || !VerifyUnionTypeVector(v, type_vec, underlying))
        return false;
      if (type_vec->size() != vec->size()) return false;
      for (uoffset_t j = 0; j < vec->size(); j++) {
        //  get union type from the prev field
        auto utype = GetAnyVectorElemI(type_vec, underlying, j);
        auto elem = vec->Get(j);
        if (!VerifyUnion(v, schema, utype, elem, vec_field)) return false;
      }
      return true;
    }
    case reflection::Vector:
    case reflection::None:
    default:
      FLATBUFFERS_ASSERT(false);
      return false;
  }
}

static bool VerifyObject(flatbuffers::Verifier& v,
                         const reflection::Schema& schema,
                         const reflection::Object& obj,
                         const flatbuffers::Table* table, bool required) {
  if (!table) return !required;
  if (!table->VerifyTableStart(v)) return false;
  for (uoffset_t i = 0; i < obj.fields()->size(); i++) {
    auto field_def = obj.fields()->Get(i);
    switch (field_def->type()->base_type()) {
      case reflection::UType:
        if (!VerifyUnionTypeField(v, *table, field_def->offset(),
                                  GetUnionUnderlyingType(schema, *field_def)))
          return false;
        break;
      case reflection::Bool:
      case reflection::Byte:
      case reflection::UByte:
        if (!table->VerifyField<int8_t>(v, field_def->offset(), sizeof(int8_t)))
          return false;
        break;
      case reflection::Short:
      case reflection::UShort:
        if (!table->VerifyField<int16_t>(v, field_def->offset(),
                                         sizeof(int16_t)))
          return false;
        break;
      case reflection::Int:
      case reflection::UInt:
        if (!table->VerifyField<int32_t>(v, field_def->offset(),
                                         sizeof(int32_t)))
          return false;
        break;
      case reflection::Long:
      case reflection::ULong:
        if (!table->VerifyField<int64_t>(v, field_def->offset(),
                                         sizeof(int64_t)))
          return false;
        break;
      case reflection::Float:
        if (!table->VerifyField<float>(v, field_def->offset(), sizeof(float)))
          return false;
        break;
      case reflection::Double:
        if (!table->VerifyField<double>(v, field_def->offset(), sizeof(double)))
          return false;
        break;
      case reflection::String:
        if (!VerifyString(v, *table, *field_def)) return false;
        break;
      case reflection::Vector:
      case reflection::Vector64:
        if (!VerifyVector(v, schema, *table, *field_def)) return false;
        break;
      case reflection::Obj: {
        auto child_obj = schema.objects()->Get(field_def->type()->index());
        if (child_obj->is_struct()) {
          if (!VerifyStruct(v, *table, field_def->offset(), *child_obj,
                            IsRequired(*field_def))) {
            return false;
          }
        } else {
          if (!VerifyOffset32Field(v, *table, *field_def) ||
              !VerifyObject(v, schema, *child_obj,
                            flatbuffers::GetFieldT(*table, *field_def),
                            IsRequired(*field_def))) {
            return false;
          }
        }
        break;
      }
      case reflection::Union: {
        if (!table->VerifyField<uoffset_t>(v, field_def->offset(),
                                           sizeof(uoffset_t)))
          return false;
        //  get union type from the prev field
        voffset_t utype_offset = field_def->offset() - sizeof(voffset_t);
        // That field, `<name>_type`, sorts after this one by name and so is
        // not verified yet.
        auto underlying = GetUnionUnderlyingType(schema, *field_def);
        if (!VerifyUnionTypeField(v, *table, utype_offset, underlying) ||
            !VerifyOffset32Field(v, *table, *field_def)) {
          return false;
        }
        auto utype_ptr = table->GetAddressOf(utype_offset);
        auto utype = utype_ptr ? GetAnyValueI(underlying, utype_ptr) : 0;
        auto uval = reinterpret_cast<const uint8_t*>(
            flatbuffers::GetFieldT(*table, *field_def));
        if (!VerifyUnion(v, schema, utype, uval, *field_def)) {
          return false;
        }
        break;
      }
      case reflection::None:
      default:
        // None is never a field type, and one this verifier does not know,
        // e.g. from a newer schema, cannot be verified: reject either in every
        // build type rather than skip the field.
        return false;
    }
  }

  if (!v.EndTable()) return false;

  return true;
}

}  // namespace

int64_t GetAnyValueI(reflection::BaseType type, const uint8_t* data) {
  // clang-format off
  #define FLATBUFFERS_GET(T) static_cast<int64_t>(ReadScalar<T>(data))
  switch (type) {
    case reflection::UType:
    case reflection::Bool:
    case reflection::UByte:  return FLATBUFFERS_GET(uint8_t);
    case reflection::Byte:   return FLATBUFFERS_GET(int8_t);
    case reflection::Short:  return FLATBUFFERS_GET(int16_t);
    case reflection::UShort: return FLATBUFFERS_GET(uint16_t);
    case reflection::Int:    return FLATBUFFERS_GET(int32_t);
    case reflection::UInt:   return FLATBUFFERS_GET(uint32_t);
    case reflection::Long:   return FLATBUFFERS_GET(int64_t);
    case reflection::ULong:  return FLATBUFFERS_GET(uint64_t);
    case reflection::Float:  return FLATBUFFERS_GET(float);
    case reflection::Double: return FLATBUFFERS_GET(double);
    case reflection::String: {
      auto s = reinterpret_cast<const String *>(ReadScalar<uoffset_t>(data) +
                                                data);
      return s ? StringToInt(s->c_str()) : 0;
    }
    default: return 0;  // Tables & vectors do not make sense.
  }
  #undef FLATBUFFERS_GET
  // clang-format on
}

double GetAnyValueF(reflection::BaseType type, const uint8_t* data) {
  switch (type) {
    case reflection::Float:
      return static_cast<double>(ReadScalar<float>(data));
    case reflection::Double:
      return ReadScalar<double>(data);
    case reflection::String: {
      auto s =
          reinterpret_cast<const String*>(ReadScalar<uoffset_t>(data) + data);
      if (s) {
        double d;
        StringToNumber(s->c_str(), &d);
        return d;
      } else {
        return 0.0;
      }
    }
    default:
      return static_cast<double>(GetAnyValueI(type, data));
  }
}

std::string GetAnyValueS(reflection::BaseType type, const uint8_t* data,
                         const reflection::Schema* schema, int type_index) {
  switch (type) {
    case reflection::UType:
      // A union's type is stored as the union's underlying type, a UType only
      // by default; `type_index` is the union's enum.
      if (schema && type_index >= 0) {
        auto union_def =
            schema->enums()->Get(static_cast<uoffset_t>(type_index));
        type = union_def->underlying_type()->base_type();
      }
      return NumToString(GetAnyValueI(type, data));
    case reflection::Float:
    case reflection::Double:
      return NumToString(GetAnyValueF(type, data));
    case reflection::String: {
      auto s =
          reinterpret_cast<const String*>(ReadScalar<uoffset_t>(data) + data);
      return s ? s->c_str() : "";
    }
    case reflection::Obj:
      if (schema) {
        // Convert the table to a string. This is mostly for debugging purposes,
        // and does NOT promise to be JSON compliant.
        // Also prefixes the type.
        auto& objectdef = *schema->objects()->Get(type_index);
        auto s = objectdef.name()->str();
        if (objectdef.is_struct()) {
          s += "(struct)";  // TODO: implement this as well.
        } else {
          auto table_field = reinterpret_cast<const Table*>(
              ReadScalar<uoffset_t>(data) + data);
          s += " { ";
          auto fielddefs = objectdef.fields();
          for (auto it = fielddefs->begin(); it != fielddefs->end(); ++it) {
            auto& fielddef = **it;
            if (!table_field->CheckField(fielddef.offset())) continue;
            auto val = GetAnyFieldS(*table_field, fielddef, schema);
            if (fielddef.type()->base_type() == reflection::String) {
              std::string esc;
              flatbuffers::EscapeString(val.c_str(), val.length(), &esc, true,
                                        false);
              val = esc;
            }
            s += fielddef.name()->str();
            s += ": ";
            s += val;
            s += ", ";
          }
          s += "}";
        }
        return s;
      } else {
        return "(table)";
      }
    case reflection::Vector:
      return "[(elements)]";  // TODO: implement this as well.
    case reflection::Union:
      return "(union)";  // TODO: implement this as well.
    default:
      return NumToString(GetAnyValueI(type, data));
  }
}

void ForAllFields(const reflection::Object* object, bool reverse,
                  std::function<void(const reflection::Field*)> func) {
  std::vector<uint32_t> field_to_id_map;
  field_to_id_map.resize(object->fields()->size());

  // Create the mapping of field ID to the index into the vector.
  for (uint32_t i = 0; i < object->fields()->size(); ++i) {
    auto field = object->fields()->Get(i);
    field_to_id_map[field->id()] = i;
  }

  for (size_t i = 0; i < field_to_id_map.size(); ++i) {
    func(object->fields()->Get(
        field_to_id_map[reverse ? field_to_id_map.size() - (i + 1) : i]));
  }
}

void SetAnyValueI(reflection::BaseType type, uint8_t* data, int64_t val) {
  // clang-format off
  #define FLATBUFFERS_SET(T) WriteScalar(data, static_cast<T>(val))
  switch (type) {
    case reflection::UType:
    case reflection::Bool:
    case reflection::UByte:  FLATBUFFERS_SET(uint8_t ); break;
    case reflection::Byte:   FLATBUFFERS_SET(int8_t  ); break;
    case reflection::Short:  FLATBUFFERS_SET(int16_t ); break;
    case reflection::UShort: FLATBUFFERS_SET(uint16_t); break;
    case reflection::Int:    FLATBUFFERS_SET(int32_t ); break;
    case reflection::UInt:   FLATBUFFERS_SET(uint32_t); break;
    case reflection::Long:   FLATBUFFERS_SET(int64_t ); break;
    case reflection::ULong:  FLATBUFFERS_SET(uint64_t); break;
    case reflection::Float:  FLATBUFFERS_SET(float   ); break;
    case reflection::Double: FLATBUFFERS_SET(double  ); break;
    // TODO: support strings
    default: break;
  }
  #undef FLATBUFFERS_SET
  // clang-format on
}

void SetAnyValueF(reflection::BaseType type, uint8_t* data, double val) {
  switch (type) {
    case reflection::Float:
      WriteScalar(data, static_cast<float>(val));
      break;
    case reflection::Double:
      WriteScalar(data, val);
      break;
    // TODO: support strings.
    default:
      SetAnyValueI(type, data, static_cast<int64_t>(val));
      break;
  }
}

void SetAnyValueS(reflection::BaseType type, uint8_t* data, const char* val) {
  switch (type) {
    case reflection::Float:
    case reflection::Double: {
      double d;
      StringToNumber(val, &d);
      SetAnyValueF(type, data, d);
      break;
    }
    // TODO: support strings.
    default:
      SetAnyValueI(type, data, StringToInt(val));
      break;
  }
}

// Resize a FlatBuffer in-place by iterating through all offsets in the buffer
// and adjusting them by "delta" if they straddle the start offset.
// Once that is done, bytes can now be inserted/deleted safely.
// "delta" may be negative (shrinking), in which case the bytes removed are
// the ones that end at "start", not the ones that begin there.
// Unless "delta" is a multiple of the largest alignment, you'll create a small
// amount of garbage space in the buffer (usually 0..7 bytes).
// If your FlatBuffer's root table is not the schema's root table, you should
// pass in your root_table type as well.
class ResizeContext {
 public:
  ResizeContext(const reflection::Schema& schema, uoffset_t start, int delta,
                std::vector<uint8_t>* flatbuf,
                const reflection::Object* root_table = nullptr)
      : schema_(schema),
        startptr_(flatbuf->data() + start),
        delta_(delta),
        buf_(*flatbuf),
        dag_check_(flatbuf->size() / sizeof(uoffset_t), false) {
    auto mask = static_cast<int>(sizeof(largest_scalar_t) - 1);
    delta_ = (delta_ + mask) & ~mask;
    if (!delta_) return;  // We can't shrink by less than largest_scalar_t.
    // Now change all the offsets by delta_.
    auto root = GetAnyRoot(buf_.data());
    Straddle<uoffset_t, 1>(buf_.data(), root, buf_.data());
    ResizeTable(root_table ? *root_table : *schema.root_table(), root);
    // We can now add or remove bytes at start.
    if (delta_ > 0)
      buf_.insert(buf_.begin() + start, delta_, 0);
    else
      buf_.erase(buf_.begin() + start + delta_, buf_.begin() + start);
  }

  // Check if the range between first (lower address) and second straddles
  // the insertion point. If it does, change the offset at offsetloc (of
  // type T, with direction D).
  template <typename T, int D>
  void Straddle(const void* first, const void* second, void* offsetloc) {
    if (first <= startptr_ && second >= startptr_) {
      WriteScalar<T>(offsetloc, ReadScalar<T>(offsetloc) + delta_ * D);
      DagCheck(offsetloc) = true;
    }
  }

  // This returns a boolean that records if the corresponding offset location
  // has been modified already. If so, we can't even read the corresponding
  // offset, since it is pointing to a location that is illegal until the
  // resize actually happens.
  // This must be checked for every offset, since we can't know which offsets
  // will straddle and which won't.
  uint8_t& DagCheck(const void* offsetloc) {
    auto dag_idx = reinterpret_cast<const uoffset_t*>(offsetloc) -
                   reinterpret_cast<const uoffset_t*>(buf_.data());
    return dag_check_[dag_idx];
  }

  void ResizeTable(const reflection::Object& objectdef, Table* table) {
    if (DagCheck(table)) return;  // Table already visited.
    auto vtable = table->GetVTable();
    // Early out: since all fields inside the table must point forwards in
    // memory, if the insertion point is before the table we can stop here.
    auto tableloc = reinterpret_cast<uint8_t*>(table);
    if (startptr_ <= tableloc) {
      // Check if insertion point is between the table and a vtable that
      // precedes it. This can't happen in current construction code, but check
      // just in case we ever change the way flatbuffers are built.
      Straddle<soffset_t, -1>(vtable, table, table);
    } else {
      // Check each field.
      auto fielddefs = objectdef.fields();
      for (auto it = fielddefs->begin(); it != fielddefs->end(); ++it) {
        auto& fielddef = **it;
        auto base_type = fielddef.type()->base_type();
        // Ignore scalars.
        if (base_type <= reflection::Double) continue;
        // Ignore fields that are not stored.
        auto offset = table->GetOptionalFieldOffset(fielddef.offset());
        if (!offset) continue;
        // Ignore structs.
        auto subobjectdef =
            base_type == reflection::Obj
                ? schema_.objects()->Get(fielddef.type()->index())
                : nullptr;
        if (subobjectdef && subobjectdef->is_struct()) continue;
        // Get this fields' offset, and read it if safe.
        auto offsetloc = tableloc + offset;
        if (DagCheck(offsetloc)) continue;  // This offset already visited.
        auto ref = offsetloc + ReadScalar<uoffset_t>(offsetloc);
        Straddle<uoffset_t, 1>(offsetloc, ref, offsetloc);
        // Recurse.
        switch (base_type) {
          case reflection::Obj: {
            if (subobjectdef) {
              ResizeTable(*subobjectdef, reinterpret_cast<Table*>(ref));
            }
            break;
          }
          case reflection::Vector: {
            auto elem_type = fielddef.type()->element();
            if (elem_type != reflection::Obj && elem_type != reflection::String)
              break;
            auto vec = reinterpret_cast<Vector<uoffset_t>*>(ref);
            auto elemobjectdef =
                elem_type == reflection::Obj
                    ? schema_.objects()->Get(fielddef.type()->index())
                    : nullptr;
            if (elemobjectdef && elemobjectdef->is_struct()) break;
            for (uoffset_t i = 0; i < vec->size(); i++) {
              auto loc = vec->Data() + i * sizeof(uoffset_t);
              if (DagCheck(loc)) continue;  // This offset already visited.
              auto dest = loc + vec->Get(i);
              Straddle<uoffset_t, 1>(loc, dest, loc);
              if (elemobjectdef)
                ResizeTable(*elemobjectdef, reinterpret_cast<Table*>(dest));
            }
            break;
          }
          case reflection::Union: {
            ResizeTable(GetUnionType(schema_, objectdef, fielddef, *table),
                        reinterpret_cast<Table*>(ref));
            break;
          }
          case reflection::String:
            break;
          default:
            FLATBUFFERS_ASSERT(false);
        }
      }
      // Check if the vtable offset points beyond the insertion point.
      // Must do this last, since GetOptionalFieldOffset above still reads
      // this value.
      Straddle<soffset_t, -1>(table, vtable, table);
    }
  }

 private:
  const reflection::Schema& schema_;
  uint8_t* startptr_;
  int delta_;
  std::vector<uint8_t>& buf_;
  std::vector<uint8_t> dag_check_;
};

void SetString(const reflection::Schema& schema, const std::string& val,
               const String* str, std::vector<uint8_t>* flatbuf,
               const reflection::Object* root_table) {
  auto delta = static_cast<int>(val.size()) - static_cast<int>(str->size());
  auto str_start = static_cast<uoffset_t>(
      reinterpret_cast<const uint8_t*>(str) - flatbuf->data());
  auto start = str_start + static_cast<uoffset_t>(sizeof(uoffset_t));
  if (delta) {
    // Clear the old string, since we don't want parts of it remaining.
    memset(flatbuf->data() + start, 0, str->size());
    // Different size, we must expand (or contract). Resize at the end of the
    // old characters, as ResizeAnyVector does at the end of the elements: a
    // shrink removes the bytes before the resize point, which must be the tail
    // of the old string, not its length field and whatever precedes it.
    ResizeContext ctx(schema, start + str->size(), delta, flatbuf, root_table);
    // Set the new length.
    WriteScalar(flatbuf->data() + str_start,
                static_cast<uoffset_t>(val.size()));
  }
  // Copy new data. Safe because we created the right amount of space:
  // ResizeContext rounds a shrink toward zero, so it never removes more than
  // the old string had to spare.
  memcpy(flatbuf->data() + start, val.c_str(), val.size() + 1);
}

uint8_t* ResizeAnyVector(const reflection::Schema& schema, uoffset_t newsize,
                         const VectorOfAny* vec, uoffset_t num_elems,
                         uoffset_t elem_size, std::vector<uint8_t>* flatbuf,
                         const reflection::Object* root_table) {
  auto delta_elem = static_cast<int>(newsize) - static_cast<int>(num_elems);
  auto delta_bytes = delta_elem * static_cast<int>(elem_size);
  auto vec_start = reinterpret_cast<const uint8_t*>(vec) - flatbuf->data();
  auto start = static_cast<uoffset_t>(vec_start) +
               static_cast<uoffset_t>(sizeof(uoffset_t)) +
               elem_size * num_elems;
  if (delta_bytes) {
    if (delta_elem < 0) {
      // Clear elements we're throwing away, since some might remain in the
      // buffer.
      auto size_clear = -delta_elem * elem_size;
      memset(flatbuf->data() + start - size_clear, 0, size_clear);
    }
    ResizeContext ctx(schema, start, delta_bytes, flatbuf, root_table);
    WriteScalar(flatbuf->data() + vec_start, newsize);  // Length field.
    // Set new elements to 0.. this can be overwritten by the caller.
    if (delta_elem > 0) {
      memset(flatbuf->data() + start, 0,
             static_cast<size_t>(delta_elem) * elem_size);
    }
  }
  return flatbuf->data() + start;
}

const uint8_t* AddFlatBuffer(std::vector<uint8_t>& flatbuf,
                             const uint8_t* newbuf, size_t newlen) {
  // Align to sizeof(uoffset_t) past sizeof(largest_scalar_t) since we're
  // going to chop off the root offset.
  if (!newbuf || newlen < sizeof(uoffset_t)) return nullptr;
  FLATBUFFERS_ASSERT(newlen >= sizeof(uoffset_t));
  auto root = ReadScalar<uoffset_t>(newbuf);
  if (root < sizeof(uoffset_t) || root >= newlen) return nullptr;
  while ((flatbuf.size() & (sizeof(uoffset_t) - 1)) ||
         !(flatbuf.size() & (sizeof(largest_scalar_t) - 1))) {
    flatbuf.push_back(0);
  }
  auto insertion_point = static_cast<uoffset_t>(flatbuf.size());
  // Insert the entire FlatBuffer minus the root pointer.
  flatbuf.insert(flatbuf.end(), newbuf + sizeof(uoffset_t), newbuf + newlen);
  auto root_offset = root - sizeof(uoffset_t);
  return flatbuf.data() + insertion_point + root_offset;
}

// The `<name>_type` field flatc pairs with a union, or a vector of unions, to
// hold its type, or types. Null if the schema has none.
static const reflection::Field* GetUnionTypeField(
    const reflection::Object& objectdef, const reflection::Field& unionfield) {
  return objectdef.fields()->LookupByKey(
      (unionfield.name()->str() + UnionTypeFieldSuffix()).c_str());
}

// Copies the value of a union, or of one element of a vector of unions, whose
// type is `type`. Its member may be a table, a struct or a string. Returns a
// null offset for NONE, which has no value to copy, and for a type the schema
// does not have, as in a buffer written with a newer schema, whose value has a
// shape this schema cannot tell.
static Offset<void> CopyUnionValue(FlatBufferBuilder& fbb,
                                   const reflection::Schema& schema,
                                   const reflection::Enum& enumdef,
                                   const int64_t type, const Table* value,
                                   const bool use_string_pooling) {
  const reflection::EnumVal* enumval = enumdef.values()->LookupByKey(type);
  if (!enumval) return Offset<void>();
  const reflection::Type& member = *enumval->union_type();
  switch (member.base_type()) {
    case reflection::String: {
      const auto str = reinterpret_cast<const String*>(value);
      return use_string_pooling ? fbb.CreateSharedString(str).Union()
                                : fbb.CreateString(str).Union();
    }
    case reflection::Obj: {
      auto& memberdef = *schema.objects()->Get(member.index());
      if (memberdef.is_struct()) {
        // Out of line, as CreateStruct() writes it.
        fbb.StartStruct(memberdef.minalign());
        fbb.PushBytes(reinterpret_cast<const uint8_t*>(value),
                      memberdef.bytesize());
        return Offset<void>(fbb.EndStruct());
      }
      return CopyTable(fbb, schema, memberdef, *value, use_string_pooling)
          .Union();
    }
    default:  // NONE.
      return Offset<void>();
  }
}

Offset<const Table*> CopyTable(FlatBufferBuilder& fbb,
                               const reflection::Schema& schema,
                               const reflection::Object& objectdef,
                               const Table& table, bool use_string_pooling) {
  // Before we can construct the table, we have to first generate any
  // subobjects, and collect their offsets.
  std::vector<uoffset_t> offsets;
  auto fielddefs = objectdef.fields();
  for (auto it = fielddefs->begin(); it != fielddefs->end(); ++it) {
    auto& fielddef = **it;
    // Skip if field is not present in the source.
    if (!table.CheckField(fielddef.offset())) continue;
    // A 32-bit builder cannot write a 64-bit offset. Fail here, before this
    // table is started, so fbb is never left mid-table. A Vector64 is checked
    // too, as a schema not written by flatc may leave offset64 unset on one,
    // and it would otherwise be copied inline below, as a scalar.
    if (fielddef.offset64() ||
        fielddef.type()->base_type() == reflection::Vector64) {
      return Offset<const Table*>();
    }
    uoffset_t offset = 0;
    switch (fielddef.type()->base_type()) {
      case reflection::String: {
        offset = use_string_pooling
                     ? fbb.CreateSharedString(GetFieldS(table, fielddef)).o
                     : fbb.CreateString(GetFieldS(table, fielddef)).o;
        break;
      }
      case reflection::Obj: {
        auto& subobjectdef = *schema.objects()->Get(fielddef.type()->index());
        if (!subobjectdef.is_struct()) {
          const auto copy =
              CopyTable(fbb, schema, subobjectdef, *GetFieldT(table, fielddef),
                        use_string_pooling);
          if (copy.IsNull()) return copy;
          offset = copy.o;
        }
        break;
      }
      case reflection::Union: {
        const reflection::Field* type_field =
            GetUnionTypeField(objectdef, fielddef);
        if (!type_field) return Offset<const Table*>();
        const auto copy = CopyUnionValue(
            fbb, schema, *schema.enums()->Get(fielddef.type()->index()),
            GetAnyFieldI(table, *type_field, schema), GetFieldT(table, fielddef),
            use_string_pooling);
        if (copy.IsNull()) return Offset<const Table*>();
        offset = copy.o;
        break;
      }
      case reflection::Vector: {
        auto vec =
            table.GetPointer<const Vector<Offset<Table>>*>(fielddef.offset());
        auto element_base_type = fielddef.type()->element();
        auto elemobjectdef =
            element_base_type == reflection::Obj
                ? schema.objects()->Get(fielddef.type()->index())
                : nullptr;
        switch (element_base_type) {
          case reflection::String: {
            std::vector<Offset<const String*>> elements(vec->size());
            auto vec_s = reinterpret_cast<const Vector<Offset<String>>*>(vec);
            for (uoffset_t i = 0; i < vec_s->size(); i++) {
              elements[i] = use_string_pooling
                                ? fbb.CreateSharedString(vec_s->Get(i)).o
                                : fbb.CreateString(vec_s->Get(i)).o;
            }
            offset = fbb.CreateVector(elements).o;
            break;
          }
          case reflection::Union: {
            // Each element's type is at its index in the `<name>_type` vector,
            // which is copied as a field of its own, a vector of scalars.
            const reflection::Field* type_field =
                GetUnionTypeField(objectdef, fielddef);
            const auto types =
                type_field ? GetFieldAnyV(table, *type_field) : nullptr;
            if (!types || types->size() != vec->size()) {
              return Offset<const Table*>();
            }
            // The types are as wide as the union's underlying type.
            const auto types_base_type =
                GetUnionUnderlyingType(schema, *type_field);
            auto& enumdef = *schema.enums()->Get(fielddef.type()->index());
            std::vector<Offset<void>> elements(vec->size());
            for (uoffset_t i = 0; i < vec->size(); i++) {
              elements[i] = CopyUnionValue(
                  fbb, schema, enumdef,
                  GetAnyVectorElemI(types, types_base_type, i), vec->Get(i),
                  use_string_pooling);
              if (elements[i].IsNull()) return Offset<const Table*>();
            }
            offset = fbb.CreateVector(elements).o;
            break;
          }
          case reflection::Obj: {
            if (!elemobjectdef->is_struct()) {
              std::vector<Offset<const Table*>> elements(vec->size());
              for (uoffset_t i = 0; i < vec->size(); i++) {
                elements[i] = CopyTable(fbb, schema, *elemobjectdef,
                                        *vec->Get(i), use_string_pooling);
                if (elements[i].IsNull()) return elements[i];
              }
              offset = fbb.CreateVector(elements).o;
              break;
            }
          }
            FLATBUFFERS_FALLTHROUGH();  // fall thru
          default: {                    // Scalars and structs.
            // A union's types are as wide as its underlying type.
            auto element_size =
                GetTypeSize(element_base_type == reflection::UType
                                ? GetUnionUnderlyingType(schema, fielddef)
                                : element_base_type);
            auto element_alignment = element_size;  // For primitive elements
            if (elemobjectdef && elemobjectdef->is_struct()) {
              // GetTypeSize(Obj) is the size of an offset, not of the struct.
              element_size = elemobjectdef->bytesize();
              element_alignment = elemobjectdef->minalign();
            }
            fbb.StartVector(vec->size(), element_size, element_alignment);
            fbb.PushBytes(vec->Data(), element_size * vec->size());
            offset = fbb.EndVector(vec->size());
            break;
          }
        }
        break;
      }
      default:  // Scalars.
        break;
    }
    if (offset) {
      offsets.push_back(offset);
    }
  }
  // Now we can build the actual table from either offsets or scalar data.
  auto start = objectdef.is_struct() ? fbb.StartStruct(objectdef.minalign())
                                     : fbb.StartTable();
  size_t offset_idx = 0;
  for (auto it = fielddefs->begin(); it != fielddefs->end(); ++it) {
    auto& fielddef = **it;
    if (!table.CheckField(fielddef.offset())) continue;
    auto base_type = fielddef.type()->base_type();
    switch (base_type) {
      case reflection::Obj: {
        auto& subobjectdef = *schema.objects()->Get(fielddef.type()->index());
        if (subobjectdef.is_struct()) {
          CopyInline(fbb, fielddef, table, subobjectdef.minalign(),
                     subobjectdef.bytesize());
          break;
        }
      }
        FLATBUFFERS_FALLTHROUGH();  // fall thru
      case reflection::Union:
      case reflection::String:
      case reflection::Vector:
        fbb.AddOffset(fielddef.offset(), Offset<void>(offsets[offset_idx++]));
        break;
      default: {  // Scalars.
        // A union's type is as wide as its underlying type.
        auto size = GetTypeSize(base_type == reflection::UType
                                    ? GetUnionUnderlyingType(schema, fielddef)
                                    : base_type);
        CopyInline(fbb, fielddef, table, size, size);
        break;
      }
    }
  }
  FLATBUFFERS_ASSERT(offset_idx == offsets.size());
  if (objectdef.is_struct()) {
    fbb.ClearOffsets();
    return fbb.EndStruct();
  } else {
    return fbb.EndTable(start);
  }
}

// Verifies the flatbuffer that starts `start` bytes into the buffer, as the
// generated VerifyBufferFromStart() does, with the schema's root in place of
// a generated T::Verify().
static bool VerifyRoot(Verifier& v, const reflection::Schema& schema,
                       const reflection::Object& root, const uint8_t* const buf,
                       const size_t start) {
  const size_t root_table = v.VerifyBufferStart(nullptr, start);
  return root_table != 0 &&
         VerifyObject(v, schema, root,
                      reinterpret_cast<const Table*>(buf + root_table),
                      /*required=*/true);
}

static Verifier::Options MakeOptions(const uoffset_t max_depth,
                                     const uoffset_t max_tables) {
  Verifier::Options opts;
  opts.max_depth = max_depth;
  opts.max_tables = max_tables;
  return opts;
}

bool Verify(const reflection::Schema& schema, const reflection::Object& root,
            const uint8_t* const buf, const size_t length,
            const Verifier::Options& opts) {
  // The Verifier asserts that the buffer is below max_size; the caller's
  // length is not known to be, so reject it here instead.
  if (length >= opts.max_size) return false;
  Verifier v(buf, length, opts);
  return VerifyRoot(v, schema, root, buf, 0);
}

bool Verify(const reflection::Schema& schema, const reflection::Object& root,
            const uint8_t* const buf, const size_t length,
            const uoffset_t max_depth, const uoffset_t max_tables) {
  return Verify(schema, root, buf, length, MakeOptions(max_depth, max_tables));
}

template <typename SizeT>
bool VerifySizePrefixed(const reflection::Schema& schema,
                        const reflection::Object& root,
                        const uint8_t* const buf, const size_t length,
                        const Verifier::Options& opts) {
  if (length >= opts.max_size) return false;
  Verifier v(buf, length, opts);
  return v.VerifySizePrefix<SizeT>() &&
         VerifyRoot(v, schema, root, buf, sizeof(SizeT));
}

template bool VerifySizePrefixed<uoffset_t>(const reflection::Schema&,
                                            const reflection::Object&,
                                            const uint8_t*, size_t,
                                            const Verifier::Options&);
template bool VerifySizePrefixed<uoffset64_t>(const reflection::Schema&,
                                              const reflection::Object&,
                                              const uint8_t*, size_t,
                                              const Verifier::Options&);

bool VerifySizePrefixed(const reflection::Schema& schema,
                        const reflection::Object& root,
                        const uint8_t* const buf, const size_t length,
                        const uoffset_t max_depth, const uoffset_t max_tables) {
  return VerifySizePrefixed<uoffset_t>(schema, root, buf, length,
                                       MakeOptions(max_depth, max_tables));
}

}  // namespace flatbuffers
