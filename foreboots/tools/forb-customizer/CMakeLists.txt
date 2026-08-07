cmake_minimum_required(VERSION 3.21)
project(forb_customizer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets)

set(APP_SOURCES
  main.cpp
  src/ConfigModel.cpp
  src/Bound.cpp
  src/PreviewWidget.cpp
  src/EntriesTab.cpp
  src/PresetGallery.cpp
  src/Inspector.cpp
  src/MainWindow.cpp
)

add_executable(forb-customizer ${APP_SOURCES})
target_include_directories(forb-customizer PRIVATE src)
target_link_libraries(forb-customizer PRIVATE Qt6::Widgets)

# ---- optional unit tests (QtTest) ----
enable_testing()
find_package(Qt6 QUIET COMPONENTS Test)
if(Qt6Test_FOUND)
  add_executable(tst_roundtrip
    tests/tst_roundtrip.cpp
    src/ConfigModel.cpp)
  target_include_directories(tst_roundtrip PRIVATE src)
  target_link_libraries(tst_roundtrip PRIVATE Qt6::Test Qt6::Gui Qt6::Core)
  add_test(NAME roundtrip COMMAND tst_roundtrip)
endif()
