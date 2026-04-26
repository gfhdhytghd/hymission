cmake_minimum_required(VERSION 3.19)
project(hymission VERSION "0.2.1")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(CTest)

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

if(BUILD_TESTING)
  add_executable(hymission-mission-layout-test
    tools/mission_layout_test.cpp
    src/mission_layout.cpp
  )

  target_include_directories(hymission-mission-layout-test PRIVATE src)
  add_test(NAME hymission-mission-layout-test COMMAND hymission-mission-layout-test)

  add_executable(hymission-overview-logic-test
    tools/overview_logic_test.cpp
    src/overview_logic.cpp
  )

  target_include_directories(hymission-overview-logic-test PRIVATE src)
  add_test(NAME hymission-overview-logic-test COMMAND hymission-overview-logic-test)
endif()

install(TARGETS hymission LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
