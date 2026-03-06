cmake_minimum_required(VERSION 3.19)
project(hymission VERSION "0.1.0")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_definitions(WLR_USE_UNSTABLE)
add_compile_options(
  -g
  -Wall
  -Wextra
  -Wno-unused-parameter
  -Wno-unused-value
  -Wno-missing-field-initializers
  -Wno-narrowing
  -Wno-pointer-arith
  --no-gnu-unique
)

if(CMAKE_EXPORT_COMPILE_COMMANDS)
  set(CMAKE_CXX_STANDARD_INCLUDE_DIRECTORIES
      ${CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES})
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(HYPR_DEPS REQUIRED hyprland)

add_library(hymission SHARED
  src/main.cpp
  src/mission_layout.cpp
  src/overview_logic.cpp
  src/overview_controller.cpp
)

target_include_directories(hymission PRIVATE ${HYPR_DEPS_INCLUDE_DIRS})
target_link_directories(hymission PRIVATE ${HYPR_DEPS_LIBRARY_DIRS})
target_link_libraries(hymission PRIVATE ${HYPR_DEPS_LIBRARIES})

add_executable(hymission-layout-demo
  tools/layout_demo.cpp
  src/mission_layout.cpp
)

target_include_directories(hymission-layout-demo PRIVATE src)

add_executable(hymission-overview-logic-test
  tools/overview_logic_test.cpp
  src/overview_logic.cpp
)

target_include_directories(hymission-overview-logic-test PRIVATE src)

install(TARGETS hymission LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
