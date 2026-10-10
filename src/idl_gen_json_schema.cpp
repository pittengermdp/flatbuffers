/*
 * Copyright 2014 Google Inc. All rights reserved.
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

#include "idl_gen_json_schema.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include "flatbuffers/code_generators.h"
#include "flatbuffers/idl.h"
#include "flatbuffers/util.h"

namespace flatbuffers {

namespace jsons {

namespace {

template <class T>
static std::string GenFullName(const T* enum_def) {
  std::string full_name;
  const auto& name_spaces = enum_def->defined_namespace->components;
  for (auto ns = name_spaces.cbegin(); ns != name_spaces.cend(); ++ns) {
    full_name.append(*ns + "_");
  }
  full_name.append(enum_def->name);
  return full_name;
}

static std::string GenType(const std::string& name) {
  return "\"type\" : \"" + name + "\"";
}

// The JSON Schema "type" name for a scalar or string base type, or nullptr for
// any other base type.
static const char* JsonTypeName(BaseType type) {
  switch (type) {
    case BASE_TYPE_BOOL:
      return "boolean";
    case BASE_TYPE_CHAR:
    case BASE_TYPE_UCHAR:
    case BASE_TYPE_SHORT:
    case BASE_TYPE_USHORT:
    case BASE_TYPE_INT:
    case BASE_TYPE_UINT:
    case BASE_TYPE_LONG:
    case BASE_TYPE_ULONG:
      return "integer";
    case BASE_TYPE_FLOAT:
    case BASE_TYPE_DOUBLE:
      return "number";
    case BASE_TYPE_STRING:
      return "string";
    default:
      return nullptr;
  }
}

// The minimum/maximum keywords bounding an integer base type to its width,
// each preceded by ", ". Empty for every non-integer base type.
static std::string GenIntegerRange(BaseType type) {
  switch (type) {
    case BASE_TYPE_CHAR:
      return ", \"minimum\" : " +
             NumToString(std::numeric_limits<int8_t>::min()) +
             ", \"maximum\" : " +
             NumToString(std::numeric_limits<int8_t>::max());
    case BASE_TYPE_UCHAR:
      return ", \"minimum\" : 0, \"maximum\" :" +
             NumToString(std::numeric_limits<uint8_t>::max());
    case BASE_TYPE_SHORT:
      return ", \"minimum\" : " +
             NumToString(std::numeric_limits<int16_t>::min()) +
             ", \"maximum\" : " +
             NumToString(std::numeric_limits<int16_t>::max());
    case BASE_TYPE_USHORT:
      return ", \"minimum\" : 0, \"maximum\" : " +
             NumToString(std::numeric_limits<uint16_t>::max());
    case BASE_TYPE_INT:
      return ", \"minimum\" : " +
             NumToString(std::numeric_limits<int32_t>::min()) +
             ", \"maximum\" : " +
             NumToString(std::numeric_limits<int32_t>::max());
    case BASE_TYPE_UINT:
      return ", \"minimum\" : 0, \"maximum\" : " +
             NumToString(std::numeric_limits<uint32_t>::max());
    case BASE_TYPE_LONG:
      return ", \"minimum\" : " +
             NumToString(std::numeric_limits<int64_t>::min()) +
             ", \"maximum\" : " +
             NumToString(std::numeric_limits<int64_t>::max());
    case BASE_TYPE_ULONG:
      return ", \"minimum\" : 0, \"maximum\" : " +
             NumToString(std::numeric_limits<uint64_t>::max());
    default:
      return "";
  }
}

static std::string GenType(BaseType type) {
  const char* type_name = JsonTypeName(type);
  if (type_name == nullptr) {
    return "";
  }
  return GenType(std::string(type_name)) + GenIntegerRange(type);
}

static std::string Join(const std::vector<std::string>& parts,
                        const std::string& separator) {
  std::string joined;
  for (auto part = parts.cbegin(); part != parts.cend(); ++part) {
    if (part != parts.cbegin()) {
      joined += separator;
    }
    joined += *part;
  }
  return joined;
}

static bool Is64BitInteger(BaseType type) {
  return type == BASE_TYPE_LONG || type == BASE_TYPE_ULONG;
}

static bool IsBitFlags(const EnumDef& enum_def) {
  return enum_def.attributes.Lookup("bit_flags") != nullptr;
}

// Regex alternatives matching exactly the canonical decimal spellings (no
// sign, no leading zero) of the integers from `first` (0 or 1) to `max`,
// itself a canonical decimal string. Every number shorter than `max` is one
// alternative; each digit of `max` that has a smaller choice adds one more;
// `max` is the last.
static std::vector<std::string> DecimalAlternatives(const std::string& max,
                                                    int first) {
  std::vector<std::string> alternatives;
  if (first == 0) {
    alternatives.push_back("0");
  }
  const size_t digits = max.size();
  if (digits >= 2) {
    alternatives.push_back("[1-9][0-9]{0," + NumToString(digits - 2) + "}");
  }
  for (size_t i = 0; i < digits; ++i) {
    const char lowest = i == 0 ? '1' : '0';
    const char below = static_cast<char>(max[i] - 1);
    if (below < lowest) {
      continue;
    }
    std::string alternative = max.substr(0, i);
    alternative += below == lowest ? std::string(1, lowest)
                                   : "[" + std::string(1, lowest) + "-" +
                                         std::string(1, below) + "]";
    const size_t rest = digits - i - 1;
    if (rest > 0) {
      alternative += "[0-9]{" + NumToString(rest) + "}";
    }
    alternatives.push_back(alternative);
  }
  alternatives.push_back(max);
  return alternatives;
}

// The largest value of an integer base type, and for a signed one the
// magnitude of its smallest (one past the largest), as decimal strings.
// Returns false for any other base type.
static bool IntegerBounds(BaseType type, std::string* max,
                          std::string* negative_magnitude) {
  switch (type) {
    case BASE_TYPE_CHAR:
      *max = "127";
      *negative_magnitude = "128";
      return true;
    case BASE_TYPE_UCHAR:
      *max = "255";
      return true;
    case BASE_TYPE_SHORT:
      *max = "32767";
      *negative_magnitude = "32768";
      return true;
    case BASE_TYPE_USHORT:
      *max = "65535";
      return true;
    case BASE_TYPE_INT:
      *max = "2147483647";
      *negative_magnitude = "2147483648";
      return true;
    case BASE_TYPE_UINT:
      *max = "4294967295";
      return true;
    case BASE_TYPE_LONG:
      *max = "9223372036854775807";
      *negative_magnitude = "9223372036854775808";
      return true;
    case BASE_TYPE_ULONG:
      *max = "18446744073709551615";
      return true;
    default:
      return false;
  }
}

// A regex accepting exactly the canonical decimal strings of an integer
// type's range. It bounds a string-spelled integer -- a 64-bit value under
// --json-int64-as-string, or a map key, which JSON always writes as a
// property name -- as tightly as minimum and maximum bound a number, with no
// custom "format" a validator must register. "-0" is not canonical, so the
// negative side starts at 1.
static std::string GenIntegerPattern(BaseType type) {
  std::string max;
  std::string negative_magnitude;
  if (!IntegerBounds(type, &max, &negative_magnitude)) {
    return "";
  }
  std::string pattern = "^(" + Join(DecimalAlternatives(max, 0), "|");
  if (!negative_magnitude.empty()) {
    pattern +=
        "|-(" + Join(DecimalAlternatives(negative_magnitude, 1), "|") + ")";
  }
  return pattern + ")$";
}

// Recognised field attributes, each declared in the schema by the user
// (`attribute "json_minimum";`) and mapped onto one JSON Schema keyword.
struct ValidationAttribute {
  const char* attribute;
  const char* keyword;
};

// Bounds on a number: integers and floats, or the elements of a vector of them.
static const ValidationAttribute kNumberAttributes[] = {
    {"json_minimum", "minimum"},
    {"json_maximum", "maximum"},
    {"json_exclusive_minimum", "exclusiveMinimum"},
    {"json_exclusive_maximum", "exclusiveMaximum"},
};

// Length bounds on a string, or on the elements of a vector of strings.
static const ValidationAttribute kLengthAttributes[] = {
    {"json_min_length", "minLength"},
    {"json_max_length", "maxLength"},
};

// String-valued constraints on a string, or on the elements of a vector of
// strings.
static const ValidationAttribute kTextAttributes[] = {
    {"json_pattern", "pattern"},
    {"json_format", "format"},
};

// Bounds on the length of a vector.
static const ValidationAttribute kItemsAttributes[] = {
    {"json_min_items", "minItems"},
    {"json_max_items", "maxItems"},
};

static bool IsKnownValidationAttribute(const std::string& name) {
  for (const auto& a : kNumberAttributes) {
    if (name == a.attribute) return true;
  }
  for (const auto& a : kLengthAttributes) {
    if (name == a.attribute) return true;
  }
  for (const auto& a : kTextAttributes) {
    if (name == a.attribute) return true;
  }
  for (const auto& a : kItemsAttributes) {
    if (name == a.attribute) return true;
  }
  return false;
}

}  // namespace

class JsonSchemaGenerator : public BaseGenerator {
 private:
  std::string code_;
  std::string error_;

  // Keywords a field's validation attributes add: `value` constrains the field
  // itself, or each element when the field is a vector; `array` constrains
  // the vector as a whole. Each keyword is preceded by ", ".
  struct ValidationKeywords {
    std::string value;
    std::string array;
  };

  bool Int64AsString() const { return parser_.opts.json_int64_as_string; }

  std::string DefsKeyword() const {
    return parser_.opts.jsonschema_2020_12 ? "$defs" : "definitions";
  }

  // The schema file a .fbs file's own definitions are written to.
  static std::string SchemaFileName(const std::string& fbs_path) {
    return StripExtension(StripPath(fbs_path)) + ".schema.json";
  }

  // A reference to a definition. In cross-file mode a type that comes from an
  // included file lives only in that file's schema, so the reference names it.
  template <class T>
  std::string GenTypeRef(const T* def) const {
    std::string file;
    if (parser_.opts.jsonschema_cross_file_refs && def->generated) {
      file = SchemaFileName(def->file);
    }
    return "\"$ref\" : \"" + file + "#/" + DefsKeyword() + "/" +
           GenFullName(def) + "\"";
  }

  // The string form of a 64-bit integer under --json-int64-as-string: its
  // decimal digits, quoted, so a JSON reader that holds numbers as doubles
  // (JavaScript's JSON.parse) cannot round it.
  std::string GenInt64StringType(BaseType type) const {
    return GenType(std::string("string")) + ", \"pattern\" : \"" +
           GenIntegerPattern(type) + "\"";
  }

  // A scalar or string. A 64-bit integer under --json-int64-as-string may be
  // written either way: the JSON parser reads both, and only the string form
  // survives a reader that holds numbers as doubles.
  std::string GenScalarType(BaseType type) const {
    if (Is64BitInteger(type) && Int64AsString()) {
      return "\"anyOf\" : [{ " + GenType(type) + " }, { " +
             GenInt64StringType(type) + " }]";
    }
    return GenType(type);
  }

  // An optional scalar (`field: T = null`) has no default to fall back on, so
  // the JSON parser reads a null value for it as "not present", exactly like
  // omitting it. The schema admits null alongside the field's own type; the
  // integer range keywords constrain only numbers, so they leave null alone.
  std::string GenOptionalScalarType(const Type& type) const {
    const std::string null_type = GenType(std::string("null"));
    if (type.enum_def != nullptr) {
      return "\"anyOf\" : [{ " + GenTypeRef(type.enum_def) + " }, { " +
             null_type + " }]";
    }
    if (Is64BitInteger(type.base_type) && Int64AsString()) {
      return "\"anyOf\" : [{ " + GenType(type.base_type) + " }, { " +
             GenInt64StringType(type.base_type) + " }, { " + null_type + " }]";
    }
    const char* type_name = JsonTypeName(type.base_type);
    FLATBUFFERS_ASSERT(type_name != nullptr);  // the parser admits scalars only
    if (type_name == nullptr) {
      return "";
    }
    return "\"type\" : [\"" + std::string(type_name) + "\", \"null\"]" +
           GenIntegerRange(type.base_type);
  }

  std::string GenBaseType(const Type& type) const {
    if (type.struct_def != nullptr) {
      return GenTypeRef(type.struct_def);
    }
    if (type.enum_def != nullptr) {
      return GenTypeRef(type.enum_def);
    }
    return GenScalarType(type.base_type);
  }

  // The schema of one union variant's value, or "" for NONE, which has none.
  // Every other variant is a table or struct: string variants are an advanced
  // union feature, which the parser rejects when generating JSON Schema.
  std::string GenUnionVariantType(const EnumVal& variant) const {
    if (variant.union_type.base_type != BASE_TYPE_STRUCT) {
      return "";
    }
    return GenTypeRef(variant.union_type.struct_def);
  }

  // The schema of a union value without regard to its type field: any one of
  // the variants. Pairing the value with the variant its type field names is a
  // cross-member constraint, stated on the enclosing table instead.
  std::string GenUnionValueType(const EnumDef& union_def) const {
    std::vector<std::string> variants;
    for (auto variant = union_def.Vals().cbegin();
         variant != union_def.Vals().cend(); ++variant) {
      const std::string variant_type = GenUnionVariantType(**variant);
      if (!variant_type.empty()) {
        variants.push_back("{ " + variant_type + " }");
      }
    }
    return "\"anyOf\": [" + Join(variants, ",") + "]";
  }

  std::string GenArrayType(const Type& type,
                           const std::string& element_keywords) const {
    std::string element_type;
    if (type.struct_def != nullptr) {
      element_type = GenTypeRef(type.struct_def);
    } else if (type.enum_def != nullptr) {
      element_type = GenTypeRef(type.enum_def);
    } else {
      element_type = GenScalarType(type.element);
    }

    return "\"type\" : \"array\", \"items\" : {" + element_type +
           element_keywords + "}";
  }

  std::string GenFieldType(const Type& type,
                           const std::string& element_keywords) const {
    switch (type.base_type) {
      case BASE_TYPE_ARRAY:
        FLATBUFFERS_FALLTHROUGH();  // fall thru
      case BASE_TYPE_VECTOR: {
        return GenArrayType(type, element_keywords);
      }
      case BASE_TYPE_STRUCT: {
        return GenTypeRef(type.struct_def);
      }
      case BASE_TYPE_UNION: {
        return GenUnionValueType(*type.enum_def);
      }
      case BASE_TYPE_UTYPE:
        return GenTypeRef(type.enum_def);
      default: {
        return GenBaseType(type);
      }
    }
  }

  // A map<K, V> field is a vector of generated entry tables with this
  // attribute; JSON spells it as an object keyed by the entry keys.
  static bool IsMapField(const FieldDef& field) {
    return field.attributes.Lookup("map_entry") != nullptr;
  }

  // A set<K> field: a vector of key-only entry tables, spelled in JSON as an
  // array of the keys themselves.
  static bool IsSetField(const FieldDef& field) {
    return field.attributes.Lookup("set_entry") != nullptr;
  }

  static const FieldDef* EntryKey(const StructDef& entry) {
    for (auto f = entry.fields.vec.cbegin(); f != entry.fields.vec.cend();
         ++f) {
      if ((*f)->key) return *f;
    }
    return nullptr;
  }

  static const FieldDef* EntryValue(const StructDef& entry) {
    return entry.fields.Lookup("value");
  }

  // A map key as a JSON property name, which is always a string: an enum by
  // its variant name, a bool as "true"/"false", an integer as its canonical
  // decimal digits.
  std::string GenMapKeyName(const Type& key_type) const {
    if (key_type.enum_def != nullptr) {
      return GenTypeRef(key_type.enum_def);
    }
    if (key_type.base_type == BASE_TYPE_BOOL) {
      return "\"enum\" : [\"false\", \"true\"]";
    }
    if (IsInteger(key_type.base_type)) {
      return GenType(std::string("string")) + ", \"pattern\" : \"" +
             GenIntegerPattern(key_type.base_type) + "\"";
    }
    return GenType(std::string("string"));
  }

  // A map field: an object whose property names are the keys and whose
  // values are the map's values. Value keywords constrain every value.
  std::string GenMapType(const FieldDef& field,
                         const std::string& value_keywords) const {
    const StructDef& entry = *field.value.type.VectorType().struct_def;
    const FieldDef* key = EntryKey(entry);
    const FieldDef* value = EntryValue(entry);
    std::string code = GenType(std::string("object"));
    if (key != nullptr) {
      code += ", \"propertyNames\" : {" + GenMapKeyName(key->value.type) + "}";
    }
    if (value != nullptr) {
      code += ", \"additionalProperties\" : {" +
              GenFieldType(value->value.type, "") + value_keywords + "}";
    }
    return code;
  }

  // A set field: an array of distinct keys. Key keywords constrain each one.
  std::string GenSetType(const FieldDef& field,
                         const std::string& key_keywords) const {
    const FieldDef* key = EntryKey(*field.value.type.VectorType().struct_def);
    const std::string items =
        key != nullptr ? GenFieldType(key->value.type, "") : "";
    return "\"type\" : \"array\", \"items\" : {" + items + key_keywords +
           "}, \"uniqueItems\" : true";
  }

  // The schema of one table or struct property, validation keywords included.
  std::string GenPropertyType(const FieldDef& field,
                              const ValidationKeywords& validation) const {
    if (field.IsScalarOptional()) {
      return GenOptionalScalarType(field.value.type) + validation.value;
    }
    if (IsMapField(field)) {
      return GenMapType(field, validation.value) + validation.array;
    }
    if (IsSetField(field)) {
      return GenSetType(field, validation.value) + validation.array;
    }
    if (IsVector(field.value.type)) {
      return GenFieldType(field.value.type, validation.value) +
             validation.array;
    }
    return GenFieldType(field.value.type, "") + validation.value;
  }

  // A 64-bit integer as the active int64 option spells it.
  std::string Int64Literal(const std::string& digits) const {
    return Int64AsString() ? "\"" + digits + "\"" : digits;
  }

  // An enum value as the JSON parser spells it: the variant name, or for
  // bit_flags the space-separated names of its set bits. A value with no such
  // spelling (an unnamed value, or bit_flags zero) is written as its number.
  static std::string GenEnumLiteral(const EnumDef& enum_def,
                                    const std::string& constant) {
    if (IsBitFlags(enum_def)) {
      uint64_t bits = 0;
      if (!StringToNumber(constant.c_str(), &bits)) {
        return "";
      }
      std::vector<std::string> names;
      uint64_t covered = 0;
      for (auto ev = enum_def.Vals().cbegin(); ev != enum_def.Vals().cend();
           ++ev) {
        const uint64_t mask = (*ev)->GetAsUInt64();
        if (mask != 0 && (bits & mask) == mask) {
          names.push_back((*ev)->name);
          covered |= mask;
        }
      }
      if (bits != 0 && covered == bits) {
        return "\"" + Join(names, " ") + "\"";
      }
      return NumToString(bits);
    }
    const EnumVal* ev = enum_def.FindByValue(constant);
    return ev != nullptr ? "\"" + ev->name + "\"" : constant;
  }

  // The value an absent table scalar takes, as a JSON literal, or "" when
  // there is none JSON can carry: optional scalars have no default, struct
  // fields are always present, and NaN or infinity are not JSON numbers.
  std::string GenDefaultValue(const StructDef& structure,
                              const FieldDef& field) const {
    const Type& type = field.value.type;
    if (structure.fixed || !IsScalar(type.base_type) ||
        type.base_type == BASE_TYPE_UTYPE || field.IsScalarOptional()) {
      return "";
    }
    const std::string& constant = field.value.constant;
    if (type.enum_def != nullptr) {
      return GenEnumLiteral(*type.enum_def, constant);
    }
    if (type.base_type == BASE_TYPE_BOOL) {
      return constant == "0" || constant == "false" ? "false" : "true";
    }
    if (IsFloat(type.base_type)) {
      double value = 0;
      if (!StringToNumber(constant.c_str(), &value) || !std::isfinite(value)) {
        return "";
      }
      return NumToString(value);
    }
    if (type.base_type == BASE_TYPE_ULONG) {
      uint64_t value = 0;
      if (!StringToNumber(constant.c_str(), &value)) {
        return "";
      }
      return Int64Literal(NumToString(value));
    }
    int64_t value = 0;
    if (!StringToNumber(constant.c_str(), &value)) {
      return "";
    }
    return Is64BitInteger(type.base_type) ? Int64Literal(NumToString(value))
                                          : NumToString(value);
  }

  bool Fail(const FieldDef& field, const std::string& message) {
    error_ = "field `" + field.name + "`: " + message;
    return false;
  }

  // Appends one numeric keyword from an attribute value, normalised so the
  // schema always holds a JSON number whatever spelling the .fbs used.
  bool AppendNumberKeyword(const FieldDef& field, const std::string& attribute,
                           const char* keyword, const Value& value,
                           std::string* keywords) {
    int64_t integer = 0;
    double real = 0;
    if (StringToNumber(value.constant.c_str(), &integer)) {
      *keywords += ", \"" + std::string(keyword) + "\" : " +
                   NumToString(integer);
    } else if (StringToNumber(value.constant.c_str(), &real) &&
               std::isfinite(real)) {
      *keywords +=
          ", \"" + std::string(keyword) + "\" : " + NumToString(real);
    } else {
      return Fail(field, attribute + " must be a finite number, got `" +
                             value.constant + "`");
    }
    return true;
  }

  bool AppendCountKeyword(const FieldDef& field, const std::string& attribute,
                          const char* keyword, const Value& value,
                          std::string* keywords) {
    uint64_t count = 0;
    if (!StringToNumber(value.constant.c_str(), &count)) {
      return Fail(field, attribute + " must be a non-negative integer, got `" +
                             value.constant + "`");
    }
    *keywords +=
        ", \"" + std::string(keyword) + "\" : " + NumToString(count);
    return true;
  }

  bool AppendTextKeyword(const FieldDef& field, const std::string& attribute,
                         const char* keyword, const Value& value,
                         std::string* keywords) {
    if (value.type.base_type != BASE_TYPE_STRING) {
      return Fail(field, attribute + " must be a string literal");
    }
    std::string escaped;
    if (!EscapeString(value.constant.c_str(), value.constant.length(),
                      &escaped, true, true)) {
      return Fail(field, attribute + " is not valid UTF-8");
    }
    *keywords += ", \"" + std::string(keyword) + "\" : " + escaped;
    return true;
  }

  // Maps a field's json_* attributes onto JSON Schema keywords. An attribute
  // that does not fit the field's type, or a json_* name this generator does
  // not know (most likely a typo), fails generation rather than being dropped:
  // a constraint that silently vanishes is worse than none at all.
  bool GenValidationKeywords(const FieldDef& field, ValidationKeywords* out) {
    const Type& type = field.value.type;
    const bool is_vector = IsVector(type);
    const bool is_map = is_vector && IsMapField(field);
    // A map's element is its value; a set's is its key: the entry tables
    // themselves never appear in JSON.
    const FieldDef* member = nullptr;
    if (is_map) {
      member = EntryValue(*type.VectorType().struct_def);
    } else if (is_vector && IsSetField(field)) {
      member = EntryKey(*type.VectorType().struct_def);
    }
    const Type element = member != nullptr ? member->value.type
                         : is_vector       ? type.VectorType()
                                           : type;
    const bool is_number = IsScalar(element.base_type) &&
                           element.base_type != BASE_TYPE_BOOL &&
                           element.base_type != BASE_TYPE_UTYPE &&
                           element.enum_def == nullptr;
    const bool is_string = element.base_type == BASE_TYPE_STRING;

    for (auto it = field.attributes.dict.cbegin();
         it != field.attributes.dict.cend(); ++it) {
      if (it->first.compare(0, 5, "json_") == 0 &&
          !IsKnownValidationAttribute(it->first)) {
        return Fail(field, "unknown validation attribute `" + it->first + "`");
      }
    }
    for (const auto& a : kNumberAttributes) {
      const Value* value = field.attributes.Lookup(a.attribute);
      if (value == nullptr) continue;
      if (!is_number) {
        return Fail(field, std::string(a.attribute) +
                               " applies only to integer and float fields");
      }
      if (Is64BitInteger(element.base_type) && Int64AsString()) {
        return Fail(field, std::string(a.attribute) +
                               " cannot bound a 64-bit field under "
                               "--json-int64-as-string: its string form "
                               "would escape the bound");
      }
      if (!AppendNumberKeyword(field, a.attribute, a.keyword, *value,
                               &out->value)) {
        return false;
      }
    }
    for (const auto& a : kLengthAttributes) {
      const Value* value = field.attributes.Lookup(a.attribute);
      if (value == nullptr) continue;
      if (!is_string) {
        return Fail(field,
                    std::string(a.attribute) + " applies only to strings");
      }
      if (!AppendCountKeyword(field, a.attribute, a.keyword, *value,
                              &out->value)) {
        return false;
      }
    }
    for (const auto& a : kTextAttributes) {
      const Value* value = field.attributes.Lookup(a.attribute);
      if (value == nullptr) continue;
      if (!is_string) {
        return Fail(field,
                    std::string(a.attribute) + " applies only to strings");
      }
      if (!AppendTextKeyword(field, a.attribute, a.keyword, *value,
                             &out->value)) {
        return false;
      }
    }
    for (const auto& a : kItemsAttributes) {
      const Value* value = field.attributes.Lookup(a.attribute);
      if (value == nullptr) continue;
      if (!is_vector) {
        return Fail(field,
                    std::string(a.attribute) + " applies only to vectors");
      }
      // An object's size is its property count, not an item count.
      const char* keyword = a.keyword;
      if (is_map) {
        keyword = std::string(a.keyword) == "minItems" ? "minProperties"
                                                       : "maxProperties";
      }
      if (!AppendCountKeyword(field, a.attribute, keyword, *value,
                              &out->array)) {
        return false;
      }
    }
    return true;
  }

 public:
  JsonSchemaGenerator(const Parser& parser, const std::string& path,
                      const std::string& file_name)
      : BaseGenerator(parser, path, file_name, "", "", "json") {}

  explicit JsonSchemaGenerator(const BaseGenerator& base_generator)
      : BaseGenerator(base_generator) {}

  std::string GeneratedFileName(const std::string& path,
                                const std::string& file_name,
                                const IDLOptions& options /* unused */) const {
    (void)options;
    return path + file_name + ".schema.json";
  }

  // If indentation is less than 0, that indicates we don't want any newlines
  // either.
  std::string NewLine() const {
    return parser_.opts.indent_step >= 0 ? "\n" : "";
  }

  std::string Indent(int indent) const {
    const auto num_spaces = indent * std::max(parser_.opts.indent_step, 0);
    return std::string(num_spaces, ' ');
  }

  std::string PrepareDescription(
      const std::vector<std::string>& comment_lines) {
    std::string comment;
    for (auto line_iterator = comment_lines.cbegin();
         line_iterator != comment_lines.cend(); ++line_iterator) {
      const auto& comment_line = *line_iterator;

      // remove leading and trailing spaces from comment line
      const auto start = std::find_if(comment_line.begin(), comment_line.end(),
                                      [](char c) { return !isspace(c); });
      const auto end =
          std::find_if(comment_line.rbegin(), comment_line.rend(), [](char c) {
            return !isspace(c);
          }).base();
      if (start < end) {
        comment.append(start, end);
      } else {
        comment.append(comment_line);
      }

      if (line_iterator + 1 != comment_lines.cend()) comment.append("\n");
    }
    if (!comment.empty()) {
      std::string description;
      if (EscapeString(comment.c_str(), comment.length(), &description, true,
                       true)) {
        return description;
      }
      return "";
    }
    return "";
  }

  // A union field is two JSON members: `<field>_type`, naming the variant, and
  // `<field>`, holding its value. The JSON parser rejects a value with no type
  // (it cannot tell what to parse), any value under NONE, and a value that
  // does not parse as the variant the type names; it accepts a type with no
  // value. Neither property schema can see the other member, so the pairing
  // is stated on the enclosing table. Empty when the table has no union.
  std::string GenUnionConstraints(const StructDef& structure) const {
    std::vector<std::string> dependencies;
    std::vector<std::string> conditions;
    const auto& fields = structure.fields.vec;
    for (auto f = fields.cbegin(); f != fields.cend(); ++f) {
      const FieldDef& field = **f;
      if (field.value.type.base_type != BASE_TYPE_UNION ||
          field.sibling_union_field == nullptr) {
        continue;
      }
      const std::string value_name = "\"" + field.name + "\"";
      const std::string type_name =
          "\"" + field.sibling_union_field->name + "\"";
      dependencies.push_back(value_name + " : [" + type_name + "]");
      const auto& variants = field.value.type.enum_def->Vals();
      for (auto v = variants.cbegin(); v != variants.cend(); ++v) {
        const EnumVal& variant = **v;
        const std::string then =
            variant.union_type.base_type == BASE_TYPE_NONE
                ? "{ \"not\" : { \"required\" : [" + value_name + "] } }"
                : "{ \"properties\" : { " + value_name + " : { " +
                      GenUnionVariantType(variant) + " } } }";
        conditions.push_back(Indent(4) + "{ \"if\" : { \"properties\" : { " +
                             type_name + " : { \"const\" : \"" + variant.name +
                             "\" } }, \"required\" : [" + type_name +
                             "] }, \"then\" : " + then + " }");
      }
    }
    if (dependencies.empty()) {
      return "";
    }
    return Indent(3) + "\"dependentRequired\" : { " +
           Join(dependencies, ", ") + " }," + NewLine() + Indent(3) +
           "\"allOf\" : [" + NewLine() + Join(conditions, "," + NewLine()) +
           NewLine() + Indent(3) + "]," + NewLine();
  }

  // An enum's definition body. A bit_flags value is written as the
  // space-separated names of its set bits, or as its number when it has none
  // (zero) or sets a bit no name covers -- both forms the JSON parser reads.
  std::string GenEnumBody(const EnumDef& enum_def) const {
    if (IsBitFlags(enum_def)) {
      std::vector<std::string> names;
      for (auto ev = enum_def.Vals().cbegin(); ev != enum_def.Vals().cend();
           ++ev) {
        names.push_back((*ev)->name);
      }
      const std::string name = "(" + Join(names, "|") + ")";
      return Indent(3) + "\"anyOf\" : [{ " + GenType(std::string("string")) +
             ", \"pattern\" : \"^" + name + "( " + name + ")*$\" }, { " +
             GenType(std::string("integer")) +
             GenIntegerRange(enum_def.underlying_type.base_type) + " }]" +
             NewLine();
    }
    std::string body = Indent(3) + GenType("string") + "," + NewLine();
    auto enumdef(Indent(3) + "\"enum\": [");
    for (auto enum_value = enum_def.Vals().begin();
         enum_value != enum_def.Vals().end(); ++enum_value) {
      enumdef.append("\"" + (*enum_value)->name + "\"");
      if (*enum_value != enum_def.Vals().back()) {
        enumdef.append(", ");
      }
    }
    enumdef.append("]");
    return body + enumdef + NewLine();
  }

  // In cross-file mode a file's schema holds only the types that file
  // declares; the parser marks every type from an included file generated.
  template <class T>
  bool IsOwnDefinition(const T* def) const {
    return !(parser_.opts.jsonschema_cross_file_refs && def->generated);
  }

  bool generate() {
    if (!GenerateSchema()) {
      std::cerr << "error: " << file_name_ << ".fbs: " << error_ << "\n";
      return false;
    }
    return true;
  }

  bool GenerateSchema() {
    code_ = "";
    code_ += "{" + NewLine();
    code_ += Indent(1) + "\"$schema\": \"https://json-schema.org/draft/" +
             (parser_.opts.jsonschema_2020_12 ? "2020-12" : "2019-09") +
             "/schema\"," + NewLine();
    if (parser_.opts.jsonschema_cross_file_refs) {
      // Relative, so each file is addressable by name next to its includes'.
      code_ += Indent(1) + "\"$id\": \"" + file_name_ + ".schema.json\"," +
               NewLine();
    }
    code_ += Indent(1) + "\"" + DefsKeyword() + "\": {" + NewLine();
    // Collected and joined, so the last one carries no trailing comma whatever
    // kind it is -- a schema of enums alone used to end in one.
    std::vector<std::string> definitions;
    for (auto e = parser_.enums_.vec.cbegin(); e != parser_.enums_.vec.cend();
         ++e) {
      if (!IsOwnDefinition(*e)) continue;
      std::string definition =
          Indent(2) + "\"" + GenFullName(*e) + "\" : {" + NewLine();
      definition += GenEnumBody(**e);
      definition += Indent(2) + "}";  // close type
      definitions.push_back(definition);
    }
    for (auto s = parser_.structs_.vec.cbegin();
         s != parser_.structs_.vec.cend(); ++s) {
      const auto& structure = *s;
      if (!IsOwnDefinition(structure)) continue;
      std::string definition =
          Indent(2) + "\"" + GenFullName(structure) + "\" : {" + NewLine();
      definition += Indent(3) + GenType("object") + "," + NewLine();
      const auto& comment_lines = structure->doc_comment;
      auto comment = PrepareDescription(comment_lines);
      if (comment != "") {
        definition +=
            Indent(3) + "\"description\" : " + comment + "," + NewLine();
      }

      definition += Indent(3) + "\"properties\" : {" + NewLine();

      const auto& properties = structure->fields.vec;
      for (auto prop = properties.cbegin(); prop != properties.cend(); ++prop) {
        const auto& property = *prop;
        ValidationKeywords validation;
        if (!GenValidationKeywords(*property, &validation)) {
          return false;
        }
        std::string arrayInfo = "";
        if (IsArray(property->value.type)) {
          arrayInfo = "," + NewLine() + Indent(8) + "\"minItems\": " +
                      NumToString(property->value.type.fixed_length) + "," +
                      NewLine() + Indent(8) + "\"maxItems\": " +
                      NumToString(property->value.type.fixed_length);
        }
        std::string default_info = "";
        if (parser_.opts.jsonschema_defaults) {
          const std::string default_value =
              GenDefaultValue(*structure, *property);
          if (!default_value.empty()) {
            default_info =
                "," + NewLine() + Indent(8) + "\"default\" : " + default_value;
          }
        }
        std::string deprecated_info = "";
        if (property->deprecated) {
          deprecated_info =
              "," + NewLine() + Indent(8) + "\"deprecated\" : true";
        }
        std::string typeLine = Indent(4) + "\"" + property->name + "\"";
        typeLine += " : {" + NewLine() + Indent(8);
        typeLine += GenPropertyType(*property, validation);
        typeLine += arrayInfo;
        typeLine += default_info;
        typeLine += deprecated_info;
        auto description = PrepareDescription(property->doc_comment);
        if (description != "") {
          typeLine +=
              "," + NewLine() + Indent(8) + "\"description\" : " + description;
        }

        typeLine += NewLine() + Indent(7) + "}";
        if (property != properties.back()) {
          typeLine.append(",");
        }
        definition += typeLine + NewLine();
      }
      definition += Indent(3) + "}," + NewLine();  // close properties

      // A struct has no optional fields: the JSON parser rejects one with any
      // field missing ("wrong number of initializers"), so all are required.
      const bool is_struct = structure->fixed;
      std::vector<FieldDef*> requiredProperties;
      std::copy_if(properties.begin(), properties.end(),
                   back_inserter(requiredProperties),
                   [is_struct](FieldDef const* prop) {
                     return is_struct || prop->IsRequired();
                   });
      if (!requiredProperties.empty()) {
        auto required_string(Indent(3) + "\"required\" : [");
        for (auto req_prop = requiredProperties.cbegin();
             req_prop != requiredProperties.cend(); ++req_prop) {
          required_string.append("\"" + (*req_prop)->name + "\"");
          if (*req_prop != requiredProperties.back()) {
            required_string.append(", ");
          }
        }
        required_string.append("],");
        definition += required_string + NewLine();
      }
      definition += GenUnionConstraints(*structure);
      definition += Indent(3) + "\"additionalProperties\" : false" + NewLine();
      definition += Indent(2) + "}";  // close type
      definitions.push_back(definition);
    }
    if (!definitions.empty()) {
      code_ += Join(definitions, "," + NewLine()) + NewLine();
    }

    // A schema without a root_type still defines types worth validating
    // against; it just names no default document type, so consumers address a
    // definition directly ("<file>.schema.json#/definitions/<Ns>_<Type>").
    if (parser_.root_struct_def_ == nullptr) {
      code_ += Indent(1) + "}" + NewLine();  // close definitions
    } else {
      code_ += Indent(1) + "}," + NewLine();  // close definitions
      code_ += Indent(1) + GenTypeRef(parser_.root_struct_def_) + NewLine();
    }

    code_ += "}" + NewLine();  // close schema root
    return true;
  }

  bool save() const {
    const auto file_path = GeneratedFileName(path_, file_name_, parser_.opts);
    return parser_.opts.file_saver->SaveFile(file_path.c_str(), code_, false);
  }

  const std::string getJson() { return code_; }
};
}  // namespace jsons

