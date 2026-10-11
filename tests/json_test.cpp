#include "json_test.h"

#include "flatbuffers/flatbuffers.h"
#include "flatbuffers/idl.h"
#include "monster_test_bfbs_generated.h"
#include "monster_test_generated.h"
#include "optional_scalars_generated.h"
#include "test_assert.h"
#include "union_underlying_type_test_generated.h"

namespace flatbuffers {
namespace tests {

using namespace MyGame::Example;

// Check stringify of an default enum value to json
void JsonDefaultTest(const std::string& tests_data_path) {
  // load FlatBuffer schema (.fbs) from disk
  std::string schemafile;
  TEST_EQ(flatbuffers::LoadFile((tests_data_path + "monster_test.fbs").c_str(),
                                false, &schemafile),
          true);
  // parse schema first, so we can use it to parse the data after
  flatbuffers::Parser parser;
  auto include_test_path =
      flatbuffers::ConCatPathFileName(tests_data_path, "include_test");
  const char* include_directories[] = {tests_data_path.c_str(),
                                       include_test_path.c_str(), nullptr};

  TEST_EQ(parser.Parse(schemafile.c_str(), include_directories), true);
  // create incomplete monster and store to json
  parser.opts.output_default_scalars_in_json = true;
  parser.opts.output_enum_identifiers = true;
  flatbuffers::FlatBufferBuilder builder;
  auto name = builder.CreateString("default_enum");
  MonsterBuilder color_monster(builder);
  color_monster.add_name(name);
  FinishMonsterBuffer(builder, color_monster.Finish());
  std::string jsongen;
  auto result = GenText(parser, builder.GetBufferPointer(), &jsongen);
  TEST_NULL(result);
  // default value of the "color" field is Blue
  TEST_EQ(std::string::npos != jsongen.find("color: \"Blue\""), true);
  // default value of the "testf" field is 3.14159
  TEST_EQ(std::string::npos != jsongen.find("testf: 3.14159"), true);
}

void JsonEnumsTest(const std::string& tests_data_path) {
  // load FlatBuffer schema (.fbs) from disk
  std::string schemafile;
  TEST_EQ(flatbuffers::LoadFile((tests_data_path + "monster_test.fbs").c_str(),
                                false, &schemafile),
          true);
  // parse schema first, so we can use it to parse the data after
  flatbuffers::Parser parser;
  auto include_test_path =
      flatbuffers::ConCatPathFileName(tests_data_path, "include_test");
  const char* include_directories[] = {tests_data_path.c_str(),
                                       include_test_path.c_str(), nullptr};
  parser.opts.output_enum_identifiers = true;
  TEST_EQ(parser.Parse(schemafile.c_str(), include_directories), true);
  flatbuffers::FlatBufferBuilder builder;
  auto name = builder.CreateString("bitflag_enum");
  MonsterBuilder color_monster(builder);
  color_monster.add_name(name);
  color_monster.add_color(Color(Color_Blue | Color_Red));
  FinishMonsterBuffer(builder, color_monster.Finish());
  std::string jsongen;
  auto result = GenText(parser, builder.GetBufferPointer(), &jsongen);
  TEST_NULL(result);
  TEST_EQ(std::string::npos != jsongen.find("color: \"Red Blue\""), true);
  // Test forward compatibility with 'output_enum_identifiers = true'.
  // Current Color doesn't have '(1u << 2)' field, let's add it.
  builder.Clear();
  std::string future_json;
  auto future_name = builder.CreateString("future bitflag_enum");
  MonsterBuilder future_color(builder);
  future_color.add_name(future_name);
  future_color.add_color(
      static_cast<Color>((1u << 2) | Color_Blue | Color_Red));
  FinishMonsterBuffer(builder, future_color.Finish());
  result = GenText(parser, builder.GetBufferPointer(), &future_json);
  TEST_NULL(result);
  TEST_EQ(std::string::npos != future_json.find("color: 13"), true);
}

void JsonOptionalTest(const std::string& tests_data_path,
                      bool default_scalars) {
  // load FlatBuffer schema (.fbs) and JSON from disk
  std::string schemafile;
  std::string jsonfile;
  TEST_EQ(
      flatbuffers::LoadFile((tests_data_path + "optional_scalars.fbs").c_str(),
                            false, &schemafile),
      true);
  TEST_EQ(flatbuffers::LoadFile((tests_data_path + "optional_scalars" +
                                 (default_scalars ? "_defaults" : "") + ".json")
                                    .c_str(),
                                false, &jsonfile),
          true);

  auto include_test_path =
      flatbuffers::ConCatPathFileName(tests_data_path, "include_test");
  const char* include_directories[] = {tests_data_path.c_str(),
                                       include_test_path.c_str(), nullptr};

  // parse schema first, so we can use it to parse the data after
  flatbuffers::Parser parser;
  parser.opts.output_default_scalars_in_json = default_scalars;
  TEST_EQ(parser.Parse(schemafile.c_str(), include_directories), true);
  TEST_EQ(parser.ParseJson(jsonfile.c_str()), true);

  // here, parser.builder_ contains a binary buffer that is the parsed data.

  // First, verify it, just in case:
  flatbuffers::Verifier verifier(parser.builder_.GetBufferPointer(),
                                 parser.builder_.GetSize());
  TEST_EQ(optional_scalars::VerifyScalarStuffBuffer(verifier), true);

  // to ensure it is correct, we now generate text back from the binary,
  // and compare the two:
  std::string jsongen;
  auto result = GenText(parser, parser.builder_.GetBufferPointer(), &jsongen);
  TEST_NULL(result);
  TEST_EQ_STR(jsongen.c_str(), jsonfile.c_str());
}

void ParseIncorrectMonsterJsonTest(const std::string& tests_data_path) {
  std::string schemafile;
  TEST_EQ(flatbuffers::LoadFile((tests_data_path + "monster_test.bfbs").c_str(),
                                true, &schemafile),
          true);
  flatbuffers::Parser parser;
  flatbuffers::Verifier verifier(
      reinterpret_cast<const uint8_t*>(schemafile.c_str()), schemafile.size());
  TEST_EQ(reflection::VerifySchemaBuffer(verifier), true);
  TEST_EQ(
      parser.Deserialize(reinterpret_cast<const uint8_t*>(schemafile.c_str()),
                         schemafile.size()),
      true);
  TEST_EQ(parser.ParseJson("{name:\"monster\"}"), true);
  TEST_EQ(parser.ParseJson(""), false);
  TEST_EQ(parser.ParseJson("{name: 1}"), false);
  TEST_EQ(parser.ParseJson("{name:+1}"), false);
  TEST_EQ(parser.ParseJson("{name:-1}"), false);
  TEST_EQ(parser.ParseJson("{name:-f}"), false);
  TEST_EQ(parser.ParseJson("{name:+f}"), false);
}

// A union declared with an underlying type stores its types as that type:
// `union ABC: int { A = 555, B = 666, C = 777 }` makes test_union_type an int,
// and test_vector_of_union_type a vector of ints. JSON must accept their
// values, which do not fit in a byte, and store them at that width.
void JsonUnionUnderlyingTypeParseTest(const std::string& tests_data_path) {
  using namespace UnionUnderlyingType;
  std::string schemafile;
  TEST_EQ(flatbuffers::LoadFile(
              (tests_data_path + "union_underlying_type_test.fbs").c_str(),
              false, &schemafile),
          true);
  // Parses `json` as a D, checks that the generated verifier accepts it, and
  // returns whether it unpacks to `expected`.
  const auto parses_to = [&](const char* json, const DT& expected) {
    flatbuffers::Parser parser;
    TEST_EQ(parser.Parse(schemafile.c_str()), true);
    TEST_EQ(parser.SetRootType("UnionUnderlyingType.D"), true);
    if (!parser.ParseJson(json)) {
      TEST_OUTPUT_LINE("%s", parser.error_.c_str());
      return false;
    }
    flatbuffers::Verifier verifier(parser.builder_.GetBufferPointer(),
                                   parser.builder_.GetSize());
    TEST_EQ(verifier.VerifyBuffer<D>(nullptr), true);
    DT parsed;
    flatbuffers::GetRoot<D>(parser.builder_.GetBufferPointer())
        ->UnPackTo(&parsed);
    return parsed == expected;
  };

  AT a;
  a.a = 42;
  BT b;
  b.b = "foo";
  CT c;
  c.c = true;

  DT all;
  all.test_union.Set(c);
  all.test_vector_of_union.resize(3);
  all.test_vector_of_union[0].Set(a);
  all.test_vector_of_union[1].Set(b);
  all.test_vector_of_union[2].Set(c);
  TEST_EQ(parses_to("{ test_union_type: \"C\", test_union: { c: true }, "
                    "test_vector_of_union_type: [ \"A\", \"B\", \"C\" ], "
                    "test_vector_of_union: "
                    "[ { a: 42 }, { b: \"foo\" }, { c: true } ] }",
                    all),
          true);

  // A type after its union is found by scanning ahead for it.
  DT single_b;
  single_b.test_union.Set(b);
  TEST_EQ(parses_to("{ test_union: { b: \"foo\" }, test_union_type: \"B\" }",
                    single_b),
          true);

  // A type may be given as its value.
  DT single_a;
  single_a.test_union.Set(a);
  TEST_EQ(
      parses_to("{ test_union_type: 555, test_union: { a: 42 } }", single_a),
      true);

  // 811 names no member, though its low byte is A's, 555's: it is rejected,
  // in a union field and in a vector of unions.
  const char* unknown_types[] = {
      "{ test_union_type: 811, test_union: { a: 42 } }",
      "{ test_vector_of_union_type: [ 811 ], test_vector_of_union: [ { a: 42 } "
      "] }",
  };
  for (auto json : unknown_types) {
    flatbuffers::Parser parser;
    TEST_EQ(parser.Parse(schemafile.c_str()), true);
    TEST_EQ(parser.SetRootType("UnionUnderlyingType.D"), true);
    TEST_EQ(parser.ParseJson(json), false);
    TEST_EQ(parser.error_.find("illegal type id") != std::string::npos, true);
  }

  // An unsigned 64-bit type above INT64_MAX names its member too, in a union
  // field and in a vector of unions.
  flatbuffers::Parser wide;
  TEST_EQ(
      wide.Parse("table A { a: int; } table B { b: int; }"
                 "union W: ulong { A = 1, B = 9223372036854775808 }"
                 "table T { w: W; ws: [W]; } root_type T;"
                 "{ w_type: \"B\", w: { b: 7 },"
                 "  ws_type: [ \"B\", \"A\" ], ws: [ { b: 1 }, { a: 2 } ] }"),
      true);
  // Both are stored 8 bytes wide.
  const uint64_t b_type = 9223372036854775808ULL;
  auto t_def = wide.structs_.Lookup("T");
  TEST_NOTNULL(t_def);
  auto t = flatbuffers::GetRoot<flatbuffers::Table>(
      wide.builder_.GetBufferPointer());
  TEST_EQ(
      t->GetField<uint64_t>(t_def->fields.Lookup("w_type")->value.offset, 0),
      b_type);
  auto ws_type = t->GetPointer<const flatbuffers::Vector<uint64_t>*>(
      t_def->fields.Lookup("ws_type")->value.offset);
  TEST_NOTNULL(ws_type);
  TEST_EQ(ws_type->size(), 2u);
  TEST_EQ(ws_type->Get(0), b_type);
  TEST_EQ(ws_type->Get(1), uint64_t{1});
}

void JsonUnsortedArrayTest() {
  flatbuffers::Parser parser;
  TEST_EQ(parser.Deserialize(MyGame::Example::MonsterBinarySchema::data(),
                             MyGame::Example::MonsterBinarySchema::size()),
          true);
  auto jsonStr = R"(
  {
    "name": "lookupTest",
    "testarrayoftables": [
      { "name": "aaa" },
      { "name": "ccc" },
      { "name": "bbb" }
    ]
  }
  )";
  TEST_EQ(parser.ParseJson(jsonStr), true);
  auto monster = flatbuffers::GetRoot<MyGame::Example::Monster>(
      parser.builder_.GetBufferPointer());

