#include "binary_annotator_test.h"

#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "binary_annotator.h"
#include "flatbuffers/idl.h"
#include "flatbuffers/reflection.h"
#include "flatbuffers/util.h"
#include "test_assert.h"
#include "tests/vtable_slot_count_test_generated.h"

namespace flatbuffers {
namespace tests {
namespace {

typedef std::map<uint64_t, BinarySection> Annotations;

// The finished buffer in `fbb`.
std::vector<uint8_t> FinishedBuffer(const FlatBufferBuilder& fbb) {
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

}  // namespace

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

}  // namespace tests
}  // namespace flatbuffers
