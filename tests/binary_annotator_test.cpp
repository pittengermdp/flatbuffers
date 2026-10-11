#include "binary_annotator_test.h"

#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "binary_annotator.h"
#include "flatbuffers/idl.h"
#include "flatbuffers/reflection.h"
#include "flatbuffers/util.h"
#include "test_assert.h"
#include "tests/deprecated_required_test_generated.h"
#include "tests/union_underlying_type_test_generated.h"
#include "tests/union_value_gap_test_generated.h"
#include "tests/vtable_slot_count_test_generated.h"

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

// Checks that union[index] of test_vector_of_union in the D in `buffer` is
// annotated as its own 4-byte offset, with `status`.
void CheckUnionSlot(const Annotations& annotations,
                    const std::vector<uint8_t>& buffer, const uoffset_t index,
                    const BinaryRegionStatus status) {
  const uint8_t* slot = GetRoot<UnionUnderlyingType::D>(buffer.data())
                            ->test_vector_of_union()
                            ->Data() +
                        index * sizeof(uoffset_t);
  const BinaryRegion* region = FindRegion(
      annotations, BinaryRegionCommentType::VectorUnionValue, "", index);
  TEST_NOTNULL(region);
  if (region != nullptr) {
    TEST_EQ(region->offset, static_cast<uint64_t>(slot - buffer.data()));
    TEST_EQ(region->length, 4u);
    TEST_EQ(region->type, BinaryRegionType::UOffset);
    TEST_EQ(region->comment.status, status);
  }
}

// The finished buffer in `fbb`.
std::vector<uint8_t> FinishedBuffer(const FlatBufferBuilder& fbb) {
  return std::vector<uint8_t>(fbb.GetBufferPointer(),
                              fbb.GetBufferPointer() + fbb.GetSize());
}

// The regions of the one vtable in `annotations`.
std::vector<BinaryRegion> VTableRegions(const Annotations& annotations) {
  std::vector<BinaryRegion> regions;
  size_t vtables = 0;
  for (const auto& section : annotations) {
    if (section.second.type == BinarySectionType::VTable) {
      regions = section.second.regions;
      ++vtables;
    }
  }
  TEST_EQ(vtables, 1u);
  return regions;
}

// The number of `regions` whose comment has `type`.
size_t CountType(const std::vector<BinaryRegion>& regions,
                 const BinaryRegionCommentType type) {
  size_t count = 0;
  for (const BinaryRegion& region : regions) {
    if (region.comment.type == type) {
      ++count;
    }
  }
  return count;
}

// Whether no two `regions` start at the same offset. A slot annotated twice
// shows up as two regions at one offset.
bool EachAtItsOwnOffset(const std::vector<BinaryRegion>& regions) {
  std::set<uint64_t> offsets;
  for (const BinaryRegion& region : regions) {
    if (!offsets.insert(region.offset).second) {
      return false;
    }
  }
  return true;
}

// The region of `regions` annotated as the slot of the field `name` with `id`,
// or null. The text output prints it as "offset to field `name` (id: N)".
const BinaryRegion* FindSlot(const std::vector<BinaryRegion>& regions,
                             const std::string& name, const int id) {
  const std::string comment_name = name + "` (id: " + std::to_string(id) + ")";
  for (const BinaryRegion& region : regions) {
    if (region.comment.type == BinaryRegionCommentType::VTableFieldOffset &&
        region.comment.name == comment_name) {
      return &region;
    }
  }
  return nullptr;
}

// Checks that the slot of the field `name` with `id` is annotated with
// `status`.
void CheckSlot(const std::vector<BinaryRegion>& regions,
               const std::string& name, const int id,
               const BinaryRegionStatus status) {
  const BinaryRegion* slot = FindSlot(regions, name, id);
  TEST_NOTNULL(slot);
  if (slot) {
    TEST_EQ(slot->comment.status, status);
  }
}

