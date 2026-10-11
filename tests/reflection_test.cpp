#include "reflection_test.h"

#include <vector>
#include <cstring>
#include <memory>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#include <unistd.h>
#endif

#include "flatbuffers/idl.h"
#include "flatbuffers/minireflect.h"
#include "flatbuffers/reflection.h"
#include "flatbuffers/reflection_generated.h"
#include "flatbuffers/verifier.h"
#include "monster_test.h"
#include "monster_test_generated.h"
#include "test_assert.h"
#include "tests/arrays_test_generated.h"
#include "tests/union_underlying_type_test_generated.h"
#include "tests/union_value_gap_test_generated.h"
#include "tests/union_underlying_type_test_generated.h"
#include "tests/union_vector/union_vector_generated.h"

namespace flatbuffers {
namespace tests {

using namespace MyGame::Example;

void ReflectionTest(const std::string& tests_data_path, uint8_t* flatbuf,
                    size_t length) {
  // Load a binary schema.
  std::string bfbsfile;
  TEST_EQ(flatbuffers::LoadFile((tests_data_path + "monster_test.bfbs").c_str(),
                                true, &bfbsfile),
          true);

  // Verify it, just in case:
  flatbuffers::Verifier verifier(
      reinterpret_cast<const uint8_t*>(bfbsfile.c_str()), bfbsfile.length());
  TEST_EQ(reflection::VerifySchemaBuffer(verifier), true);

  // Make sure the schema is what we expect it to be.
  auto& schema = *reflection::GetSchema(bfbsfile.c_str());
  auto root_table = schema.root_table();

  // Check the declaration files.
  TEST_EQ_STR(root_table->name()->c_str(), "MyGame.Example.Monster");
  TEST_EQ_STR(root_table->declaration_file()->c_str(), "//monster_test.fbs");
  TEST_EQ_STR(
      schema.objects()->LookupByKey("TableA")->declaration_file()->c_str(),
      "//include_test/include_test1.fbs");
  TEST_EQ_STR(schema.objects()
                  ->LookupByKey("MyGame.OtherNameSpace.Unused")
                  ->declaration_file()
                  ->c_str(),
              "//include_test/sub/include_test2.fbs");
  TEST_EQ_STR(schema.enums()
                  ->LookupByKey("MyGame.OtherNameSpace.FromInclude")
                  ->declaration_file()
                  ->c_str(),
              "//include_test/sub/include_test2.fbs");

  // Check scheam filenames and their includes.
  TEST_EQ(schema.fbs_files()->size(), 3);

  const auto fbs0 = schema.fbs_files()->Get(0);
  TEST_EQ_STR(fbs0->filename()->c_str(), "//include_test/include_test1.fbs");
  const auto fbs0_includes = fbs0->included_filenames();
  TEST_EQ(fbs0_includes->size(), 2);

  // TODO(caspern): Should we force or disallow inclusion of self?
  TEST_EQ_STR(fbs0_includes->Get(0)->c_str(),
              "//include_test/include_test1.fbs");
  TEST_EQ_STR(fbs0_includes->Get(1)->c_str(),
              "//include_test/sub/include_test2.fbs");

  const auto fbs1 = schema.fbs_files()->Get(1);
  TEST_EQ_STR(fbs1->filename()->c_str(),
              "//include_test/sub/include_test2.fbs");
  const auto fbs1_includes = fbs1->included_filenames();
  TEST_EQ(fbs1_includes->size(), 2);
  TEST_EQ_STR(fbs1_includes->Get(0)->c_str(),
              "//include_test/include_test1.fbs");
  TEST_EQ_STR(fbs1_includes->Get(1)->c_str(),
              "//include_test/sub/include_test2.fbs");

  const auto fbs2 = schema.fbs_files()->Get(2);
  TEST_EQ_STR(fbs2->filename()->c_str(), "//monster_test.fbs");
  const auto fbs2_includes = fbs2->included_filenames();
  TEST_EQ(fbs2_includes->size(), 1);
  TEST_EQ_STR(fbs2_includes->Get(0)->c_str(),
              "//include_test/include_test1.fbs");

  // Check Root table fields
  auto fields = root_table->fields();
  auto hp_field_ptr = fields->LookupByKey("hp");
  TEST_NOTNULL(hp_field_ptr);
  auto& hp_field = *hp_field_ptr;
  TEST_EQ_STR(hp_field.name()->c_str(), "hp");
  TEST_EQ(hp_field.id(), 2);
  TEST_EQ(hp_field.type()->base_type(), reflection::Short);

  auto friendly_field_ptr = fields->LookupByKey("friendly");
  TEST_NOTNULL(friendly_field_ptr);
  TEST_NOTNULL(friendly_field_ptr->attributes());
  TEST_NOTNULL(friendly_field_ptr->attributes()->LookupByKey("priority"));

  // A union declared without an underlying type stores its type as a UType.
  auto test_type_field_ptr = fields->LookupByKey("test_type");
  TEST_NOTNULL(test_type_field_ptr);
  TEST_EQ(flatbuffers::GetUnionUnderlyingType(schema, *test_type_field_ptr),
          reflection::UType);

  // Make sure the table index is what we expect it to be.
  auto pos_field_ptr = fields->LookupByKey("pos");
  TEST_NOTNULL(pos_field_ptr);
  TEST_EQ(pos_field_ptr->type()->base_type(), reflection::Obj);
  auto pos_table_ptr = schema.objects()->Get(pos_field_ptr->type()->index());
  TEST_NOTNULL(pos_table_ptr);
  TEST_EQ_STR(pos_table_ptr->name()->c_str(), "MyGame.Example.Vec3");

  // Test nullability of fields: hp is a 0-default scalar, pos is a struct =>
  // optional, and name is a required string => not optional.
  TEST_EQ(hp_field.optional(), false);
  TEST_EQ(pos_field_ptr->optional(), true);
  TEST_EQ(fields->LookupByKey("name")->optional(), false);

  // Now use it to dynamically access a buffer.
  auto& root = *flatbuffers::GetAnyRoot(flatbuf);

  // Verify the buffer first using reflection based verification
  TEST_EQ(flatbuffers::Verify(schema, *schema.root_table(), flatbuf, length),
          true);

  auto hp = flatbuffers::GetFieldI<uint16_t>(root, hp_field);
  TEST_EQ(hp, 80);

  // Rather than needing to know the type, we can also get the value of
  // any field as an int64_t/double/string, regardless of what it actually is.
  auto hp_int64 = flatbuffers::GetAnyFieldI(root, hp_field);
  TEST_EQ(hp_int64, 80);
  auto hp_double = flatbuffers::GetAnyFieldF(root, hp_field);
  TEST_EQ(hp_double, 80.0);
  auto hp_string = flatbuffers::GetAnyFieldS(root, hp_field, &schema);
  TEST_EQ_STR(hp_string.c_str(), "80");
  // A union's type is read as its union stores it: here as a UType.
  TEST_EQ_STR(
      flatbuffers::GetAnyFieldS(root, *test_type_field_ptr, &schema).c_str(),
      "1");

  // Get struct field through reflection
  auto pos_struct = flatbuffers::GetFieldStruct(root, *pos_field_ptr);
  TEST_NOTNULL(pos_struct);
  TEST_EQ(flatbuffers::GetAnyFieldF(*pos_struct,
                                    *pos_table_ptr->fields()->LookupByKey("z")),
          3.0f);

  auto test3_field = pos_table_ptr->fields()->LookupByKey("test3");
  auto test3_struct = flatbuffers::GetFieldStruct(*pos_struct, *test3_field);
  TEST_NOTNULL(test3_struct);
  auto test3_object = schema.objects()->Get(test3_field->type()->index());

  TEST_EQ(flatbuffers::GetAnyFieldF(*test3_struct,
                                    *test3_object->fields()->LookupByKey("a")),
          10);

  // We can also modify it.
  flatbuffers::SetField<uint16_t>(&root, hp_field, 200);
  hp = flatbuffers::GetFieldI<uint16_t>(root, hp_field);
  TEST_EQ(hp, 200);

  // We can also set fields generically:
  flatbuffers::SetAnyFieldI(&root, hp_field, 300);
  hp_int64 = flatbuffers::GetAnyFieldI(root, hp_field);
  TEST_EQ(hp_int64, 300);
  flatbuffers::SetAnyFieldF(&root, hp_field, 300.5);
  hp_int64 = flatbuffers::GetAnyFieldI(root, hp_field);
  TEST_EQ(hp_int64, 300);
  flatbuffers::SetAnyFieldS(&root, hp_field, "300");
  hp_int64 = flatbuffers::GetAnyFieldI(root, hp_field);
  TEST_EQ(hp_int64, 300);

  // Test buffer is valid after the modifications
  TEST_EQ(flatbuffers::Verify(schema, *schema.root_table(), flatbuf, length),
          true);

  // Reset it, for further tests.
  flatbuffers::SetField<uint16_t>(&root, hp_field, 80);

  // More advanced functionality: changing the size of items in-line!
  // First we put the FlatBuffer inside an std::vector.
  std::vector<uint8_t> resizingbuf(flatbuf, flatbuf + length);
  // Find the field we want to modify.
  auto& name_field = *fields->LookupByKey("name");
  // Get the root.
  // This time we wrap the result from GetAnyRoot in a smartpointer that
  // will keep rroot valid as resizingbuf resizes.
  auto rroot = flatbuffers::piv(flatbuffers::GetAnyRoot(resizingbuf.data()),
                                resizingbuf);
  SetString(schema, "totally new string", GetFieldS(**rroot, name_field),
            &resizingbuf);
  // Here resizingbuf has changed, but rroot is still valid.
  TEST_EQ_STR(GetFieldS(**rroot, name_field)->c_str(), "totally new string");
  // Now lets extend a vector by 100 elements (10 -> 110).
  auto& inventory_field = *fields->LookupByKey("inventory");
  auto rinventory = flatbuffers::piv(
      flatbuffers::GetFieldV<uint8_t>(**rroot, inventory_field), resizingbuf);
  flatbuffers::ResizeVector<uint8_t>(schema, 110, 50, *rinventory,
                                     &resizingbuf);
  // rinventory still valid, so lets read from it.
  TEST_EQ(rinventory->Get(10), 50);

  // For reflection uses not covered already, there is a more powerful way:
  // we can simply generate whatever object we want to add/modify in a
  // FlatBuffer of its own, then add that to an existing FlatBuffer:
  // As an example, let's add a string to an array of strings.
  // First, find our field:
  auto& testarrayofstring_field = *fields->LookupByKey("testarrayofstring");
  // Find the vector value:
  auto rtestarrayofstring = flatbuffers::piv(
      flatbuffers::GetFieldV<flatbuffers::Offset<flatbuffers::String>>(
          **rroot, testarrayofstring_field),
      resizingbuf);
  // It's a vector of 2 strings, to which we add one more, initialized to
  // offset 0.
  flatbuffers::ResizeVector<flatbuffers::Offset<flatbuffers::String>>(
      schema, 3, 0, *rtestarrayofstring, &resizingbuf);
  // Here we just create a buffer that contans a single string, but this
  // could also be any complex set of tables and other values.
  flatbuffers::FlatBufferBuilder stringfbb;
  stringfbb.Finish(stringfbb.CreateString("hank"));
  // Add the contents of it to our existing FlatBuffer.
  // We do this last, so the pointer doesn't get invalidated (since it is
  // at the end of the buffer):
  auto string_ptr = flatbuffers::AddFlatBuffer(
      resizingbuf, stringfbb.GetBufferPointer(), stringfbb.GetSize());
  // Finally, set the new value in the vector.
  rtestarrayofstring->MutateOffset(2, string_ptr);
  TEST_EQ_STR(rtestarrayofstring->Get(0)->c_str(), "bob");
  TEST_EQ_STR(rtestarrayofstring->Get(2)->c_str(), "hank");
  // Test integrity of all resize operations above.
  flatbuffers::Verifier resize_verifier(
      reinterpret_cast<const uint8_t*>(resizingbuf.data()), resizingbuf.size());
  TEST_EQ(VerifyMonsterBuffer(resize_verifier), true);

  // Test buffer is valid using reflection as well
  TEST_EQ(flatbuffers::Verify(schema, *schema.root_table(), resizingbuf.data(),
                              resizingbuf.size()),
          true);

  // As an additional test, also set it on the name field.
  // Note: unlike the name change above, this just overwrites the offset,
  // rather than changing the string in-place.
  SetFieldT(*rroot, name_field, string_ptr);
  TEST_EQ_STR(GetFieldS(**rroot, name_field)->c_str(), "hank");

  // Using reflection, rather than mutating binary FlatBuffers, we can also copy
  // tables and other things out of other FlatBuffers into a FlatBufferBuilder,
  // either part or whole.
  flatbuffers::FlatBufferBuilder fbb;
  auto root_offset = flatbuffers::CopyTable(
      fbb, schema, *root_table, *flatbuffers::GetAnyRoot(flatbuf), true);
  fbb.Finish(root_offset, MonsterIdentifier());
  // Test that it was copied correctly:
  AccessFlatBufferTest(fbb.GetBufferPointer(), fbb.GetSize());

  // Test buffer is valid using reflection as well
  TEST_EQ(flatbuffers::Verify(schema, *schema.root_table(),
                              fbb.GetBufferPointer(), fbb.GetSize()),
          true);
}

// Packs `d`, checks that the generated verifier accepts it, and returns
// whether the reflection verifier accepts the same bytes.
static bool ReflectionVerifiesD(const reflection::Schema& schema,
                                const reflection::Object& d_def,
                                const UnionUnderlyingType::DT& d) {
  flatbuffers::FlatBufferBuilder fbb;
  fbb.Finish(UnionUnderlyingType::D::Pack(fbb, &d));
  flatbuffers::Verifier verifier(fbb.GetBufferPointer(), fbb.GetSize());
  TEST_EQ(verifier.VerifyBuffer<UnionUnderlyingType::D>(nullptr), true);
  return flatbuffers::Verify(schema, d_def, fbb.GetBufferPointer(),
                             fbb.GetSize());
}

// A union declared with an underlying type stores its types as that type, not
// as a 1-byte UType: `union ABC: int` makes test_union_type an int, and
// test_vector_of_union_type a vector of ints, though the schema still gives
// both fields the base type UType.
void ReflectionUnionUnderlyingTypeTest(const std::string& tests_data_path) {
  using namespace UnionUnderlyingType;

  std::string schemafile;
  TEST_EQ(flatbuffers::LoadFile(
              (tests_data_path + "union_underlying_type_test.fbs").c_str(),
              false, &schemafile),
          true);
  flatbuffers::Parser parser;
  TEST_EQ(parser.Parse(schemafile.c_str()), true);
  parser.Serialize();
  auto& schema = *reflection::GetSchema(parser.builder_.GetBufferPointer());
  auto d_def = schema.objects()->LookupByKey("UnionUnderlyingType.D");
  TEST_NOTNULL(d_def);
  auto union_field = d_def->fields()->LookupByKey("test_union");
  auto type_field = d_def->fields()->LookupByKey("test_union_type");
  auto types_field = d_def->fields()->LookupByKey("test_vector_of_union_type");
  TEST_NOTNULL(union_field);
  TEST_NOTNULL(type_field);
  TEST_NOTNULL(types_field);

  // The premise: to their fields the types are UTypes; to the union, ints.
  TEST_EQ(type_field->type()->base_type(), reflection::UType);
  TEST_EQ(types_field->type()->element(), reflection::UType);
  TEST_EQ(flatbuffers::GetUnionUnderlyingType(schema, *union_field),
          reflection::Int);
  TEST_EQ(flatbuffers::GetUnionUnderlyingType(schema, *type_field),
          reflection::Int);
  TEST_EQ(flatbuffers::GetUnionUnderlyingType(schema, *types_field),
          reflection::Int);

  AT a;
  a.a = 42;
  BT b;
  b.b = "foo";
  CT c;
  c.c = true;

  DT none;
  TEST_EQ(ReflectionVerifiesD(schema, *d_def, none), true);

  DT single_b;
  single_b.test_union.Set(b);
  TEST_EQ(ReflectionVerifiesD(schema, *d_def, single_b), true);

  DT many;
  many.test_vector_of_union.resize(3);
  many.test_vector_of_union[0].Set(a);
  many.test_vector_of_union[1].Set(b);
  many.test_vector_of_union[2].Set(c);
  TEST_EQ(ReflectionVerifiesD(schema, *d_def, many), true);

  DT both = many;
  both.test_union.Set(b);
  TEST_EQ(ReflectionVerifiesD(schema, *d_def, both), true);

  // 512 names no member. The generated verifier accepts an unknown type for
  // forward compatibility; reflection verifies against the schema it is given,
  // so it rejects one. Read as a byte, 512 was 0, NONE, and passed unchecked.
  DT unknown;
  unknown.test_union.type = static_cast<ABC>(512);
  TEST_EQ(ReflectionVerifiesD(schema, *d_def, unknown), false);

  // Pack() writes the types of a vector of unions before its members, so the
  // last type of `many` ends the buffer. Cut off the last byte of that type:
  // the buffer is now short, though the 3 bytes the vector holds as 1-byte
  // types are all still there. What is left is copied to a buffer of its own,
  // so that a sanitizer reports any read past its end.
  {
    flatbuffers::FlatBufferBuilder fbb;
    fbb.Finish(D::Pack(fbb, &many));
    const uint8_t* buf = fbb.GetBufferPointer();
    const size_t size = fbb.GetSize();
    auto types = GetRoot<D>(buf)->test_vector_of_union_type();
    TEST_NOTNULL(types);
    TEST_EQ(types->Data() + types->size() * sizeof(ABC) == buf + size, true);
    std::vector<uint8_t> truncated(buf, buf + size - 1);
    flatbuffers::Verifier verifier(truncated.data(), truncated.size());
    TEST_EQ(verifier.VerifyBuffer<D>(nullptr), false);
    TEST_EQ(
        flatbuffers::Verify(schema, *d_def, truncated.data(), truncated.size()),
        false);
  }

  flatbuffers::FlatBufferBuilder fbb;
  fbb.Finish(D::Pack(fbb, &single_b));
  auto root = flatbuffers::GetAnyRoot(fbb.GetBufferPointer());
  TEST_EQ_STR(flatbuffers::GetUnionType(schema, *d_def, *union_field, *root)
                  .name()
                  ->c_str(),
              "UnionUnderlyingType.B");

  // CopyTable() copies the type, an int, and the member it names.
  {
    flatbuffers::FlatBufferBuilder copy;
    copy.Finish(flatbuffers::CopyTable(copy, schema, *d_def, *root));
    flatbuffers::Verifier verifier(copy.GetBufferPointer(), copy.GetSize());
    TEST_EQ(verifier.VerifyBuffer<D>(nullptr), true);
    DT copied;
    GetRoot<D>(copy.GetBufferPointer())->UnPackTo(&copied);
    TEST_ASSERT(copied == single_b);
  }

  // It copies the types of a vector of unions as a vector of scalars, each as
  // wide as an int. Only the types are compared: CopyTable() copies the vector
  // of unions itself as if it held scalars too, so its members are not copied.
  {
    flatbuffers::FlatBufferBuilder src;
    src.Finish(D::Pack(src, &many));
    flatbuffers::FlatBufferBuilder copy;
    copy.Finish(flatbuffers::CopyTable(
        copy, schema, *d_def,
        *flatbuffers::GetAnyRoot(src.GetBufferPointer())));
    auto types =
        GetRoot<D>(copy.GetBufferPointer())->test_vector_of_union_type();
    TEST_NOTNULL(types);
    TEST_EQ(types->size(), 3u);
    TEST_EQ(types->Get(0), ABC::A);
    TEST_EQ(types->Get(1), ABC::B);
    TEST_EQ(types->Get(2), ABC::C);
  }

  // SetString() resizes the buffer, so ResizeContext adjusts every offset in
  // it, recursing into the member the union's type names.
  {
    std::vector<uint8_t> buf(fbb.GetBufferPointer(),
                             fbb.GetBufferPointer() + fbb.GetSize());
    flatbuffers::SetString(schema, "foobar",
                           GetRoot<D>(buf.data())->test_union_as_B()->b(), &buf,
                           d_def);
    flatbuffers::Verifier verifier(buf.data(), buf.size());
    TEST_EQ(verifier.VerifyBuffer<D>(nullptr), true);
    TEST_EQ(flatbuffers::Verify(schema, *d_def, buf.data(), buf.size()), true);
    auto resized = GetRoot<D>(buf.data())->test_union_as_B();
    TEST_NOTNULL(resized);
    TEST_EQ_STR(resized->b()->c_str(), "foobar");
  }
}

// Packs `holder`, checks that the generated verifier accepts it, and returns
// whether the reflection verifier accepts the same bytes.
static bool ReflectionVerifiesGapHolder(const reflection::Schema& schema,
                                        const UnionValueGap::HolderT& holder) {
  flatbuffers::FlatBufferBuilder fbb;
  UnionValueGap::FinishHolderBuffer(fbb,
                                    UnionValueGap::Holder::Pack(fbb, &holder));
  flatbuffers::Verifier verifier(fbb.GetBufferPointer(), fbb.GetSize());
  TEST_EQ(UnionValueGap::VerifyHolderBuffer(verifier), true);
  return flatbuffers::Verify(schema, *schema.root_table(),
                             fbb.GetBufferPointer(), fbb.GetSize());
}

// Union members are keyed by value, not by their position in the union's list
// of values. The reflection verifier used to index that list by the type, so
// with `union Gap { Alpha = 1, Beta = 3, Gamma = 4 }` it verified a Beta as
// the Gamma at position 3 and rejected a Gamma as out of range, though the
// generated verifier accepts both.
void ReflectionVerifyUnionByValueTest(const std::string& tests_data_path) {
  std::string schemafile;
  TEST_EQ(flatbuffers::LoadFile(
              (tests_data_path + "union_value_gap_test.fbs").c_str(), false,
              &schemafile),
          true);
  flatbuffers::IDLOptions idl_opts;
  idl_opts.lang_to_generate |= flatbuffers::IDLOptions::kBinary;
  flatbuffers::Parser parser(idl_opts);
  TEST_EQ(parser.Parse(schemafile.c_str()), true);
  parser.Serialize();
  auto schema = reflection::GetSchema(parser.builder_.GetBufferPointer());

  // The premise: Beta and Gamma do not sit at the positions their values name.
  auto gap = schema->enums()->LookupByKey("UnionValueGap.Gap");
  TEST_NOTNULL(gap);
  TEST_EQ(gap->values()->size(), 4u);
  TEST_EQ(gap->values()->Get(2)->value(), 3);
  TEST_EQ(gap->values()->Get(3)->value(), 4);

  UnionValueGap::BetaT beta;
  beta.b = 1000000;  // Read as Gamma's string offset, it points off the end.
  UnionValueGap::GammaT gamma;
  gamma.g = "gamma";

  UnionValueGap::HolderT none;
  TEST_EQ(ReflectionVerifiesGapHolder(*schema, none), true);

  UnionValueGap::HolderT single_alpha;
  single_alpha.single.Set(UnionValueGap::AlphaT());
  TEST_EQ(ReflectionVerifiesGapHolder(*schema, single_alpha), true);

  UnionValueGap::HolderT single_beta;
  single_beta.single.Set(beta);
  TEST_EQ(ReflectionVerifiesGapHolder(*schema, single_beta), true);

  UnionValueGap::HolderT single_gamma;
  single_gamma.single.Set(gamma);
  TEST_EQ(ReflectionVerifiesGapHolder(*schema, single_gamma), true);

  UnionValueGap::HolderT many;
  many.many.resize(3);
  many.many[0].Set(UnionValueGap::AlphaT());
  many.many[1].Set(beta);
  many.many[2].Set(gamma);
  TEST_EQ(ReflectionVerifiesGapHolder(*schema, many), true);

  // 2 lies in the gap and names no member. The generated verifier accepts an
  // unknown type for forward compatibility; reflection verifies against the
  // schema it is given, so it rejects one.
  UnionValueGap::HolderT unknown;
  unknown.single.type = static_cast<UnionValueGap::Gap>(2);
  TEST_EQ(ReflectionVerifiesGapHolder(*schema, unknown), false);
}

// SetString must leave every other value in the buffer as it was, whether the
// string shrinks or grows. A shrink used to remove the bytes just before the
// string's characters, i.e. its own length field and the end of whatever
// precedes it in the buffer, rather than the tail of the old characters. The
// result still passed both verifiers, but had silently lost the inventory and
// the strings.
void SetStringTest(const std::string& tests_data_path) {
  std::string bfbsfile;
  TEST_EQ(flatbuffers::LoadFile((tests_data_path + "monster_test.bfbs").c_str(),
                                true, &bfbsfile),
          true);
  auto& schema = *reflection::GetSchema(bfbsfile.c_str());
  auto fields = schema.root_table()->fields();
  auto& name_field = *fields->LookupByKey("name");
  auto& strings_field = *fields->LookupByKey("testarrayofstring");

  MonsterT expected;
  expected.name = "a monster name long enough to shrink by more than eight";
  expected.inventory = {1, 2, 3};
  expected.testarrayofstring = {"bob", "fred"};
  // NaN never compares equal, so with its default the generated operator==
  // would reject every MonsterT, however faithfully it was recovered.
  expected.nan_default = 0.0f;
  flatbuffers::FlatBufferBuilder fbb;
  FinishMonsterBuffer(fbb, Monster::Pack(fbb, &expected));
  std::vector<uint8_t> buf(fbb.GetBufferPointer(),
                           fbb.GetBufferPointer() + fbb.GetSize());

  const char* const longer =
      "a string longer than the original name, so the buffer grows";
  struct Step {
    bool element;  // The first of testarrayofstring, rather than the name.
    const char* value;
  };
  const Step steps[] = {
      // More than sizeof(largest_scalar_t), and not a multiple of it.
      {false, "short"},
      // Less than sizeof(largest_scalar_t): rewritten in place, no resize.
      {false, "shor"},
      {false, longer},
      {false, ""},
      {true, longer},
      {true, "bo"},
  };
  for (const auto& step : steps) {
    auto& root = *flatbuffers::GetAnyRoot(buf.data());
    auto str = step.element
                   ? flatbuffers::GetFieldV<flatbuffers::Offset<String>>(
                         root, strings_field)
                         ->Get(0)
                   : GetFieldS(root, name_field);
    SetString(schema, step.value, str, &buf);
    (step.element ? expected.testarrayofstring[0] : expected.name) = step.value;

    flatbuffers::Verifier verifier(buf.data(), buf.size());
    TEST_EQ(VerifyMonsterBuffer(verifier), true);
    TEST_EQ(flatbuffers::Verify(schema, *schema.root_table(), buf.data(),
                                buf.size()),
            true);
    auto actual = UnPackMonster(buf.data());
    TEST_EQ(actual->inventory.size(), expected.inventory.size());
    TEST_EQ(actual->testarrayofstring.size(),
            expected.testarrayofstring.size());
    TEST_ASSERT(*actual == expected);
  }
}

// Calls `verify` on a copy of `buf` that ends where a page that faults on any
// access begins, so a verifier that reads past the end of the buffer crashes
// in every build type. Past an ordinary allocation it reads whatever is there,
// which only a sanitizer build (FLATBUFFERS_CODE_SANITIZE) reports, and CI
// runs none. Without mmap(), `verify` gets `buf` itself.
template <typename F>
static bool VerifyBeforeGuardPage(const std::vector<uint8_t>& buf, F verify) {
#if defined(__unix__) || defined(__APPLE__)
  const size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
  const size_t mapped = (buf.size() / page + 2) * page;
  void* const map = mmap(nullptr, mapped, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  TEST_ASSERT(map != MAP_FAILED);
  uint8_t* const guard = static_cast<uint8_t*>(map) + mapped - page;
  TEST_EQ(mprotect(guard, page, PROT_NONE), 0);
  // FlatBufferBuilder pads a buffer to its alignment, so the copy keeps it.
  uint8_t* const copy = guard - buf.size();
  std::memcpy(copy, buf.data(), buf.size());
  const bool verified = verify(copy, buf.size());
  TEST_EQ(munmap(map, mapped), 0);
  return verified;
#else
  return verify(buf.data(), buf.size());
#endif
}

// The position in `buf` of the root table's vtable entry for `field`, which
// the vtable must be long enough to hold.
static size_t RootVTableEntry(const std::vector<uint8_t>& buf,
                              const voffset_t field) {
  const uint8_t* const vtable = GetAnyRoot(buf.data())->GetVTable();
  TEST_ASSERT(field < ReadScalar<voffset_t>(vtable));
  return static_cast<size_t>(vtable - buf.data()) + field;
}

// `buf` with the root table's slot for `field` moved to the end of the buffer,
// wholly past it.
static std::vector<uint8_t> WithSlotPastEnd(std::vector<uint8_t> buf,
                                            const voffset_t field) {
  const size_t table = static_cast<size_t>(
      reinterpret_cast<const uint8_t*>(GetAnyRoot(buf.data())) - buf.data());
  WriteScalar(buf.data() + RootVTableEntry(buf, field),
              static_cast<voffset_t>(buf.size() - table));
  return buf;
}

// `buf` with `field` absent from the root table.
static std::vector<uint8_t> WithFieldAbsent(std::vector<uint8_t> buf,
                                            const voffset_t field) {
  WriteScalar<voffset_t>(buf.data() + RootVTableEntry(buf, field), 0);
  return buf;
}

// `buf` with `offset` in the root table's slot for `field`.
static std::vector<uint8_t> WithOffset(std::vector<uint8_t> buf,
                                       const voffset_t field,
                                       const uoffset_t offset) {
  const uint8_t* const slot = GetAnyRoot(buf.data())->GetAddressOf(field);
  TEST_NOTNULL(slot);
  WriteScalar(buf.data() + (slot - buf.data()), offset);
  return buf;
}

// Expects the generated and the reflection verifier both to reject `buf`, and
// names it by `what` if one does not. Each verifies it where it is, on the
// heap, so a sanitizer build reports a read past its end, and again before a
// guard page.
template <typename G, typename R>
static void TestRejected(G generated, R reflected,
                         const std::vector<uint8_t>& buf,
                         const std::string& what) {
  const bool generated_accepts = generated(buf.data(), buf.size()) ||
                                 VerifyBeforeGuardPage(buf, generated);
  const bool reflected_accepts = reflected(buf.data(), buf.size()) ||
                                 VerifyBeforeGuardPage(buf, reflected);
  if (generated_accepts) {
    TEST_OUTPUT_LINE("Generated verifier accepts %s", what.c_str());
  }
  if (reflected_accepts) {
    TEST_OUTPUT_LINE("Reflection verifier accepts %s", what.c_str());
  }
  TEST_EQ(generated_accepts, false);
  TEST_EQ(reflected_accepts, false);
}

// Expects both verifiers to reject `buf` with the slot of each field set in
// its root table, in turn, moved past its end.
template <typename G, typename R>
static void TestSlotsPastEnd(G generated, R reflected,
                             const reflection::Schema& schema,
                             const std::vector<uint8_t>& buf) {
  const Table& root = *GetAnyRoot(buf.data());
  for (const reflection::Field* const field : *schema.root_table()->fields()) {
    if (!root.GetAddressOf(field->offset())) continue;
    TestRejected(generated, reflected, WithSlotPastEnd(buf, field->offset()),
                 field->name()->str() + " with its slot past the end");
  }
}

// The generated verifier calls VerifyOffset(), or VerifyOffsetRequired(), on
// the slot of every string, vector, table and union field before it reads the
// offset there. That is what bounds the slot by the buffer, as the vtable can
// put a field anywhere in the 64KiB past its table. It also rejects an offset
// of 0, which points a field at its own slot, and one that is negative read
// as signed. The reflection verifier must reject the same buffers, without
// reading a slot it has not checked.
void ReflectionVerifyOffsetFieldsTest() {
  Verifier schema_verifier(Monster::BinarySchema::data(),
                           Monster::BinarySchema::size());
  TEST_EQ(reflection::VerifySchemaBuffer(schema_verifier), true);
  const reflection::Schema& schema =
      *reflection::GetSchema(Monster::BinarySchema::data());
  // `name` is the key, which makes it required.
  TEST_EQ(schema.root_table()->fields()->LookupByKey("name")->required(), true);

  const auto generated = [](const uint8_t* buf, size_t length) -> bool {
    Verifier verifier(buf, length);
    return VerifyMonsterBuffer(verifier);
  };
  const auto reflected = [&schema](const uint8_t* buf, size_t length) -> bool {
    return flatbuffers::Verify(schema, *schema.root_table(), buf, length);
  };

  // A string, a vector, two tables and a union holding a table. testempty and
  // the union's table have no required field, so an offset of 0, which puts
  // an empty table on the slot, is rejected only for being 0. enemy's
  // required name rejects it too.
  MonsterT monster;
  monster.name = "Wilma";
  monster.inventory = {1, 2, 3};
  monster.enemy.reset(new MonsterT());
  monster.enemy->name = "Fred";
  monster.testempty.reset(new StatT());
  monster.test.Set(TestSimpleTableWithEnumT());
  FlatBufferBuilder fbb;
  FinishMonsterBuffer(fbb, Monster::Pack(fbb, &monster));
  const std::vector<uint8_t> buf(fbb.GetBufferPointer(),
                                 fbb.GetBufferPointer() + fbb.GetSize());
  // MonsterT's operator== is never true, as its nan_default field is unequal
  // to itself, so compare the fields set here.
  const std::unique_ptr<MonsterT> unpacked(GetMonster(buf.data())->UnPack());
  TEST_EQ_STR(unpacked->name.c_str(), "Wilma");
  TEST_ASSERT(unpacked->inventory == monster.inventory);
  TEST_EQ_STR(unpacked->enemy->name.c_str(), "Fred");
  TEST_NOTNULL(unpacked->testempty.get());
  TEST_ASSERT(unpacked->test.type == Any_TestSimpleTableWithEnum);
  TEST_EQ(generated(buf.data(), buf.size()), true);
  TEST_EQ(reflected(buf.data(), buf.size()), true);
  TEST_EQ(VerifyBeforeGuardPage(buf, reflected), true);

  // Among them `test_type`, the union's type, which the union field reads: it
  // sorts after `test`.
  TestSlotsPastEnd(generated, reflected, schema, buf);

  TestRejected(generated, reflected, WithFieldAbsent(buf, Monster::VT_NAME),
               "a Monster without its required name");

  // Each offset field set above with an offset of 0.
  const Table& root = *GetAnyRoot(buf.data());
  size_t offset_fields = 0;
  for (const reflection::Field* const field : *schema.root_table()->fields()) {
    const reflection::BaseType type = field->type()->base_type();
    const bool is_offset =
        type == reflection::String || type == reflection::Vector ||
        type == reflection::Union ||
        (type == reflection::Obj &&
         !schema.objects()->Get(field->type()->index())->is_struct());
    if (!is_offset || !root.GetAddressOf(field->offset())) continue;
    offset_fields++;
    TestRejected(generated, reflected, WithOffset(buf, field->offset(), 0),
                 field->name()->str() + " with an offset of 0");
  }
  // name, inventory, enemy, testempty and test.
  TEST_EQ(offset_fields, 5);
}

// A table with no fields set satisfies only a type without a required field,
// whatever the kind of the required field, as the generated
// VerifyOffsetRequired() and VerifyFieldRequired() have it. A deprecated field
// does not count: the generated builder and verifier both skip it.
// DeprecatedRequiredStruct, -Structs and -Table hold one kind each, so a
// failure names the path.
void ReflectionVerifyRequiredOffsetFieldsTest() {
  Parser parser;
  TEST_EQ(parser.Parse("struct Leaf { a:int; }"
                       "table Child { a:int; }"
                       "union Choice { Child }"
                       "table Optional {"
                       "  s:string;"
                       "  v:[int];"
                       "  structs:[Leaf];"
                       "  t:Child;"
                       "  u:Choice;"
                       "  uv:[Choice];"
                       "}"
                       "table RequiredString { s:string (required); }"
                       "table RequiredVector { v:[int] (required); }"
                       "table RequiredStruct { st:Leaf (required); }"
                       "table RequiredStructs { v:[Leaf] (required); }"
                       "table RequiredTable { t:Child (required); }"
                       "table RequiredUnion { u:Choice (required); }"
                       "table RequiredUnionVector { u:[Choice] (required); }"
                       "table DeprecatedRequired {"
                       "  s:string (required, deprecated);"
                       "  v:[int] (required, deprecated);"
                       "  u:Choice (required, deprecated);"
                       "  uv:[Choice] (required, deprecated);"
                       "  x:int;"
                       "}"
                       "table DeprecatedRequiredStruct {"
                       "  st:Leaf (required, deprecated);"
                       "}"
                       "table DeprecatedRequiredStructs {"
                       "  v:[Leaf] (required, deprecated);"
                       "}"
                       "table DeprecatedRequiredTable {"
                       "  t:Child (required, deprecated);"
                       "}"),
          true);
  parser.Serialize();
  const reflection::Schema& schema =
      *reflection::GetSchema(parser.builder_.GetBufferPointer());

  FlatBufferBuilder fbb;
  fbb.Finish(Offset<Table>(fbb.EndTable(fbb.StartTable())));

  for (const reflection::Object* const obj : *schema.objects()) {
    if (obj->is_struct()) continue;
    bool has_required = false;
    for (const reflection::Field* const field : *obj->fields()) {
      has_required =
          has_required || (field->required() && !field->deprecated());
    }
    const bool verified = flatbuffers::Verify(
        schema, *obj, fbb.GetBufferPointer(), fbb.GetSize());
    if (verified == has_required) {
      TEST_OUTPUT_LINE("An empty table %s %s",
                       verified ? "verifies as" : "fails",
                       obj->name()->c_str());
    }
    TEST_EQ(verified, !has_required);
  }

  // A deprecated field that is present is still verified, as reflection can
  // still read it: here, a table field with its slot past the end.
  const reflection::Object& deprecated_table =
      *schema.objects()->LookupByKey("DeprecatedRequiredTable");
  const voffset_t t = deprecated_table.fields()->LookupByKey("t")->offset();
  FlatBufferBuilder present;
  const Offset<Table> child(present.EndTable(present.StartTable()));
  const uoffset_t start = present.StartTable();
  present.AddOffset(t, child);
  present.Finish(Offset<Table>(present.EndTable(start)));
  const std::vector<uint8_t> with_t(
      present.GetBufferPointer(),
      present.GetBufferPointer() + present.GetSize());
  TEST_EQ(flatbuffers::Verify(schema, deprecated_table, with_t.data(),
                              with_t.size()),
          true);
  const std::vector<uint8_t> past_end = WithSlotPastEnd(with_t, t);
  TEST_EQ(flatbuffers::Verify(schema, deprecated_table, past_end.data(),
                              past_end.size()),
          false);
}

// The reflection verifier visits a table's fields sorted by name, so it
// reaches a union, or a vector of unions, before its `<name>_type` field. It
// must check that slot before reading the types from it, as the generated
// verifier does by verifying the type field first.
void ReflectionVerifyUnionTypeSlotsTest(const std::string& tests_data_path) {
  std::string schemafile;
  TEST_EQ(flatbuffers::LoadFile(
              (tests_data_path + "union_vector/union_vector.fbs").c_str(),
              false, &schemafile),
          true);
  // The schema's unions of structs and strings need a language to generate.
  IDLOptions idl_opts;
  idl_opts.lang_to_generate |= IDLOptions::kBinary;
  Parser parser(idl_opts);
  TEST_EQ(parser.Parse(schemafile.c_str()), true);
  parser.Serialize();
  const reflection::Schema& schema =
      *reflection::GetSchema(parser.builder_.GetBufferPointer());

  const auto generated = [](const uint8_t* buf, size_t length) -> bool {
    Verifier verifier(buf, length);
    return VerifyMovieBuffer(verifier);
  };
  const auto reflected = [&schema](const uint8_t* buf, size_t length) -> bool {
    return flatbuffers::Verify(schema, *schema.root_table(), buf, length);
  };

  MovieT movie;
  movie.main_character.type = Character_MuLan;
  movie.main_character.value = new AttackerT();
  movie.characters.resize(2);
  movie.characters[0].type = Character_MuLan;
  movie.characters[0].value = new AttackerT();
  movie.characters[1].type = Character_Rapunzel;
  movie.characters[1].value = new Rapunzel(7);
  FlatBufferBuilder fbb;
  FinishMovieBuffer(fbb, Movie::Pack(fbb, &movie));
  const std::vector<uint8_t> buf(fbb.GetBufferPointer(),
                                 fbb.GetBufferPointer() + fbb.GetSize());
  const std::unique_ptr<MovieT> unpacked(GetMovie(buf.data())->UnPack());
  TEST_ASSERT(*unpacked == movie);
  TEST_EQ(generated(buf.data(), buf.size()), true);
  TEST_EQ(reflected(buf.data(), buf.size()), true);
  TEST_EQ(VerifyBeforeGuardPage(buf, reflected), true);

  // Both type fields are set, so this moves each of their slots.
  TEST_NOTNULL(
      GetAnyRoot(buf.data())->GetAddressOf(Movie::VT_MAIN_CHARACTER_TYPE));
  TEST_NOTNULL(GetAnyRoot(buf.data())->GetAddressOf(Movie::VT_CHARACTERS_TYPE));
  TestSlotsPastEnd(generated, reflected, schema, buf);
}

// Given the schema, GetAnyFieldS() reads a union's type at the union's
// underlying width, as does the table printer of GetAnyValueS(), which calls
// it: test_union_type is an int for `union ABC: int`, though the schema gives
// it the base type UType.
void ReflectionUnionUnderlyingTypeToStringTest(
    const std::string& tests_data_path) {
  using namespace UnionUnderlyingType;

  std::string schemafile;
  TEST_EQ(flatbuffers::LoadFile(
              (tests_data_path + "union_underlying_type_test.fbs").c_str(),
              false, &schemafile),
          true);
  flatbuffers::Parser parser;
  TEST_EQ(parser.Parse(schemafile.c_str()), true);
  parser.Serialize();
  auto& schema = *reflection::GetSchema(parser.builder_.GetBufferPointer());
  int d_index = -1;
  for (uoffset_t i = 0; i < schema.objects()->size(); i++) {
    if (schema.objects()->Get(i)->name()->str() == "UnionUnderlyingType.D") {
      d_index = static_cast<int>(i);
    }
  }
  TEST_EQ(d_index >= 0, true);
  auto d_def = schema.objects()->Get(static_cast<uoffset_t>(d_index));
  auto type_field = d_def->fields()->LookupByKey("test_union_type");
  TEST_NOTNULL(type_field);

  // A = 555, whose low byte is 43.
  AT a;
  a.a = 42;
  DT d;
  d.test_union.Set(a);
  flatbuffers::FlatBufferBuilder fbb;
  fbb.Finish(D::Pack(fbb, &d));
  auto root = flatbuffers::GetAnyRoot(fbb.GetBufferPointer());

  TEST_EQ_STR(flatbuffers::GetAnyFieldS(*root, *type_field, &schema).c_str(),
              "555");
  TEST_EQ_STR(flatbuffers::GetAnyValueS(reflection::Obj, fbb.GetBufferPointer(),
                                        &schema, d_index)
                  .c_str(),
              "UnionUnderlyingType.D { test_union: (union), "
              "test_union_type: 555, }");
}

// Test that ForAllFields with reverse=true iterates fields in descending
// ID order. This exercises a fix for an operator precedence bug where the
// expression `size() - i + 1` was evaluated as `(size() - i) + 1` instead
// of the correct `size() - (i + 1)`, causing an out-of-bounds read.
void ForAllFieldsReverseTest(const std::string& tests_data_path) {
  // Load the binary schema.
  std::string bfbsfile;
  TEST_EQ(flatbuffers::LoadFile((tests_data_path + "monster_test.bfbs").c_str(),
                                true, &bfbsfile),
          true);

  // Verify the schema.
  flatbuffers::Verifier verifier(
      reinterpret_cast<const uint8_t*>(bfbsfile.c_str()), bfbsfile.length());
  TEST_EQ(reflection::VerifySchemaBuffer(verifier), true);

  auto& schema = *reflection::GetSchema(bfbsfile.c_str());

  // Use the Stat table which has 3 fields with sequential IDs:
  //   id:string   (id: 0)
  //   val:long     (id: 1)
  //   count:ushort (id: 2)
  auto stat_object = schema.objects()->LookupByKey("MyGame.Example.Stat");
  TEST_NOTNULL(stat_object);
  TEST_EQ(stat_object->fields()->size(), 3u);

  // Test forward iteration: fields should come in ascending ID order (0,1,2).
  {
    std::vector<uint16_t> forward_ids;
    flatbuffers::ForAllFields(
        stat_object, /*reverse=*/false,
        [&forward_ids](const reflection::Field* field) {
          forward_ids.push_back(field->id());
        });
    TEST_EQ(forward_ids.size(), 3u);
    TEST_EQ(forward_ids[0], 0);  // id
    TEST_EQ(forward_ids[1], 1);  // val
    TEST_EQ(forward_ids[2], 2);  // count
  }

  // Test reverse iteration: fields should come in descending ID order (2,1,0).
  // With the old buggy code `size() - i + 1`, at i=0 this would compute
  // index 3 - 0 + 1 = 4, which is out of bounds for a size-3 vector.
  {
    std::vector<uint16_t> reverse_ids;
    flatbuffers::ForAllFields(
        stat_object, /*reverse=*/true,
        [&reverse_ids](const reflection::Field* field) {
          reverse_ids.push_back(field->id());
        });
    TEST_EQ(reverse_ids.size(), 3u);
    TEST_EQ(reverse_ids[0], 2);  // count (highest ID first)
    TEST_EQ(reverse_ids[1], 1);  // val
    TEST_EQ(reverse_ids[2], 0);  // id (lowest ID last)
  }

  // Also test with the root Monster table which has many fields and
  // non-sequential definition order vs. ID order (e.g., pos=id:0, hp=id:2,
  // mana=id:1). This ensures the ID-to-index mapping works correctly.
  auto root_table = schema.root_table();
  TEST_NOTNULL(root_table);
  {
    std::vector<uint16_t> forward_ids;
    flatbuffers::ForAllFields(
        root_table, /*reverse=*/false,
        [&forward_ids](const reflection::Field* field) {
          forward_ids.push_back(field->id());
        });
    // Verify ascending order.
    for (size_t i = 1; i < forward_ids.size(); ++i) {
      TEST_ASSERT(forward_ids[i - 1] < forward_ids[i]);
    }
  }
  {
    std::vector<uint16_t> reverse_ids;
    flatbuffers::ForAllFields(
        root_table, /*reverse=*/true,
        [&reverse_ids](const reflection::Field* field) {
          reverse_ids.push_back(field->id());
        });
    // Verify descending order.
    for (size_t i = 1; i < reverse_ids.size(); ++i) {
      TEST_ASSERT(reverse_ids[i - 1] > reverse_ids[i]);
    }
  }
}

namespace {

// Copies the buffer finished in `fbb` into a vector of exactly its size, so
// that ASan reports a read past its end. ASan checks a read only against the
// 8-byte granule it starts in, so it misses one that straddles the end from a
// granule wholly inside the vector. A size that is a multiple of 8 is padded
// with 4 zero bytes, to end the vector halfway through a granule.
std::vector<uint8_t> CopyFinishedBuffer(const FlatBufferBuilder& fbb) {
  const auto size = fbb.GetSize();
  std::vector<uint8_t> buf(size % 8 ? size : size + 4);
  std::copy(fbb.GetBufferPointer(), fbb.GetBufferPointer() + size, buf.begin());
  return buf;
}

// Rewrites the vtable entry of `field`, which must be present in the root
// table of `buf`, to put its slot `slot` bytes into `buf`, or, if `slot` is 0,
// to mark it absent.
void SetRootFieldSlot(std::vector<uint8_t>& buf, voffset_t field, size_t slot) {
  auto root = GetRoot<Table>(buf.data());
  TEST_ASSERT(root->GetOptionalFieldOffset(field) != 0);
  auto table_pos =
      static_cast<size_t>(reinterpret_cast<const uint8_t*>(root) - buf.data());
  auto vtable_pos = static_cast<size_t>(root->GetVTable() - buf.data());
  TEST_ASSERT(!slot || (slot > table_pos && slot - table_pos <= 0xFFFF));
  WriteScalar(&buf[vtable_pos + field],
              static_cast<voffset_t>(slot ? slot - table_pos : 0));
}

}  // namespace

// A union's value is an offset in its slot, which a vtable can put anywhere in
// the 64KiB past its table. Verify() read that offset before checking the slot
// was in the buffer.
void ReflectionVerifyUnionValueSlotTest(const std::string& tests_data_path) {
  std::string bfbsfile;
  TEST_EQ(flatbuffers::LoadFile((tests_data_path + "monster_test.bfbs").c_str(),
                                true, &bfbsfile),
          true);
  auto& schema = *reflection::GetSchema(bfbsfile.c_str());

  MonsterT inner;
  inner.name = "inner";
  MonsterT monster;
  monster.name = "outer";
  monster.test.Set(std::move(inner));
  FlatBufferBuilder fbb;
  FinishMonsterBuffer(fbb, Monster::Pack(fbb, &monster));
  const auto packed = CopyFinishedBuffer(fbb);
  TEST_EQ(Verify(schema, *schema.root_table(), packed.data(), packed.size()),
          true);

  // Leave 3, 2, 1 and then none of the offset's 4 bytes in the buffer.
  for (auto slot = packed.size() - 3; slot <= packed.size(); slot++) {
    auto buf = packed;
    SetRootFieldSlot(buf, Monster::VT_TEST, slot);
    TEST_EQ(Verify(schema, *schema.root_table(), buf.data(), buf.size()),
            false);

    // Without its type field the union is NONE, and Verify() accepted it
    // whatever its slot held.
    SetRootFieldSlot(buf, Monster::VT_TEST_TYPE, 0);
    TEST_EQ(Verify(schema, *schema.root_table(), buf.data(), buf.size()),
            false);
  }
}

// A vector of unions finds its types through the offset in the slot of its
// `<name>_type` field, which sorts after it by name and so is not verified
// when Verify() reaches the union. Verify() read that offset before checking
// the slot was in the buffer.
void ReflectionVerifyUnionTypesSlotTest(const std::string& tests_data_path) {
  std::string schemafile;
  TEST_EQ(flatbuffers::LoadFile(
              (tests_data_path + "union_vector/union_vector.fbs").c_str(),
              false, &schemafile),
          true);
  flatbuffers::IDLOptions idl_opts;
  idl_opts.lang_to_generate |= flatbuffers::IDLOptions::kBinary;
  flatbuffers::Parser parser(idl_opts);
  TEST_EQ(parser.Parse(schemafile.c_str()), true);
  parser.Serialize();
  auto& schema = *reflection::GetSchema(parser.builder_.GetBufferPointer());

  // CharacterUnion has no Set(): two of Character's members share a type.
  MovieT movie;
  movie.characters.resize(2);
  movie.characters[0].type = Character_MuLan;
  movie.characters[0].value = new AttackerT();
  movie.characters[1].type = Character_Rapunzel;
  movie.characters[1].value = new Rapunzel(7);
  FlatBufferBuilder fbb;
  FinishMovieBuffer(fbb, Movie::Pack(fbb, &movie));
  const auto packed = CopyFinishedBuffer(fbb);
  TEST_EQ(Verify(schema, *schema.root_table(), packed.data(), packed.size()),
          true);

  // Leave 3, 2, 1 and then none of the offset's 4 bytes in the buffer.
  for (auto slot = packed.size() - 3; slot <= packed.size(); slot++) {
    auto buf = packed;
    SetRootFieldSlot(buf, Movie::VT_CHARACTERS_TYPE, slot);
    TEST_EQ(Verify(schema, *schema.root_table(), buf.data(), buf.size()),
            false);
  }
}

void MiniReflectFlatBuffersTest(uint8_t* flatbuf) {
  auto s =
      flatbuffers::FlatBufferToString(flatbuf, Monster::MiniReflectTypeTable());
  TEST_EQ_STR(
      s.c_str(),
      "{ "
      "pos: { x: 1.0, y: 2.0, z: 3.0, test1: 0.0, test2: Red, test3: "
      "{ a: 10, b: 20 } }, "
      "hp: 80, "
      "name: \"MyMonster\", "
      "inventory: [ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 ], "
      "test_type: Monster, "
      "test: { name: \"Fred\" }, "
      "test4: [ { a: 10, b: 20 }, { a: 30, b: 40 } ], "
      "testarrayofstring: [ \"bob\", \"fred\", \"bob\", \"fred\" ], "
      "testarrayoftables: [ { hp: 1000, name: \"Barney\" }, { name: \"Fred\" "
      "}, "
      "{ name: \"Wilma\" } ], "
      // TODO(wvo): should really print this nested buffer correctly.
      "testnestedflatbuffer: [ 124, 0, 0, 0, 77, 79, 78, 83, 0, 0, 114, 0, 16, "
      "0, 0, 0, 4, 0, 6, 0, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, "
      "0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, "
      "0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, "
      "0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, "
      "0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 12, 0, 114, 0, 0, 0, 0, 0, 0, 0, "
      "8, 0, 0, 0, 0, 0, 192, 127, 13, 0, 0, 0, 78, 101, 115, 116, 101, 100, "
      "77, 111, 110, 115, 116, 101, 114, 0, 0, 0 ], "
      "testarrayofstring2: [ \"jane\", \"mary\" ], "
      "testarrayofsortedstruct: [ { id: 0, distance: 0 }, "
      "{ id: 2, distance: 20 }, { id: 3, distance: 30 }, "
      "{ id: 4, distance: 40 } ], "
      "flex: [ 210, 4, 5, 2 ], "
      "test5: [ { a: 10, b: 20 }, { a: 30, b: 40 } ], "
      "vector_of_enums: [ Blue, Green ], "
      "scalar_key_sorted_tables: [ { id: \"miss\" } ], "
      "nan_default: nan "
      "}");

  Test test(16, 32);
  Vec3 vec(1, 2, 3, 1.5, Color_Red, test);
  flatbuffers::FlatBufferBuilder vec_builder;
  vec_builder.Finish(vec_builder.CreateStruct(vec));
  auto vec_buffer = vec_builder.Release();
  auto vec_str = flatbuffers::FlatBufferToString(vec_buffer.data(),
                                                 Vec3::MiniReflectTypeTable());
  TEST_EQ_STR(vec_str.c_str(),
              "{ x: 1.0, y: 2.0, z: 3.0, test1: 1.5, test2: Red, test3: { a: "
              "16, b: 32 } }");
}

void MiniReflectFixedLengthArrayTest() {
  // VS10 does not support typed enums, exclude from tests
#if !defined(_MSC_VER) || _MSC_VER >= 1700
  flatbuffers::FlatBufferBuilder fbb;
  MyGame::Example::ArrayStruct aStruct(2, 12, 1);
  auto aTable = MyGame::Example::CreateArrayTable(fbb, &aStruct);
  fbb.Finish(aTable);

  auto flatbuf = fbb.Release();
  auto s = flatbuffers::FlatBufferToString(
      flatbuf.data(), MyGame::Example::ArrayTableTypeTable());
  TEST_EQ_STR(
      "{ "
      "a: { a: 2.0, "
      "b: [ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 ], "
      "c: 12, "
      "d: [ { a: [ 0, 0 ], b: A, c: [ A, A ], d: [ 0, 0 ] }, "
      "{ a: [ 0, 0 ], b: A, c: [ A, A ], d: [ 0, 0 ] } ], "
      "e: 1, f: [ 0, 0 ] } "
      "}",
      s.c_str());
#endif
}

// A vector of unions whose `<field>_type` vector is absent must be rejected,
// not dereferenced. Verifier::VerifyVector(nullptr) returns true, since an
// absent vector is valid on its own, so the reflection verifier used to go on
// to read the null type vector's size and crash. The generated verifier
// (VerifyCharacterVector) rejects the same buffer.
void ReflectionVerifyUnionVectorWithoutTypesTest(
    const std::string& tests_data_path) {
  std::string schemafile;
  TEST_EQ(flatbuffers::LoadFile(
              (tests_data_path + "union_vector/union_vector.fbs").c_str(),
              false, &schemafile),
          true);
  flatbuffers::IDLOptions idl_opts;
  idl_opts.lang_to_generate |= flatbuffers::IDLOptions::kBinary;
  flatbuffers::Parser parser(idl_opts);
  TEST_EQ(parser.Parse(schemafile.c_str()), true);
  parser.Serialize();
  auto schema = reflection::GetSchema(parser.builder_.GetBufferPointer());

  flatbuffers::FlatBufferBuilder fbb;
  std::vector<flatbuffers::Offset<void>> characters;
  characters.push_back(CreateAttacker(fbb, /*sword_attack_damage=*/5).Union());
  FinishMovieBuffer(fbb,
                    CreateMovie(fbb, Character_NONE, 0, /*characters_type=*/0,
                                fbb.CreateVector(characters)));

  auto movie = GetMovie(fbb.GetBufferPointer());
  TEST_NOTNULL(movie->characters());
  TEST_NULL(movie->characters_type());

  flatbuffers::Verifier verifier(fbb.GetBufferPointer(), fbb.GetSize());
  TEST_EQ(VerifyMovieBuffer(verifier), false);

  TEST_EQ(flatbuffers::Verify(*schema, *schema->root_table(),
                              fbb.GetBufferPointer(), fbb.GetSize()),
          false);
}

// A union declared with an underlying type stores its types as that type:
// `union ABC: int` makes test_union_type an int, and test_vector_of_union_type
// a vector of ints. Mini reflection must read them, and find the members they
// name, at that width, not as 1-byte UTypes.
void MiniReflectUnionUnderlyingTypeTest() {
  using namespace UnionUnderlyingType;
  AT a;
  a.a = 42;
  BT b;
  b.b = "foo";
  CT c;
  c.c = true;

  DT single;
  single.test_union.Set(a);
  flatbuffers::FlatBufferBuilder single_fbb;
  single_fbb.Finish(D::Pack(single_fbb, &single));
  TEST_EQ_STR(flatbuffers::FlatBufferToString(single_fbb.GetBufferPointer(),
                                              D::MiniReflectTypeTable())
                  .c_str(),
              "{ test_union_type: A, test_union: { a: 42 } }");

  DT many;
  many.test_union.Set(c);
  many.test_vector_of_union.resize(3);
  many.test_vector_of_union[0].Set(a);
  many.test_vector_of_union[1].Set(b);
  many.test_vector_of_union[2].Set(c);
  flatbuffers::FlatBufferBuilder many_fbb;
  many_fbb.Finish(D::Pack(many_fbb, &many));
  TEST_EQ_STR(flatbuffers::FlatBufferToString(many_fbb.GetBufferPointer(),
                                              D::MiniReflectTypeTable())
                  .c_str(),
              "{ test_union_type: C, test_union: { c: true }, "
              "test_vector_of_union_type: [ A, B, C ], "
              "test_vector_of_union: [ { a: 42 }, { b: \"foo\" }, "
              "{ c: true } ] }");
}

}  // namespace tests
}  // namespace flatbuffers
