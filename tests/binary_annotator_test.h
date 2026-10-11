#ifndef TESTS_BINARY_ANNOTATOR_TEST_H
#define TESTS_BINARY_ANNOTATOR_TEST_H

#include <string>

namespace flatbuffers {
namespace tests {

void AnnotateUnionByValueTest(const std::string& tests_data_path);
void AnnotateUnionUnderlyingTypeTest(const std::string& tests_data_path);
void AnnotateUnionVectorTest(const std::string& tests_data_path);

}  // namespace tests
}  // namespace flatbuffers

#endif
