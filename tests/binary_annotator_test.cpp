#include "binary_annotator_test.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "binary_annotator.h"
#include "flatbuffers/idl.h"
#include "flatbuffers/reflection.h"
#include "flatbuffers/util.h"
#include "test_assert.h"
#include "tests/union_underlying_type_test_generated.h"
#include "tests/union_value_gap_test_generated.h"

namespace flatbuffers {
namespace tests {
namespace {

typedef std::map<uint64_t, BinarySection> Annotations;

// Packs `holder` and checks that the generated verifier accepts the bytes.
std::vector<uint8_t> PackGapHolder(const UnionValueGap::HolderT& holder) {
  FlatBufferBuilder fbb;
  UnionValueGap::FinishHolderBuffer(fbb,
                                    UnionValueGap::Holder::Pack(fbb, &holder));
  Verifier verifier(fbb.GetBufferPointer(), fbb.GetSize());
  TEST_EQ(UnionValueGap::VerifyHolderBuffer(verifier), true);
  return std::vector<uint8_t>(fbb.GetBufferPointer(),
                              fbb.GetBufferPointer() + fbb.GetSize());
}

// Annotates `buffer` against the binary schema `bfbs`, as `flatc --annotate`
// does.
Annotations Annotate(const std::string& bfbs,
                     const std::vector<uint8_t>& buffer) {
  BinaryAnnotator annotator(reinterpret_cast<const uint8_t*>(bfbs.data()),
                            bfbs.size(), buffer.data(), buffer.size(),
                            /*is_size_prefixed=*/false);
  return annotator.Annotate();
}

// The number of table sections annotated as the table `name`.
size_t CountTables(const Annotations& annotations, const std::string& name) {
  size_t count = 0;
  for (const auto& section : annotations) {
    if (section.second.type == BinarySectionType::Table &&
        section.second.name == name) {
      ++count;
    }
  }
  return count;
}

// The number of regions annotated with `status`.
size_t CountStatus(const Annotations& annotations,
                   const BinaryRegionStatus status) {
  size_t count = 0;
  for (const auto& section : annotations) {
    for (const BinaryRegion& region : section.second.regions) {
      if (region.comment.status == status) {
        ++count;
      }
    }
  }
  return count;
}

// The region whose comment has `type`, `name` and `index`, or null.
const BinaryRegion* FindRegion(const Annotations& annotations,
                               const BinaryRegionCommentType type,
                               const std::string& name, const size_t index) {
  for (const auto& section : annotations) {
    for (const BinaryRegion& region : section.second.regions) {
      if (region.comment.type == type && region.comment.name == name &&
          region.comment.index == index) {
        return &region;
      }
    }
  }
  return nullptr;
}

// The member type named in the annotation of the `single` union field.
std::string SingleMember(const Annotations& annotations) {
  const BinaryRegion* region = FindRegion(
      annotations, BinaryRegionCommentType::TableOffsetField, "single", 0);
  TEST_NOTNULL(region);
  return region ? region->comment.default_value : std::string();
}

// The member type named in the annotation of `many[index]`, or "" if that
// element was not annotated as a member.
std::string ManyMember(const Annotations& annotations, const size_t index) {
  const BinaryRegion* region = FindRegion(
      annotations, BinaryRegionCommentType::VectorUnionValue, "", index);
  return region ? region->comment.default_value : std::string();
}

// A holder whose `many` is [Alpha, `beta`, `gamma`].
UnionValueGap::HolderT ManyOfEach(UnionValueGap::BetaT beta,
                                  UnionValueGap::GammaT gamma) {
  UnionValueGap::HolderT holder;
  holder.many.resize(3);
  holder.many[0].Set(UnionValueGap::AlphaT());
  holder.many[1].Set(beta);
  holder.many[2].Set(gamma);
  return holder;
}

// Packs `d` and checks that the generated verifier accepts the bytes.
std::vector<uint8_t> PackD(const UnionUnderlyingType::DT& d) {
  FlatBufferBuilder fbb;
  fbb.Finish(UnionUnderlyingType::D::Pack(fbb, &d));
  Verifier verifier(fbb.GetBufferPointer(), fbb.GetSize());
  TEST_EQ(verifier.VerifyBuffer<UnionUnderlyingType::D>(nullptr), true);
  return std::vector<uint8_t>(fbb.GetBufferPointer(),
                              fbb.GetBufferPointer() + fbb.GetSize());
}

// Checks that the int at `value` in `buffer` is annotated as one region, typed
// as the annotator types an int.
void CheckIntRegion(const Annotations& annotations,
                    const std::vector<uint8_t>& buffer,
                    const uint8_t* const value) {
  TEST_NOTNULL(value);
  const uint64_t offset = static_cast<uint64_t>(value - buffer.data());
  const BinaryRegion* found = nullptr;
  for (const auto& section : annotations) {
    for (const BinaryRegion& region : section.second.regions) {
      if (region.offset == offset) {
        found = &region;
      }
    }
  }
  TEST_NOTNULL(found);
  if (found != nullptr) {
    TEST_EQ(found->length, 4u);
    TEST_EQ(found->type, BinaryRegionType::Uint32);
  }
}

}  // namespace

// Union members are keyed by value, not by their position in the union's list
// of values. The annotator used to index that list by the type, so with
// `union Gap { Alpha = 1, Beta = 3, Gamma = 4 }` it annotated a Beta as the
// Gamma at position 3, rejected a Gamma as an invalid type, and accepted 2,
// which names no member, as the Beta at position 2.
void AnnotateUnionByValueTest(const std::string& tests_data_path) {
  std::string schemafile;
  TEST_EQ(LoadFile((tests_data_path + "union_value_gap_test.fbs").c_str(),
                   false, &schemafile),
          true);
  IDLOptions opts;
  opts.lang_to_generate |= IDLOptions::kBinary;
  Parser parser(opts);
  TEST_EQ(parser.Parse(schemafile.c_str()), true);
  parser.Serialize();
  const std::string bfbs(
      reinterpret_cast<const char*>(parser.builder_.GetBufferPointer()),
      parser.builder_.GetSize());

  // The premise: Beta and Gamma do not sit at the positions their values name.
  const reflection::Schema* schema =
      reflection::GetSchema(parser.builder_.GetBufferPointer());
  const reflection::Enum* gap =
      schema->enums()->LookupByKey("UnionValueGap.Gap");
  TEST_NOTNULL(gap);
  TEST_EQ(gap->values()->size(), 4u);
  TEST_EQ(gap->values()->Get(2)->value(), 3);
  TEST_EQ(gap->values()->Get(3)->value(), 4);

  // The value after Alpha lies in the gap and names no member.
  const uint8_t no_member = static_cast<uint8_t>(UnionValueGap::Gap_Alpha + 1);
  TEST_EQ(gap->values()->LookupByKey(no_member) == nullptr, true);

  UnionValueGap::BetaT beta;
  beta.b = 1000000;  // Read as Gamma's string offset, it points off the end.
  UnionValueGap::GammaT gamma;
  gamma.g = "gamma";

  {
    UnionValueGap::HolderT holder;
    holder.single.Set(UnionValueGap::AlphaT());
    const Annotations annotations = Annotate(bfbs, PackGapHolder(holder));
    TEST_EQ_STR(SingleMember(annotations).c_str(), "(union of type `Alpha`)");
    TEST_EQ(CountTables(annotations, "UnionValueGap.Alpha"), 1u);
    TEST_EQ(
        CountStatus(annotations, BinaryRegionStatus::ERROR_INVALID_UNION_TYPE),
        0u);
  }

  {
    UnionValueGap::HolderT holder;
    holder.single.Set(beta);
    const Annotations annotations = Annotate(bfbs, PackGapHolder(holder));
    TEST_EQ_STR(SingleMember(annotations).c_str(), "(union of type `Beta`)");
    TEST_EQ(CountTables(annotations, "UnionValueGap.Beta"), 1u);
    TEST_EQ(CountTables(annotations, "UnionValueGap.Gamma"), 0u);
    TEST_EQ(
        CountStatus(annotations, BinaryRegionStatus::ERROR_INVALID_UNION_TYPE),
        0u);
    TEST_EQ(CountStatus(annotations,
                        BinaryRegionStatus::ERROR_OFFSET_OUT_OF_BINARY),
            0u);
  }

  {
    UnionValueGap::HolderT holder;
    holder.single.Set(gamma);
    const Annotations annotations = Annotate(bfbs, PackGapHolder(holder));
    TEST_EQ_STR(SingleMember(annotations).c_str(), "(union of type `Gamma`)");
    TEST_EQ(CountTables(annotations, "UnionValueGap.Gamma"), 1u);
    TEST_EQ(
        CountStatus(annotations, BinaryRegionStatus::ERROR_INVALID_UNION_TYPE),
        0u);
  }

  {
    const UnionValueGap::HolderT holder = ManyOfEach(beta, gamma);
    const Annotations annotations = Annotate(bfbs, PackGapHolder(holder));
    TEST_EQ_STR(ManyMember(annotations, 0).c_str(), "(`Alpha`)");
    TEST_EQ_STR(ManyMember(annotations, 1).c_str(), "(`Beta`)");
    TEST_EQ_STR(ManyMember(annotations, 2).c_str(), "(`Gamma`)");
    TEST_EQ(CountTables(annotations, "UnionValueGap.Alpha"), 1u);
    TEST_EQ(CountTables(annotations, "UnionValueGap.Beta"), 1u);
    TEST_EQ(CountTables(annotations, "UnionValueGap.Gamma"), 1u);
    TEST_EQ(
        CountStatus(annotations, BinaryRegionStatus::ERROR_INVALID_UNION_TYPE),
        0u);
    TEST_EQ(CountStatus(annotations,
                        BinaryRegionStatus::ERROR_OFFSET_OUT_OF_BINARY),
            0u);
  }

  // The Object API cannot pack a value for a type that names no member, so
  // pack a Beta and retype it in place.
  {
    UnionValueGap::HolderT holder;
    holder.single.Set(beta);
    std::vector<uint8_t> buffer = PackGapHolder(holder);
    const reflection::Field* single_type =
        schema->root_table()->fields()->LookupByKey("single_type");
    TEST_NOTNULL(single_type);
    TEST_EQ(
        SetField<uint8_t>(GetAnyRoot(buffer.data()), *single_type, no_member),
        true);
    const Annotations annotations = Annotate(bfbs, buffer);
    TEST_EQ(
        CountStatus(annotations, BinaryRegionStatus::ERROR_INVALID_UNION_TYPE),
        1u);
    TEST_EQ(CountTables(annotations, "UnionValueGap.Beta"), 0u);
  }

  {
    const UnionValueGap::HolderT holder = ManyOfEach(beta, gamma);
    std::vector<uint8_t> buffer = PackGapHolder(holder);
    UnionValueGap::GetMutableHolder(buffer.data())
        ->mutable_many_type()
        ->Mutate(1, no_member);
    const Annotations annotations = Annotate(bfbs, buffer);
    TEST_EQ_STR(ManyMember(annotations, 0).c_str(), "(`Alpha`)");
    TEST_EQ_STR(ManyMember(annotations, 1).c_str(), "");
    TEST_EQ_STR(ManyMember(annotations, 2).c_str(), "(`Gamma`)");
    TEST_EQ(
        CountStatus(annotations, BinaryRegionStatus::ERROR_INVALID_UNION_TYPE),
        1u);
    TEST_EQ(CountTables(annotations, "UnionValueGap.Beta"), 0u);
  }
}

// `union ABC: int` stores test_union_type as an int, and
// test_vector_of_union_type as ints, though the binary schema gives both the
// base type UType. The annotator must read and size them as ints.
void AnnotateUnionUnderlyingTypeTest(const std::string& tests_data_path) {
  using UnionUnderlyingType::ABC;

  std::string schemafile;
  TEST_EQ(LoadFile((tests_data_path + "union_underlying_type_test.fbs").c_str(),
                   false, &schemafile),
          true);
  IDLOptions opts;
  opts.lang_to_generate |= IDLOptions::kBinary;
  Parser parser(opts);
  TEST_EQ(parser.Parse(schemafile.c_str()), true);
  // The schema declares no root_type; name one, as `flatc --root-type` does.
  TEST_EQ(parser.SetRootType("D"), true);
  parser.Serialize();
  const std::string bfbs(
      reinterpret_cast<const char*>(parser.builder_.GetBufferPointer()),
      parser.builder_.GetSize());

  // The premise: to their fields the types are UTypes; to the union, ints.
  const reflection::Schema* schema =
      reflection::GetSchema(parser.builder_.GetBufferPointer());
  const reflection::Field* type_field =
      schema->root_table()->fields()->LookupByKey("test_union_type");
  const reflection::Field* types_field =
      schema->root_table()->fields()->LookupByKey("test_vector_of_union_type");
  TEST_NOTNULL(type_field);
  TEST_NOTNULL(types_field);
  TEST_EQ(type_field->type()->base_type(), reflection::UType);
  TEST_EQ(types_field->type()->element(), reflection::UType);
  TEST_EQ(schema->enums()
              ->Get(type_field->type()->index())
              ->underlying_type()
              ->base_type(),
          reflection::Int);

  UnionUnderlyingType::AT a;
  a.a = 42;
  UnionUnderlyingType::BT b;
  b.b = "foo";
  UnionUnderlyingType::CT c;
  c.c = true;

  UnionUnderlyingType::DT many;
  many.test_vector_of_union.resize(3);
  many.test_vector_of_union[0].Set(a);
  many.test_vector_of_union[1].Set(b);
  many.test_vector_of_union[2].Set(c);

  UnionUnderlyingType::DT both = many;
  both.test_union.Set(b);

  // Each type is one int region and names its member.
  {
    const std::vector<uint8_t> buffer = PackD(both);
    const Annotations annotations = Annotate(bfbs, buffer);

    CheckIntRegion(
        annotations, buffer,
        GetAnyRoot(buffer.data())->GetAddressOf(type_field->offset()));
    const Vector<ABC>* types = GetRoot<UnionUnderlyingType::D>(buffer.data())
                                   ->test_vector_of_union_type();
    TEST_NOTNULL(types);
    for (uoffset_t i = 0; types != nullptr && i < types->size(); ++i) {
      CheckIntRegion(annotations, buffer, types->Data() + i * sizeof(ABC));
    }

    const BinaryRegion* single =
        FindRegion(annotations, BinaryRegionCommentType::TableOffsetField,
                   "test_union", 0);
    TEST_NOTNULL(single);
    if (single != nullptr) {
      TEST_EQ_STR(single->comment.default_value.c_str(), "(union of type `B`)");
    }
    TEST_EQ_STR(ManyMember(annotations, 0).c_str(), "(`A`)");
    TEST_EQ_STR(ManyMember(annotations, 1).c_str(), "(`B`)");
    TEST_EQ_STR(ManyMember(annotations, 2).c_str(), "(`C`)");
    TEST_EQ(CountTables(annotations, "UnionUnderlyingType.A"), 1u);
    TEST_EQ(CountTables(annotations, "UnionUnderlyingType.B"), 2u);
    TEST_EQ(CountTables(annotations, "UnionUnderlyingType.C"), 1u);
    TEST_EQ(
        CountStatus(annotations, BinaryRegionStatus::ERROR_INVALID_UNION_TYPE),
        0u);
  }

  // 512 names no member, in the field and in the vector.
  {
    UnionUnderlyingType::DT unknown;
    unknown.test_union.type = static_cast<ABC>(512);
    const Annotations annotations = Annotate(bfbs, PackD(unknown));
    TEST_EQ(
        CountStatus(annotations, BinaryRegionStatus::ERROR_INVALID_UNION_TYPE),
        1u);
  }

  {
    std::vector<uint8_t> buffer = PackD(many);
    GetMutableRoot<UnionUnderlyingType::D>(buffer.data())
        ->mutable_test_vector_of_union_type()
        ->Mutate(1, static_cast<ABC>(512));
    const Annotations annotations = Annotate(bfbs, buffer);
    TEST_EQ_STR(ManyMember(annotations, 0).c_str(), "(`A`)");
    TEST_EQ_STR(ManyMember(annotations, 1).c_str(), "");
    TEST_EQ_STR(ManyMember(annotations, 2).c_str(), "(`C`)");
    TEST_EQ(
        CountStatus(annotations, BinaryRegionStatus::ERROR_INVALID_UNION_TYPE),
        1u);
    TEST_EQ(CountTables(annotations, "UnionUnderlyingType.B"), 0u);
  }

  // The last type of `many` ends the buffer. Cut off its last byte, into a
  // buffer of its own so a sanitizer reports any read past the end. What is
  // left of the vector still holds 3 types read as bytes, but not 3 ints.
  {
    const std::vector<uint8_t> buffer = PackD(many);
    const Vector<ABC>* types = GetRoot<UnionUnderlyingType::D>(buffer.data())
                                   ->test_vector_of_union_type();
    TEST_NOTNULL(types);
    if (types != nullptr) {
      TEST_EQ(types->Data() + types->size() * sizeof(ABC) ==
                  buffer.data() + buffer.size(),
              true);
    }
    const std::vector<uint8_t> cut(buffer.begin(), buffer.end() - 1);
    const Annotations annotations = Annotate(bfbs, cut);
    TEST_EQ(CountStatus(annotations, BinaryRegionStatus::ERROR_LENGTH_TOO_LONG),
            1u);
    TEST_EQ_STR(ManyMember(annotations, 0).c_str(), "(`A`)");
    TEST_EQ_STR(ManyMember(annotations, 1).c_str(), "(`B`)");
    const BinaryRegion* last = FindRegion(
        annotations, BinaryRegionCommentType::VectorUnionValue, "", 2);
    TEST_NOTNULL(last);
    if (last != nullptr) {
      TEST_EQ(last->comment.status,
              BinaryRegionStatus::ERROR_INCOMPLETE_BINARY);
      TEST_EQ_STR(last->comment.status_message.c_str(), "4");
    }
  }
}

}  // namespace tests
}  // namespace flatbuffers
