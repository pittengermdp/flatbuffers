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

// independent from idl_parser, since this code is not needed for most clients

#include "idl_gen_go.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <set>
#include <sstream>
#include <string>

#include "flatbuffers/base.h"
#include "flatbuffers/code_generators.h"
#include "flatbuffers/flatbuffers.h"
#include "flatbuffers/idl.h"
#include "flatbuffers/util.h"
#include "idl_namer.h"

#ifdef _WIN32
#include <direct.h>
#define PATH_SEPARATOR "\\"
#define mkdir(n, m) _mkdir(n)
#else
#include <sys/stat.h>
#define PATH_SEPARATOR "/"
#endif

namespace flatbuffers {

namespace go {

namespace {

// see https://golang.org/ref/spec#Keywords
static std::set<std::string> GoKeywords() {
  return {
      "break",    "default",     "func",   "interface", "select",
      "case",     "defer",       "go",     "map",       "struct",
      "chan",     "else",        "goto",   "package",   "switch",
      "const",    "fallthrough", "if",     "range",     "type",
      "continue", "for",         "import", "return",    "var",
  };
}

static Namer::Config GoDefaultConfig() {
  // Note that the functions with user defined types in the name use
  // upper camel case for all but the user defined type itself, which is keep
  // cased. Despite being a function, we interpret it as a Type.
  return {/*types=*/Case::kKeep,
          /*constants=*/Case::kUnknown,
          /*methods=*/Case::kUpperCamel,
          /*functions=*/Case::kUpperCamel,
          /*fields=*/Case::kUpperCamel,
          /*variables=*/Case::kLowerCamel,
          /*variants=*/Case::kKeep,
          /*enum_variant_seperator=*/"",  // I.e. Concatenate.
          /*escape_keywords=*/Namer::Config::Escape::AfterConvertingCase,
          /*namespaces=*/Case::kKeep,
          /*namespace_seperator=*/"__",
          /*object_prefix=*/"",
          /*object_suffix=*/"T",
          /*keyword_prefix=*/"",
          /*keyword_suffix=*/"_",
          /*keywords_casing=*/Namer::Config::KeywordsCasing::CaseSensitive,
          /*filenames=*/Case::kKeep,
          /*directories=*/Case::kKeep,
          /*output_path=*/"",
          /*filename_suffix=*/"",
          /*filename_extension=*/".go"};
}

}  // namespace

class GoGenerator : public BaseGenerator {
 public:
  GoGenerator(const Parser& parser, const std::string& path,
              const std::string& file_name, const std::string& go_namespace)
      : BaseGenerator(parser, path, file_name, "" /* not used*/,
                      "" /* not used */, "go"),
        cur_name_space_(nullptr),
        namer_(WithFlagOptions(GoDefaultConfig(), parser.opts, path),
               GoKeywords()) {
    std::istringstream iss(go_namespace);
    std::string component;
    while (std::getline(iss, component, '.')) {
      go_namespace_.components.push_back(component);
    }
  }

  bool generate() {
    std::string one_file_code;

    if (!generateEnums(&one_file_code)) return false;
    if (!generateStructs(&one_file_code)) return false;
    if (!json_error_.empty()) {
      std::cerr << "error: " << json_error_ << "\n";
      return false;
    }

    if (parser_.opts.one_file) {
      std::string code = "";
      const bool is_enum = !parser_.enums_.vec.empty();
      BeginFile(LastNamespacePart(go_namespace_), true, is_enum, &code);
      code += one_file_code;
      const std::string filename =
          GeneratedFileName(path_, file_name_, parser_.opts);
      if (!parser_.opts.file_saver->SaveFile(filename.c_str(), code, false))
        return false;
    }

    if (parser_.opts.binary_schema_gen_embed) {
      if (!generate_bfbs_embed()) return false;
    }

    return true;
  }

  // Generates a Go source file containing the binary schema (.bfbs) as an
  // embedded byte slice, enabling runtime reflection without file I/O.
  //
  // Output file: {Type}BinarySchema.go
  // Contents:
  //   var {Type}BfbsData = []byte{0xAB, 0xCD, ...}
  //   func {Type}BinarySchemaBytes() []byte { return {Type}BfbsData }
  bool generate_bfbs_embed() {
    if (!parser_.root_struct_def_) return true;  // nothing to embed

    auto& struct_def = *parser_.root_struct_def_;
    const std::string type_name = namer_.Type(struct_def);

    Namespace& ns = go_namespace_.components.empty()
                        ? *struct_def.defined_namespace
                        : go_namespace_;

    std::string code;
    code += "// Code generated by the FlatBuffers compiler. DO NOT EDIT.\n\n";
    code += "package " +
            (ns.components.empty() ? struct_def.name
                                   : LastNamespacePart(ns)) +
            "\n\n";

    auto binary_schema_hex_text =
        BufferToHexText(parser_.builder_.GetBufferPointer(),
                        parser_.builder_.GetSize(), 80, "\t", "");

    code += "// " + type_name +
            "BfbsData contains the embedded binary schema (.bfbs)\n";
    code += "// for runtime reflection without external file I/O.\n";
    code += "var " + type_name + "BfbsData = []byte{\n";
    code += binary_schema_hex_text;
    code += "}\n\n";

    code += "// " + type_name +
            "BinarySchemaBytes returns the embedded binary schema.\n";
    code += "func " + type_name + "BinarySchemaBytes() []byte {\n";
    code += "\treturn " + type_name + "BfbsData\n";
    code += "}\n";

    std::string directory = namer_.Directories(ns);
    EnsureDirExists(directory);
    std::string filename = directory + type_name + "BinarySchema.go";
    return parser_.opts.file_saver->SaveFile(filename.c_str(), code, false);
  }

 private:
  bool generateEnums(std::string* one_file_code) {
    bool needs_imports = false;
    for (auto it = parser_.enums_.vec.begin(); it != parser_.enums_.vec.end();
         ++it) {
      if (!parser_.opts.one_file) {
        needs_imports = false;
        ResetImports();
      }
      auto& enum_def = **it;
      std::string enumcode;
      GenEnum(enum_def, &enumcode);
      if (enum_def.is_union && parser_.opts.generate_object_based_api) {
        GenNativeUnionCreator(enum_def, &enumcode);
        needs_imports = true;
      }
      if (parser_.opts.one_file) {
        *one_file_code += enumcode;
      } else {
        if (!SaveType(enum_def, enumcode, needs_imports, true)) return false;
      }
    }
    return true;
  }

  void GenNativeUnionCreator(const EnumDef& enum_def, std::string* code_ptr) {
    if (enum_def.generated) return;

    GenNativeUnion(enum_def, code_ptr);
    GenNativeUnionPack(enum_def, code_ptr);
    GenNativeUnionUnPack(enum_def, code_ptr);
    GenUnionMatch(enum_def, code_ptr);
    if (parser_.opts.go_gen_json) GenNativeUnionJSON(enum_def, code_ptr);
  }

  bool generateStructs(std::string* one_file_code) {
    for (auto it = parser_.structs_.vec.begin();
         it != parser_.structs_.vec.end(); ++it) {
      if (!parser_.opts.one_file) {
        ResetImports();
      }
      std::string declcode;
      auto& struct_def = **it;
      GenStruct(struct_def, &declcode);
      if (parser_.opts.one_file) {
        *one_file_code += declcode;
      } else {
        if (!SaveType(struct_def, declcode, true, false)) return false;
      }
    }
    return true;
  }

  Namespace go_namespace_;
  Namespace* cur_name_space_;
  const IdlNamer namer_;

  struct NamespacePtrLess {
    bool operator()(const Definition* a, const Definition* b) const {
      return *a->defined_namespace < *b->defined_namespace;
    }
  };
  std::set<const Definition*, NamespacePtrLess> tracked_imported_namespaces_;
  bool needs_math_import_ = false;
  bool needs_bytes_import_ = false;
  bool needs_sort_import_ = false;

  // Every receiver, parameter and local the generated Go declares ends in an
  // underscore: rcv_, t_, builder_, o_, x_, <field>Offset_ and so on. Generated
  // bodies name imported packages -- flatbuffers, math, and each referenced
  // namespace, imported under its own name -- and the schema's own types, any
  // of which may be a single word such as x, o or t. A plain local, or a
  // parameter named after a field, would shadow one and break the build. Only
  // a schema name that itself ends in an underscore can still collide.
  //
  // A name derived from a field also puts a role between the field and the
  // underscore: <field>Arg_ for a builder function's parameter (FieldArg),
  // <field>Offset_, <field>Length_ and so on for a local. No field can then
  // produce builder_, the parameter declared beside every <field>Arg_, as a
  // field named builder would if its parameter were just <field>_.

  // The parameter a builder function -- <Table>Add<Field>, Create<Struct> --
  // takes for a field. See the naming rule above.
  std::string FieldArg(const FieldDef& field) {
    return namer_.Variable(field) + "Arg_";
  }

  // Most field accessors need to retrieve and test the field offset first,
  // this is the prefix code for that.
  std::string OffsetPrefix(const FieldDef& field) {
    return "{\n\to_ := flatbuffers.UOffsetT(rcv_._tab.Offset(" +
           NumToString(field.value.offset) + "))\n\tif o_ != 0 {\n";
  }

  // Begin a class declaration.
  void BeginClass(const StructDef& struct_def, std::string* code_ptr) {
    std::string& code = *code_ptr;

    code += "type " + namer_.Type(struct_def) + " struct {\n\t";

    // _ is reserved in flatbuffers field names, so no chance of name
    // conflict:
    code += "_tab ";
    code += struct_def.fixed ? "flatbuffers.Struct" : "flatbuffers.Table";
    code += "\n}\n\n";
  }

  // Construct the name of the type for this enum.
  std::string GetEnumTypeName(const EnumDef& enum_def) {
    return WrapInNameSpaceAndTrack(&enum_def, namer_.Type(enum_def));
  }

  // Create a type for the enum values.
  void GenEnumType(const EnumDef& enum_def, std::string* code_ptr) {
    std::string& code = *code_ptr;
    code += "type " + GetEnumTypeName(enum_def) + " ";
    code += GenTypeBasic(enum_def.underlying_type) + "\n\n";
  }

  // Begin enum code with a class declaration.
  void BeginEnum(std::string* code_ptr) {
    std::string& code = *code_ptr;
    code += "const (\n";
  }

  // A single enum member.
  void EnumMember(const EnumDef& enum_def, const EnumVal& ev,
                  size_t max_name_length, std::string* code_ptr) {
    std::string& code = *code_ptr;
    code += "\t";
    code += namer_.EnumVariant(enum_def, ev);
    code += " ";
    code += std::string(max_name_length - ev.name.length(), ' ');
    code += GetEnumTypeName(enum_def);
    code += " = ";
    code += enum_def.ToString(ev) + "\n";
  }

  // End enum code.
  void EndEnum(std::string* code_ptr) {
    std::string& code = *code_ptr;
    code += ")\n\n";
  }

  // Begin enum name map.
  void BeginEnumNames(const EnumDef& enum_def, std::string* code_ptr) {
    std::string& code = *code_ptr;
    code += "var EnumNames";
    code += enum_def.name;
    code += " = map[" + GetEnumTypeName(enum_def) + "]string{\n";
  }

  // A single enum name member.
  void EnumNameMember(const EnumDef& enum_def, const EnumVal& ev,
                      size_t max_name_length, std::string* code_ptr) {
    std::string& code = *code_ptr;
    code += "\t";
    code += namer_.EnumVariant(enum_def, ev);
    code += ": ";
    code += std::string(max_name_length - ev.name.length(), ' ');
    code += "\"";
    code += ev.name;
    code += "\",\n";
  }

  // End enum name map.
  void EndEnumNames(std::string* code_ptr) {
    std::string& code = *code_ptr;
    code += "}\n\n";
  }

  // Generate String() method on enum type.
  void EnumStringer(const EnumDef& enum_def, std::string* code_ptr) {
    std::string& code = *code_ptr;
    const std::string enum_type = namer_.Type(enum_def);
    code += "func (v_ " + enum_type + ") String() string {\n";
    code += "\tif s_, ok_ := EnumNames" + enum_type + "[v_]; ok_ {\n";
    code += "\t\treturn s_\n";
    code += "\t}\n";
    code += "\treturn \"" + enum_def.name;
    code += "(\" + strconv.FormatInt(int64(v_), 10) + \")\"\n";
    code += "}\n\n";
  }

  // Begin enum value map.
  void BeginEnumValues(const EnumDef& enum_def, std::string* code_ptr) {
    std::string& code = *code_ptr;
    code += "var EnumValues";
    code += namer_.Type(enum_def);
    code += " = map[string]" + GetEnumTypeName(enum_def) + "{\n";
  }

  // A single enum value member.
  void EnumValueMember(const EnumDef& enum_def, const EnumVal& ev,
                       size_t max_name_length, std::string* code_ptr) {
    std::string& code = *code_ptr;
    code += "\t\"";
    code += ev.name;
    code += "\": ";
    code += std::string(max_name_length - ev.name.length(), ' ');
    code += namer_.EnumVariant(enum_def, ev);
    code += ",\n";
  }

  // End enum value map.
  void EndEnumValues(std::string* code_ptr) {
    std::string& code = *code_ptr;
    code += "}\n\n";
  }

  // Initialize a new struct or table from existing data.
  void NewRootTypeFromBuffer(const StructDef& struct_def,
                             std::string* code_ptr) {
    std::string& code = *code_ptr;
    const std::string size_prefix[] = {"", "SizePrefixed"};
    const std::string struct_type = namer_.Type(struct_def);

    bool has_file_identifier = (parser_.root_struct_def_ == &struct_def) &&
                               parser_.file_identifier_.length();

    if (has_file_identifier) {
      code += "const " + struct_type + "Identifier = \"" +
              parser_.file_identifier_ + "\"\n\n";
    }

    for (int i = 0; i < 2; i++) {
      code += "func Get" + size_prefix[i] + "RootAs" + struct_type;
      code += "(buf_ []byte, offset_ flatbuffers.UOffsetT) ";
      code += "*" + struct_type + "";
      code += " {\n";
      if (i == 0) {
        code += "\tn_ := flatbuffers.GetUOffsetT(buf_[offset_:])\n";
      } else {
        code +=
            "\tn_ := "
            "flatbuffers.GetUOffsetT(buf_[offset_+flatbuffers.SizeUint32:])\n";
      }
      code += "\tx_ := &" + struct_type + "{}\n";
      if (i == 0) {
        code += "\tx_.Init(buf_, n_+offset_)\n";
      } else {
        code += "\tx_.Init(buf_, n_+offset_+flatbuffers.SizeUint32)\n";
      }
      code += "\treturn x_\n";
      code += "}\n\n";

      code += "func Finish" + size_prefix[i] + struct_type +
              "Buffer(builder_ *flatbuffers.Builder, offset_ "
              "flatbuffers.UOffsetT) {\n";
      if (has_file_identifier) {
        code += "\tidentifierBytes_ := []byte(" + struct_type + "Identifier)\n";
        code += "\tbuilder_.Finish" + size_prefix[i] +
                "WithFileIdentifier(offset_, identifierBytes_)\n";
      } else {
        code += "\tbuilder_.Finish" + size_prefix[i] + "(offset_)\n";
      }
      code += "}\n\n";

      if (has_file_identifier) {
        code += "func " + size_prefix[i] + struct_type +
                "BufferHasIdentifier(buf_ []byte) bool {\n";
        code += "\treturn flatbuffers." + size_prefix[i] +
                "BufferHasIdentifier(buf_, " + struct_type + "Identifier)\n";
        code += "}\n\n";
      }
    }
  }

  // Initialize an existing object with other data, to avoid an allocation.
  void InitializeExisting(const StructDef& struct_def, std::string* code_ptr) {
    std::string& code = *code_ptr;

    GenReceiver(struct_def, code_ptr);
    code += " Init(buf_ []byte, i_ flatbuffers.UOffsetT) ";
    code += "{\n";
    code += "\trcv_._tab.Bytes = buf_\n";
    code += "\trcv_._tab.Pos = i_\n";
    code += "}\n\n";
  }

