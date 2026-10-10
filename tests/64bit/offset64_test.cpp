#include "offset64_test.h"

#include <stdint.h>

#include <cstdint>
#include <fstream>
#include <limits>
#include <ostream>

#include "flatbuffers/base.h"
#include "flatbuffers/buffer.h"
#include "flatbuffers/flatbuffer_builder.h"
#include "flatbuffers/flatbuffers.h"
#include "flatbuffers/idl.h"
#include "flatbuffers/reflection.h"
#include "tests/64bit/evolution/v1_generated.h"
#include "tests/64bit/evolution/v2_generated.h"
#include "tests/64bit/test_64bit_generated.h"
#include "tests/test_assert.h"

namespace flatbuffers {
namespace tests {

void Offset64Test() {
  FlatBufferBuilder64 builder;

  const size_t far_vector_size = 1LL << 2;
  // Make a large number if wanting to test a real large buffer.
  const size_t big_vector_size = 1LL << 2;

  {
    // First create the vectors that will be copied to the buffer.
    std::vector<uint8_t> far_data;
    far_data.resize(far_vector_size);
    far_data[0] = 4;
    far_data[far_vector_size - 1] = 2;

    std::vector<bool> big_bool_data = {true,  false, true, true,
                                       false, false, true};

    std::vector<uint8_t> big_data;
    big_data.resize(big_vector_size);
    big_data[0] = 8;
    big_data[big_vector_size - 1] = 3;

    // Then serialize all the fields that have 64-bit offsets, as these must be
    // serialized before any 32-bit fields are added to the buffer.
    const Offset64<Vector<uint8_t>> far_vector_offset =
        builder.CreateVector64<Vector>(far_data);

    const Offset64<String> far_string_offset =
        builder.CreateString<Offset64>("some far string");

    const Offset64<Vector64<uint8_t>> big_vector_offset =
        builder.CreateVector64(big_data);

    const Offset64<Vector64<uint8_t>> big_bool_vector_offset =
        builder.CreateVector64(big_bool_data);

    // Now that we are done with the 64-bit fields, we can create and add the
    // normal fields.
    const Offset<String> near_string_offset =
        builder.CreateString("some near string");

    // Finish by building the root table by passing in all the offsets.
    const Offset<RootTable> root_table_offset = CreateRootTable(
        builder, far_vector_offset, 0, far_string_offset,
        big_bool_vector_offset, big_vector_offset, near_string_offset);

    // Finish the buffer.
    builder.Finish(root_table_offset);

    Verifier::Options options;
    // Allow the verifier to verify 64-bit buffers.
    options.max_size = FLATBUFFERS_MAX_64_BUFFER_SIZE;
    options.assert = true;

    Verifier verifier(builder.GetBufferPointer(), builder.GetSize(), options);

    TEST_EQ(VerifyRootTableBuffer(verifier), true);
  }

  {
    const RootTable* root_table = GetRootTable(builder.GetBufferPointer());

    // Expect the far vector to be properly sized.
    TEST_EQ(root_table->far_vector()->size(), far_vector_size);
    TEST_EQ(root_table->far_vector()->Get(0), 4);
    TEST_EQ(root_table->far_vector()->Get(far_vector_size - 1), 2);

    TEST_EQ_STR(root_table->far_string()->c_str(), "some far string");

    // Expect the big vector to be properly sized.
    TEST_EQ(root_table->big_vector()->size(), big_vector_size);
    TEST_EQ(root_table->big_vector()->Get(0), 8);
    TEST_EQ(root_table->big_vector()->Get(big_vector_size - 1), 3);

    TEST_EQ_STR(root_table->near_string()->c_str(), "some near string");
  }
}

void Offset64SerializedFirst() {
  FlatBufferBuilder64 fbb;

  // First create the vectors that will be copied to the buffer.
  std::vector<uint8_t> data;
  data.resize(64);

  // Then serialize all the fields that have 64-bit offsets, as these must be
  // serialized before any 32-bit fields are added to the buffer.
  fbb.CreateVector64(data);

  // TODO(derekbailey): figure out how to test assertions.
  // Uncommenting this line should fail the test with an assertion.
  // fbb.CreateString("some near string");

  fbb.CreateVector64(data);
}

void Offset64NestedFlatBuffer() {
  FlatBufferBuilder64 fbb;

  // First serialize a nested buffer.
  const Offset<String> near_string_offset =
      fbb.CreateString("nested: some near string");

  // Finish by building the root table by passing in all the offsets.
  const Offset<RootTable> root_table_offset =
      CreateRootTable(fbb, 0, 0, 0, 0, 0, near_string_offset, 0);

  // Finish the buffer.
  fbb.Finish(root_table_offset);

  // Ensure the buffer is valid.
  const RootTable* root_table = GetRootTable(fbb.GetBufferPointer());
  TEST_EQ_STR(root_table->near_string()->c_str(), "nested: some near string");

  // Copy the data out of the builder.
  std::vector<uint8_t> nested_data{fbb.GetBufferPointer(),
                                   fbb.GetBufferPointer() + fbb.GetSize()};

  {
    // Clear so we can reuse the builder.
    fbb.Clear();

    const Offset64<Vector64<uint8_t>> nested_flatbuffer_offset =
        fbb.CreateVector64<Vector64>(nested_data);

    // Now that we are done with the 64-bit fields, we can create and add the
    // normal fields.
    const Offset<String> near_string_offset =
        fbb.CreateString("some near string");

    // Finish by building the root table by passing in all the offsets.
    const Offset<RootTable> root_table_offset = CreateRootTable(
        fbb, 0, 0, 0, 0, 0, near_string_offset, nested_flatbuffer_offset);

    // Finish the buffer.
    fbb.Finish(root_table_offset);

    Verifier::Options options;
    // Allow the verifier to verify 64-bit buffers.
    options.max_size = FLATBUFFERS_MAX_64_BUFFER_SIZE;
    options.assert = true;

    Verifier verifier(fbb.GetBufferPointer(), fbb.GetSize(), options);

    TEST_EQ(VerifyRootTableBuffer(verifier), true);
  }

  {
    const RootTable* root_table = GetRootTable(fbb.GetBufferPointer());

    // Test that the parent buffer field is ok.
    TEST_EQ_STR(root_table->near_string()->c_str(), "some near string");

    // Expect nested buffer to be properly sized.
    TEST_EQ(root_table->nested_root()->size(), nested_data.size());

    // Expect the direct accessors to the nested buffer work.
    TEST_EQ_STR(root_table->nested_root_nested_root()->near_string()->c_str(),
                "nested: some near string");
  }
}

void Offset64CreateDirect() {
  FlatBufferBuilder64 fbb;

  // Create a vector of some data
  std::vector<uint8_t> data{0, 1, 2};

  // Call the "Direct" creation method to ensure that things are added to the
  // buffer in the correct order, Offset64 first followed by any Offsets.
  const Offset<RootTable> root_table_offset = CreateRootTableDirect(
      fbb, &data, 0, "some far string", nullptr, &data, "some near string");

  // Finish the buffer.
  fbb.Finish(root_table_offset);

  Verifier::Options options;
  // Allow the verifier to verify 64-bit buffers.
  options.max_size = FLATBUFFERS_MAX_64_BUFFER_SIZE;
  options.assert = true;

  Verifier verifier(fbb.GetBufferPointer(), fbb.GetSize(), options);

  TEST_EQ(VerifyRootTableBuffer(verifier), true);

  // Verify the data.
  const RootTable* root_table = GetRootTable(fbb.GetBufferPointer());
  TEST_EQ(root_table->far_vector()->size(), data.size());
  TEST_EQ(root_table->big_vector()->size(), data.size());
  TEST_EQ_STR(root_table->far_string()->c_str(), "some far string");
  TEST_EQ_STR(root_table->near_string()->c_str(), "some near string");
}

void Offset64Evolution() {
  // Some common data for the tests.
  const std::vector<uint8_t> data = {1, 2, 3, 4};
  const std::vector<uint8_t> big_data = {6, 7, 8, 9, 10};

  // Built V1 read V2
  {
    // Use the 32-bit builder since V1 doesn't have any 64-bit offsets.
    FlatBufferBuilder builder;

    builder.Finish(v1::CreateRootTableDirect(builder, 1234, &data));

    // Use each version to get a view at the root table.
    auto v1_root = v1::GetRootTable(builder.GetBufferPointer());
    auto v2_root = v2::GetRootTable(builder.GetBufferPointer());

    // Test field equivalents for fields common to V1 and V2.
    TEST_EQ(v1_root->a(), v2_root->a());

    TEST_EQ(v1_root->b(), v2_root->b());
    TEST_EQ(v1_root->b()->Get(2), 3);
    TEST_EQ(v2_root->b()->Get(2), 3);

    // This field is added in V2, so it should be null since V1 couldn't have
    // written it.
    TEST_ASSERT(v2_root->big_vector() == nullptr);
  }

  // Built V2 read V1
  {
    // Use the 64-bit builder since V2 has 64-bit offsets.
    FlatBufferBuilder64 builder;

    builder.Finish(v2::CreateRootTableDirect(builder, 1234, &data, &big_data));

    // Use each version to get a view at the root table.
    auto v1_root = v1::GetRootTable(builder.GetBufferPointer());
    auto v2_root = v2::GetRootTable(builder.GetBufferPointer());

    // Test field equivalents for fields common to V1 and V2.
    TEST_EQ(v1_root->a(), v2_root->a());

    TEST_EQ(v1_root->b(), v2_root->b());
    TEST_EQ(v1_root->b()->Get(2), 3);
    TEST_EQ(v2_root->b()->Get(2), 3);

    // Test that V2 can read the big vector, which V1 doesn't even have
    // accessors for (i.e. v1_root->big_vector() doesn't exist).
    TEST_ASSERT(v2_root->big_vector() != nullptr);
    TEST_EQ(v2_root->big_vector()->size(), big_data.size());
    TEST_EQ(v2_root->big_vector()->Get(2), 8);
  }

  // Built V2 read V1, bigger than max 32-bit buffer sized.
  // This checks that even a large buffer can still be read by V1.
  {
    // Use the 64-bit builder since V2 has 64-bit offsets.
    FlatBufferBuilder64 builder;

    std::vector<uint8_t> giant_data;
    giant_data.resize(1LL << 3);
    giant_data[2] = 42;

    builder.Finish(
        v2::CreateRootTableDirect(builder, 1234, &data, &giant_data));

    // Use each version to get a view at the root table.
    auto v1_root = v1::GetRootTable(builder.GetBufferPointer());
    auto v2_root = v2::GetRootTable(builder.GetBufferPointer());

    // Test field equivalents for fields common to V1 and V2.
    TEST_EQ(v1_root->a(), v2_root->a());

    TEST_EQ(v1_root->b(), v2_root->b());
    TEST_EQ(v1_root->b()->Get(2), 3);
    TEST_EQ(v2_root->b()->Get(2), 3);

    // Test that V2 can read the big vector, which V1 doesn't even have
    // accessors for (i.e. v1_root->big_vector() doesn't exist).
    TEST_ASSERT(v2_root->big_vector() != nullptr);
    TEST_EQ(v2_root->big_vector()->size(), giant_data.size());
    TEST_EQ(v2_root->big_vector()->Get(2), 42);
  }
}

void Offset64VectorOfStructs() {
  FlatBufferBuilder64 builder;

  std::vector<LeafStruct> far_leaves;
  far_leaves.emplace_back(LeafStruct{123, 4.567});
  far_leaves.emplace_back(LeafStruct{987, 6.543});

  std::vector<LeafStruct> big_leaves;
  big_leaves.emplace_back(LeafStruct{72, 72.8});
  big_leaves.emplace_back(LeafStruct{82, 82.8});
  big_leaves.emplace_back(LeafStruct{92, 92.8});

  // Add the two vectors of leaf structs.
  const Offset<RootTable> root_table_offset =
      CreateRootTableDirect(builder, nullptr, 0, nullptr, nullptr, nullptr,
                            nullptr, nullptr, &far_leaves, &big_leaves);

  // Finish the buffer.
  builder.Finish(root_table_offset);

  Verifier::Options options;
  // Allow the verifier to verify 64-bit buffers.
  options.max_size = FLATBUFFERS_MAX_64_BUFFER_SIZE;
  options.assert = true;

  Verifier verifier(builder.GetBufferPointer(), builder.GetSize(), options);

  TEST_EQ(VerifyRootTableBuffer(verifier), true);

  // Verify the data.
  const RootTable* root_table = GetRootTable(builder.GetBufferPointer());
  TEST_EQ(root_table->far_struct_vector()->size(), far_leaves.size());
  TEST_EQ(root_table->far_struct_vector()->Get(0)->a(), 123);
  TEST_EQ(root_table->far_struct_vector()->Get(0)->b(), 4.567);
  TEST_EQ(root_table->far_struct_vector()->Get(1)->a(), 987);
  TEST_EQ(root_table->far_struct_vector()->Get(1)->b(), 6.543);

  TEST_EQ(root_table->big_struct_vector()->size(), big_leaves.size());
  TEST_EQ(root_table->big_struct_vector()->Get(0)->a(), 72);
  TEST_EQ(root_table->big_struct_vector()->Get(0)->b(), 72.8);
  TEST_EQ(root_table->big_struct_vector()->Get(1)->a(), 82);
  TEST_EQ(root_table->big_struct_vector()->Get(1)->b(), 82.8);
  TEST_EQ(root_table->big_struct_vector()->Get(2)->a(), 92);
  TEST_EQ(root_table->big_struct_vector()->Get(2)->b(), 92.8);
}

// Serializes a RootTable whose far_struct_vector elements are aligned to
// `elem_align`, and returns the offset of the element data in the buffer.
static size_t BuildFarStructVector(FlatBufferBuilder64& builder,
                                   const size_t elem_align) {
  const std::vector<LeafStruct> leaves{LeafStruct{123, 4.567},
                                       LeafStruct{987, 6.543}};

  // Start 4 bytes past an 8-byte boundary. Everything before a 64-bit-offset
  // object must be in the 64-bit region too: Pad() would put the 4 bytes in
  // the 32-bit region, which the builder asserts against (in a build with
  // assertions on). An unreferenced 64-bit vector -- 8 bytes of data and a
  // 4-byte length -- leaves the builder 12 bytes in, inside that region.
  const std::vector<uint8_t> filler(8, 0);
  builder.CreateVector64<Vector>(filler);
  TEST_EQ(builder.GetSize() % AlignOf<LeafStruct>(), sizeof(uint32_t));

  // CreateVectorOfStructs64<Vector>() with a caller-chosen element alignment.
  builder.StartVector<Offset64, uint32_t>(leaves.size(), sizeof(LeafStruct),
                                          elem_align);
  builder.PushBytes(reinterpret_cast<const uint8_t*>(leaves.data()),
                    sizeof(LeafStruct) * leaves.size());
  const Offset64<Vector<const LeafStruct*>> far_struct_vector(
      builder.EndVector<uint32_t, uoffset64_t>(leaves.size()));

  RootTableBuilder root_table_builder(builder);
  root_table_builder.add_far_struct_vector(far_struct_vector);
  builder.Finish(root_table_builder.Finish());

  const uint8_t* const buf = builder.GetBufferPointer();
  return static_cast<size_t>(GetRootTable(buf)->far_struct_vector()->Data() -
                             buf);
}

void Offset64VectorOfStructsMisaligned() {
  Verifier::Options options;
  // Allow the verifier to verify 64-bit buffers.
  options.max_size = FLATBUFFERS_MAX_64_BUFFER_SIZE;
  const auto verifies = [](const FlatBufferBuilder64& builder,
                           const Verifier::Options& opts) {
    Verifier verifier(builder.GetBufferPointer(), builder.GetSize(), opts);
    return VerifyRootTableBuffer(verifier);
  };

  FlatBufferBuilder64 aligned;
  const size_t aligned_offset =
      BuildFarStructVector(aligned, AlignOf<LeafStruct>());
  TEST_EQ(aligned_offset % AlignOf<LeafStruct>(), 0u);
  TEST_EQ(verifies(aligned, options), true);

  // Elements at 4 mod 8: the length prefix is aligned, the element data is not.
  FlatBufferBuilder64 misaligned;
  const size_t misaligned_offset =
      BuildFarStructVector(misaligned, sizeof(uint32_t));
  TEST_EQ(misaligned_offset % AlignOf<LeafStruct>(), sizeof(uint32_t));
  TEST_EQ(verifies(misaligned, options), false);

  options.check_alignment = false;
  TEST_EQ(verifies(misaligned, options), true);
}

void Offset64SizePrefix() {
  FlatBufferBuilder64 builder;

  // First serialize a nested buffer.
  const Offset<String> near_string_offset =
      builder.CreateString("some near string");

  // Finish by building the root table by passing in all the offsets.
  const Offset<RootTable> root_table_offset =
      CreateRootTable(builder, 0, 0, 0, 0, 0, near_string_offset, 0);

  // Finish the buffer.
  FinishSizePrefixedRootTableBuffer(builder, root_table_offset);

  TEST_EQ(GetPrefixedSize<uoffset64_t>(builder.GetBufferPointer()),
          builder.GetSize() - sizeof(uoffset64_t));

  Verifier::Options options;
  // Allow the verifier to verify 64-bit buffers.
  options.max_size = FLATBUFFERS_MAX_64_BUFFER_SIZE;
  options.assert = true;

  Verifier verifier(builder.GetBufferPointer(), builder.GetSize(), options);

  TEST_EQ(VerifySizePrefixedRootTableBuffer(verifier), true);

  const RootTable* root_table =
      GetSizePrefixedRootTable(builder.GetBufferPointer());

  // Verify the fields.
  TEST_EQ_STR(root_table->near_string()->c_str(), "some near string");
}

void Offset64ManyVectors() {
  FlatBufferBuilder64 builder;

  // Setup some data to serialize.
  std::vector<int8_t> data;
  data.resize(20);
  data.front() = 42;
  data.back() = 18;

  const size_t kNumVectors = 20;

  // First serialize all the 64-bit address vectors. We need to store all the
  // offsets to later add to a wrapper table. We cannot serialize one vector and
  // then add it to a table immediately, as it would violate the strict ordering
  // of putting all 64-bit things at the tail of the buffer.
  std::array<Offset64<Vector<int8_t>>, kNumVectors> offsets_64bit;
  for (size_t i = 0; i < kNumVectors; ++i) {
    offsets_64bit[i] = builder.CreateVector64<Vector>(data);
  }

  // Create some unrelated, 64-bit offset value for later testing.
  const Offset64<String> far_string_offset =
      builder.CreateString<Offset64>("some far string");

  // Now place all the offsets into their own wrapper tables. Again, we have to
  // store the offsets before we can add them to the root table vector.
  std::array<Offset<WrapperTable>, kNumVectors> offsets_wrapper;
  for (size_t i = 0; i < kNumVectors; ++i) {
    offsets_wrapper[i] = CreateWrapperTable(builder, offsets_64bit[i]);
  }

  // Now create the 32-bit vector that is stored in the root table.
  // TODO(derekbailey): the array type wasn't auto deduced, see if that could be
  // fixed.
  const Offset<Vector<Offset<WrapperTable>>> many_vectors_offset =
      builder.CreateVector<Offset<WrapperTable>>(offsets_wrapper);

  // Finish by building using the root table builder, to exercise a different
  // code path than the other tests.
  RootTableBuilder root_table_builder(builder);
  root_table_builder.add_many_vectors(many_vectors_offset);
  root_table_builder.add_far_string(far_string_offset);
  const Offset<RootTable> root_table_offset = root_table_builder.Finish();

  // Finish the buffer.
  FinishRootTableBuffer(builder, root_table_offset);

  Verifier::Options options;
  // Allow the verifier to verify 64-bit buffers.
  options.max_size = FLATBUFFERS_MAX_64_BUFFER_SIZE;
  options.assert = true;

  Verifier verifier(builder.GetBufferPointer(), builder.GetSize(), options);

  TEST_EQ(VerifyRootTableBuffer(verifier), true);

  const RootTable* root_table = GetRootTable(builder.GetBufferPointer());

  // Verify the fields.
  TEST_EQ_STR(root_table->far_string()->c_str(), "some far string");
  TEST_EQ(root_table->many_vectors()->size(), kNumVectors);

  // Spot check one of the vectors.
  TEST_EQ(root_table->many_vectors()->Get(12)->vector()->size(), 20);
  TEST_EQ(root_table->many_vectors()->Get(12)->vector()->Get(0), 42);
  TEST_EQ(root_table->many_vectors()->Get(12)->vector()->Get(19), 18);
}

void Offset64ForceAlign() {
  FlatBufferBuilder64 builder;

  // Setup some data to serialize that is less than the force_align size of 32
  // bytes.
  std::vector<uint8_t> data{1, 2, 3};

  // Use the CreateDirect which calls the ForceVectorAlign
  const auto root_table_offset =
      CreateRootTableDirect(builder, nullptr, 0, nullptr, nullptr, nullptr,
                            nullptr, nullptr, nullptr, nullptr, nullptr, &data);

  // Finish the buffer.
  FinishRootTableBuffer(builder, root_table_offset);
}

// Packs a RootTable with every 64-bit offset field set. near_string and
// many_vectors stay empty: Pack() serializes them ahead of the 64-bit
// nested_root and forced_aligned_vector, which the builder does not allow.
static std::vector<uint8_t> PackRootTableWith64BitFields() {
  RootTableT nested;
  nested.a = 7;
  FlatBufferBuilder64 nested_fbb;
  FinishRootTableBuffer(nested_fbb, RootTable::Pack(nested_fbb, &nested));

  RootTableT root;
  root.far_vector = {1, 2, 3};
  root.a = 42;
  root.far_string = "some far string";
  root.big_bool_vector = {1, 0, 1};
  root.big_vector = {4, 5, 6, 7};
  root.nested_root.assign(nested_fbb.GetBufferPointer(),
                          nested_fbb.GetBufferPointer() + nested_fbb.GetSize());
  root.far_struct_vector = {LeafStruct{123, 4.567}, LeafStruct{987, 6.543}};
  root.big_struct_vector = {LeafStruct{72, 72.8}};
  root.forced_aligned_vector = {8, 9};

  FlatBufferBuilder64 fbb;
  FinishRootTableBuffer(fbb, RootTable::Pack(fbb, &root));
  std::vector<uint8_t> buf(fbb.GetBufferPointer(),
                           fbb.GetBufferPointer() + fbb.GetSize());

  const std::unique_ptr<RootTableT> unpacked(
      GetRootTable(buf.data())->UnPack());
  TEST_ASSERT(*unpacked == root);
  return buf;
}

static bool GeneratedVerify(const std::vector<uint8_t>& buf) {
  Verifier::Options options;
  // Allow the verifier to verify 64-bit buffers.
  options.max_size = FLATBUFFERS_MAX_64_BUFFER_SIZE;
  Verifier verifier(buf.data(), buf.size(), options);
  return VerifyRootTableBuffer(verifier);
}

// Returns `buf` with every 64-bit field of its root table except `keep` (none
// when null) made absent, by zeroing its vtable entry as if it was never set.
static std::vector<uint8_t> Keep64BitField(const std::vector<uint8_t>& buf,
                                           const reflection::Object& root_table,
                                           const reflection::Field* keep) {
  std::vector<uint8_t> out = buf;
  const Table* const root = GetAnyRoot(out.data());
  const size_t vtable = static_cast<size_t>(root->GetVTable() - out.data());
  for (const reflection::Field* const field : *root_table.fields()) {
    if (!field->offset64() || field == keep) continue;
    TEST_EQ(root->CheckField(field->offset()), true);
    WriteScalar<voffset_t>(out.data() + vtable + field->offset(), 0);
  }
  TEST_EQ(GeneratedVerify(out), true);
  return out;
}

// Copies the root table of `buf` into a 32-bit builder with reflection.
static Offset<const Table*> ReflectionCopy(FlatBufferBuilder& fbb,
                                           const reflection::Schema& schema,
                                           const std::vector<uint8_t>& buf,
                                           bool use_string_pooling = false) {
  return CopyTable(fbb, schema, *schema.root_table(), *GetAnyRoot(buf.data()),
                   use_string_pooling);
}

// Copies the fixture without its 64-bit fields into `fbb` and checks the copy,
// so a rejected copy before it shows that it left fbb usable.
static void CheckCopyWithout64BitFields(FlatBufferBuilder& fbb,
                                        const reflection::Schema& schema,
                                        const std::vector<uint8_t>& buf) {
  const Offset<const Table*> copy = ReflectionCopy(
      fbb, schema, Keep64BitField(buf, *schema.root_table(), nullptr));
  TEST_EQ(copy.IsNull(), false);
  fbb.Finish(copy);
  const std::vector<uint8_t> copied(fbb.GetBufferPointer(),
                                    fbb.GetBufferPointer() + fbb.GetSize());
  TEST_EQ(GeneratedVerify(copied), true);
  RootTableT expected;
  expected.a = 42;
  const std::unique_ptr<RootTableT> unpacked(
      GetRootTable(copied.data())->UnPack());
  TEST_ASSERT(*unpacked == expected);
}

void Offset64ReflectionCopyTable() {
  const reflection::Schema& schema =
      *reflection::GetSchema(RootTable::BinarySchema::data());
  const reflection::Object& root_table = *schema.root_table();
  const std::vector<uint8_t> buf = PackRootTableWith64BitFields();

  // Without its 64-bit fields, the table copies as it always has.
  {
    FlatBufferBuilder fbb;
    CheckCopyWithout64BitFields(fbb, schema, buf);
  }

  // A 32-bit builder cannot write a 64-bit offset, so a copy that reaches one
  // is rejected with a null offset, in every build type. Each field on its
  // own, as the first one CopyTable reaches would otherwise mask the rest.
  for (const reflection::Field* const field : *root_table.fields()) {
    if (!field->offset64()) continue;
    const std::vector<uint8_t> only_field =
        Keep64BitField(buf, root_table, field);
    FlatBufferBuilder fbb;
    TEST_EQ(ReflectionCopy(fbb, schema, only_field).IsNull(), true);
    CheckCopyWithout64BitFields(fbb, schema, buf);
  }

  // A schema not written by flatc may leave offset64 unset on a Vector64
  // field. Its 64-bit offset is still no scalar to copy inline.
  {
    const uint8_t* const data = RootTable::BinarySchema::data();
    std::vector<uint8_t> bfbs(data, data + RootTable::BinarySchema::size());
    const reflection::Schema& flagless = *reflection::GetSchema(bfbs.data());
    const reflection::Field& big_vector =
        *flagless.root_table()->fields()->LookupByKey("big_vector");
    // Field derives privately from Table, which knows where the flag is stored.
    const uint8_t* const flag =
        reinterpret_cast<const Table*>(&big_vector)
            ->GetAddressOf(reflection::Field::VT_OFFSET64);
    TEST_NOTNULL(flag);
    bfbs[static_cast<size_t>(flag - bfbs.data())] = 0;
    TEST_EQ(big_vector.offset64(), false);
    TEST_EQ(big_vector.type()->base_type(), reflection::Vector64);

    const std::vector<uint8_t> only_big_vector = Keep64BitField(
        buf, root_table, root_table.fields()->LookupByKey("big_vector"));
    FlatBufferBuilder fbb;
    TEST_EQ(ReflectionCopy(fbb, flagless, only_big_vector).IsNull(), true);
    CheckCopyWithout64BitFields(fbb, schema, buf);
  }
}

// A 64-bit field below a table, union or vector of tables, none of which
// test_64bit.fbs has. Field names sort as s, t, ts, u, so each place a copy
// fails in leaves what came before it copied.
static const char kSubTablesSchema[] =
    "table Inner { v:[ubyte] (offset64); }"
    "union U { Inner }"
    "table Outer { s:string; t:Inner; u:U; ts:[Inner]; }"
    "root_type Outer;";

// Which Inner of an Outer has its 64-bit field set.
enum class Far { kNone, kTable, kUnion, kLastOfVector };

// Packs an Outer with every field set, but Inner.v only in the Inner at `far`.
// The schema has no generated code, so fields go to their reflected offsets.
static std::vector<uint8_t> PackOuter(const reflection::Schema& schema,
                                      const Far far) {
  const auto offset_of = [](const reflection::Object& object,
                            const char* const name) {
    return object.fields()->LookupByKey(name)->offset();
  };
  const reflection::Object& inner = *schema.objects()->LookupByKey("Inner");
  const reflection::Object& outer = *schema.root_table();
  const reflection::EnumVal& inner_type =
      *schema.enums()->LookupByKey("U")->values()->Get(1);
  TEST_EQ_STR(inner_type.name()->c_str(), "Inner");

  FlatBufferBuilder64 fbb;
  // The builder keeps 64-bit data at the tail of the buffer: serialize it
  // before anything else.
  const std::vector<uint8_t> data = {1, 2, 3};
  const Offset64<Vector<uint8_t>> v = fbb.CreateVector64<Vector>(data);
  const auto pack_inner = [&](const bool with_v) {
    const uoffset_t start = fbb.StartTable();
    if (with_v) fbb.AddOffset(offset_of(inner, "v"), v);
    return Offset<void>(fbb.EndTable(start));
  };
  const Offset<void> t = pack_inner(far == Far::kTable);
  const Offset<void> u = pack_inner(far == Far::kUnion);
  const std::vector<Offset<void>> ts = {pack_inner(false), pack_inner(false),
                                        pack_inner(far == Far::kLastOfVector)};
  const auto ts_vector = fbb.CreateVector(ts);
  const Offset<String> s = fbb.CreateString("near");

  const uoffset_t start = fbb.StartTable();
  fbb.AddOffset(offset_of(outer, "s"), s);
  fbb.AddOffset(offset_of(outer, "t"), t);
  fbb.AddElement<uint8_t>(offset_of(outer, "u_type"),
                          static_cast<uint8_t>(inner_type.value()), 0);
  fbb.AddOffset(offset_of(outer, "u"), u);
  fbb.AddOffset(offset_of(outer, "ts"), ts_vector);
  fbb.Finish(Offset<void>(fbb.EndTable(start)));
  return std::vector<uint8_t>(fbb.GetBufferPointer(),
                              fbb.GetBufferPointer() + fbb.GetSize());
}

// Copies an Outer without its 64-bit field into `fbb` and checks the copy.
static void CheckOuterCopy(FlatBufferBuilder& fbb,
                           const reflection::Schema& schema,
                           const bool use_string_pooling) {
  const Offset<const Table*> copy = ReflectionCopy(
      fbb, schema, PackOuter(schema, Far::kNone), use_string_pooling);
  TEST_EQ(copy.IsNull(), false);
  fbb.Finish(copy);

  const reflection::Object& outer = *schema.root_table();
  TEST_EQ(
      flatbuffers::Verify(schema, outer, fbb.GetBufferPointer(), fbb.GetSize()),
      true);
  const Table& root = *GetAnyRoot(fbb.GetBufferPointer());
  const auto field = [&](const char* const name) -> const reflection::Field& {
    return *outer.fields()->LookupByKey(name);
  };
  TEST_EQ_STR(GetFieldS(root, field("s"))->c_str(), "near");
  TEST_NOTNULL(GetFieldT(root, field("t")));
  TEST_NOTNULL(GetFieldT(root, field("u")));
  TEST_EQ(GetFieldAnyV(root, field("ts"))->size(), 3u);
}

void Offset64ReflectionCopyTableSubTables() {
  Parser parser;
  TEST_EQ(parser.Parse(kSubTablesSchema), true);
  parser.Serialize();
  const reflection::Schema& schema =
      *reflection::GetSchema(parser.builder_.GetBufferPointer());

  for (const bool use_string_pooling : {false, true}) {
    {
      FlatBufferBuilder fbb;
      CheckOuterCopy(fbb, schema, use_string_pooling);
    }
    // A sub-table that fails the copy fails its parent, and every table above
    // that, rather than leave a null offset in them.
    for (const Far far : {Far::kTable, Far::kUnion, Far::kLastOfVector}) {
      FlatBufferBuilder fbb;
      TEST_EQ(ReflectionCopy(fbb, schema, PackOuter(schema, far),
                             use_string_pooling)
                  .IsNull(),
              true);
      CheckOuterCopy(fbb, schema, use_string_pooling);
    }
  }
}

}  // namespace tests
}  // namespace flatbuffers