// A Reader written by Reader's generated code, whose vtable slot `slot`, a
// Reader::VT_* constant, then has `voffset` written over it.
std::vector<uint8_t> ReaderWithSlot(const voffset_t slot,
                                    const voffset_t voffset) {
  VTableSlotCount::ReaderT reader_t;
  reader_t.name = "name";
  reader_t.count = 7;
  FlatBufferBuilder fbb;
  VTableSlotCount::FinishReaderBuffer(
      fbb, VTableSlotCount::Reader::Pack(fbb, &reader_t));
  Verifier verifier(fbb.GetBufferPointer(), fbb.GetSize());
  TEST_EQ(VTableSlotCount::VerifyReaderBuffer(verifier), true);

  std::vector<uint8_t> buffer = FinishedBuffer(fbb);
  const uint8_t* vtable = GetRoot<Table>(buffer.data())->GetVTable();
  WriteScalar(buffer.data() + (vtable - buffer.data()) + slot, voffset);
  return buffer;
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

// The vtable slot annotated for the field `name` with `id`, or null. The text
// output prints it as "offset to field `name` (id: N)".
const BinaryRegion* FindSlot(const Annotations& annotations,
                             const std::string& name, const int id) {
  return FindRegion(annotations, BinaryRegionCommentType::VTableFieldOffset,
                    name + "` (id: " + std::to_string(id) + ")", 0);
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

// A vector of unions finds its types through a 32-bit offset to its `_type`
// vector, reads no type past that vector's end, and annotates each element in
// its own slot, even one whose member offset or type is bad.
void AnnotateUnionVectorTest(const std::string& tests_data_path) {
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
  const reflection::Field* types_field =
      reflection::GetSchema(parser.builder_.GetBufferPointer())
          ->root_table()
          ->fields()
          ->LookupByKey("test_vector_of_union_type");
  TEST_NOTNULL(types_field);

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

  // B's 64 KiB string puts the `_type` vector more than 0xFFFF bytes past its
  // field, so the offset to it needs more than 16 bits.
  {
    UnionUnderlyingType::BT big;
    big.b = std::string(0x10000, 'x');
    UnionUnderlyingType::DT far = many;
    far.test_vector_of_union[1].Set(std::move(big));
    const std::vector<uint8_t> buffer = PackD(far);
    const uint8_t* field =
        GetAnyRoot(buffer.data())->GetAddressOf(types_field->offset());
    const uint8_t* types = reinterpret_cast<const uint8_t*>(
        GetRoot<UnionUnderlyingType::D>(buffer.data())
            ->test_vector_of_union_type());
    TEST_EQ(types - field > 0xFFFF, true);

    const Annotations annotations = Annotate(bfbs, buffer);
    TEST_EQ_STR(ManyMember(annotations, 0).c_str(), "(`A`)");
    TEST_EQ_STR(ManyMember(annotations, 1).c_str(), "(`B`)");
    TEST_EQ_STR(ManyMember(annotations, 2).c_str(), "(`C`)");
    TEST_EQ(
        CountStatus(annotations, BinaryRegionStatus::ERROR_INVALID_UNION_TYPE),
        0u);
  }

  // The `_type` vector ends the buffer. Cut off its last type and the last
  // byte of the one before: union[1] and union[2] have no whole type, and each
  // is still its own slot.
  {
    const std::vector<uint8_t> buffer = PackD(many);
    const std::vector<uint8_t> cut(buffer.begin(), buffer.end() - 5);
    const Annotations annotations = Annotate(bfbs, cut);
    TEST_EQ_STR(ManyMember(annotations, 0).c_str(), "(`A`)");
    CheckUnionSlot(annotations, buffer, 1,
                   BinaryRegionStatus::ERROR_INCOMPLETE_BINARY);
    CheckUnionSlot(annotations, buffer, 2,
                   BinaryRegionStatus::ERROR_INCOMPLETE_BINARY);
  }

  // union[1]'s offset points past the end of the binary; union[2] is still C.
  {
    std::vector<uint8_t> buffer = PackD(many);
    GetMutableRoot<UnionUnderlyingType::D>(buffer.data())
        ->mutable_test_vector_of_union()
        ->Mutate(1, Offset<void>(0x7FFFFFF0));
    const Annotations annotations = Annotate(bfbs, buffer);
    CheckUnionSlot(annotations, buffer, 1,
                   BinaryRegionStatus::ERROR_OFFSET_OUT_OF_BINARY);
    CheckUnionSlot(annotations, buffer, 2, BinaryRegionStatus::OK);
    TEST_EQ_STR(ManyMember(annotations, 2).c_str(), "(`C`)");
  }

  // A `_type` vector with one type for three unions. The Object API cannot
  // write one, so this uses the generated builder; the generated verifier
  // rejects it. union[1] and union[2] have no type, so no member.
  {
    FlatBufferBuilder fbb;
    const Offset<void> members[] = {
        UnionUnderlyingType::A::Pack(fbb, &a).Union(),
        UnionUnderlyingType::B::Pack(fbb, &b).Union(),
        UnionUnderlyingType::C::Pack(fbb, &c).Union()};
    const ABC types[] = {ABC::A};
    const auto types_vector = fbb.CreateVector(types, 1);
    const auto members_vector = fbb.CreateVector(members, 3);
    fbb.Finish(UnionUnderlyingType::CreateD(fbb, ABC::NONE, 0, types_vector,
                                            members_vector));
    Verifier verifier(fbb.GetBufferPointer(), fbb.GetSize());
    TEST_EQ(verifier.VerifyBuffer<UnionUnderlyingType::D>(nullptr), false);
    const std::vector<uint8_t> buffer(fbb.GetBufferPointer(),
                                      fbb.GetBufferPointer() + fbb.GetSize());

    const Annotations annotations = Annotate(bfbs, buffer);
    TEST_EQ_STR(ManyMember(annotations, 0).c_str(), "(`A`)");
    CheckUnionSlot(annotations, buffer, 1,
                   BinaryRegionStatus::ERROR_INVALID_UNION_TYPE);
    CheckUnionSlot(annotations, buffer, 2,
                   BinaryRegionStatus::ERROR_INVALID_UNION_TYPE);
    TEST_EQ(CountTables(annotations, "UnionUnderlyingType.B"), 0u);
    TEST_EQ(CountTables(annotations, "UnionUnderlyingType.C"), 0u);
  }
}

// A union with no `_type` to read. A single union's absent type is its
// default, NONE, which is valid; a vector of unions without its `_type` vector
// has 0 types, which is not. Either way the type is never read from the
// table's own vtable offset, which sits where an absent field's offset (0)
// points. A `_type` field whose vtable slot points outside the binary is left
// out of the vtable, so its union's type is unknown.
void AnnotateUnionWithoutTypeTest(const std::string& tests_data_path) {
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
  const reflection::Object* d_def =
      reflection::GetSchema(parser.builder_.GetBufferPointer())->root_table();
  const reflection::Field* type_field =
      d_def->fields()->LookupByKey("test_union_type");
  const reflection::Field* types_field =
      d_def->fields()->LookupByKey("test_vector_of_union_type");
  TEST_NOTNULL(type_field);
  TEST_NOTNULL(types_field);

  UnionUnderlyingType::AT a;
  a.a = 42;
  UnionUnderlyingType::BT b;
  b.b = "foo";
  UnionUnderlyingType::CT c;
  c.c = true;

  // test_union holds a B, but test_union_type is absent: its default, NONE,
  // is the type. The generated verifier accepts this. The B is not annotated.
  {
    FlatBufferBuilder fbb;
    const Offset<void> member = UnionUnderlyingType::B::Pack(fbb, &b).Union();
    fbb.Finish(UnionUnderlyingType::CreateD(fbb, ABC::NONE, member));
    Verifier verifier(fbb.GetBufferPointer(), fbb.GetSize());
    TEST_EQ(verifier.VerifyBuffer<UnionUnderlyingType::D>(nullptr), true);
    const std::vector<uint8_t> buffer(fbb.GetBufferPointer(),
                                      fbb.GetBufferPointer() + fbb.GetSize());
    TEST_NULL(GetAnyRoot(buffer.data())->GetAddressOf(type_field->offset()));

    const Annotations annotations = Annotate(bfbs, buffer);
    const BinaryRegion* single =
        FindRegion(annotations, BinaryRegionCommentType::TableOffsetField,
                   "test_union", 0);
    TEST_NOTNULL(single);
    if (single != nullptr) {
      TEST_EQ(single->comment.status, BinaryRegionStatus::OK);
      TEST_EQ_STR(single->comment.default_value.c_str(),
                  "(union of type `NONE`)");
    }
    TEST_EQ(CountTables(annotations, "UnionUnderlyingType.B"), 0u);
  }

  // test_union_type's vtable slot points outside the binary. No API writes a
  // vtable, so this sets that one slot of a packed buffer directly.
  {
    UnionUnderlyingType::DT d;
    d.test_union.Set(b);
    std::vector<uint8_t> buffer = PackD(d);
    uint8_t* vtable =
        const_cast<uint8_t*>(GetAnyRoot(buffer.data())->GetVTable());
    WriteScalar<voffset_t>(vtable + type_field->offset(), 0xFFFF);

    const Annotations annotations = Annotate(bfbs, buffer);
    const BinaryRegion* single =
        FindRegion(annotations, BinaryRegionCommentType::TableOffsetField,
                   "test_union", 0);
    TEST_NOTNULL(single);
    if (single != nullptr) {
      TEST_EQ(single->comment.status,
              BinaryRegionStatus::ERROR_INVALID_UNION_TYPE);
    }
    TEST_EQ(CountTables(annotations, "UnionUnderlyingType.B"), 0u);
  }

  // test_vector_of_union holds [A, B, C], but test_vector_of_union_type is
  // absent. The generated verifier rejects this. No union has a type.
  {
    FlatBufferBuilder fbb;
    const Offset<void> members[] = {
        UnionUnderlyingType::A::Pack(fbb, &a).Union(),
        UnionUnderlyingType::B::Pack(fbb, &b).Union(),
        UnionUnderlyingType::C::Pack(fbb, &c).Union()};
    const auto members_vector = fbb.CreateVector(members, 3);
    fbb.Finish(
        UnionUnderlyingType::CreateD(fbb, ABC::NONE, 0, 0, members_vector));
    Verifier verifier(fbb.GetBufferPointer(), fbb.GetSize());
    TEST_EQ(verifier.VerifyBuffer<UnionUnderlyingType::D>(nullptr), false);
    const std::vector<uint8_t> buffer(fbb.GetBufferPointer(),
                                      fbb.GetBufferPointer() + fbb.GetSize());
    TEST_NULL(GetAnyRoot(buffer.data())->GetAddressOf(types_field->offset()));

    const Annotations annotations = Annotate(bfbs, buffer);
    for (uoffset_t i = 0; i < 3; ++i) {
      CheckUnionSlot(annotations, buffer, i,
                     BinaryRegionStatus::ERROR_INVALID_UNION_TYPE);
    }
    TEST_EQ(CountTables(annotations, "UnionUnderlyingType.A"), 0u);
    TEST_EQ(CountTables(annotations, "UnionUnderlyingType.B"), 0u);
    TEST_EQ(CountTables(annotations, "UnionUnderlyingType.C"), 0u);
  }

  // test_vector_of_union_type's vtable slot points outside the binary.
  {
    UnionUnderlyingType::DT many;
    many.test_vector_of_union.resize(3);
    many.test_vector_of_union[0].Set(a);
    many.test_vector_of_union[1].Set(b);
    many.test_vector_of_union[2].Set(c);
    std::vector<uint8_t> buffer = PackD(many);
    uint8_t* vtable =
        const_cast<uint8_t*>(GetAnyRoot(buffer.data())->GetVTable());
    WriteScalar<voffset_t>(vtable + types_field->offset(), 0xFFFF);

    const Annotations annotations = Annotate(bfbs, buffer);
    for (uoffset_t i = 0; i < 3; ++i) {
      CheckUnionSlot(annotations, buffer, i,
                     BinaryRegionStatus::ERROR_INVALID_UNION_TYPE);
    }
    TEST_EQ(CountTables(annotations, "UnionUnderlyingType.A"), 0u);
    TEST_EQ(CountTables(annotations, "UnionUnderlyingType.B"), 0u);
    TEST_EQ(CountTables(annotations, "UnionUnderlyingType.C"), 0u);
  }
}

// The annotator labels each vtable slot once: as the slot of a field in the
// schema, or, past the schema's last field, as the slot of an unknown field.
// It counted only the slots it annotated without error, and labelled every slot
// from that count on as unknown. So for each field it reported as a required
// field not present, or as an offset outside the binary, it annotated a later
// slot a second time.
void AnnotateVTableSlotsOnceTest(const std::string& tests_data_path) {
  std::string schemafile;
  TEST_EQ(LoadFile((tests_data_path + "vtable_slot_count_test.fbs").c_str(),
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

  // A buffer without `name`, which Reader requires.
  {
    VTableSlotCount::WriterT writer_t;
    writer_t.count = 7;
    FlatBufferBuilder fbb;
    fbb.Finish(VTableSlotCount::Writer::Pack(fbb, &writer_t));
    Verifier writer_verifier(fbb.GetBufferPointer(), fbb.GetSize());
    TEST_EQ(writer_verifier.VerifyBuffer<VTableSlotCount::Writer>(nullptr),
            true);
    Verifier reader_verifier(fbb.GetBufferPointer(), fbb.GetSize());
    TEST_EQ(VTableSlotCount::VerifyReaderBuffer(reader_verifier), false);

    const std::vector<BinaryRegion> vtable =
        VTableRegions(Annotate(bfbs, FinishedBuffer(fbb)));
    TEST_EQ(EachAtItsOwnOffset(vtable), true);
    TEST_EQ(CountType(vtable, BinaryRegionCommentType::VTableFieldOffset), 3u);
    TEST_EQ(
        CountType(vtable, BinaryRegionCommentType::VTableUnknownFieldOffset),
        0u);
    CheckSlot(vtable, "gone", 0, BinaryRegionStatus::OK);
    CheckSlot(vtable, "name", 1,
              BinaryRegionStatus::ERROR_REQUIRED_FIELD_NOT_PRESENT);
    CheckSlot(vtable, "count", 2, BinaryRegionStatus::OK);
  }

  // The same, written at a newer version: the one slot past Reader's fields is
  // still annotated as unknown, and only that one.
  {
    VTableSlotCount::NewerWriterT newer_t;
    newer_t.count = 7;
    newer_t.extra = 9;
    FlatBufferBuilder fbb;
    fbb.Finish(VTableSlotCount::NewerWriter::Pack(fbb, &newer_t));
    Verifier verifier(fbb.GetBufferPointer(), fbb.GetSize());
    TEST_EQ(verifier.VerifyBuffer<VTableSlotCount::NewerWriter>(nullptr), true);

    const std::vector<BinaryRegion> vtable =
        VTableRegions(Annotate(bfbs, FinishedBuffer(fbb)));
    TEST_EQ(EachAtItsOwnOffset(vtable), true);
    TEST_EQ(CountType(vtable, BinaryRegionCommentType::VTableFieldOffset), 3u);
    TEST_EQ(
        CountType(vtable, BinaryRegionCommentType::VTableUnknownFieldOffset),
        1u);
    for (const BinaryRegion& region : vtable) {
      if (region.comment.type ==
          BinaryRegionCommentType::VTableUnknownFieldOffset) {
        TEST_EQ(region.comment.index, 3u);
      }
    }
    CheckSlot(vtable, "name", 1,
              BinaryRegionStatus::ERROR_REQUIRED_FIELD_NOT_PRESENT);
  }

  // A slot whose offset points past the end of the buffer, for a field ahead
  // of the last, and for the last.
  const voffset_t past_the_end = std::numeric_limits<voffset_t>::max();
  {
    const std::vector<uint8_t> buffer =
        ReaderWithSlot(VTableSlotCount::Reader::VT_NAME, past_the_end);
    TEST_EQ(buffer.size() < past_the_end, true);

    const std::vector<BinaryRegion> vtable =
        VTableRegions(Annotate(bfbs, buffer));
    TEST_EQ(EachAtItsOwnOffset(vtable), true);
    TEST_EQ(CountType(vtable, BinaryRegionCommentType::VTableFieldOffset), 3u);
    TEST_EQ(
        CountType(vtable, BinaryRegionCommentType::VTableUnknownFieldOffset),
        0u);
    CheckSlot(vtable, "name", 1,
              BinaryRegionStatus::ERROR_OFFSET_OUT_OF_BINARY);
    CheckSlot(vtable, "count", 2, BinaryRegionStatus::OK);
  }
  {
    const std::vector<uint8_t> buffer =
        ReaderWithSlot(VTableSlotCount::Reader::VT_COUNT, past_the_end);

    const std::vector<BinaryRegion> vtable =
        VTableRegions(Annotate(bfbs, buffer));
    TEST_EQ(EachAtItsOwnOffset(vtable), true);
    TEST_EQ(CountType(vtable, BinaryRegionCommentType::VTableFieldOffset), 3u);
    TEST_EQ(
        CountType(vtable, BinaryRegionCommentType::VTableUnknownFieldOffset),
        0u);
    CheckSlot(vtable, "name", 1, BinaryRegionStatus::OK);
    CheckSlot(vtable, "count", 2,
              BinaryRegionStatus::ERROR_OFFSET_OUT_OF_BINARY);
  }
}



// A field marked both required and deprecated need not be present: the
// generated builder's Finish() does not require it, and the generated verifier
// skips it. The annotator reported such a field, when absent, as
// ERROR_REQUIRED_FIELD_NOT_PRESENT.
void AnnotateDeprecatedRequiredFieldTest(const std::string& tests_data_path) {
  std::string schemafile;
  TEST_EQ(LoadFile((tests_data_path + "deprecated_required_test.fbs").c_str(),
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

  // The premise: the binary schema keeps `gone` required as well as
  // deprecated, and `name` required only.
  const reflection::Object* reader =
      reflection::GetSchema(parser.builder_.GetBufferPointer())->root_table();
  TEST_NOTNULL(reader);
  if (!reader) return;
  const reflection::Field* gone = reader->fields()->LookupByKey("gone");
  TEST_NOTNULL(gone);
  if (!gone) return;
  TEST_EQ(gone->required(), true);
  TEST_EQ(gone->deprecated(), true);
  const reflection::Field* name = reader->fields()->LookupByKey("name");
  TEST_NOTNULL(name);
  if (!name) return;
  TEST_EQ(name->required(), true);
  TEST_EQ(name->deprecated(), false);

  // What Reader's generated code writes and accepts: it cannot set `gone`.
  {
    DeprecatedRequired::ReaderT reader_t;
    reader_t.name = "name";
    FlatBufferBuilder fbb;
    DeprecatedRequired::FinishReaderBuffer(
        fbb, DeprecatedRequired::Reader::Pack(fbb, &reader_t));
    Verifier verifier(fbb.GetBufferPointer(), fbb.GetSize());
    TEST_EQ(DeprecatedRequired::VerifyReaderBuffer(verifier), true);

    const Annotations annotations = Annotate(bfbs, FinishedBuffer(fbb));
    TEST_EQ(CountStatus(annotations,
                        BinaryRegionStatus::ERROR_REQUIRED_FIELD_NOT_PRESENT),
            0u);
    // `gone` has a slot, holding 0, so the annotator did look at it.
    const BinaryRegion* gone_slot = FindSlot(annotations, "gone", 0);
    TEST_NOTNULL(gone_slot);
    if (gone_slot) {
      TEST_EQ(gone_slot->comment.status, BinaryRegionStatus::OK);
      TEST_EQ_STR(gone_slot->comment.default_value.c_str(), "<null> (String)");
    }
    // Nor is `name`'s slot annotated a second time, as an unknown field.
    TEST_NULL(FindRegion(
        annotations, BinaryRegionCommentType::VTableUnknownFieldOffset, "", 1));
  }

  // A buffer without `name`, which Reader's generated builder refuses to write
  // and its generated verifier rejects, is still an error, and the only one.
  {
    DeprecatedRequired::WriterT writer_t;
    writer_t.count = 7;  // Gives `gone` and `name` slots, each holding 0.
    FlatBufferBuilder fbb;
    fbb.Finish(DeprecatedRequired::Writer::Pack(fbb, &writer_t));
    Verifier writer_verifier(fbb.GetBufferPointer(), fbb.GetSize());
    TEST_EQ(writer_verifier.VerifyBuffer<DeprecatedRequired::Writer>(nullptr),
            true);
    Verifier reader_verifier(fbb.GetBufferPointer(), fbb.GetSize());
    TEST_EQ(DeprecatedRequired::VerifyReaderBuffer(reader_verifier), false);

    const Annotations annotations = Annotate(bfbs, FinishedBuffer(fbb));
    TEST_EQ(CountStatus(annotations,
                        BinaryRegionStatus::ERROR_REQUIRED_FIELD_NOT_PRESENT),
            1u);
    const BinaryRegion* name_slot = FindSlot(annotations, "name", 1);
    TEST_NOTNULL(name_slot);
    if (name_slot) {
      TEST_EQ(name_slot->comment.status,
              BinaryRegionStatus::ERROR_REQUIRED_FIELD_NOT_PRESENT);
    }
    const BinaryRegion* gone_slot = FindSlot(annotations, "gone", 0);
    TEST_NOTNULL(gone_slot);
    if (gone_slot) {
      TEST_EQ(gone_slot->comment.status, BinaryRegionStatus::OK);
    }
  }
}


}  // namespace tests
}  // namespace flatbuffers