  // Implement the table accessor
  void GenTableAccessor(const StructDef& struct_def, std::string* code_ptr) {
    std::string& code = *code_ptr;

    GenReceiver(struct_def, code_ptr);
    code += " Table() flatbuffers.Table ";
    code += "{\n";

    if (struct_def.fixed) {
      code += "\treturn rcv_._tab.Table\n";
    } else {
      code += "\treturn rcv_._tab\n";
    }
    code += "}\n\n";
  }

  // Get the length of a vector.
  void GetVectorLen(const StructDef& struct_def, const FieldDef& field,
                    std::string* code_ptr) {
    std::string& code = *code_ptr;

    GenReceiver(struct_def, code_ptr);
    code += " " + namer_.Function(field) + "Length(";
    code += ") int " + OffsetPrefix(field);
    code += "\t\treturn rcv_._tab.VectorLen(o_)\n\t}\n";
    code += "\treturn 0\n}\n\n";
  }

  // Get a [ubyte] vector as a byte slice.
  void GetUByteSlice(const StructDef& struct_def, const FieldDef& field,
                     std::string* code_ptr) {
    std::string& code = *code_ptr;

    GenReceiver(struct_def, code_ptr);
    code += " " + namer_.Function(field) + "Bytes(";
    code += ") []byte " + OffsetPrefix(field);
    code += "\t\treturn rcv_._tab.ByteVector(o_ + rcv_._tab.Pos)\n\t}\n";
    code += "\treturn nil\n}\n\n";
  }

  // Get the value of a struct's scalar.
  void GetScalarFieldOfStruct(const StructDef& struct_def,
                              const FieldDef& field, std::string* code_ptr) {
    std::string& code = *code_ptr;
    std::string getter = GenGetter(field.value.type);
    GenReceiver(struct_def, code_ptr);
    code += " " + namer_.Function(field);
    code += "() " + TypeName(field) + " {\n";
    code += "\treturn " +
            CastToEnum(field.value.type,
                       getter + "(rcv_._tab.Pos + flatbuffers.UOffsetT(" +
                           NumToString(field.value.offset) + "))");
    code += "\n}\n";
  }

  // Get the value of a table's scalar.
  void GetScalarFieldOfTable(const StructDef& struct_def, const FieldDef& field,
                             std::string* code_ptr) {
    std::string& code = *code_ptr;
    std::string getter = GenGetter(field.value.type);
    GenReceiver(struct_def, code_ptr);
    code += " " + namer_.Function(field);
    code += "() " + TypeName(field) + " ";
    code += OffsetPrefix(field);
    if (field.IsScalarOptional()) {
      code += "\t\tv_ := ";
    } else {
      code += "\t\treturn ";
    }
    code += CastToEnum(field.value.type, getter + "(o_ + rcv_._tab.Pos)");
    if (field.IsScalarOptional()) {
      code += "\n\t\treturn &v_";
    }
    code += "\n\t}\n";
    code += "\treturn " + GenConstant(field) + "\n";
    code += "}\n\n";
  }

  // Get a struct by initializing an existing struct.
  // Specific to Struct.
  void GetStructFieldOfStruct(const StructDef& struct_def,
                              const FieldDef& field, std::string* code_ptr) {
    std::string& code = *code_ptr;
    GenReceiver(struct_def, code_ptr);
    code += " " + namer_.Function(field);
    code += "(obj_ *" + TypeName(field);
    code += ") *" + TypeName(field);
    code += " {\n";
    code += "\tif obj_ == nil {\n";
    code += "\t\tobj_ = new(" + TypeName(field) + ")\n";
    code += "\t}\n";
    code += "\tobj_.Init(rcv_._tab.Bytes, rcv_._tab.Pos+";
    code += NumToString(field.value.offset) + ")";
    code += "\n\treturn obj_\n";
    code += "}\n";
  }

  // Get a struct by initializing an existing struct.
  // Specific to Table.
  void GetStructFieldOfTable(const StructDef& struct_def, const FieldDef& field,
                             std::string* code_ptr) {
    std::string& code = *code_ptr;
    GenReceiver(struct_def, code_ptr);
    code += " " + namer_.Function(field);
    code += "(obj_ *";
    code += TypeName(field);
    code += ") *" + TypeName(field) + " " + OffsetPrefix(field);
    if (field.value.type.struct_def->fixed) {
      code += "\t\tx_ := o_ + rcv_._tab.Pos\n";
    } else {
      code += "\t\tx_ := rcv_._tab.Indirect(o_ + rcv_._tab.Pos)\n";
    }
    code += "\t\tif obj_ == nil {\n";
    code += "\t\t\tobj_ = new(" + TypeName(field) + ")\n";
    code += "\t\t}\n";
    code += "\t\tobj_.Init(rcv_._tab.Bytes, x_)\n";
    code += "\t\treturn obj_\n\t}\n\treturn nil\n";
    code += "}\n\n";
  }

  // Get the value of a string.
  void GetStringField(const StructDef& struct_def, const FieldDef& field,
                      std::string* code_ptr) {
    std::string& code = *code_ptr;
    GenReceiver(struct_def, code_ptr);
    code += " " + namer_.Function(field);
    code += "() " + TypeName(field) + " ";
    code += OffsetPrefix(field) + "\t\treturn " + GenGetter(field.value.type);
    code += "(o_ + rcv_._tab.Pos)\n\t}\n\treturn nil\n";
    code += "}\n\n";
  }

  // Get the value of a union from an object.
  void GetUnionField(const StructDef& struct_def, const FieldDef& field,
                     std::string* code_ptr) {
    std::string& code = *code_ptr;
    GenReceiver(struct_def, code_ptr);
    code += " " + namer_.Function(field) + "(";
    code += "obj_ " + GenTypePointer(field.value.type) + ") bool ";
    code += OffsetPrefix(field);
    code += "\t\t" + GenGetter(field.value.type);
    code += "(obj_, o_)\n\t\treturn true\n\t}\n";
    code += "\treturn false\n";
    code += "}\n\n";
  }

  // Get the value of a vector's struct member.
  void GetMemberOfVectorOfStruct(const StructDef& struct_def,
                                 const FieldDef& field, std::string* code_ptr) {
    std::string& code = *code_ptr;
    auto vectortype = field.value.type.VectorType();

    GenReceiver(struct_def, code_ptr);
    code += " " + namer_.Function(field);
    code += "(obj_ *" + TypeName(field);
    code += ", j_ int) bool " + OffsetPrefix(field);
    code += "\t\tx_ := rcv_._tab.Vector(o_)\n";
    code += "\t\tx_ += flatbuffers.UOffsetT(j_) * ";
    code += NumToString(InlineSize(vectortype)) + "\n";
    if (!(vectortype.struct_def->fixed)) {
      code += "\t\tx_ = rcv_._tab.Indirect(x_)\n";
    }
    code += "\t\tobj_.Init(rcv_._tab.Bytes, x_)\n";
    code += "\t\treturn true\n\t}\n";
    code += "\treturn false\n";
    code += "}\n\n";
  }

  void GetMemberOfVectorOfStructByKey(const StructDef& struct_def,
                                      const FieldDef& field,
                                      std::string* code_ptr) {
    std::string& code = *code_ptr;
    auto vectortype = field.value.type.VectorType();
    FLATBUFFERS_ASSERT(vectortype.struct_def->has_key);

    auto& vector_struct_fields = vectortype.struct_def->fields.vec;
    auto kit =
        std::find_if(vector_struct_fields.begin(), vector_struct_fields.end(),
                     [&](FieldDef* vector_struct_field) {
                       return vector_struct_field->key;
                     });

    auto& key_field = **kit;
    FLATBUFFERS_ASSERT(key_field.key);

    GenReceiver(struct_def, code_ptr);
    code += " " + namer_.Field(field) + "ByKey";
    code += "(obj_ *" + TypeName(field);
    code += ", key_ " + NativeType(key_field.value.type) + ") bool " +
            OffsetPrefix(field);
    code += "\t\tx_ := rcv_._tab.Vector(o_)\n";
    code += "\t\treturn ";
    code += "obj_.LookupByKey(key_, x_, rcv_._tab.Bytes)\n";
    code += "\t}\n";
    code += "\treturn false\n";
    code += "}\n\n";
  }

  // Get the value of a vector's non-struct member.
  void GetMemberOfVectorOfNonStruct(const StructDef& struct_def,
                                    const FieldDef& field,
                                    std::string* code_ptr) {
    std::string& code = *code_ptr;
    auto vectortype = field.value.type.VectorType();

    GenReceiver(struct_def, code_ptr);
    code += " " + namer_.Function(field);
    code += "(j_ int) " + TypeName(field) + " ";
    code += OffsetPrefix(field);
    code += "\t\ta_ := rcv_._tab.Vector(o_)\n";
    code += "\t\treturn " +
            CastToEnum(field.value.type,
                       GenGetter(field.value.type) +
                           "(a_ + flatbuffers.UOffsetT(j_*" +
                           NumToString(InlineSize(vectortype)) + "))");
    code += "\n\t}\n";
    if (IsString(vectortype)) {
      code += "\treturn nil\n";
    } else if (vectortype.base_type == BASE_TYPE_BOOL) {
      code += "\treturn false\n";
    } else {
      code += "\treturn 0\n";
    }
    code += "}\n\n";
  }

  // Begin the creator function signature.
  void BeginBuilderArgs(const StructDef& struct_def, std::string* code_ptr) {
    std::string& code = *code_ptr;

    if (code.substr(code.length() - 2) != "\n\n") {
      // a previous mutate has not put an extra new line
      code += "\n";
    }
    code += "func Create" + struct_def.name;
    code += "(builder_ *flatbuffers.Builder";
  }

  // Recursively generate arguments for a constructor, to deal with nested
  // structs.
  void StructBuilderArgs(const StructDef& struct_def, const char* nameprefix,
                         std::string* code_ptr) {
    for (auto it = struct_def.fields.vec.begin();
         it != struct_def.fields.vec.end(); ++it) {
      auto& field = **it;
      if (IsStruct(field.value.type)) {
        // Generate arguments for a struct inside a struct. To ensure names
        // don't clash, and to make it obvious these arguments are constructing
        // a nested struct, prefix the name with the field name.
        StructBuilderArgs(*field.value.type.struct_def,
                          (nameprefix + (field.name + "_")).c_str(), code_ptr);
      } else {
        std::string& code = *code_ptr;
        code += std::string(", ") + nameprefix;
        code += FieldArg(field);
        code += " " + TypeName(field);
      }
    }
  }

  // End the creator function signature.
  void EndBuilderArgs(std::string* code_ptr) {
    std::string& code = *code_ptr;
    code += ") flatbuffers.UOffsetT {\n";
  }

  // Recursively generate struct construction statements and instert manual
  // padding.
  void StructBuilderBody(const StructDef& struct_def, const char* nameprefix,
                         std::string* code_ptr) {
    std::string& code = *code_ptr;
    code += "\tbuilder_.Prep(" + NumToString(struct_def.minalign) + ", ";
    code += NumToString(struct_def.bytesize) + ")\n";
    for (auto it = struct_def.fields.vec.rbegin();
         it != struct_def.fields.vec.rend(); ++it) {
      auto& field = **it;
      if (field.padding)
        code += "\tbuilder_.Pad(" + NumToString(field.padding) + ")\n";
      if (IsStruct(field.value.type)) {
        StructBuilderBody(*field.value.type.struct_def,
                          (nameprefix + (field.name + "_")).c_str(), code_ptr);
      } else {
        code += "\tbuilder_.Prepend" + GenMethod(field) + "(";
        code += CastToBaseType(field.value.type, nameprefix + FieldArg(field)) +
                ")\n";
      }
    }
  }

  void EndBuilderBody(std::string* code_ptr) {
    std::string& code = *code_ptr;
    code += "\treturn builder_.Offset()\n";
    code += "}\n";
  }

  // Get the value of a table's starting offset.
  void GetStartOfTable(const StructDef& struct_def, std::string* code_ptr) {
    std::string& code = *code_ptr;
    code += "func " + namer_.Type(struct_def) + "Start";
    code += "(builder_ *flatbuffers.Builder) {\n";
    code += "\tbuilder_.StartObject(";
    code += NumToString(struct_def.fields.vec.size());
    code += ")\n}\n";
  }

  // Set the value of a table's field.
  void BuildFieldOfTable(const StructDef& struct_def, const FieldDef& field,
                         const size_t offset, std::string* code_ptr) {
    std::string& code = *code_ptr;
    const std::string field_var = FieldArg(field);
    code += "func " + namer_.Type(struct_def) + "Add" + namer_.Function(field);
    code += "(builder_ *flatbuffers.Builder, ";
    code += field_var + " ";
    if (!IsScalar(field.value.type.base_type) && (!struct_def.fixed)) {
      code += "flatbuffers.UOffsetT";
    } else {
      code += GenTypeGet(field.value.type);
    }
    code += ") {\n\t";
    code += "builder_.Prepend";
    code += GenMethod(field);
    if (field.IsScalarOptional()) {
      code += "(";
    } else {
      code += "Slot(" + NumToString(offset) + ", ";
    }
    if (!IsScalar(field.value.type.base_type) && (!struct_def.fixed)) {
      code += "flatbuffers.UOffsetT";
      code += "(" + field_var + ")";
    } else {
      code += CastToBaseType(field.value.type, field_var);
    }
    if (field.IsScalarOptional()) {
      code += ")\n";
      code += "\tbuilder_.Slot(" + NumToString(offset);
    } else {
      code += ", " + GenConstant(field);
    }
    code += ")\n";
    code += "}\n";
  }

  // Set the value of one of the members of a table's vector.
  void BuildVectorOfTable(const StructDef& struct_def, const FieldDef& field,
                          std::string* code_ptr) {
    std::string& code = *code_ptr;
    code += "func " + namer_.Type(struct_def) + "Start";
    code += namer_.Function(field);
    code += "Vector(builder_ *flatbuffers.Builder, numElems_ int) ";
    code += "flatbuffers.UOffsetT {\n\treturn builder_.StartVector(";
    auto vector_type = field.value.type.VectorType();
    auto alignment = InlineAlignment(vector_type);
    auto elem_size = InlineSize(vector_type);
    code += NumToString(elem_size);
    code += ", numElems_, " + NumToString(alignment);
    code += ")\n}\n";
  }

  // Get the offset of the end of a table.
  void GetEndOffsetOnTable(const StructDef& struct_def, std::string* code_ptr) {
    std::string& code = *code_ptr;
    code += "func " + namer_.Type(struct_def) + "End";
    code += "(builder_ *flatbuffers.Builder) flatbuffers.UOffsetT ";
    code += "{\n\treturn builder_.EndObject()\n}\n";
  }

  // Generate the receiver for function signatures.
  void GenReceiver(const StructDef& struct_def, std::string* code_ptr) {
    std::string& code = *code_ptr;
    code += "func (rcv_ *" + namer_.Type(struct_def) + ")";
  }

  // Generate a struct field getter, conditioned on its child type(s).
  void GenStructAccessor(const StructDef& struct_def, const FieldDef& field,
                         std::string* code_ptr) {
    GenComment(field.doc_comment, code_ptr, nullptr, "");
    if (IsScalar(field.value.type.base_type)) {
      if (struct_def.fixed) {
        GetScalarFieldOfStruct(struct_def, field, code_ptr);
      } else {
        GetScalarFieldOfTable(struct_def, field, code_ptr);
      }
    } else {
      switch (field.value.type.base_type) {
        case BASE_TYPE_STRUCT:
          if (struct_def.fixed) {
            GetStructFieldOfStruct(struct_def, field, code_ptr);
          } else {
            GetStructFieldOfTable(struct_def, field, code_ptr);
          }
          break;
        case BASE_TYPE_STRING:
          GetStringField(struct_def, field, code_ptr);
          break;
        case BASE_TYPE_VECTOR: {
          auto vectortype = field.value.type.VectorType();
          if (vectortype.base_type == BASE_TYPE_STRUCT) {
            GetMemberOfVectorOfStruct(struct_def, field, code_ptr);
            // TODO(michaeltle): Support querying fixed struct by key.
            // Currently, we only support keyed tables.
            if (!vectortype.struct_def->fixed &&
                vectortype.struct_def->has_key) {
              GetMemberOfVectorOfStructByKey(struct_def, field, code_ptr);
            }
          } else {
            GetMemberOfVectorOfNonStruct(struct_def, field, code_ptr);
          }
          break;
        }
        case BASE_TYPE_UNION:
          GetUnionField(struct_def, field, code_ptr);
          break;
        default:
          FLATBUFFERS_ASSERT(0);
      }
    }
    if (IsVector(field.value.type)) {
      GetVectorLen(struct_def, field, code_ptr);
      if (field.value.type.element == BASE_TYPE_UCHAR) {
        GetUByteSlice(struct_def, field, code_ptr);
      }
    }
  }

