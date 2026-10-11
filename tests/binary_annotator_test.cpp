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
#include "tests/deprecated_required_test_generated.h"

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

// The vtable slot annotated for the field `name` with `id`, or null. The text
// output prints it as "offset to field `name` (id: N)".
const BinaryRegion* FindSlot(const Annotations& annotations,
                             const std::string& name, const int id) {
  return FindRegion(annotations, BinaryRegionCommentType::VTableFieldOffset,
                    name + "` (id: " + std::to_string(id) + ")", 0);
}

}  // namespace

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
