# NOTE The followings SHOULD be defined before using this CMakeLists.txt
#
#  'TensorFlowSource_DIR' variable
#  'FlatBuffersSource_DIR' variable
#  'eigen' target
#  'gemmlowp' target
#  'neon2sse' target
#  'farmhash' target
#
set(TensorFlowLiteSource_DIR ${TensorFlowSource_DIR}/tensorflow/contrib/lite)

file(GLOB CORE_SRCS "${TensorFlowLiteSource_DIR}/*.c" "${TensorFlowLiteSource_DIR}/*.cc")
file(GLOB CORE_TESTS "${TensorFlowLiteSource_DIR}/*test*.cc")
list(REMOVE_ITEM CORE_SRCS ${CORE_TESTS})

file(GLOB_RECURSE KERNEL_SRCS "${TensorFlowLiteSource_DIR}/kernels/*.cc")
file(GLOB_RECURSE KERNEL_TESTS "${TensorFlowLiteSource_DIR}/kernels/*test*.cc")
list(REMOVE_ITEM KERNEL_SRCS ${KERNEL_TESTS})
# Exclude buggy kernel(s) from the build
list(REMOVE_ITEM KERNEL_SRCS "${TensorFlowLiteSource_DIR}/kernels/internal/spectrogram.cc")

list(APPEND SRCS ${CORE_SRCS})
list(APPEND SRCS ${KERNEL_SRCS})

include(CheckCXXCompilerFlag)

CHECK_CXX_COMPILER_FLAG(-Wno-extern-c-compat COMPILER_SUPPORT_EXTERN_C_COMPAT_WARNING)

add_library(tensorflowlite-1.7 ${SRCS})
set_target_properties(tensorflowlite-1.7 PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(tensorflowlite-1.7 PUBLIC ${TensorFlowSource_DIR})
target_include_directories(tensorflowlite-1.7 PUBLIC ${FlatBuffersSource_DIR}/include)
target_compile_options(tensorflowlite-1.7 PUBLIC -Wno-ignored-attributes)
if(COMPILER_SUPPORT_EXTERN_C_COMPAT_WARNING)
  target_compile_options(tensorflowlite-1.7 PUBLIC -Wno-extern-c-compat)
endif(COMPILER_SUPPORT_EXTERN_C_COMPAT_WARNING)
target_compile_definitions(tensorflowlite-1.7 PUBLIC "GEMMLOWP_ALLOW_SLOW_SCALAR_FALLBACK")
target_link_libraries(tensorflowlite-1.7 eigen gemmlowp neon2sse farmhash dl)