  // Mutate the value of a struct's scalar.
  void MutateScalarFieldOfStruct(const StructDef& struct_def,
                                 const FieldDef& field, std::string* code_ptr) {
    std::string& code = *code_ptr;
    std::string setter =
        "rcv_._tab.Mutate" + namer_.Method(GenTypeBasic(field.value.type));
    GenReceiver(struct_def, code_ptr);
    code += " Mutate" + namer_.Function(field);
    code +=
        "(n_ " + GenTypeGet(field.value.type) + ") bool {\n\treturn " + setter;
    code += "(rcv_._tab.Pos+flatbuffers.UOffsetT(";
    code += NumToString(field.value.offset) + "), ";
    code += CastToBaseType(field.value.type, "n_") + ")\n}\n\n";
  }

  // Mutate the value of a table's scalar.
  void MutateScalarFieldOfTable(const StructDef& struct_def,
                                const FieldDef& field, std::string* code_ptr) {
    std::string& code = *code_ptr;
    std::string setter = "rcv_._tab.Mutate" +
                         namer_.Method(GenTypeBasic(field.value.type)) + "Slot";
    GenReceiver(struct_def, code_ptr);
    code += " Mutate" + namer_.Function(field);
    code += "(n_ " + GenTypeGet(field.value.type) + ") bool {\n\treturn ";
    code += setter + "(" + NumToString(field.value.offset) + ", ";
    code += CastToBaseType(field.value.type, "n_") + ")\n";
    code += "}\n\n";
  }

  // Mutate an element of a vector of scalars.
  void MutateElementOfVectorOfNonStruct(const StructDef& struct_def,
                                        const FieldDef& field,
                                        std::string* code_ptr) {
    std::string& code = *code_ptr;
    auto vectortype = field.value.type.VectorType();
    std::string setter =
        "rcv_._tab.Mutate" + namer_.Method(GenTypeBasic(vectortype));
    GenReceiver(struct_def, code_ptr);
    code += " Mutate" + namer_.Function(field);
    code += "(j_ int, n_ " + TypeName(field) + ") bool ";
    code += OffsetPrefix(field);
    code += "\t\ta_ := rcv_._tab.Vector(o_)\n";
    code += "\t\treturn " + setter + "(";
    code += "a_+flatbuffers.UOffsetT(j_*";
    code += NumToString(InlineSize(vectortype)) + "), ";
    code += CastToBaseType(vectortype, "n_") + ")\n";
    code += "\t}\n";
    code += "\treturn false\n";
    code += "}\n\n";
  }

  // Generate a struct field setter, conditioned on its child type(s).
  void GenStructMutator(const StructDef& struct_def, const FieldDef& field,
                        std::string* code_ptr) {
    GenComment(field.doc_comment, code_ptr, nullptr, "");
    if (IsScalar(field.value.type.base_type)) {
      if (struct_def.fixed) {
        MutateScalarFieldOfStruct(struct_def, field, code_ptr);
      } else {
        MutateScalarFieldOfTable(struct_def, field, code_ptr);
      }
    } else if (IsVector(field.value.type)) {
      if (IsScalar(field.value.type.element)) {
        MutateElementOfVectorOfNonStruct(struct_def, field, code_ptr);
      }
    }
  }

  // Generate the root verifier entry point and per-table verifier function.
  // Only called for non-fixed (table) types.
  void GenVerifier(const StructDef& struct_def, std::string* code_ptr) {
    std::string& code = *code_ptr;
    const std::string struct_type = namer_.Type(struct_def);

    // Public entry point: VerifyRootAs{Type}
    code += "func VerifyRootAs" + struct_type +
            "(buf_ []byte, opts_ *flatbuffers.VerifierOptions) error {\n";
    code += "\tv_ := flatbuffers.NewVerifier(buf_, opts_)\n";
    code += "\ttablePos_, err_ := v_.CheckUOffsetT(0)\n";
    code += "\tif err_ != nil {\n";
    code += "\t\treturn err_\n";
    code += "\t}\n";
    code += "\treturn verify" + struct_type + "(v_, int(tablePos_))\n";
    code += "}\n\n";

    // Exported per-table verifier for cross-package callers: Verify{Type}
    code += "func Verify" + struct_type +
            "(v_ *flatbuffers.Verifier, tablePos_ int) error {\n";
    code += "\treturn verify" + struct_type + "(v_, tablePos_)\n";
    code += "}\n\n";

    // Unexported per-table verifier: verify{Type}
    code += "func verify" + struct_type +
            "(v_ *flatbuffers.Verifier, tablePos_ int) error {\n";
    code += "\tif err_ := v_.CheckTable(tablePos_); err_ != nil {\n";
    code += "\t\treturn err_\n";
    code += "\t}\n";
    code += "\tif err_ := v_.CountTable(); err_ != nil {\n";
    code += "\t\treturn err_\n";
    code += "\t}\n";
    code += "\tif err_ := v_.PushDepth(); err_ != nil {\n";
    code += "\t\treturn err_\n";
    code += "\t}\n";
    code += "\tdefer v_.PopDepth()\n\n";

    for (auto it = struct_def.fields.vec.begin();
         it != struct_def.fields.vec.end(); ++it) {
      const FieldDef& field = **it;
      if (field.deprecated) continue;

      const std::string voffset = NumToString(field.value.offset);
      const std::string field_name = namer_.Field(field);

      if (IsScalar(field.value.type.base_type)) {
        // Scalar field (includes union type discriminant enum fields).
        const std::string scalar_size =
            NumToString(SizeOf(field.value.type.base_type));
        code += "\tif err_ := v_.CheckScalarField(tablePos_, " + voffset +
                ", " + scalar_size + "); err_ != nil {\n";
        code += "\t\treturn err_\n";
        code += "\t}\n";
      } else {
        switch (field.value.type.base_type) {
          case BASE_TYPE_STRING: {
            code += "\tif pos_, err_ := v_.CheckOffsetField(tablePos_, " +
                    voffset + "); err_ != nil {\n";
            code += "\t\treturn err_\n";
            code += "\t} else if pos_ != 0 {\n";
            code += "\t\tif err_ := v_.CheckString(pos_); err_ != nil {\n";
            code += "\t\t\treturn err_\n";
            code += "\t\t}\n";
            code += "\t}\n";
            break;
          }
          case BASE_TYPE_STRUCT: {
            if (field.value.type.struct_def->fixed) {
              // Inline fixed struct — treat like a scalar block.
              const std::string struct_size =
                  NumToString(field.value.type.struct_def->bytesize);
              code += "\tif err_ := v_.CheckScalarField(tablePos_, " + voffset +
                      ", " + struct_size + "); err_ != nil {\n";
              code += "\t\treturn err_\n";
              code += "\t}\n";
            } else {
              // Nested table.
              const std::string verify_call =
                  VerifyFuncForTable(field.value.type.struct_def);
              code += "\tif pos_, err_ := v_.CheckOffsetField(tablePos_, " +
                      voffset + "); err_ != nil {\n";
              code += "\t\treturn err_\n";
              code += "\t} else if pos_ != 0 {\n";
              code += "\t\tif err_ := " + verify_call +
                      "(v_, pos_); err_ != nil {\n";
              code += "\t\t\treturn err_\n";
              code += "\t\t}\n";
              code += "\t}\n";
            }
            break;
          }
          case BASE_TYPE_VECTOR: {
            const Type vector_type = field.value.type.VectorType();
            code += "\tif pos_, err_ := v_.CheckOffsetField(tablePos_, " +
                    voffset + "); err_ != nil {\n";
            code += "\t\treturn err_\n";
            code += "\t} else if pos_ != 0 {\n";
            if (IsScalar(vector_type.base_type)) {
              const std::string elem_size =
                  NumToString(InlineSize(vector_type));
              code += "\t\tif _, err_ := v_.CheckVector(pos_, " + elem_size +
                      "); err_ != nil {\n";
              code += "\t\t\treturn err_\n";
              code += "\t\t}\n";
            } else if (vector_type.base_type == BASE_TYPE_STRING) {
              // Vector of strings: verify vector bounds AND each string.
              code +=
                  "\t\tif err_ := v_.CheckVectorOfStrings(pos_); err_ != nil "
                  "{\n";
              code += "\t\t\treturn err_\n";
              code += "\t\t}\n";
            } else if (vector_type.base_type == BASE_TYPE_STRUCT) {
              if (vector_type.struct_def->fixed) {
                // Vector of fixed structs — inline, each element has known
                // size.
                const std::string elem_size =
                    NumToString(vector_type.struct_def->bytesize);
                code += "\t\tif _, err_ := v_.CheckVector(pos_, " + elem_size +
                        "); err_ != nil {\n";
                code += "\t\t\treturn err_\n";
                code += "\t\t}\n";
              } else {
                // Vector of tables.
                const std::string verify_call =
                    VerifyFuncForTable(vector_type.struct_def);
                code += "\t\tif err_ := v_.CheckVectorOfTables(pos_, " +
                        verify_call + "); err_ != nil {\n";
                code += "\t\t\treturn err_\n";
                code += "\t\t}\n";
              }
            } else {
              // Fallback: treat as opaque scalar elements of size 1.
              code +=
                  "\t\tif _, err_ := v_.CheckVector(pos_, 1); err_ != nil {\n";
              code += "\t\t\treturn err_\n";
              code += "\t\t}\n";
            }
            code += "\t}\n";
            break;
          }
          case BASE_TYPE_UNION: {
            // Find THIS field's companion _type field. It must be looked up by
            // field name, not by union type: a table may hold several fields
            // of one union type, each with its own discriminant slot.
            const EnumDef& union_enum = *field.value.type.enum_def;
            const FieldDef* type_field =
                struct_def.fields.Lookup(field.name + UnionTypeFieldSuffix());
            FLATBUFFERS_ASSERT(type_field);  // Guaranteed by ParseField().
            const std::string type_voffset =
                NumToString(type_field->value.offset);
            // Verify type and value are both present or both absent.
            code += "\tif err_ := v_.CheckUnionConsistency(tablePos_, " +
                    type_voffset + ", " + voffset + ", \"" + field.name +
                    "\"); err_ != nil {\n";
            code += "\t\treturn err_\n";
            code += "\t}\n";
            // Follow the value offset to get the union data position.
            code += "\tif pos_, err_ := v_.CheckOffsetField(tablePos_, " +
                    voffset + "); err_ != nil {\n";
            code += "\t\treturn err_\n";
            code += "\t} else if pos_ != 0 {\n";
            // Read the type discriminant byte.
            code += "\t\tuType_, err_ := v_.ReadFieldByte(tablePos_, " +
                    type_voffset + ")\n";
            code += "\t\tif err_ != nil {\n";
            code += "\t\t\treturn err_\n";
            code += "\t\t}\n";
            // Dispatch per-variant verification.
            const std::string enum_type = GetEnumTypeName(union_enum);
            code += "\t\tswitch " + enum_type + "(uType_) {\n";
            for (auto vit = union_enum.Vals().begin();
                 vit != union_enum.Vals().end(); ++vit) {
              const EnumVal& ev = **vit;
              if (ev.IsZero()) continue;           // Skip NONE
              if (ev.union_type.struct_def == nullptr) continue;
              const std::string variant_name =
                  WrapInNameSpaceAndTrack(&union_enum, namer_.EnumVariant(union_enum, ev));
              if (ev.union_type.struct_def->fixed) {
                // Fixed struct variant: check byte range.
                const std::string struct_size =
                    NumToString(ev.union_type.struct_def->bytesize);
                code += "\t\tcase " + variant_name + ":\n";
                code += "\t\t\tif err_ := v_.CheckRange(pos_, " + struct_size +
                        "); err_ != nil {\n";
                code += "\t\t\t\treturn err_\n";
                code += "\t\t\t}\n";
              } else {
                // Table variant: recurse into per-table verify.
                const std::string verify_call =
                    VerifyFuncForTable(ev.union_type.struct_def);
                code += "\t\tcase " + variant_name + ":\n";
                code += "\t\t\tif err_ := " + verify_call +
                        "(v_, pos_); err_ != nil {\n";
                code += "\t\t\t\treturn err_\n";
                code += "\t\t\t}\n";
              }
            }
            // Unknown discriminant — tolerate for forward compatibility.
            code += "\t\t}\n";  // end switch
            code += "\t}\n";    // end if pos != 0
            break;
          }
          default:
            break;
        }
      }

      // Required field check (emitted after the content check for the field).
      if (field.IsRequired()) {
        code += "\tif err_ := v_.CheckRequiredField(tablePos_, " + voffset +
                ", \"" + field.name + "\"); err_ != nil {\n";
        code += "\t\treturn err_\n";
        code += "\t}\n";
      }
    }

    code += "\treturn nil\n";
    code += "}\n\n";
  }

  // Generate table constructors, conditioned on its members' types.
  void GenTableBuilders(const StructDef& struct_def, std::string* code_ptr) {
    GetStartOfTable(struct_def, code_ptr);

    for (auto it = struct_def.fields.vec.begin();
         it != struct_def.fields.vec.end(); ++it) {
      auto& field = **it;
      if (field.deprecated) continue;

      auto offset = it - struct_def.fields.vec.begin();
      BuildFieldOfTable(struct_def, field, offset, code_ptr);
      if (IsVector(field.value.type)) {
        BuildVectorOfTable(struct_def, field, code_ptr);
      }
    }

    GetEndOffsetOnTable(struct_def, code_ptr);
  }

  // Generate struct or table methods.
  void GenStruct(const StructDef& struct_def, std::string* code_ptr) {
    if (struct_def.generated) return;

    cur_name_space_ = struct_def.defined_namespace;

    GenComment(struct_def.doc_comment, code_ptr, nullptr);
    if (parser_.opts.generate_object_based_api) {
      GenNativeStruct(struct_def, code_ptr);
    }
    BeginClass(struct_def, code_ptr);
    if (!struct_def.fixed) {
      // Generate a special accessor for the table that has been declared as
      // the root type.
      NewRootTypeFromBuffer(struct_def, code_ptr);
      // Generate the root verifier entry point and per-table verifier.
      GenVerifier(struct_def, code_ptr);
    }
    // Generate the Init method that sets the field in a pre-existing
    // accessor object. This is to allow object reuse.
    InitializeExisting(struct_def, code_ptr);
    // Generate _tab accessor
    GenTableAccessor(struct_def, code_ptr);

    // Generate struct fields accessors
    for (auto it = struct_def.fields.vec.begin();
         it != struct_def.fields.vec.end(); ++it) {
      auto& field = **it;
      if (field.deprecated) continue;

      GenStructAccessor(struct_def, field, code_ptr);
      GenStructMutator(struct_def, field, code_ptr);
      // TODO(michaeltle): Support querying fixed struct by key. Currently,
      // we only support keyed tables.
      if (!struct_def.fixed && field.key) {
        GenKeyCompare(struct_def, field, code_ptr);
        GenLookupByKey(struct_def, field, code_ptr);
      }
    }

    // Generate builders
    if (struct_def.fixed) {
      // create a struct constructor function
      GenStructBuilder(struct_def, code_ptr);
    } else {
      // Create a set of functions that allow table construction.
      GenTableBuilders(struct_def, code_ptr);
    }
  }

  void GenKeyCompare(const StructDef& struct_def, const FieldDef& field,
                     std::string* code_ptr) {
    FLATBUFFERS_ASSERT(struct_def.has_key);
    FLATBUFFERS_ASSERT(field.key);
    std::string& code = *code_ptr;

    code += "func " + namer_.Type(struct_def) + "KeyCompare(";
    code += "o1_, o2_ flatbuffers.UOffsetT, buf_ []byte) bool {\n";
    // Stack-allocate the two accessors (no pointer escapes this function) to
    // avoid two heap allocations on every comparator call during a sort.
    code += "\tvar obj1_, obj2_ " + namer_.Type(struct_def) + "\n";
    code += "\tobj1_.Init(buf_, flatbuffers.UOffsetT(len(buf_))-o1_)\n";
    code += "\tobj2_.Init(buf_, flatbuffers.UOffsetT(len(buf_))-o2_)\n";
    if (IsString(field.value.type)) {
      code += "\treturn string(obj1_." + namer_.Function(field.name) + "()) < ";
      code += "string(obj2_." + namer_.Function(field.name) + "())\n";
    } else {
      code += "\treturn " +
              GoKeyLess(field.value.type,
                        "obj1_." + namer_.Function(field.name) + "()",
                        "obj2_." + namer_.Function(field.name) + "()") +
              "\n";
    }
    code += "}\n\n";
  }

