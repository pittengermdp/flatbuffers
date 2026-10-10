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

template <class T>
static std::string GenTypeRef(const T* enum_def) {
  return "\"$ref\" : \"#/definitions/" + GenFullName(enum_def) + "\"";
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

// An optional scalar (`field: T = null`) has no default to fall back on, so
// the JSON parser reads a null value for it as "not present", exactly like
// omitting it. The schema admits null alongside the field's own type; the
// integer range keywords constrain only numbers, so they leave null alone.
static std::string GenOptionalScalarType(const Type& type) {
  if (type.enum_def != nullptr) {
    return "\"anyOf\" : [{ " + GenTypeRef(type.enum_def) + " }, { " +
           GenType(std::string("null")) + " }]";
  }
  const char* type_name = JsonTypeName(type.base_type);
  FLATBUFFERS_ASSERT(type_name != nullptr);  // the parser admits scalars only
  if (type_name == nullptr) {
    return "";
  }
  return "\"type\" : [\"" + std::string(type_name) + "\", \"null\"]" +
         GenIntegerRange(type.base_type);
}

static std::string GenBaseType(const Type& type) {
  if (type.struct_def != nullptr) {
    return GenTypeRef(type.struct_def);
  }
  if (type.enum_def != nullptr) {
    return GenTypeRef(type.enum_def);
  }
  return GenType(type.base_type);
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

// The schema of one union variant's value, or "" for NONE, which has none.
// Every other variant is a table or struct: string variants are an advanced
// union feature, which the parser rejects when generating JSON Schema.
static std::string GenUnionVariantType(const EnumVal& variant) {
  if (variant.union_type.base_type != BASE_TYPE_STRUCT) {
    return "";
  }
  return GenTypeRef(variant.union_type.struct_def);
}

// The schema of a union value without regard to its type field: any one of
// the variants. Pairing the value with the variant its type field names is a
// cross-member constraint, stated on the enclosing table instead.
static std::string GenUnionValueType(const EnumDef& union_def) {
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

static std::string GenArrayType(const Type& type) {
  std::string element_type;
  if (type.struct_def != nullptr) {
    element_type = GenTypeRef(type.struct_def);
  } else if (type.enum_def != nullptr) {
    element_type = GenTypeRef(type.enum_def);
  } else {
    element_type = GenType(type.element);
  }

  return "\"type\" : \"array\", \"items\" : {" + element_type + "}";
}

static std::string GenType(const Type& type) {
  switch (type.base_type) {
    case BASE_TYPE_ARRAY:
      FLATBUFFERS_FALLTHROUGH();  // fall thru
    case BASE_TYPE_VECTOR: {
      return GenArrayType(type);
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

}  // namespace

class JsonSchemaGenerator : public BaseGenerator {
 private:
  std::string code_;

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

  bool generate() {
    code_ = "";
    code_ += "{" + NewLine();
    code_ += Indent(1) +
             "\"$schema\": \"https://json-schema.org/draft/2019-09/schema\"," +
             NewLine();
    code_ += Indent(1) + "\"definitions\": {" + NewLine();
    // Collected and joined, so the last one carries no trailing comma whatever
    // kind it is -- a schema of enums alone used to end in one.
    std::vector<std::string> definitions;
    for (auto e = parser_.enums_.vec.cbegin(); e != parser_.enums_.vec.cend();
         ++e) {
      std::string definition =
          Indent(2) + "\"" + GenFullName(*e) + "\" : {" + NewLine();
      definition += Indent(3) + GenType("string") + "," + NewLine();
      auto enumdef(Indent(3) + "\"enum\": [");
      for (auto enum_value = (*e)->Vals().begin();
           enum_value != (*e)->Vals().end(); ++enum_value) {
        enumdef.append("\"" + (*enum_value)->name + "\"");
        if (*enum_value != (*e)->Vals().back()) {
          enumdef.append(", ");
        }
      }
      enumdef.append("]");
      definition += enumdef + NewLine();
      definition += Indent(2) + "}";  // close type
      definitions.push_back(definition);
    }
    for (auto s = parser_.structs_.vec.cbegin();
         s != parser_.structs_.vec.cend(); ++s) {
      const auto& structure = *s;
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
        std::string arrayInfo = "";
        if (IsArray(property->value.type)) {
          arrayInfo = "," + NewLine() + Indent(8) + "\"minItems\": " +
                      NumToString(property->value.type.fixed_length) + "," +
                      NewLine() + Indent(8) + "\"maxItems\": " +
                      NumToString(property->value.type.fixed_length);
        }
        std::string deprecated_info = "";
        if (property->deprecated) {
          deprecated_info =
              "," + NewLine() + Indent(8) + "\"deprecated\" : true";
        }
        std::string typeLine = Indent(4) + "\"" + property->name + "\"";
        typeLine += " : {" + NewLine() + Indent(8);
        typeLine += property->IsScalarOptional()
                        ? GenOptionalScalarType(property->value.type)
                        : GenType(property->value.type);
        typeLine += arrayInfo;
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
      code_ += Indent(1) + "\"$ref\" : \"#/definitions/" +
               GenFullName(parser_.root_struct_def_) + "\"" + NewLine();
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
