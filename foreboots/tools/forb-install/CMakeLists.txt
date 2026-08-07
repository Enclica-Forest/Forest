cmake_minimum_required(VERSION 3.20)
project(forb-install LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
add_compile_options(-Wall -Wextra -Wpedantic)

find_package(ZLIB REQUIRED)

# REPO_ROOT: the ForeB tree that holds BOOTX64.EFI / forebo.cfg / assets.
set(REPO_ROOT "${CMAKE_SOURCE_DIR}/../.." CACHE PATH "ForeB repo root")

# ---------------------------------------------------------------------------
#  Host tool: bin2c (binary -> C array). Built first, used to embed payload.
# ---------------------------------------------------------------------------
add_executable(forb_bin2c tools/bin2c.cpp)

# ---------------------------------------------------------------------------
#  Payload staging: assemble the exact ESP-relative install tree, tar it,
#  then embed it as a C byte array.
# ---------------------------------------------------------------------------
set(PAYLOAD_STAGE "${CMAKE_BINARY_DIR}/payload")
set(PAYLOAD_TAR   "${CMAKE_BINARY_DIR}/payload.tar")
set(PAYLOAD_SRC   "${CMAKE_BINARY_DIR}/generated/payload_blob.c")

add_custom_command(
  OUTPUT ${PAYLOAD_TAR}
  COMMAND ${CMAKE_COMMAND} -E rm -rf ${PAYLOAD_STAGE}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${PAYLOAD_STAGE}/EFI/forb
  COMMAND ${CMAKE_COMMAND} -E make_directory ${PAYLOAD_STAGE}/forebo/icons
  COMMAND ${CMAKE_COMMAND} -E copy ${REPO_ROOT}/BOOTX64.EFI
          ${PAYLOAD_STAGE}/EFI/forb/BOOTX64.EFI
  COMMAND ${CMAKE_COMMAND} -E copy ${REPO_ROOT}/forebo.cfg
          ${PAYLOAD_STAGE}/forebo/forebo.cfg
  COMMAND ${CMAKE_COMMAND} -E copy ${REPO_ROOT}/assets/bg.bmp
          ${PAYLOAD_STAGE}/forebo/bg.bmp
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${REPO_ROOT}/assets/icons
          ${PAYLOAD_STAGE}/forebo/icons
  COMMAND ${CMAKE_COMMAND} -E chdir ${PAYLOAD_STAGE}
          ${CMAKE_COMMAND} -E tar cf ${PAYLOAD_TAR} --format=gnutar .
  DEPENDS ${REPO_ROOT}/BOOTX64.EFI ${REPO_ROOT}/forebo.cfg
          ${REPO_ROOT}/assets/bg.bmp
  COMMENT "Staging + taring ForeB payload for embedding"
  VERBATIM)

add_custom_command(
  OUTPUT ${PAYLOAD_SRC}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/generated
  COMMAND forb_bin2c ${PAYLOAD_TAR} ${PAYLOAD_SRC} forb_payload_tar
  DEPENDS ${PAYLOAD_TAR} forb_bin2c
  COMMENT "Embedding payload.tar -> C byte array"
  VERBATIM)

# The embedded blob is large; compile it without optimization for speed.
set_source_files_properties(${PAYLOAD_SRC} PROPERTIES
  COMPILE_OPTIONS "-O0"
  GENERATED TRUE)

file(GLOB FORB_SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_executable(forb-install ${FORB_SOURCES} ${PAYLOAD_SRC})
target_include_directories(forb-install PRIVATE include)
target_link_libraries(forb-install PRIVATE ZLIB::ZLIB)
target_compile_definitions(forb-install PRIVATE
  FORB_TESTS_DIR="${CMAKE_SOURCE_DIR}/tests")

# Drop-in `forb-mkrescue`: a symlink to forb-install; argv0 dispatch routes it
# to the grub-mkrescue-compatible rescue-image maker.
add_custom_command(TARGET forb-install POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E create_symlink
          forb-install $<TARGET_FILE_DIR:forb-install>/forb-mkrescue
  COMMENT "Creating forb-mkrescue -> forb-install symlink")

enable_testing()
add_test(NAME selftest COMMAND forb-install --selftest)