  void GenLookupByKey(const StructDef& struct_def, const FieldDef& field,
                      std::string* code_ptr) {
    FLATBUFFERS_ASSERT(struct_def.has_key);
    FLATBUFFERS_ASSERT(field.key);
    std::string& code = *code_ptr;

    GenReceiver(struct_def, code_ptr);
    code += " LookupByKey(";
    code += "key_ " + NativeType(field.value.type) + ", ";
    code += "vectorLocation_ flatbuffers.UOffsetT, ";
    code += "buf_ []byte) bool {\n";
    code += "\tspan_ := flatbuffers.GetUOffsetT(buf_[vectorLocation_-4:])\n";
    code += "\tstart_ := flatbuffers.UOffsetT(0)\n";
    if (IsString(field.value.type)) {
      code += "\tbKey_ := []byte(key_)\n";
    }
    code += "\tfor span_ != 0 {\n";
    code += "\t\tmiddle_ := span_ / 2\n";
    code += "\t\ttableOffset_ := flatbuffers.GetIndirectOffset(buf_, ";
    code += "vectorLocation_+4*(start_+middle_))\n";

    // Stack-allocate the accessor (no pointer escapes the loop) to avoid a heap
    // allocation on every step of the O(log n) binary search.
    code += "\t\tvar obj_ " + namer_.Type(struct_def) + "\n";
    code += "\t\tobj_.Init(buf_, tableOffset_)\n";

    if (IsString(field.value.type)) {
      needs_bytes_import_ = true;
      code += "\t\tcomp_ := bytes.Compare(obj_." + namer_.Function(field.name) +
              "()";
      code += ", bKey_)\n";
    } else {
      code += "\t\tval_ := obj_." + namer_.Function(field.name) + "()\n";
      code += "\t\tcomp_ := 0\n";
      code += "\t\tif " +
              (field.value.type.base_type == BASE_TYPE_BOOL
                   ? GoKeyLess(field.value.type, "key_", "val_")
                   : std::string("val_ > key_")) +
              " {\n";
      code += "\t\t\tcomp_ = 1\n";
      code += "\t\t} else if " + GoKeyLess(field.value.type, "val_", "key_") +
              " {\n";
      code += "\t\t\tcomp_ = -1\n";
      code += "\t\t}\n";
    }
    code += "\t\tif comp_ > 0 {\n";
    code += "\t\t\tspan_ = middle_\n";
    code += "\t\t} else if comp_ < 0 {\n";
    code += "\t\t\tmiddle_ += 1\n";
    code += "\t\t\tstart_ += middle_\n";
    code += "\t\t\tspan_ -= middle_\n";
    code += "\t\t} else {\n";
    code += "\t\t\trcv_.Init(buf_, tableOffset_)\n";
    code += "\t\t\treturn true\n";
    code += "\t\t}\n";
    code += "\t}\n";
    code += "\treturn false\n";
    code += "}\n\n";
  }

  // Returns true if this vector field represents a map<K,V>.
  static bool IsMapField(const FieldDef& field) {
    return field.attributes.Lookup("map_entry") != nullptr;
  }

  // Returns true if this vector field represents a set<V>.
  static bool IsSetField(const FieldDef& field) {
    return field.attributes.Lookup("set_entry") != nullptr;
  }

  // Returns the key FieldDef in an auto-generated map/set entry StructDef.
  static const FieldDef *GetEntryKeyField(const StructDef& entry) {
    for (auto *f : entry.fields.vec) {
      if (f->key) return f;
    }
    return nullptr;
  }

  // Returns the value FieldDef in an auto-generated map entry StructDef.
  static const FieldDef *GetEntryValueField(const StructDef& entry) {
    return entry.fields.Lookup("value");
  }

  // Maps a FlatBuffers scalar/string type to the appropriate Go map-key type
  // string. Enums use their generated Go type name.
  std::string GoKeyType(const Type& type) {
    if (IsString(type)) return "string";
    if (type.enum_def) { return GetEnumTypeName(*type.enum_def); }
    switch (type.base_type) {
      case BASE_TYPE_BOOL:   return "bool";
      case BASE_TYPE_CHAR:   return "int8";
      case BASE_TYPE_UCHAR:  return "byte";
      case BASE_TYPE_SHORT:  return "int16";
      case BASE_TYPE_USHORT: return "uint16";
      case BASE_TYPE_INT:    return "int32";
      case BASE_TYPE_UINT:   return "uint32";
      case BASE_TYPE_LONG:   return "int64";
      case BASE_TYPE_ULONG:  return "uint64";
      default:               return "int32";
    }
  }

  // A Go expression ordering two map/set keys (or key fields) a and b the way
  // FlatBuffers sorts a keyed vector. Go has no < on bool: false sorts first.
  static std::string GoKeyLess(const Type& type, const std::string& a,
                               const std::string& b) {
    if (type.base_type == BASE_TYPE_BOOL) return "!" + a + " && " + b;
    return a + " < " + b;
  }

  void GenNativeStruct(const StructDef& struct_def, std::string* code_ptr) {
    std::string& code = *code_ptr;

    code += "type " + NativeName(struct_def) + " struct {\n";
    for (auto it = struct_def.fields.vec.begin();
         it != struct_def.fields.vec.end(); ++it) {
      const FieldDef& field = **it;
      if (field.deprecated) continue;
      if (IsScalar(field.value.type.base_type) &&
          field.value.type.enum_def != nullptr &&
          field.value.type.enum_def->is_union)
        continue;
      // Map/set fields stored as vectors with auto-generated entry structs.
      if (IsVector(field.value.type) && IsMapField(field)) {
        const auto& entry = *field.value.type.struct_def;
        const auto *kf = GetEntryKeyField(entry);
        const auto *vf = GetEntryValueField(entry);
        const std::string key_go = GoKeyType(kf->value.type);
        const std::string val_go = NativeType(vf->value.type);
        code += "\t" + namer_.Field(field) + " map[" + key_go + "]" + val_go +
                " `json:\"" + field.name + ",omitempty\"`\n";
        continue;
      } else if (IsVector(field.value.type) && IsSetField(field)) {
        const auto& entry = *field.value.type.struct_def;
        const auto *kf = GetEntryKeyField(entry);
        const std::string key_go = GoKeyType(kf->value.type);
        code += "\t" + namer_.Field(field) + " map[" + key_go + "]struct{}" +
                " `json:\"" + field.name + ",omitempty\"`\n";
        continue;
      }
      code += "\t" + namer_.Field(field) + " ";
      if (field.IsScalarOptional()) {
        code += "*";
      }
      code += NativeType(field.value.type) + " `json:\"" + field.name +
              ",omitempty\"`" + "\n";
    }
    code += "}\n\n";

    if (!struct_def.fixed) {
      GenNativeTablePack(struct_def, code_ptr);
      GenNativeTableUnPack(struct_def, code_ptr);
      GenSelectiveUnpack(struct_def, code_ptr);
    } else {
      GenNativeStructPack(struct_def, code_ptr);
      GenNativeStructUnPack(struct_def, code_ptr);
    }
    if (parser_.opts.go_gen_json) GenNativeJSON(struct_def, code_ptr);
  }

  void GenNativeUnion(const EnumDef& enum_def, std::string* code_ptr) {
    if (enum_def.generated) return;

    std::string& code = *code_ptr;
    code += "type " + NativeName(enum_def) + " struct {\n";
    code += "\tType " + namer_.Type(enum_def) + "\n";
    code += "\tValue interface{}\n";
    code += "}\n\n";
  }

  void GenNativeUnionPack(const EnumDef& enum_def, std::string* code_ptr) {
    if (enum_def.generated) return;

    std::string& code = *code_ptr;
    code += "func (t_ *" + NativeName(enum_def) +
            ") Pack(builder_ *flatbuffers.Builder) flatbuffers.UOffsetT {\n";
    code += "\tif t_ == nil {\n\t\treturn 0\n\t}\n";

    code += "\tswitch t_.Type {\n";
    for (auto it2 = enum_def.Vals().begin(); it2 != enum_def.Vals().end();
         ++it2) {
      const EnumVal& ev = **it2;
      if (ev.IsZero()) continue;
      code += "\tcase " + namer_.EnumVariant(enum_def, ev) + ":\n";
      code += "\t\treturn t_.Value.(" + NativeType(ev.union_type) +
              ").Pack(builder_)\n";
    }
    code += "\t}\n";
    code += "\treturn 0\n";
    code += "}\n\n";
  }

  void GenNativeUnionUnPack(const EnumDef& enum_def, std::string* code_ptr) {
    if (enum_def.generated) return;

    std::string& code = *code_ptr;

    code += "func (rcv_ " + namer_.Type(enum_def) +
            ") UnPack(table_ flatbuffers.Table) *" + NativeName(enum_def) +
            " {\n";
    code += "\tswitch rcv_ {\n";

    for (auto it2 = enum_def.Vals().begin(); it2 != enum_def.Vals().end();
         ++it2) {
      const EnumVal& ev = **it2;
      if (ev.IsZero()) continue;
      code += "\tcase " + namer_.EnumVariant(enum_def, ev) + ":\n";
      code += "\t\tvar x_ " +
              WrapInNameSpaceAndTrack(ev.union_type.struct_def,
                                      ev.union_type.struct_def->name) +
              "\n";
      code += "\t\tx_.Init(table_.Bytes, table_.Pos)\n";

      code += "\t\treturn &" +
              WrapInNameSpaceAndTrack(&enum_def, NativeName(enum_def)) +
              "{Type: " + namer_.EnumVariant(enum_def, ev) +
              ", Value: x_.UnPack()}\n";
    }
    code += "\t}\n";
    code += "\treturn nil\n";
    code += "}\n\n";
  }

  void GenUnionMatch(const EnumDef& enum_def, std::string* code_ptr) {
    if (enum_def.generated) return;

    std::string& code = *code_ptr;
    const std::string native_name = NativeName(enum_def);

    code += "// Match dispatches to the appropriate callback based on the union variant.\n";
    code += "// Each callback receives the correctly typed value. If the variant is NONE\n";
    code += "// or unrecognized, no callback is invoked.\n";
    code += "func (t_ *" + native_name + ") Match(\n";
    for (auto it2 = enum_def.Vals().begin(); it2 != enum_def.Vals().end();
         ++it2) {
      const EnumVal& ev = **it2;
      if (ev.IsZero()) continue;
      code += "\ton" + namer_.Variant(ev) + "_ func(" +
              NativeType(ev.union_type) + "),\n";
    }
    code += ") {\n";
    code += "\tif t_ == nil {\n\t\treturn\n\t}\n";
    code += "\tswitch t_.Type {\n";
    for (auto it2 = enum_def.Vals().begin(); it2 != enum_def.Vals().end();
         ++it2) {
      const EnumVal& ev = **it2;
      if (ev.IsZero()) continue;
      code += "\tcase " + namer_.EnumVariant(enum_def, ev) + ":\n";
      code += "\t\tif v_, ok_ := t_.Value.(" + NativeType(ev.union_type) +
              "); ok_ {\n";
      code += "\t\t\ton" + namer_.Variant(ev) + "_(v_)\n";
      code += "\t\t}\n";
    }
    code += "\t}\n";
    code += "}\n\n";
  }

