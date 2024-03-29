# To use this template, 
cmake_minimum_required(VERSION 3.11)
project(SKOverlayApp VERSION "0.1.0" LANGUAGES CXX C)

# Grab and build StereoKit from the GitHub repository. Here we're setting SK up
# as a statically linked library.
include(FetchContent)
set(SK_BUILD_TESTS       OFF CACHE INTERNAL "")
set(SK_BUILD_SHARED_LIBS OFF CACHE INTERNAL "")
FetchContent_Declare(
  StereoKitC
  GIT_REPOSITORY https://github.com/StereoKit/StereoKit.git
  GIT_TAG        v0.3.9
)
FetchContent_MakeAvailable(StereoKitC)

# Import Varjo SDK.
add_library(VarjoLib SHARED IMPORTED)
set_property(TARGET VarjoLib PROPERTY IMPORTED_IMPLIB ${CMAKE_CURRENT_SOURCE_DIR}/Varjo-SDK/lib/VarjoLib.lib)
set_property(TARGET VarjoLib APPEND PROPERTY INTERFACE_INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/Varjo-SDK/include)

# Import Detours.
set(DETOURS_SRC
	Detours/src/creatwth.cpp
	Detours/src/detours.cpp
	Detours/src/disasm.cpp
	Detours/src/disolx64.cpp
	Detours/src/image.cpp
	Detours/src/modules.cpp)
add_compile_definitions(_AMD64_)
include_directories(Detours/src)

# Add source files to our target executable
add_executable(SKOverlayApp
  src/main.cpp
  src/utils.h
  ${DETOURS_SRC}
)

target_compile_features(SKOverlayApp PRIVATE cxx_std_17)

# Link to dependencies
target_link_libraries(SKOverlayApp
  PRIVATE StereoKitC VarjoLib
)
