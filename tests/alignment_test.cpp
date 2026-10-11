#include "alignment_test.h"

#include <string>
#include <vector>

#include "flatbuffers/flatbuffer_builder.h"
#include "flatbuffers/idl.h"
#include "flatbuffers/reflection.h"
#include "flatbuffers/util.h"
#include "test_assert.h"
#include "tests/alignment_test_generated.h"

namespace flatbuffers {
namespace tests {
namespace {

// Where BuildLargeStructs() put each BadAlignmentLarge, as an offset from the
// start of the finished buffer.
struct LargeStructsPlacement {
  size_t structs;  // The element data of LargeStructs.structs.
  size_t value;    // The struct LargeStructs.value points to.
};

// Serializes a LargeStructs whose `structs` element data is aligned only to
// `structs_align`, and whose `value` struct only to `value_align`. Each starts
// 4 bytes past an 8-byte boundary, so an alignment of 4 leaves it at 4 mod 8
// and AlignOf<BadAlignmentLarge>() pads it onto the boundary.
LargeStructsPlacement BuildLargeStructs(FlatBufferBuilder& builder,
                                        const size_t structs_align,
                                        const size_t value_align) {
  // Pads to 4 mod 8 and raises the buffer's minimum alignment to 8, so
  // Finish() keeps that placement in the finished buffer.
  const auto start_past_boundary = [&builder]() {
    builder.ForceVectorAlignment(1, sizeof(uint32_t),
                                 AlignOf<BadAlignmentLarge>());
  };

  // CreateVectorOfStructs() with a caller-chosen element alignment.
  const std::vector<BadAlignmentLarge> structs{BadAlignmentLarge(1),
                                               BadAlignmentLarge(2)};
  start_past_boundary();
  builder.StartVector(structs.size(), sizeof(BadAlignmentLarge), structs_align);
  builder.PushBytes(reinterpret_cast<const uint8_t*>(structs.data()),
                    sizeof(BadAlignmentLarge) * structs.size());
  const Offset<Vector<const BadAlignmentLarge*>> structs_offset(
      builder.EndVector(structs.size()));

  // CreateStruct() with a caller-chosen alignment.
  const BadAlignmentLarge value(3);
  start_past_boundary();
  builder.Align(value_align);
  builder.PushBytes(reinterpret_cast<const uint8_t*>(&value), sizeof(value));
  const Offset<void> value_offset(builder.GetSize());

  LargeStructsBuilder large_structs_builder(builder);
  large_structs_builder.add_structs(structs_offset);
  large_structs_builder.add_value_type(LargeStructUnion_BadAlignmentLarge);
  large_structs_builder.add_value(value_offset);
  builder.Finish(large_structs_builder.Finish());
  TEST_EQ(builder.GetBufferMinAlignment(), AlignOf<BadAlignmentLarge>());

  const uint8_t* const buf = builder.GetBufferPointer();
  const LargeStructs* const large_structs = GetRoot<LargeStructs>(buf);
  return {static_cast<size_t>(large_structs->structs()->Data() - buf),
          static_cast<size_t>(
              static_cast<const uint8_t*>(large_structs->value()) - buf)};
}

}  // namespace

void AlignmentTest() {
  FlatBufferBuilder builder;

  BadAlignmentLarge large;
  Offset<OuterLarge> outer_large = CreateOuterLarge(builder, &large);

  BadAlignmentSmall* small;
  Offset<Vector<const BadAlignmentSmall*>> small_offset =
      builder.CreateUninitializedVectorOfStructs(9, &small);
  (void)small;  // We do not have to write data to trigger the test failure

  Offset<BadAlignmentRoot> root =
      CreateBadAlignmentRoot(builder, outer_large, small_offset);

  builder.Finish(root);

  Verifier verifier(builder.GetBufferPointer(), builder.GetSize());
  TEST_ASSERT(verifier.VerifyBuffer<BadAlignmentRoot>(nullptr));

  // ============= Test Small Structs Vector misalignment ========

  builder.Clear();

  // creating 5 structs with 2 bytes each
  // 10 bytes in total for Vector data is needed
  std::vector<EvenSmallStruct> even_vector = {{2, 1}, {3, 1}, {4, 1}};
  std::vector<OddSmallStruct> odd_vector = {{6, 5, 4}, {9, 8, 7}, {1, 2, 3}};
  // CreateVectorOfStructs is used in the generated CreateSmallStructsDirect()
  // method, but we test it directly
  Offset<Vector<const EvenSmallStruct*>> even_structs_offset =
      builder.CreateVectorOfStructs<EvenSmallStruct>(even_vector);
  Offset<Vector<const OddSmallStruct*>> odd_structs_offset =
      builder.CreateVectorOfStructs<OddSmallStruct>(odd_vector);
  Offset<SmallStructs> small_structs_root =
      CreateSmallStructs(builder, even_structs_offset, odd_structs_offset);

  builder.Finish(small_structs_root);

  // Save the binary that we later can annotate with `flatc --annotate` command
  // NOTE: the conversion of the JSON data to --binary via `flatc --binary`
  //       command is not changed with that fix and was always producing the
  //       correct binary data.
  // SaveFile("alignment_test_{before,after}_fix.bin",
  //          reinterpret_cast<char *>(builder.GetBufferPointer()),
  //          builder.GetSize(), true);

  Verifier verifier_small_structs(builder.GetBufferPointer(),
                                  builder.GetSize());
  TEST_ASSERT(verifier_small_structs.VerifyBuffer<SmallStructs>(nullptr));

  // Reading SmallStructs vector values back and compare with original
  auto root_msg =
      flatbuffers::GetRoot<SmallStructs>(builder.GetBufferPointer());

  TEST_EQ(root_msg->even_structs()->size(), even_vector.size());
  for (flatbuffers::uoffset_t i = 0; i < root_msg->even_structs()->size();
       ++i) {
    TEST_EQ(even_vector[i].var_0(), root_msg->even_structs()->Get(i)->var_0());
    TEST_EQ(even_vector[i].var_1(), root_msg->even_structs()->Get(i)->var_1());
  }

  TEST_EQ(root_msg->odd_structs()->size(), even_vector.size());
  for (flatbuffers::uoffset_t i = 0; i < root_msg->odd_structs()->size(); ++i) {
    TEST_EQ(odd_vector[i].var_0(), root_msg->odd_structs()->Get(i)->var_0());
    TEST_EQ(odd_vector[i].var_1(), root_msg->odd_structs()->Get(i)->var_1());
  }
}

void LargeStructsAlignmentTest(const std::string& tests_data_path) {
  std::string schemafile;
  TEST_ASSERT(LoadFile((tests_data_path + "alignment_test.fbs").c_str(), false,
                       &schemafile));
  // The parser only accepts a struct as a union member when the target
  // language supports one; this test targets a binary schema, which does.
  IDLOptions opts;
  opts.lang_to_generate = IDLOptions::kBinary;
  Parser parser(opts);
  TEST_ASSERT(parser.Parse(schemafile.c_str()));
  parser.Serialize();
  const reflection::Schema& schema =
      *reflection::GetSchema(parser.builder_.GetBufferPointer());
  const reflection::Object* const root =
      schema.objects()->LookupByKey("LargeStructs");
  TEST_NOTNULL(root);

  const auto reflection_verifies = [&](const FlatBufferBuilder& builder) {
    return Verify(schema, *root, builder.GetBufferPointer(), builder.GetSize());
  };
  const auto generated_verifies = [](const FlatBufferBuilder& builder,
                                     const bool check_alignment) {
    Verifier::Options options;
    options.check_alignment = check_alignment;
    Verifier verifier(builder.GetBufferPointer(), builder.GetSize(), options);
    return verifier.VerifyBuffer<LargeStructs>(nullptr);
  };

  const size_t aligned = AlignOf<BadAlignmentLarge>();
  const size_t misaligned = sizeof(uint32_t);

  FlatBufferBuilder control;
  const LargeStructsPlacement control_at =
      BuildLargeStructs(control, aligned, aligned);
  TEST_EQ(control_at.structs % aligned, 0u);
  TEST_EQ(control_at.value % aligned, 0u);
  TEST_EQ(reflection_verifies(control), true);
  TEST_EQ(generated_verifies(control, true), true);

  // Each buffer below misaligns one struct and keeps the other aligned, so
  // that one rejection cannot stand in for the other. Both are otherwise
  // valid: the generated verifier accepts them with check_alignment off.

  // The vector's elements at 4 mod 8: its length prefix is aligned, they are
  // not.
  FlatBufferBuilder bad_structs;
  const LargeStructsPlacement bad_structs_at =
      BuildLargeStructs(bad_structs, misaligned, aligned);
  TEST_EQ(bad_structs_at.structs % aligned, misaligned);
  TEST_EQ(bad_structs_at.value % aligned, 0u);
  TEST_EQ(reflection_verifies(bad_structs), false);
  TEST_EQ(generated_verifies(bad_structs, true), false);
  TEST_EQ(generated_verifies(bad_structs, false), true);

  // The union's struct value at 4 mod 8.
  FlatBufferBuilder bad_value;
  const LargeStructsPlacement bad_value_at =
      BuildLargeStructs(bad_value, aligned, misaligned);
  TEST_EQ(bad_value_at.structs % aligned, 0u);
  TEST_EQ(bad_value_at.value % aligned, misaligned);
  TEST_EQ(reflection_verifies(bad_value), false);
  TEST_EQ(generated_verifies(bad_value, true), false);
  TEST_EQ(generated_verifies(bad_value, false), true);
}

}  // namespace tests
}  // namespace flatbuffers