  void GenNativeTablePack(const StructDef& struct_def, std::string* code_ptr) {
    std::string& code = *code_ptr;
    const std::string struct_type = namer_.Type(struct_def);

    code += "func (t_ *" + NativeName(struct_def) +
            ") Pack(builder_ *flatbuffers.Builder) flatbuffers.UOffsetT {\n";
    code += "\tif t_ == nil {\n\t\treturn 0\n\t}\n";
    for (auto it = struct_def.fields.vec.begin();
         it != struct_def.fields.vec.end(); ++it) {
      const FieldDef& field = **it;
      if (field.deprecated) continue;
      if (IsScalar(field.value.type.base_type)) continue;

      const std::string field_field = namer_.Field(field);
      const std::string field_var = namer_.Variable(field);
      const std::string offset = field_var + "Offset_";

      if (IsString(field.value.type)) {
        if (!field.IsRequired()) {
          code += "\t" + offset + " := flatbuffers.UOffsetT(0)\n";
          code += "\tif t_." + field_field + " != \"\" {\n";
          code += "\t\t" + offset + " = builder_.CreateString(t_." +
                  field_field + ")\n";
          code += "\t}\n";
        } else {
          code += "\t" + offset + " := builder_.CreateString(t_." +
                  field_field + ")\n";
        }
      } else if (IsVector(field.value.type) &&
                 field.value.type.element == BASE_TYPE_UCHAR &&
                 field.value.type.enum_def == nullptr) {
        code += "\t" + offset + " := flatbuffers.UOffsetT(0)\n";
        code += "\tif t_." + field_field + " != nil {\n";
        code += "\t\t" + offset + " = builder_.CreateByteString(t_." +
                field_field + ")\n";
        code += "\t}\n";
      } else if (IsVector(field.value.type) && IsMapField(field)) {
        // Pack a map[K]V field as a sorted vector of entry structs.
        needs_sort_import_ = true;
        const auto& entry = *field.value.type.struct_def;
        const auto *kf = GetEntryKeyField(entry);
        const auto *vf = GetEntryValueField(entry);
        const std::string entry_type =
            WrapInNameSpaceAndTrack(field.value.type.struct_def,
                                    field.value.type.struct_def->name);
        const std::string key_go = GoKeyType(kf->value.type);
        const std::string val_go = NativeType(vf->value.type);
        const std::string offsets = field_var + "Offsets_";
        const std::string entries_var = field_var + "Entries_";
        const std::string length = field_var + "Length_";
        const std::string kf_fn = namer_.Function(*kf);
        const std::string vf_fn = namer_.Function(*vf);
        code += "\t" + offset + " := flatbuffers.UOffsetT(0)\n";
        code += "\tif t_." + field_field + " != nil {\n";
        // Collect map entries into a typed slice so we can sort them.
        code += "\t\ttype " + field_var + "KV_ struct { k " + key_go + "; v " +
                val_go + " }\n";
        code += "\t\t" + entries_var + " := make([]" + field_var +
                "KV_, 0, len(t_." + field_field + "))\n";
        code += "\t\tfor k_, v_ := range t_." + field_field + " {\n";
        code += "\t\t\t" + entries_var + " = append(" + entries_var + ", " +
                field_var + "KV_{k_, v_})\n";
        code += "\t\t}\n";
        code += "\t\tsort.Slice(" + entries_var +
                ", func(i_, j_ int) bool { return " +
                GoKeyLess(kf->value.type, entries_var + "[i_].k",
                          entries_var + "[j_].k") +
                " })\n";
        // Pack each entry struct, collecting offsets.
        code += "\t\t" + length + " := len(" + entries_var + ")\n";
        code += "\t\t" + offsets + " := make([]flatbuffers.UOffsetT, " + length +
                ")\n";
        code += "\t\tfor i_, e_ := range " + entries_var + " {\n";
        // Build any offset-type fields before starting the entry table.
        const bool key_is_string = IsString(kf->value.type);
        const bool val_is_string = IsString(vf->value.type);
        const bool val_is_table =
            vf->value.type.base_type == BASE_TYPE_STRUCT &&
            !vf->value.type.struct_def->fixed;
        if (key_is_string) {
          code += "\t\t\tkeyOffset_ := builder_.CreateString(e_.k)\n";
        }
        if (val_is_string) {
          code += "\t\t\tvalOffset_ := builder_.CreateString(e_.v)\n";
        } else if (val_is_table) {
          code += "\t\t\tvalOffset_ := e_.v.Pack(builder_)\n";
        }
        code += "\t\t\t" + entry_type + "Start(builder_)\n";
        if (key_is_string) {
          code += "\t\t\t" + entry_type + "Add" + kf_fn +
                  "(builder_, keyOffset_)\n";
        } else {
          code += "\t\t\t" + entry_type + "Add" + kf_fn + "(builder_, e_.k)\n";
        }
        if (val_is_string || val_is_table) {
          code += "\t\t\t" + entry_type + "Add" + vf_fn +
                  "(builder_, valOffset_)\n";
        } else if (IsStruct(vf->value.type)) {
          // A struct value is stored inline, so it is built inside the entry.
          code += "\t\t\t" + entry_type + "Add" + vf_fn +
                  "(builder_, e_.v.Pack(builder_))\n";
        } else {
          code += "\t\t\t" + entry_type + "Add" + vf_fn + "(builder_, e_.v)\n";
        }
        code += "\t\t\t" + offsets + "[i_] = " + entry_type + "End(builder_)\n";
        code += "\t\t}\n";
        // Create the vector in reverse order (FlatBuffers convention).
        code += "\t\t" + struct_type + "Start" + namer_.Function(field) +
                "Vector(builder_, " + length + ")\n";
        code += "\t\tfor i_ := " + length + " - 1; i_ >= 0; i_-- {\n";
        code += "\t\t\tbuilder_.PrependUOffsetT(" + offsets + "[i_])\n";
        code += "\t\t}\n";
        code += "\t\t" + offset + " = builder_.EndVector(" + length + ")\n";
        code += "\t}\n";
      } else if (IsVector(field.value.type) && IsSetField(field)) {
        // Pack a map[K]struct{} field as a sorted vector of entry structs.
        needs_sort_import_ = true;
        const auto& entry = *field.value.type.struct_def;
        const auto *kf = GetEntryKeyField(entry);
        const std::string entry_type =
            WrapInNameSpaceAndTrack(field.value.type.struct_def,
                                    field.value.type.struct_def->name);
        const std::string key_go = GoKeyType(kf->value.type);
        const std::string offsets = field_var + "Offsets_";
        const std::string keys_var = field_var + "Keys_";
        const std::string length = field_var + "Length_";
        const std::string kf_fn = namer_.Function(*kf);
        code += "\t" + offset + " := flatbuffers.UOffsetT(0)\n";
        code += "\tif t_." + field_field + " != nil {\n";
        // Collect set keys into a sorted slice.
        code += "\t\t" + keys_var + " := make([]" + key_go + ", 0, len(t_." +
                field_field + "))\n";
        code += "\t\tfor k_ := range t_." + field_field + " {\n";
        code += "\t\t\t" + keys_var + " = append(" + keys_var + ", k_)\n";
        code += "\t\t}\n";
        code += "\t\tsort.Slice(" + keys_var +
                ", func(i_, j_ int) bool { return " +
                GoKeyLess(kf->value.type, keys_var + "[i_]", keys_var + "[j_]") +
                " })\n";
        code += "\t\t" + length + " := len(" + keys_var + ")\n";
        code += "\t\t" + offsets + " := make([]flatbuffers.UOffsetT, " + length +
                ")\n";
        code += "\t\tfor i_, k_ := range " + keys_var + " {\n";
        const bool key_is_string = IsString(kf->value.type);
        if (key_is_string) {
          code += "\t\t\tkeyOffset_ := builder_.CreateString(k_)\n";
        }
        code += "\t\t\t" + entry_type + "Start(builder_)\n";
        if (key_is_string) {
          code += "\t\t\t" + entry_type + "Add" + kf_fn +
                  "(builder_, keyOffset_)\n";
        } else {
          code += "\t\t\t" + entry_type + "Add" + kf_fn + "(builder_, k_)\n";
        }
        code += "\t\t\t" + offsets + "[i_] = " + entry_type + "End(builder_)\n";
        code += "\t\t}\n";
        code += "\t\t" + struct_type + "Start" + namer_.Function(field) +
                "Vector(builder_, " + length + ")\n";
        code += "\t\tfor i_ := " + length + " - 1; i_ >= 0; i_-- {\n";
        code += "\t\t\tbuilder_.PrependUOffsetT(" + offsets + "[i_])\n";
        code += "\t\t}\n";
        code += "\t\t" + offset + " = builder_.EndVector(" + length + ")\n";
        code += "\t}\n";
      } else if (IsVector(field.value.type)) {
        code += "\t" + offset + " := flatbuffers.UOffsetT(0)\n";
        code += "\tif t_." + field_field + " != nil {\n";
        std::string length = field_var + "Length_";
        std::string offsets = field_var + "Offsets_";
        code += "\t\t" + length + " := len(t_." + field_field + ")\n";
        if (field.value.type.element == BASE_TYPE_STRING) {
          code += "\t\t" + offsets + " := make([]flatbuffers.UOffsetT, " +
                  length + ")\n";
          code += "\t\tfor j_ := 0; j_ < " + length + "; j_++ {\n";
          code += "\t\t\t" + offsets + "[j_] = builder_.CreateString(t_." +
                  field_field + "[j_])\n";
          code += "\t\t}\n";
        } else if (field.value.type.element == BASE_TYPE_STRUCT &&
                   !field.value.type.struct_def->fixed) {
          code += "\t\t" + offsets + " := make([]flatbuffers.UOffsetT, " +
                  length + ")\n";
          code += "\t\tfor j_ := 0; j_ < " + length + "; j_++ {\n";
          code += "\t\t\t" + offsets + "[j_] = t_." + field_field +
                  "[j_].Pack(builder_)\n";
          code += "\t\t}\n";
        }
        code += "\t\t" + struct_type + "Start" + namer_.Function(field) +
                "Vector(builder_, " + length + ")\n";
        code += "\t\tfor j_ := " + length + " - 1; j_ >= 0; j_-- {\n";
        if (IsScalar(field.value.type.element)) {
          code += "\t\t\tbuilder_.Prepend" +
                  namer_.Method(GenTypeBasic(field.value.type.VectorType())) +
                  "(" +
                  CastToBaseType(field.value.type.VectorType(),
                                 "t_." + field_field + "[j_]") +
                  ")\n";
        } else if (field.value.type.element == BASE_TYPE_STRUCT &&
                   field.value.type.struct_def->fixed) {
          code += "\t\t\tt_." + field_field + "[j_].Pack(builder_)\n";
        } else {
          code += "\t\t\tbuilder_.PrependUOffsetT(" + offsets + "[j_])\n";
        }
        code += "\t\t}\n";
        code += "\t\t" + offset + " = builder_.EndVector(" + length + ")\n";
        code += "\t}\n";
      } else if (field.value.type.base_type == BASE_TYPE_STRUCT) {
        if (field.value.type.struct_def->fixed) continue;
        code += "\t" + offset + " := t_." + field_field + ".Pack(builder_)\n";
      } else if (field.value.type.base_type == BASE_TYPE_UNION) {
        code += "\t" + offset + " := t_." + field_field + ".Pack(builder_)\n\n";
      } else {
        FLATBUFFERS_ASSERT(0);
      }
    }
    code += "\t" + struct_type + "Start(builder_)\n";
    for (auto it = struct_def.fields.vec.begin();
         it != struct_def.fields.vec.end(); ++it) {
      const FieldDef& field = **it;
      if (field.deprecated) continue;
      const std::string field_field = namer_.Field(field);
      const std::string field_fn = namer_.Function(field);
      const std::string offset = namer_.Variable(field) + "Offset_";

      if (IsScalar(field.value.type.base_type)) {
        std::string prefix;
        if (field.IsScalarOptional()) {
          code += "\tif t_." + field_field + " != nil {\n\t";
          prefix = "*";
        }
        if (field.value.type.enum_def == nullptr ||
            !field.value.type.enum_def->is_union) {
          code += "\t" + struct_type + "Add" + field_fn + "(builder_, " +
                  prefix + "t_." + field_field + ")\n";
        }
        if (field.IsScalarOptional()) {
          code += "\t}\n";
        }
      } else {
        if (field.value.type.base_type == BASE_TYPE_STRUCT &&
            field.value.type.struct_def->fixed) {
          code += "\t" + offset + " := t_." + field_field + ".Pack(builder_)\n";
        } else if (field.value.type.enum_def != nullptr &&
                   field.value.type.enum_def->is_union) {
          code += "\tif t_." + field_field + " != nil {\n";
          code += "\t\t" + struct_type + "Add" +
                  namer_.Method(field.name + UnionTypeFieldSuffix()) +
                  "(builder_, t_." + field_field + ".Type)\n";
          code += "\t}\n";
        }
        code += "\t" + struct_type + "Add" + field_fn + "(builder_, " + offset +
                ")\n";
      }
    }
    code += "\treturn " + struct_type + "End(builder_)\n";
    code += "}\n\n";
  }

  void GenNativeTableUnPack(const StructDef& struct_def,
                            std::string* code_ptr) {
    std::string& code = *code_ptr;

    GenReceiver(struct_def, code_ptr);
    code += " UnPackTo(t_ *" + NativeName(struct_def) + ") {\n";
    for (auto it = struct_def.fields.vec.begin();
         it != struct_def.fields.vec.end(); ++it) {
      const FieldDef& field = **it;
      if (field.deprecated) continue;
      const std::string field_field = namer_.Field(field);
      const std::string field_var = namer_.Variable(field);
      const std::string length = field_var + "Length_";
      if (IsScalar(field.value.type.base_type)) {
        if (field.value.type.enum_def != nullptr &&
            field.value.type.enum_def->is_union)
          continue;
        code += "\tt_." + field_field + " = rcv_." + field_field + "()\n";
      } else if (IsString(field.value.type)) {
        code +=
            "\tt_." + field_field + " = string(rcv_." + field_field + "())\n";
      } else if (IsVector(field.value.type) &&
                 field.value.type.element == BASE_TYPE_UCHAR &&
                 field.value.type.enum_def == nullptr) {
        code += "\tt_." + field_field + " = rcv_." + field_field + "Bytes()\n";
      } else if (IsVector(field.value.type) && IsMapField(field)) {
        const auto& entry = *field.value.type.struct_def;
        const auto *kf = GetEntryKeyField(entry);
        const auto *vf = GetEntryValueField(entry);
        const std::string entry_type =
            WrapInNameSpaceAndTrack(field.value.type.struct_def,
                                    field.value.type.struct_def->name);
        const std::string key_go = GoKeyType(kf->value.type);
        const std::string val_go = NativeType(vf->value.type);
        code += "\t" + length + " := rcv_." + field_field + "Length()\n";
        code += "\tt_." + field_field + " = make(map[" + key_go + "]" + val_go +
                ", " + length + ")\n";
        code += "\tfor j_ := 0; j_ < " + length + "; j_++ {\n";
        code += "\t\tx_ := " + entry_type + "{}\n";
        code += "\t\trcv_." + field_field + "(&x_, j_)\n";
        // Key extraction — string keys need a cast.
        const std::string key_expr =
            IsString(kf->value.type) ? "string(x_." + namer_.Method(*kf) + "())"
                                     : "x_." + namer_.Method(*kf) + "()";
        // Value extraction — depends on value base type.
        std::string val_expr;
        if (IsScalar(vf->value.type.base_type)) {
          val_expr = "x_." + namer_.Method(*vf) + "()";
        } else if (IsString(vf->value.type)) {
          val_expr = "string(x_." + namer_.Method(*vf) + "())";
        } else if (vf->value.type.base_type == BASE_TYPE_STRUCT) {
          val_expr = "x_." + namer_.Method(*vf) + "(nil).UnPack()";
        } else {
          val_expr = "x_." + namer_.Method(*vf) + "()";
        }
        code +=
            "\t\tt_." + field_field + "[" + key_expr + "] = " + val_expr + "\n";
        code += "\t}\n";
      } else if (IsVector(field.value.type) && IsSetField(field)) {
        const auto& entry = *field.value.type.struct_def;
        const auto *kf = GetEntryKeyField(entry);
        const std::string entry_type =
            WrapInNameSpaceAndTrack(field.value.type.struct_def,
                                    field.value.type.struct_def->name);
        const std::string key_go = GoKeyType(kf->value.type);
        code += "\t" + length + " := rcv_." + field_field + "Length()\n";
        code += "\tt_." + field_field + " = make(map[" + key_go +
                "]struct{}, " + length + ")\n";
        code += "\tfor j_ := 0; j_ < " + length + "; j_++ {\n";
        code += "\t\tx_ := " + entry_type + "{}\n";
        code += "\t\trcv_." + field_field + "(&x_, j_)\n";
        const std::string key_expr =
            IsString(kf->value.type) ? "string(x_." + namer_.Method(*kf) + "())"
                                     : "x_." + namer_.Method(*kf) + "()";
        code += "\t\tt_." + field_field + "[" + key_expr + "] = struct{}{}\n";
        code += "\t}\n";
      } else if (IsVector(field.value.type)) {
        code += "\t" + length + " := rcv_." + field_field + "Length()\n";
        code += "\tt_." + field_field + " = make(" +
                NativeType(field.value.type) + ", " + length + ")\n";
        code += "\tfor j_ := 0; j_ < " + length + "; j_++ {\n";
        if (field.value.type.element == BASE_TYPE_STRUCT) {
          code += "\t\tx_ := " +
                  WrapInNameSpaceAndTrack(field.value.type.struct_def,
                                          field.value.type.struct_def->name) +
                  "{}\n";
          code += "\t\trcv_." + field_field + "(&x_, j_)\n";
        }
        code += "\t\tt_." + field_field + "[j_] = ";
        if (IsScalar(field.value.type.element)) {
          code += "rcv_." + field_field + "(j_)";
        } else if (field.value.type.element == BASE_TYPE_STRING) {
          code += "string(rcv_." + field_field + "(j_))";
        } else if (field.value.type.element == BASE_TYPE_STRUCT) {
          code += "x_.UnPack()";
        } else {
          // TODO(iceboy): Support vector of unions.
          FLATBUFFERS_ASSERT(0);
        }
        code += "\n";
        code += "\t}\n";
      } else if (field.value.type.base_type == BASE_TYPE_STRUCT) {
        code += "\tt_." + field_field + " = rcv_." + field_field +
                "(nil).UnPack()\n";
      } else if (field.value.type.base_type == BASE_TYPE_UNION) {
        const std::string field_table = field_var + "Table_";
        code += "\t" + field_table + " := flatbuffers.Table{}\n";
        code +=
            "\tif rcv_." + namer_.Method(field) + "(&" + field_table + ") {\n";
        code += "\t\tt_." + field_field + " = rcv_." +
                namer_.Method(field.name + UnionTypeFieldSuffix()) +
                "().UnPack(" + field_table + ")\n";
        code += "\t}\n";
      } else {
        FLATBUFFERS_ASSERT(0);
      }
    }
    code += "}\n\n";

    GenReceiver(struct_def, code_ptr);
    code += " UnPack() *" + NativeName(struct_def) + " {\n";
    code += "\tif rcv_ == nil {\n\t\treturn nil\n\t}\n";
    code += "\tt_ := &" + NativeName(struct_def) + "{}\n";
    code += "\trcv_.UnPackTo(t_)\n";
    code += "\treturn t_\n";
    code += "}\n\n";
  }

