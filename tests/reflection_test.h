#ifndef TESTS_REFLECTION_TEST_H
#define TESTS_REFLECTION_TEST_H

#include <stdint.h>

#include <string>

namespace flatbuffers {
namespace tests {

void ReflectionTest(const std::string& tests_data_path, uint8_t* flatbuf,
                    size_t length);
void ReflectionVerifyOffsetFieldsTest();
void ReflectionVerifyRequiredOffsetFieldsTest();
void ReflectionVerifyUnionTypeSlotsTest(const std::string& tests_data_path);
void ForAllFieldsReverseTest(const std::string& tests_data_path);
void MiniReflectFixedLengthArrayTest();
void MiniReflectFlatBuffersTest(uint8_t* flatbuf);
void MiniReflectUnionUnderlyingTypeTest();

}  // namespace tests
}  // namespace flatbuffers

#endif
