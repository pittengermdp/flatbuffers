#ifndef TESTS_REFLECTION_TEST_H
#define TESTS_REFLECTION_TEST_H

#include <stdint.h>

#include <string>

namespace flatbuffers {
namespace tests {

void ReflectionTest(const std::string& tests_data_path, uint8_t* flatbuf,
                    size_t length);
void ReflectionUnionUnderlyingTypeTest(const std::string& tests_data_path);
void ReflectionVerifyUnionByValueTest(const std::string& tests_data_path);
void SetStringTest(const std::string& tests_data_path);
void ReflectionVerifyOffsetFieldsTest();
void ReflectionVerifyRequiredOffsetFieldsTest();
void ReflectionVerifyUnionTypeSlotsTest(const std::string& tests_data_path);
void ReflectionUnionUnderlyingTypeToStringTest(
    const std::string& tests_data_path);
void ForAllFieldsReverseTest(const std::string& tests_data_path);
void ReflectionVerifyUnionValueSlotTest(const std::string& tests_data_path);
void ReflectionVerifyUnionTypesSlotTest(const std::string& tests_data_path);
void ReflectionUnionUnderlyingTypeAnyFieldTest(
    const std::string& tests_data_path);
void CopyTableUnionTest(const std::string& tests_data_path);
void CopyTableStructVectorAlignmentTest();
void MiniReflectFixedLengthArrayTest();
void MiniReflectFlatBuffersTest(uint8_t* flatbuf);
void ReflectionVerifyUnionVectorWithoutTypesTest(
    const std::string& tests_data_path);
void MiniReflectUnionUnderlyingTypeTest();

}  // namespace tests
}  // namespace flatbuffers

#endif