  void GenSelectiveUnpack(const StructDef& struct_def, std::string* code_ptr) {
    std::string& code = *code_ptr;
    const std::string struct_type = namer_.Type(struct_def);
    const std::string native_type = NativeName(struct_def);

    // Exported field-name tokens for UnpackFields. Callers select fields with
    // these compile-checked constants instead of raw schema-name strings;
    // passing an unknown/misspelled name would otherwise silently populate
    // nothing. Each constant's value is the schema field name UnpackFields
    // matches on.
    {
      std::string consts;
      for (auto it = struct_def.fields.vec.begin();
           it != struct_def.fields.vec.end(); ++it) {
        const FieldDef& field = **it;
        if (field.deprecated) continue;
        if (IsScalar(field.value.type.base_type) &&
            field.value.type.enum_def != nullptr &&
            field.value.type.enum_def->is_union)
          continue;
        consts += "\t" + struct_type + "Field" + namer_.Field(field) + " = \"" +
                  field.name + "\"\n";
      }
      if (!consts.empty()) { code += "const (\n" + consts + ")\n\n"; }
    }

    code += "// UnpackFields returns a partial *" + native_type +
            " with only the named fields populated.\n";
    code += "// Fields not in the list are left at their zero/default values.\n";
    code += "// This avoids materializing the entire table tree. Pass the\n";
    code += "// generated " + struct_type +
            "Field* constants rather than raw strings.\n";
    GenReceiver(struct_def, code_ptr);
    code += " UnpackFields(fields_ ...string) *" + native_type + " {\n";
    code += "\tt_ := &" + native_type + "{}\n";
    code += "\tfieldSet_ := make(map[string]bool, len(fields_))\n";
    code += "\tfor _, f_ := range fields_ {\n";
    code += "\t\tfieldSet_[f_] = true\n";
    code += "\t}\n";

    for (auto it = struct_def.fields.vec.begin();
         it != struct_def.fields.vec.end(); ++it) {
      const FieldDef& field = **it;
      if (field.deprecated) continue;
      if (IsScalar(field.value.type.base_type) &&
          field.value.type.enum_def != nullptr &&
          field.value.type.enum_def->is_union)
        continue;

      const std::string field_field = namer_.Field(field);
      const std::string field_var = namer_.Variable(field);
      const std::string length = field_var + "Length_";

      code += "\tif fieldSet_[\"" + field.name + "\"] {\n";

      if (IsScalar(field.value.type.base_type)) {
        code += "\t\tt_." + field_field + " = rcv_." + field_field + "()\n";
      } else if (IsString(field.value.type)) {
        code +=
            "\t\tt_." + field_field + " = string(rcv_." + field_field + "())\n";
      } else if (IsVector(field.value.type) &&
                 field.value.type.element == BASE_TYPE_UCHAR &&
                 field.value.type.enum_def == nullptr) {
        code +=
            "\t\tt_." + field_field + " = rcv_." + field_field + "Bytes()\n";
      } else if (IsVector(field.value.type) && IsMapField(field)) {
        const auto& entry = *field.value.type.struct_def;
        const auto *kf = GetEntryKeyField(entry);
        const auto *vf = GetEntryValueField(entry);
        const std::string entry_type =
            WrapInNameSpaceAndTrack(field.value.type.struct_def,
                                    field.value.type.struct_def->name);
        const std::string key_go = GoKeyType(kf->value.type);
        const std::string val_go = NativeType(vf->value.type);
        code += "\t\t" + length + " := rcv_." + field_field + "Length()\n";
        code += "\t\tt_." + field_field + " = make(map[" + key_go + "]" +
                val_go + ", " + length + ")\n";
        code += "\t\tfor j_ := 0; j_ < " + length + "; j_++ {\n";
        code += "\t\t\tx_ := " + entry_type + "{}\n";
        code += "\t\t\trcv_." + field_field + "(&x_, j_)\n";
        const std::string key_expr =
            IsString(kf->value.type) ? "string(x_." + namer_.Method(*kf) + "())"
                                     : "x_." + namer_.Method(*kf) + "()";
        std::string val_expr;
        if (IsScalar(vf->value.type.base_type)) {
          val_expr = "x_." + namer_.Method(*vf) + "()";
        } else if (IsString(vf->value.type)) {
          val_expr = "string(x_." + namer_.Method(*vf) + "())";
        } else if (vf->value.type.base_type == BASE_TYPE_STRUCT) {
          val_expr = "x_." + namer_.Method(*vf) + "(nil).UnPack()";
        } else {
          val_expr = "x_." + namer_.Method(*vf) + "()";
        }
        code += "\t\t\tt_." + field_field + "[" + key_expr + "] = " + val_expr +
                "\n";
        code += "\t\t}\n";
      } else if (IsVector(field.value.type) && IsSetField(field)) {
        const auto& entry = *field.value.type.struct_def;
        const auto *kf = GetEntryKeyField(entry);
        const std::string entry_type =
            WrapInNameSpaceAndTrack(field.value.type.struct_def,
                                    field.value.type.struct_def->name);
        const std::string key_go = GoKeyType(kf->value.type);
        code += "\t\t" + length + " := rcv_." + field_field + "Length()\n";
        code += "\t\tt_." + field_field + " = make(map[" + key_go +
                "]struct{}, " + length + ")\n";
        code += "\t\tfor j_ := 0; j_ < " + length + "; j_++ {\n";
        code += "\t\t\tx_ := " + entry_type + "{}\n";
        code += "\t\t\trcv_." + field_field + "(&x_, j_)\n";
        const std::string key_expr =
            IsString(kf->value.type) ? "string(x_." + namer_.Method(*kf) + "())"
                                     : "x_." + namer_.Method(*kf) + "()";
        code += "\t\t\tt_." + field_field + "[" + key_expr + "] = struct{}{}\n";
        code += "\t\t}\n";
      } else if (IsVector(field.value.type)) {
        code += "\t\t" + length + " := rcv_." + field_field + "Length()\n";
        code += "\t\tt_." + field_field + " = make(" +
                NativeType(field.value.type) + ", " + length + ")\n";
        code += "\t\tfor j_ := 0; j_ < " + length + "; j_++ {\n";
        if (field.value.type.element == BASE_TYPE_STRUCT) {
          code += "\t\t\tx_ := " +
                  WrapInNameSpaceAndTrack(field.value.type.struct_def,
                                          field.value.type.struct_def->name) +
                  "{}\n";
          code += "\t\t\trcv_." + field_field + "(&x_, j_)\n";
        }
        code += "\t\t\tt_." + field_field + "[j_] = ";
        if (IsScalar(field.value.type.element)) {
          code += "rcv_." + field_field + "(j_)";
        } else if (field.value.type.element == BASE_TYPE_STRING) {
          code += "string(rcv_." + field_field + "(j_))";
        } else if (field.value.type.element == BASE_TYPE_STRUCT) {
          code += "x_.UnPack()";
        } else {
          // TODO(iceboy): Support vector of unions.
          FLATBUFFERS_ASSERT(0);
        }
        code += "\n";
        code += "\t\t}\n";
      } else if (field.value.type.base_type == BASE_TYPE_STRUCT) {
        code += "\t\tt_." + field_field + " = rcv_." + field_field +
                "(nil).UnPack()\n";
      } else if (field.value.type.base_type == BASE_TYPE_UNION) {
        const std::string field_table = field_var + "Table_";
        code += "\t\t" + field_table + " := flatbuffers.Table{}\n";
        code += "\t\tif rcv_." + namer_.Method(field) + "(&" + field_table +
                ") {\n";
        code += "\t\t\tt_." + field_field + " = rcv_." +
                namer_.Method(field.name + UnionTypeFieldSuffix()) +
                "().UnPack(" + field_table + ")\n";
        code += "\t\t}\n";
      } else {
        FLATBUFFERS_ASSERT(0);
      }

      code += "\t}\n";
    }

    code += "\treturn t_\n";
    code += "}\n\n";
  }

  void GenNativeStructPack(const StructDef& struct_def, std::string* code_ptr) {
    std::string& code = *code_ptr;

    code += "func (t_ *" + NativeName(struct_def) +
            ") Pack(builder_ *flatbuffers.Builder) flatbuffers.UOffsetT {\n";
    code += "\tif t_ == nil {\n\t\treturn 0\n\t}\n";
    StructPackGuards(struct_def, "", code_ptr);
    code += "\treturn Create" + namer_.Type(struct_def) + "(builder_";
    StructPackArgs(struct_def, "", code_ptr);
    code += ")\n";
    code += "}\n";
  }

  void StructPackArgs(const StructDef& struct_def, const char* nameprefix,
                      std::string* code_ptr) {
    std::string& code = *code_ptr;
    for (auto it = struct_def.fields.vec.begin();
         it != struct_def.fields.vec.end(); ++it) {
      const FieldDef& field = **it;
      if (field.value.type.base_type == BASE_TYPE_STRUCT) {
        StructPackArgs(*field.value.type.struct_def,
                       (nameprefix + namer_.Field(field) + ".").c_str(),
                       code_ptr);
      } else {
        code += std::string(", t_.") + nameprefix + namer_.Field(field);
      }
    }
  }

  // Emits nil-guards for nested struct fields before they are flattened into the
  // CreateX(...) call. Nested structs are mandatory inline members in the binary
  // layout, but the Object API represents them as pointers; a hand-built *T that
  // leaves one nil would otherwise dereference nil in Pack. Substituting the zero
  // value matches what the field would occupy in the buffer.
  void StructPackGuards(const StructDef& struct_def, const std::string& nameprefix,
                        std::string* code_ptr) {
    std::string& code = *code_ptr;
    for (auto it = struct_def.fields.vec.begin();
         it != struct_def.fields.vec.end(); ++it) {
      const FieldDef& field = **it;
      if (field.value.type.base_type == BASE_TYPE_STRUCT) {
        const std::string path = nameprefix + namer_.Field(field);
        code += "\tif t_." + path + " == nil {\n";
        code +=
            "\t\tt_." + path + " = &" +
            WrapInNameSpaceAndTrack(field.value.type.struct_def,
                                    NativeName(*field.value.type.struct_def)) +
            "{}\n";
        code += "\t}\n";
        StructPackGuards(*field.value.type.struct_def, path + ".", code_ptr);
      }
    }
  }

  void GenNativeStructUnPack(const StructDef& struct_def,
                             std::string* code_ptr) {
    std::string& code = *code_ptr;

    GenReceiver(struct_def, code_ptr);
    code += " UnPackTo(t_ *" + NativeName(struct_def) + ") {\n";
    for (auto it = struct_def.fields.vec.begin();
         it != struct_def.fields.vec.end(); ++it) {
      const FieldDef& field = **it;
      if (field.value.type.base_type == BASE_TYPE_STRUCT) {
        code += "\tt_." + namer_.Field(field) + " = rcv_." +
                namer_.Method(field) + "(nil).UnPack()\n";
      } else {
        code += "\tt_." + namer_.Field(field) + " = rcv_." +
                namer_.Method(field) + "()\n";
      }
    }
    code += "}\n\n";

    GenReceiver(struct_def, code_ptr);
    code += " UnPack() *" + NativeName(struct_def) + " {\n";
    code += "\tif rcv_ == nil {\n\t\treturn nil\n\t}\n";
    code += "\tt_ := &" + NativeName(struct_def) + "{}\n";
    code += "\trcv_.UnPackTo(t_)\n";
    code += "\treturn t_\n";
    code += "}\n\n";
  }

  // Generate enum declarations.
  void GenEnum(const EnumDef& enum_def, std::string* code_ptr) {
    if (enum_def.generated) return;

    auto max_name_length = MaxNameLength(enum_def);
    cur_name_space_ = enum_def.defined_namespace;

    GenComment(enum_def.doc_comment, code_ptr, nullptr);
    GenEnumType(enum_def, code_ptr);
    BeginEnum(code_ptr);
    for (auto it = enum_def.Vals().begin(); it != enum_def.Vals().end(); ++it) {
      const EnumVal& ev = **it;
      GenComment(ev.doc_comment, code_ptr, nullptr, "\t");
      EnumMember(enum_def, ev, max_name_length, code_ptr);
    }
    EndEnum(code_ptr);

    BeginEnumNames(enum_def, code_ptr);
    for (auto it = enum_def.Vals().begin(); it != enum_def.Vals().end(); ++it) {
      const EnumVal& ev = **it;
      EnumNameMember(enum_def, ev, max_name_length, code_ptr);
    }
    EndEnumNames(code_ptr);

    BeginEnumValues(enum_def, code_ptr);
    for (auto it = enum_def.Vals().begin(); it != enum_def.Vals().end(); ++it) {
      auto& ev = **it;
      EnumValueMember(enum_def, ev, max_name_length, code_ptr);
    }
    EndEnumValues(code_ptr);

    EnumStringer(enum_def, code_ptr);
  }

  // Returns the function name that is able to read a value of the given type.
  std::string GenGetter(const Type& type) {
    switch (type.base_type) {
      case BASE_TYPE_STRING:
        return "rcv_._tab.ByteVector";
      case BASE_TYPE_UNION:
        return "rcv_._tab.Union";
      case BASE_TYPE_VECTOR:
        return GenGetter(type.VectorType());
      default:
        return "rcv_._tab.Get" + namer_.Function(GenTypeBasic(type));
    }
  }

  // Returns the method name for use with add/put calls.
  std::string GenMethod(const FieldDef& field) {
    return IsScalar(field.value.type.base_type)
               ? namer_.Method(GenTypeBasic(field.value.type))
               : (IsStruct(field.value.type) ? "Struct" : "UOffsetT");
  }

  std::string GenTypeBasic(const Type& type) {
    // clang-format off
    static const char *ctypename[] = {
      #define FLATBUFFERS_TD(ENUM, IDLTYPE, CTYPE, JTYPE, GTYPE, ...) \
        #GTYPE,
        FLATBUFFERS_GEN_TYPES(FLATBUFFERS_TD)
      #undef FLATBUFFERS_TD
    };
    // clang-format on
    return ctypename[type.base_type];
  }

  std::string GenTypePointer(const Type& type) {
    switch (type.base_type) {
      case BASE_TYPE_STRING:
        return "[]byte";
      case BASE_TYPE_VECTOR:
        return GenTypeGet(type.VectorType());
      case BASE_TYPE_STRUCT:
        return WrapInNameSpaceAndTrack(type.struct_def, type.struct_def->name);
      case BASE_TYPE_UNION:
        // fall through
      default:
        return "*flatbuffers.Table";
    }
  }

  std::string GenTypeGet(const Type& type) {
    if (type.enum_def != nullptr) {
      return GetEnumTypeName(*type.enum_def);
    }
    return IsScalar(type.base_type) ? GenTypeBasic(type) : GenTypePointer(type);
  }

  std::string TypeName(const FieldDef& field) {
    std::string prefix;
    if (field.IsScalarOptional()) {
      prefix = "*";
    }
    return prefix + GenTypeGet(field.value.type);
  }

  // If type is an enum, returns value with a cast to the enum type, otherwise
  // returns value as-is.
  std::string CastToEnum(const Type& type, std::string value) {
    if (type.enum_def == nullptr) {
      return value;
    } else {
      return GenTypeGet(type) + "(" + value + ")";
    }
  }

  // If type is an enum, returns value with a cast to the enum base type,
  // otherwise returns value as-is.
  std::string CastToBaseType(const Type& type, std::string value) {
    if (type.enum_def == nullptr) {
      return value;
    } else {
      return GenTypeBasic(type) + "(" + value + ")";
    }
  }

  // ── Canonical JSON (--go-json) ───────────────────────────────────────
  //
  // Every Object API type gets methods reading and writing canonical
  // FlatBuffers JSON through the flatbuffers runtime (go/json_canonical.go):
  // the format flatc's own JSON parser and printer use, with defaults, empty
  // strings and empty collections omitted. Schema reflection
  // (ReflectToCanonicalJSON) writes through the same runtime, so both give
  // the same bytes for the same message.

  std::string json_error_;

  void JSONUnsupported(const StructDef& struct_def, const FieldDef& field,
                       const std::string& what) {
    if (json_error_.empty()) {
      json_error_ = struct_def.name + "." + field.name +
                    ": --go-json does not support " + what;
    }
  }

  // The definition name flatc --jsonschema gives a type.
  static std::string JSONSchemaDefinition(const Definition& def) {
    std::string name;
    for (const auto& component : def.defined_namespace->components) {
      name += component + "_";
    }
    return name + def.name;
  }

  std::string JSONEnumNames(const EnumDef& enum_def) {
    return WrapInNameSpaceAndTrack(&enum_def, "EnumNames" + enum_def.name);
  }

  std::string JSONEnumValues(const EnumDef& enum_def) {
    return WrapInNameSpaceAndTrack(&enum_def,
                                   "EnumValues" + namer_.Type(enum_def));
  }

  static bool IsJSONBitFlags(const EnumDef& enum_def) {
    return enum_def.attributes.Lookup("bit_flags") != nullptr;
  }

  // Like all generated code (see the note above needs_sort_import_), the JSON
  // methods give every receiver, parameter and local a trailing underscore.