static bool GenerateJsonSchema(const Parser& parser, const std::string& path,
                               const std::string& file_name) {
  jsons::JsonSchemaGenerator generator(parser, path, file_name);
  if (!generator.generate()) {
    return false;
  }
  return generator.save();
}

namespace {

class JsonSchemaCodeGenerator : public CodeGenerator {
 public:
  Status GenerateCode(const Parser& parser, const std::string& path,
                      const std::string& filename) override {
    if (!GenerateJsonSchema(parser, path, filename)) {
      return Status::ERROR;
    }
    return Status::OK;
  }

  Status GenerateCode(const uint8_t*, int64_t, const CodeGenOptions&) override {
    return Status::NOT_IMPLEMENTED;
  }

  Status GenerateMakeRule(const Parser& parser, const std::string& path,
                          const std::string& filename,
                          std::string& output) override {
    (void)parser;
    (void)path;
    (void)filename;
    (void)output;
    return Status::NOT_IMPLEMENTED;
  }

  Status GenerateGrpcCode(const Parser& parser, const std::string& path,
                          const std::string& filename) override {
    (void)parser;
    (void)path;
    (void)filename;
    return Status::NOT_IMPLEMENTED;
  }

  Status GenerateRootFile(const Parser& parser,
                          const std::string& path) override {
    (void)parser;
    (void)path;
    return Status::NOT_IMPLEMENTED;
  }
  bool IsSchemaOnly() const override { return true; }

  bool SupportsBfbsGeneration() const override { return false; }

  bool SupportsRootFileGeneration() const override { return false; }

  IDLOptions::Language Language() const override {
    return IDLOptions::kJsonSchema;
  }

  std::string LanguageName() const override { return "JsonSchema"; }
};
}  // namespace

std::unique_ptr<CodeGenerator> NewJsonSchemaCodeGenerator() {
  return std::unique_ptr<JsonSchemaCodeGenerator>(
      new JsonSchemaCodeGenerator());
}
}  // namespace flatbuffers
