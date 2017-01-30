INCLUDE(CheckCXXSourceCompiles)

set(CMAKE_REQUIRED_FLAGS "-std=c++11")

# ---[ Check if the data type long and int32_t/int64_t overlap. 
CHECK_CXX_SOURCE_COMPILES(
    "#include <cstdint>

    template <typename T> void Foo();
    template<> void Foo<int32_t>() {}
    template<> void Foo<int64_t>() {}
    int main(int argc, char** argv) {
      Foo<long>();
      return 0;
    }" CAFFE2_LONG_IS_INT32_OR_64)

if (CAFFE2_LONG_IS_INT32_OR_64)
  message(STATUS "Does not need to define long separately.")
else()
  message(STATUS "Need to define long as a separate typeid.")
  add_definitions(-DCAFFE2_UNIQUE_LONG_TYPEMETA)
endif()

# ---[ Check if __builtin_cpu_supports is supported by the compiler
CHECK_CXX_SOURCE_COMPILES(
    "#include <iostream>

    int main(int argc, char** argv) {
      std::cout << __builtin_cpu_supports(\"avx2\") << std::endl;
      return 0;
    }" HAS_BUILTIN_CPU_SUPPORTS)
if (HAS_BUILTIN_CPU_SUPPORTS)
  message(STATUS "This compiler has builtin_cpu_supports feature.")
else()
  message(STATUS "This compiler does not have builtin_cpu_supports feature.")
  add_definitions(-DCAFFE2_NO_BUILTIN_CPU_SUPPORTS)
endif()