  // A Go statement writing `expr`, a scalar, string, struct or table of
  // `type` as the Object API holds it, to the JSONWriter w_.
  std::string GenJSONWrite(const Type& type, const std::string& expr) {
    const bool int64_strings = parser_.opts.json_int64_as_string;
    if (type.enum_def != nullptr && IsScalar(type.base_type)) {
      // A 64-bit value no variant names is a 64-bit number, quoted under
      // --json-int64-as-string like any other.
      const bool quoted = int64_strings && (type.base_type == BASE_TYPE_LONG ||
                                            type.base_type == BASE_TYPE_ULONG);
      std::string call = IsJSONBitFlags(*type.enum_def)
                             ? "flatbuffers.WriteJSONBitFlags"
                             : "flatbuffers.WriteJSONEnum";
      if (quoted) call += "Quoted";
      return call + "(w_, " + expr + ", " + JSONEnumNames(*type.enum_def) + ")";
    }
    switch (type.base_type) {
      case BASE_TYPE_BOOL:
        return "w_.Bool(" + expr + ")";
      case BASE_TYPE_CHAR:
      case BASE_TYPE_SHORT:
      case BASE_TYPE_INT:
        return "w_.Int(int64(" + expr + "))";
      case BASE_TYPE_UCHAR:
      case BASE_TYPE_USHORT:
      case BASE_TYPE_UINT:
        return "w_.Uint(uint64(" + expr + "))";
      case BASE_TYPE_LONG:
        return std::string(int64_strings ? "w_.IntString(" : "w_.Int(") +
               expr + ")";
      case BASE_TYPE_ULONG:
        return std::string(int64_strings ? "w_.UintString(" : "w_.Uint(") +
               expr + ")";
      case BASE_TYPE_FLOAT:
        return "w_.Float32(" + expr + ")";
      case BASE_TYPE_DOUBLE:
        return "w_.Float64(" + expr + ")";
      case BASE_TYPE_STRING:
        return "w_.String(" + expr + ")";
      case BASE_TYPE_STRUCT:
        return expr + ".WriteFlatJSON(w_)";
      default:
        FLATBUFFERS_ASSERT(0);
        return "";
    }
  }

  // Go statements reading the JSON value `src`, located by the JSON Pointer
  // expression `ptr`, into `lhs`: a scalar, string, struct or table of
  // `type`. A failure returns `ret` + "err_" from the enclosing function.
  std::string GenJSONRead(const Type& type, const std::string& lhs,
                          const std::string& src, const std::string& ptr,
                          const std::string& ret, const std::string& indent) {
    std::string call;
    if (type.enum_def != nullptr && IsScalar(type.base_type)) {
      call = std::string(IsJSONBitFlags(*type.enum_def)
                             ? "flatbuffers.JSONBitFlags("
                             : "flatbuffers.JSONEnum(") +
             src + ", " + ptr + ", " + JSONEnumValues(*type.enum_def) + ")";
    } else if (type.base_type == BASE_TYPE_BOOL) {
      call = "flatbuffers.JSONBool(" + src + ", " + ptr + ")";
    } else if (IsFloat(type.base_type)) {
      call = "flatbuffers.JSONFloat[" + GenTypeBasic(type) + "](" + src +
             ", " + ptr + ")";
    } else if (IsInteger(type.base_type)) {
      call = "flatbuffers.JSONInt[" + GenTypeBasic(type) + "](" + src + ", " +
             ptr + ")";
    } else if (IsString(type)) {
      call = "flatbuffers.JSONString(" + src + ", " + ptr + ")";
    } else if (type.base_type == BASE_TYPE_STRUCT) {
      const std::string native =
          WrapInNameSpaceAndTrack(type.struct_def, NativeName(*type.struct_def));
      return indent + lhs + " = &" + native + "{}\n" + indent +
             "if err_ := " + lhs + ".UnmarshalFlatJSONValue(" + src + ", " +
             ptr + "); err_ != nil {\n" + indent + "\t" + ret + "err_\n" +
             indent + "}\n";
    } else {
      FLATBUFFERS_ASSERT(0);
      return "";
    }
    return indent + "if " + lhs + ", err_ = " + call + "; err_ != nil {\n" +
           indent + "\t" + ret + "err_\n" + indent + "}\n";
  }

  // The keys of a map or set field, in FlatBuffers key order.
  std::string GenJSONSortedKeys(const FieldDef& key_field,
                                const std::string& expr) {
    return key_field.value.type.base_type == BASE_TYPE_BOOL
               ? "flatbuffers.JSONBoolMapKeys(" + expr + ")"
               : "flatbuffers.JSONMapKeys(" + expr + ")";
  }

  // A map key `k_` spelled as a JSON property name.
  std::string GenJSONKeyText(const Type& key_type) {
    if (IsString(key_type)) return "k_";
    if (key_type.enum_def != nullptr) {
      return std::string(IsJSONBitFlags(*key_type.enum_def)
                             ? "flatbuffers.JSONBitFlagsKey(k_, "
                             : "flatbuffers.JSONEnumKey(k_, ") +
             JSONEnumNames(*key_type.enum_def) + ")";
    }
    if (key_type.base_type == BASE_TYPE_BOOL) {
      return "flatbuffers.JSONBoolKeyText(k_)";
    }
    return "flatbuffers.JSONIntegerKey(k_)";
  }

  // Go statements parsing map key text `k_` (at pointer `p_`) into `key_`.
  std::string GenJSONKeyParse(const Type& key_type, const std::string& indent) {
    if (IsString(key_type)) return indent + "key_ := k_\n";
    std::string call;
    if (key_type.enum_def != nullptr) {
      call = std::string(IsJSONBitFlags(*key_type.enum_def)
                             ? "flatbuffers.JSONBitFlags(k_, p_, "
                             : "flatbuffers.JSONEnum(k_, p_, ") +
             JSONEnumValues(*key_type.enum_def) + ")";
    } else if (key_type.base_type == BASE_TYPE_BOOL) {
      call = "flatbuffers.JSONBoolKey(k_, p_)";
    } else {
      call = "flatbuffers.JSONInt[" + GoKeyType(key_type) + "](k_, p_)";
    }
    return indent + "key_, err_ := " + call + "\n" + indent +
           "if err_ != nil {\n" + indent + "\treturn err_\n" + indent + "}\n";
  }

  // The Go condition under which a scalar table field differs from its
  // default and so is written. NaN equals nothing, itself included, so a NaN
  // default is matched by math.IsNaN instead of !=.
  std::string GenJSONNotDefault(const FieldDef& field, const std::string& f) {
    if (IsFloat(field.value.type.base_type) &&
        StringIsFlatbufferNan(field.value.constant)) {
      needs_math_import_ = true;
      return "!math.IsNaN(float64(" + f + "))";
    }
    return f + " != " + GenConstant(field);
  }

  void GenJSONWriteField(const StructDef& struct_def, const FieldDef& field,
                         std::string* code_ptr) {
    std::string& code = *code_ptr;
    const Type& type = field.value.type;
    if (field.deprecated) return;
    // A union's type field is written together with the union.
    if (type.base_type == BASE_TYPE_UTYPE) return;
    const std::string f = "t_." + namer_.Field(field);
    const std::string key = "\"" + field.name + "\"";
    const std::string write_key = "w_.Key(" + key + ")\n";

    if (field.IsScalarOptional()) {
      code += "\tif " + f + " != nil {\n\t\t" + write_key + "\t\t" +
              GenJSONWrite(type, "*" + f) + "\n\t}\n";
    } else if (IsScalar(type.base_type)) {
      if (struct_def.fixed) {
        code += "\t" + write_key + "\t" + GenJSONWrite(type, f) + "\n";
      } else {
        code += "\tif " + GenJSONNotDefault(field, f) + " {\n\t\t" +
                write_key + "\t\t" + GenJSONWrite(type, f) + "\n\t}\n";
      }
    } else if (IsString(type)) {
      if (field.IsRequired()) {
        code += "\t" + write_key + "\t" + GenJSONWrite(type, f) + "\n";
      } else {
        code += "\tif " + f + " != \"\" {\n\t\t" + write_key + "\t\t" +
                GenJSONWrite(type, f) + "\n\t}\n";
      }
    } else if (type.base_type == BASE_TYPE_STRUCT) {
      if (struct_def.fixed) {
        code += "\t" + write_key + "\t" + GenJSONWrite(type, f) + "\n";
      } else if (field.IsRequired()) {
        // A required table or struct that is missing cannot be packed
        // either; JSON has no spelling for it but an error.
        code += "\tif " + f + " == nil {\n\t\tw_.Fail(flatbuffers.JSONErrorf(" +
                "\"\", \"required member " + field.name +
                " is missing\"))\n\t} else {\n\t\t" + write_key + "\t\t" +
                GenJSONWrite(type, f) + "\n\t}\n";
      } else {
        code += "\tif " + f + " != nil {\n\t\t" + write_key + "\t\t" +
                GenJSONWrite(type, f) + "\n\t}\n";
      }
    } else if (type.base_type == BASE_TYPE_UNION) {
      // A union with no value is no union, as UnPack reads one: a type
      // without a value, or a variant this code does not know, unpacks nil.
      code += "\tif " + f + " != nil && " + f + ".Type != 0 && " + f +
              ".Value != nil {\n";
      code += "\t\tw_.Key(\"" + field.sibling_union_field->name + "\")\n";
      code += "\t\t" + f + ".WriteFlatJSONType(w_)\n";
      code += "\t\t" + write_key + "\t\t" + f + ".WriteFlatJSONValue(w_)\n\t}\n";
    } else if (IsVector(type)) {
      const bool is_map = IsMapField(field);
      const bool is_set = IsSetField(field);
      std::string body;
      if (is_map || is_set) {
        const auto& entry = *type.struct_def;
        const FieldDef* kf = GetEntryKeyField(entry);
        body += is_map ? "\t\tw_.BeginObject()\n" : "\t\tw_.BeginArray()\n";
        body += "\n\t\tfor _, k_ := range " + GenJSONSortedKeys(*kf, f) +
                " {\n";
        if (is_map) {
          const FieldDef* vf = GetEntryValueField(entry);
          body += "\t\t\tw_.MapKey(" + GenJSONKeyText(kf->value.type) + ")\n";
          body += "\t\t\t" + GenJSONWrite(vf->value.type, f + "[k_]") + "\n";
        } else {
          body += "\t\t\t" + GenJSONWrite(kf->value.type, "k_") + "\n";
        }
        body += "\t\t}\n\n";
        body += is_map ? "\t\tw_.EndObject()\n" : "\t\tw_.EndArray()\n";
      } else {
        body += "\t\tw_.BeginArray()\n\n\t\tfor _, e_ := range " + f + " {\n";
        body += "\t\t\t" + GenJSONWrite(type.VectorType(), "e_") + "\n";
        body += "\t\t}\n\n\t\tw_.EndArray()\n";
      }
      if (field.IsRequired()) {
        code += "\t{\n\t\t" + write_key + body + "\t}\n";
      } else {
        code += "\tif len(" + f + ") > 0 {\n\t\t" + write_key + body +
                "\t}\n";
      }
    }
  }

  void GenJSONReadField(const StructDef& struct_def, const FieldDef& field,
                        std::string* code_ptr) {
    std::string& code = *code_ptr;
    const Type& type = field.value.type;
    if (field.deprecated) return;
    if (type.base_type == BASE_TYPE_UTYPE) return;
    const std::string f = "t_." + namer_.Field(field);
    const std::string key = "\"" + field.name + "\"";
    const std::string fp = "flatbuffers.JSONPointer(pointer_, " + key + ")";
    const bool required = field.IsRequired() || struct_def.fixed;
    const std::string missing =
        required ? " else {\n\t\treturn flatbuffers.JSONRequired(pointer_, " +
                       key + ")\n\t}\n"
                 : "\n";

    if (type.base_type == BASE_TYPE_UNION) {
      code += "\tif " + f + ", err_ = " +
              WrapInNameSpaceAndTrack(
                  type.enum_def,
                  "Unmarshal" + namer_.Type(*type.enum_def) + "FlatJSON") +
              "(m_, pointer_, \"" + field.sibling_union_field->name + "\", " +
              key + "); err_ != nil {\n\t\treturn err_\n\t}\n";
      return;
    }

    code += "\tif x_, ok_ := flatbuffers.JSONMember(m_, " + key + "); ok_ {\n";
    if (field.IsScalarOptional()) {
      code += "\t\tvar value_ " + NativeType(type) + "\n";
      code += GenJSONRead(type, "value_", "x_", fp, "return ", "\t\t");
      code += "\t\t" + f + " = &value_\n";
    } else if (!IsVector(type)) {
      code += GenJSONRead(type, f, "x_", fp, "return ", "\t\t");
    } else if (IsMapField(field)) {
      const auto& entry = *type.struct_def;
      const FieldDef* kf = GetEntryKeyField(entry);
      const FieldDef* vf = GetEntryValueField(entry);
      code += "\t\tfp_ := " + fp + "\n\n";
      code += "\t\to_, err_ := flatbuffers.JSONObject(x_, fp_)\n";
      code += "\t\tif err_ != nil {\n\t\t\treturn err_\n\t\t}\n\n";
      code += "\t\t" + f + " = make(map[" + GoKeyType(kf->value.type) + "]" +
              NativeType(vf->value.type) + ", len(o_))\n\n";
      // Two spellings of one key ("1" and "01") collapse to the last in
      // sorted order, as two entries with one key do in UnPack.
      code += "\t\tfor _, k_ := range flatbuffers.JSONSortedKeys(o_) {\n";
      code += "\t\t\tp_ := flatbuffers.JSONPointer(fp_, k_)\n";
      code += GenJSONKeyParse(kf->value.type, "\t\t\t");
      code += "\n\t\t\tvar value_ " + NativeType(vf->value.type) + "\n";
      code += "\t\t\tif e_, ok_ := flatbuffers.JSONMember(o_, k_); ok_ {\n";
      code += GenJSONRead(vf->value.type, "value_", "e_", "p_", "return ",
                          "\t\t\t\t");
      code += "\t\t\t}\n\n";
      code += "\t\t\t" + f + "[key_] = value_\n\t\t}\n";
    } else if (IsSetField(field)) {
      const FieldDef* kf = GetEntryKeyField(*type.struct_def);
      code += "\t\tfp_ := " + fp + "\n\n";
      code += "\t\ta_, err_ := flatbuffers.JSONArray(x_, fp_)\n";
      code += "\t\tif err_ != nil {\n\t\t\treturn err_\n\t\t}\n\n";
      code += "\t\t" + f + " = make(map[" + GoKeyType(kf->value.type) +
              "]struct{}, len(a_))\n\n";
      code += "\t\tfor i_, e_ := range a_ {\n";
      code += "\t\t\tvar key_ " + GoKeyType(kf->value.type) + "\n";
      code += GenJSONRead(kf->value.type, "key_", "e_",
                          "flatbuffers.JSONIndexPointer(fp_, i_)", "return ",
                          "\t\t\t");
      code += "\t\t\t" + f + "[key_] = struct{}{}\n\t\t}\n";
    } else {
      code += "\t\tfp_ := " + fp + "\n\n";
      code += "\t\ta_, err_ := flatbuffers.JSONArray(x_, fp_)\n";
      code += "\t\tif err_ != nil {\n\t\t\treturn err_\n\t\t}\n\n";
      code += "\t\t" + f + " = make(" + NativeType(type) + ", len(a_))\n\n";
      code += "\t\tfor i_, e_ := range a_ {\n";
      code += GenJSONRead(type.VectorType(), f + "[i_]", "e_",
                          "flatbuffers.JSONIndexPointer(fp_, i_)", "return ",
                          "\t\t\t");
      code += "\t\t}\n";
    }
    code += "\t}" + missing;
  }