  TEST_NOTNULL(monster->testarrayoftables()->LookupByKey("aaa"));
  TEST_NOTNULL(monster->testarrayoftables()->LookupByKey("bbb"));
  TEST_NOTNULL(monster->testarrayoftables()->LookupByKey("ccc"));
}

void JsonUnionStructTest() {
  // schema to parse data
  auto schema = R"(
struct MyStruct { field: int; }
union UnionWithStruct { MyStruct }
table JsonUnionStructTest { union_with_struct: UnionWithStruct; }
root_type JsonUnionStructTest;
)";
  // source text to parse and expected result of generation text back
  auto json_source = R"({
  union_with_struct_type: "MyStruct",
  union_with_struct: {
    field: 12345
  }
}
)";

  flatbuffers::Parser parser;
  // set output language to JSON, so we assure that is supported
  parser.opts.lang_to_generate = IDLOptions::kJson;
  // parse schema first, so we assure that output language is supported
  // and can use it to parse the data after
  TEST_EQ(true, parser.Parse(schema));
  TEST_EQ(true, parser.ParseJson(json_source));

  // now generate text back from the binary, and compare the two:
  std::string json_generated;
  auto generate_result =
      GenText(parser, parser.builder_.GetBufferPointer(), &json_generated);
  TEST_NULL(generate_result);
  TEST_EQ_STR(json_source, json_generated.c_str());
}

