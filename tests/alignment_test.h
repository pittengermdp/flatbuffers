#ifndef TESTS_ALIGNMENT_TEST_H
#define TESTS_ALIGNMENT_TEST_H

#include <string>

namespace flatbuffers {
namespace tests {

void AlignmentTest();
void LargeStructsAlignmentTest(const std::string& tests_data_path);

}  // namespace tests
}  // namespace flatbuffers

#endif