  void GenNativeJSON(const StructDef& struct_def, std::string* code_ptr) {
    std::string& code = *code_ptr;
    const std::string native = NativeName(struct_def);

    std::string members;
    for (const FieldDef* field : struct_def.fields.vec) {
      const Type& type = field->value.type;
      members += ", \"" + field->name + "\"";
      if (field->deprecated) continue;
      if (IsArray(type)) JSONUnsupported(struct_def, *field, "fixed-size arrays");
      if (field->nested_flatbuffer) {
        JSONUnsupported(struct_def, *field, "nested_flatbuffer");
      }
      if (field->flexbuffer) JSONUnsupported(struct_def, *field, "flexbuffer");
      if (IsVector(type) && type.element == BASE_TYPE_UNION) {
        JSONUnsupported(struct_def, *field, "vectors of unions");
      }
      if (field->offset64) {
        JSONUnsupported(struct_def, *field, "64-bit offsets (offset64)");
      }
    }
    if (!json_error_.empty()) return;

    code += "// FlatJSONSchema names the JSON Schema definition flatc --jsonschema\n";
    code += "// generates for " + struct_def.name + ": its file, and its name there.\n";
    code += "func (*" + native + ") FlatJSONSchema() (string, string) {\n";
    code += "\treturn \"" + StripExtension(StripPath(struct_def.file)) +
            ".schema.json\", \"" + JSONSchemaDefinition(struct_def) + "\"\n";
    code += "}\n\n";

    code += "// MarshalFlatJSON returns t as canonical FlatBuffers JSON.\n";
    code += "func (t_ *" + native + ") MarshalFlatJSON() ([]byte, error) {\n";
    code += "\tw_ := flatbuffers.NewJSONWriter()\n\tt_.WriteFlatJSON(w_)\n\n";
    code += "\treturn w_.Bytes()\n}\n\n";

    code += "// WriteFlatJSON writes t to w_ as canonical FlatBuffers JSON.\n";
    code += "func (t_ *" + native + ") WriteFlatJSON(w_ *flatbuffers.JSONWriter) {\n";
    code += "\tif t_ == nil {\n\t\tw_.Null()\n\n\t\treturn\n\t}\n\n";
    code += "\tw_.BeginObject()\n";
    for (const FieldDef* field : struct_def.fields.vec) {
      GenJSONWriteField(struct_def, *field, &code);
    }
    code += "\tw_.EndObject()\n}\n\n";

    code += "// UnmarshalFlatJSON replaces t with the value the canonical FlatBuffers\n";
    code += "// JSON in data_ describes; an absent scalar takes its schema default.\n";
    code += "func (t_ *" + native + ") UnmarshalFlatJSON(data_ []byte) error {\n";
    code += "\tv_, err_ := flatbuffers.ParseJSON(data_)\n";
    code += "\tif err_ != nil {\n\t\treturn err_\n\t}\n\n";
    code += "\treturn t_.UnmarshalFlatJSONValue(v_, \"\")\n}\n\n";

    code += "// UnmarshalFlatJSONValue is UnmarshalFlatJSON for a value\n";
    code += "// flatbuffers.ParseJSON has already parsed; pointer_ locates it in errors.\n";
    code += "func (t_ *" + native +
            ") UnmarshalFlatJSONValue(v_ any, pointer_ string) error {\n";
    code += "\tm_, err_ := flatbuffers.JSONObject(v_, pointer_)\n";
    code += "\tif err_ != nil {\n\t\treturn err_\n\t}\n\n";
    code += "\terr_ = flatbuffers.JSONCheckMembers(m_, pointer_" + members + ")\n";
    code += "\tif err_ != nil {\n\t\treturn err_\n\t}\n\n";
    code += "\t*t_ = " + native + "{}\n";
    if (!struct_def.fixed) {
      for (const FieldDef* field : struct_def.fields.vec) {
        const Type& type = field->value.type;
        if (field->deprecated || field->IsScalarOptional() ||
            !IsScalar(type.base_type) || type.base_type == BASE_TYPE_UTYPE) {
          continue;
        }
        const std::string constant = GenConstant(*field);
        if (constant == "0" || constant == "false" || constant == "0.0") {
          continue;
        }
        code += "\tt_." + namer_.Field(*field) + " = " + constant + "\n";
      }
    }
    code += "\n";
    for (const FieldDef* field : struct_def.fields.vec) {
      GenJSONReadField(struct_def, *field, &code);
    }
    code += "\n\treturn nil\n}\n\n";
  }

  void GenNativeUnionJSON(const EnumDef& enum_def, std::string* code_ptr) {
    if (enum_def.generated) return;
    std::string& code = *code_ptr;
    const std::string native = NativeName(enum_def);
    const std::string type_name = namer_.Type(enum_def);
    bool has_variants = false;
    for (const EnumVal* ev : enum_def.Vals()) {
      if (!ev->IsZero()) has_variants = true;
    }

    code += "// WriteFlatJSONType writes the union's type as its variant name.\n";
    code += "func (t_ *" + native +
            ") WriteFlatJSONType(w_ *flatbuffers.JSONWriter) {\n";
    code += "\tflatbuffers.WriteJSONEnum(w_, t_.Type, EnumNames" + enum_def.name +
            ")\n}\n\n";

    code += "// WriteFlatJSONValue writes the union's value; the enclosing table\n";
    code += "// writes its type.\n";
    code += "func (t_ *" + native +
            ") WriteFlatJSONValue(w_ *flatbuffers.JSONWriter) {\n";
    // A union with no variants has nothing to bind the value to.
    code += has_variants ? "\tswitch v_ := t_.Value.(type) {\n"
                         : "\tswitch t_.Value.(type) {\n";
    std::set<std::string> seen;
    for (const EnumVal* ev : enum_def.Vals()) {
      if (ev->IsZero()) continue;
      const std::string variant = NativeType(ev->union_type);
      if (!seen.insert(variant).second) continue;
      code += "\tcase " + variant + ":\n\t\tv_.WriteFlatJSON(w_)\n";
    }
    code += "\tdefault:\n\t\tw_.Fail(flatbuffers.JSONErrorf(\"\", \"union " +
            enum_def.name + " holds %T\", t_.Value))\n\t}\n}\n\n";

    code += "// Unmarshal" + type_name +
            "FlatJSON reads the union held by the members typeKey_\n";
    code += "// (the variant name) and valueKey_ (its value) of the object m_. A\n";
    code += "// type with no value, or no type at all, reads as no union.\n";
    code += "func Unmarshal" + type_name +
            "FlatJSON(m_ map[string]any, pointer_, typeKey_, valueKey_ string) (*" +
            native + ", error) {\n";
    code += "\ttv_, hasType_ := flatbuffers.JSONMember(m_, typeKey_)\n";
    code += std::string("\t") + (has_variants ? "value_" : "_") +
            ", hasValue_ := flatbuffers.JSONMember(m_, valueKey_)\n\n";
    code += "\tif !hasType_ {\n\t\tif hasValue_ {\n";
    code += "\t\t\treturn nil, flatbuffers.JSONErrorf(flatbuffers.JSONPointer(pointer_, valueKey_), \"union value without %s\", typeKey_)\n";
    code += "\t\t}\n\n\t\treturn nil, nil\n\t}\n\n";
    code += "\ttyp_, err_ := flatbuffers.JSONEnum(tv_, flatbuffers.JSONPointer(pointer_, typeKey_), EnumValues" +
            type_name + ")\n";
    code += "\tif err_ != nil {\n\t\treturn nil, err_\n\t}\n\n";
    code += "\tif !hasValue_ {\n\t\treturn nil, nil\n\t}\n\n";
    code += "\tvp_ := flatbuffers.JSONPointer(pointer_, valueKey_)\n\n";
    code += "\tswitch typ_ {\n";
    for (const EnumVal* ev : enum_def.Vals()) {
      const std::string constant = namer_.EnumVariant(enum_def, *ev);
      if (ev->IsZero()) {
        code += "\tcase " + constant +
                ":\n\t\treturn nil, flatbuffers.JSONErrorf(vp_, \"union value under NONE\")\n";
        continue;
      }
      std::string variant = NativeType(ev->union_type);  // "*Pkg.FooT"
      code += "\tcase " + constant + ":\n";
      code += "\t\tx_ := &" + variant.substr(1) + "{}\n";
      code += "\t\tif err_ := x_.UnmarshalFlatJSONValue(value_, vp_); err_ != nil {\n";
      code += "\t\t\treturn nil, err_\n\t\t}\n\n";
      code += "\t\treturn &" + native + "{Type: typ_, Value: x_}, nil\n";
    }
    code += "\t}\n\n";
    code += "\treturn nil, flatbuffers.JSONErrorf(flatbuffers.JSONPointer(pointer_, typeKey_), \"%d is not a variant of " +
            enum_def.name + "\", typ_)\n}\n\n";
  }

  std::string GenConstant(const FieldDef& field) {
    if (field.IsScalarOptional()) {
      return "nil";
    }
    switch (field.value.type.base_type) {
      case BASE_TYPE_BOOL:
        return field.value.constant == "0" ? "false" : "true";
      case BASE_TYPE_FLOAT:
      case BASE_TYPE_DOUBLE: {
        const std::string float_type =
            field.value.type.base_type == BASE_TYPE_FLOAT ? "float32"
                                                          : "float64";
        if (StringIsFlatbufferNan(field.value.constant)) {
          needs_math_import_ = true;
          return float_type + "(math.NaN())";
        } else if (StringIsFlatbufferPositiveInfinity(field.value.constant)) {
          needs_math_import_ = true;
          return float_type + "(math.Inf(1))";
        } else if (StringIsFlatbufferNegativeInfinity(field.value.constant)) {
          needs_math_import_ = true;
          return float_type + "(math.Inf(-1))";
        }
        return field.value.constant;
      }
      default:
        return field.value.constant;
    }
  }

  std::string NativeName(const StructDef& struct_def) const {
    return namer_.ObjectType(struct_def);
  }

  std::string NativeName(const EnumDef& enum_def) const {
    return namer_.ObjectType(enum_def);
  }

  std::string NativeType(const Type& type) {
    if (IsScalar(type.base_type)) {
      if (type.enum_def == nullptr) {
        return GenTypeBasic(type);
      } else {
        return GetEnumTypeName(*type.enum_def);
      }
    } else if (IsString(type)) {
      return "string";
    } else if (IsVector(type)) {
      return "[]" + NativeType(type.VectorType());
    } else if (type.base_type == BASE_TYPE_STRUCT) {
      return "*" + WrapInNameSpaceAndTrack(type.struct_def,
                                           NativeName(*type.struct_def));
    } else if (type.base_type == BASE_TYPE_UNION) {
      return "*" +
             WrapInNameSpaceAndTrack(type.enum_def, NativeName(*type.enum_def));
    }
    FLATBUFFERS_ASSERT(0);
    return std::string();
  }

  // Create a struct with a builder and the struct's arguments.
  void GenStructBuilder(const StructDef& struct_def, std::string* code_ptr) {
    BeginBuilderArgs(struct_def, code_ptr);
    StructBuilderArgs(struct_def, "", code_ptr);
    EndBuilderArgs(code_ptr);

    StructBuilderBody(struct_def, "", code_ptr);
    EndBuilderBody(code_ptr);
  }

  // Begin by declaring namespace and imports.
  void BeginFile(const std::string& name_space_name, const bool needs_imports,
                 const bool is_enum, std::string* code_ptr) {
    std::string& code = *code_ptr;
    code = code +
           "// Code generated by the FlatBuffers compiler. DO NOT EDIT.\n\n";
    code += "package " + name_space_name + "\n\n";
    if (needs_imports) {
      code += "import (\n";
      // standard imports, in alphabetical order for go fmt
      if (needs_bytes_import_) code += "\t\"bytes\"\n";
      if (!parser_.opts.go_import.empty()) {
        code += "\tflatbuffers \"" + parser_.opts.go_import + "\"\n";
      } else {
        code += "\tflatbuffers \"github.com/google/flatbuffers/go\"\n";
      }
      // math is needed to support non-finite scalar default values.
      if (needs_math_import_) {
        code += "\t\"math\"\n";
      }
      // sort is needed for deterministic map/set Pack() serialization.
      if (needs_sort_import_) {
        code += "\t\"sort\"\n";
      }
      if (is_enum) {
        code += "\t\"strconv\"\n";
      }

      if (tracked_imported_namespaces_.size() > 0) {
        code += "\n";
        for (auto it = tracked_imported_namespaces_.begin();
             it != tracked_imported_namespaces_.end(); ++it) {
          if ((*it)->defined_namespace->components.empty()) {
            code += "\t" + (*it)->name + " \"" + (*it)->name + "\"\n";
          } else {
            code += "\t" + NamespaceImportName((*it)->defined_namespace) +
                    " \"" + NamespaceImportPath((*it)->defined_namespace) +
                    "\"\n";
          }
        }
      }
      code += ")\n\n";
    } else {
      if (is_enum) {
        code += "import \"strconv\"\n\n";
      }
      if (needs_math_import_) {
        // math is needed to support non-finite scalar default values.
        code += "import \"math\"\n\n";
      }
    }
  }

  // Resets the needed imports before generating a new file.
  void ResetImports() {
    tracked_imported_namespaces_.clear();
    needs_bytes_import_ = false;
    needs_math_import_ = false;
    needs_sort_import_ = false;
  }

  // Save out the generated code for a Go Table type.
  bool SaveType(const Definition& def, const std::string& classcode,
                const bool needs_imports, const bool is_enum) {
    if (!classcode.length()) return true;

    Namespace& ns = go_namespace_.components.empty() ? *def.defined_namespace
                                                     : go_namespace_;
    std::string code = "";
    BeginFile(ns.components.empty() ? def.name : LastNamespacePart(ns),
              needs_imports, is_enum, &code);
    code += classcode;
    // Strip extra newlines at end of file to make it gofmt-clean.
    while (code.length() > 2 && code.substr(code.length() - 2) == "\n\n") {
      code.pop_back();
    }
    std::string directory = namer_.Directories(ns);
    std::string file = namer_.File(def, SkipFile::Suffix);
    // Go ignores files starting with '_'. Strip leading underscores from
    // auto-generated entry table filenames (__Map_*, __Set_*).
    size_t start = 0;
    while (start < file.size() && file[start] == '_') ++start;
    if (start > 0) file = file.substr(start);
    EnsureDirExists(directory);
    std::string filename = directory + file;
    return parser_.opts.file_saver->SaveFile(filename.c_str(), code, false);
  }

  // Create the full name of the imported namespace (format: A__B__C).
  std::string NamespaceImportName(const Namespace* ns) const {
    return namer_.Namespace(*ns);
  }

  // Create the full path for the imported namespace (format: A/B/C).
  std::string NamespaceImportPath(const Namespace* ns) const {
    std::string path =
        namer_.Directories(*ns, SkipDir::OutputPathAndTrailingPathSeparator);
    if (!parser_.opts.go_module_name.empty()) {
      path = parser_.opts.go_module_name + "/" + path;
    }
    return path;
  }

  // Ensure that a type is prefixed with its go package import name if it is
  // used outside of its namespace.
  // Returns the correct verifier function call for a referenced table type.
  // Same namespace: "verify{Type}" (unexported, same file).
  // Cross namespace: "{Pkg}.Verify{Type}" (exported, different package).
  std::string VerifyFuncForTable(const StructDef* struct_def) {
    const std::string type_name = namer_.Type(*struct_def);
    if (CurrentNameSpace() == struct_def->defined_namespace) {
      return "verify" + type_name;
    }
    tracked_imported_namespaces_.insert(struct_def);
    std::string pkg;
    if (struct_def->defined_namespace->components.empty())
      pkg = struct_def->name;
    else
      pkg = NamespaceImportName(struct_def->defined_namespace);
    return pkg + ".Verify" + type_name;
  }

  std::string WrapInNameSpaceAndTrack(const Definition* def,
                                      const std::string& name) {
    if (CurrentNameSpace() == def->defined_namespace) return name;
    tracked_imported_namespaces_.insert(def);
    if (def->defined_namespace->components.empty())
      return def->name + "." + name;
    else
      return NamespaceImportName(def->defined_namespace) + "." + name;
  }

  const Namespace* CurrentNameSpace() const { return cur_name_space_; }

  static size_t MaxNameLength(const EnumDef& enum_def) {
    size_t max = 0;
    for (auto it = enum_def.Vals().begin(); it != enum_def.Vals().end(); ++it) {
      max = std::max((*it)->name.length(), max);
    }
    return max;
  }
};
}  // namespace go

static bool GenerateGo(const Parser& parser, const std::string& path,
                       const std::string& file_name) {
  go::GoGenerator generator(parser, path, file_name, parser.opts.go_namespace);
  return generator.generate();
}

namespace {

class GoCodeGenerator : public CodeGenerator {
 public:
  Status GenerateCode(const Parser& parser, const std::string& path,
                      const std::string& filename) override {
    if (!GenerateGo(parser, path, filename)) {
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
    if (!GenerateGoGRPC(parser, path, filename)) {
      return Status::ERROR;
    }
    return Status::OK;
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

  IDLOptions::Language Language() const override { return IDLOptions::kGo; }

  std::string LanguageName() const override { return "Go"; }
};
}  // namespace

std::unique_ptr<CodeGenerator> NewGoCodeGenerator() {
  return std::unique_ptr<GoCodeGenerator>(new GoCodeGenerator());
}

}  // namespace flatbuffers