// A union declared with an underlying type stores its types as that type:
// `union ABC: int { A = 555, B = 666, C = 777 }` makes test_union_type an int,
// and test_vector_of_union_type a vector of ints. The text generator must read
// them at that width, to name them and to find the members they type.
void JsonUnionUnderlyingTypeGenTextTest(const std::string& tests_data_path) {
  using namespace UnionUnderlyingType;
  std::string schemafile;
  TEST_EQ(flatbuffers::LoadFile(
              (tests_data_path + "union_underlying_type_test.fbs").c_str(),
              false, &schemafile),
          true);
  flatbuffers::Parser parser;
  TEST_EQ(parser.Parse(schemafile.c_str()), true);
  TEST_EQ(parser.SetRootType("UnionUnderlyingType.D"), true);

  AT a;
  a.a = 42;
  BT b;
  b.b = "foo";
  CT c;
  c.c = true;
  DT d;
  d.test_union.Set(c);
  d.test_vector_of_union.resize(3);
  d.test_vector_of_union[0].Set(a);
  d.test_vector_of_union[1].Set(b);
  d.test_vector_of_union[2].Set(c);
  flatbuffers::FlatBufferBuilder fbb;
  fbb.Finish(D::Pack(fbb, &d));

  std::string json;
  TEST_NULL(GenText(parser, fbb.GetBufferPointer(), &json));
  TEST_EQ_STR(json.c_str(),
              "{\n"
              "  test_union_type: \"C\",\n"
              "  test_union: {\n"
              "    c: true\n"
              "  },\n"
              "  test_vector_of_union_type: [\n"
              "    \"A\",\n"
              "    \"B\",\n"
              "    \"C\"\n"
              "  ],\n"
              "  test_vector_of_union: [\n"
              "    {\n"
              "      a: 42\n"
              "    },\n"
              "    {\n"
              "      b: \"foo\"\n"
              "    },\n"
              "    {\n"
              "      c: true\n"
              "    }\n"
              "  ]\n"
              "}\n");
}

}  // namespace tests
}  // namespace flatbuffers
