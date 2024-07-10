# - Find Ragel executable and provides macros to generate custom build rules
# The module defines the following variables:
#
#  RAGEL_EXECUTABLE - path to the ragel program
#  RAGEL_VERSION - version of ragel
#  RAGEL_FOUND - true if the program was found
#
# If ragel is found, the module defines the macros:
#
#  RAGEL_TARGET(<Name> INPUTS <inputs> OUTPUT <output>
#              [COMPILE_FLAGS <string>] [DEPENDS <depends>])
#
# which will create  a custom rule to generate a state machine. <RagelInp> is
# the path to a Ragel file. <CodeOutput> is the name of the source file
# generated by ragel. If  COMPILE_FLAGS option is specified, the next
# parameter is  added in the ragel  command line.
#
# The macro defines a set of variables:
#  RAGEL_${Name}_DEFINED       - true is the macro ran successfully
#  RAGEL_${Name}_INPUT         - The input source file, an alias for <RagelInp>
#  RAGEL_${Name}_OUTPUT_SOURCE - The source file generated by ragel
#  RAGEL_${Name}_OUTPUT_HEADER - The header file generated by ragel
#  RAGEL_${Name}_OUTPUTS       - The sources files generated by ragel
#  RAGEL_${Name}_COMPILE_FLAGS - Options used in the ragel command line
#
#  ====================================================================
#  Example:
#
#   find_package(RAGEL) # or e.g.: find_package(RAGEL 6.6 REQUIRED)
#   RAGEL_TARGET(MyMachine machine.rl ${CMAKE_CURRENT_BINARY_DIR}/machine.cc)
#   add_executable(Foo main.cc ${RAGEL_MyMachine_OUTPUTS})
#  ====================================================================

# 2014-02-09, Georg Sauthoff <mail@georg.so>
#
# I don't think that these few lines are even copyrightable material,
# but I am fine with using the BSD/MIT/GPL license on it ...
#
# I've used following references:
# http://www.cmake.org/cmake/help/v2.8.12/cmake.html
# /usr/share/cmake/Modules/FindFLEX.cmake
# /usr/share/cmake/Modules/FindBISON.cmake

find_program(RAGEL_EXECUTABLE NAMES ragel DOC "path to the ragel executable")
mark_as_advanced(RAGEL_EXECUTABLE)

if(RAGEL_EXECUTABLE)

  execute_process(COMMAND ${RAGEL_EXECUTABLE} --version
    OUTPUT_VARIABLE RAGEL_version_output
    ERROR_VARIABLE  RAGEL_version_error
    RESULT_VARIABLE RAGEL_version_result
    OUTPUT_STRIP_TRAILING_WHITESPACE)

  if(${RAGEL_version_result} EQUAL 0)
    string(REGEX REPLACE "^Ragel State Machine Compiler version ([^ ]+) .*$"
                         "\\1"
                         RAGEL_VERSION "${RAGEL_version_output}")
  else()
    message(SEND_ERROR
            "Command \"${RAGEL_EXECUTABLE} --version\" failed with output:
${RAGEL_version_error}")
  endif()

  #============================================================
  # RAGEL_TARGET (public macro)
  #============================================================
  #
  macro(RAGEL_TARGET Name)
    CMAKE_PARSE_ARGUMENTS(RAGEL "" "OUTPUT"
		"INPUTS;DEPENDS;COMPILE_FLAGS" ${ARGN})

    file(RELATIVE_PATH RAGEL_OUTPUT_RELATIVE "${CMAKE_CURRENT_BINARY_DIR}"
         "${RAGEL_OUTPUT}")
    file(RELATIVE_PATH RAGEL_INPUTS_RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}"
         "${RAGEL_INPUTS}")

    add_custom_command(OUTPUT ${RAGEL_OUTPUT}
      COMMAND ${RAGEL_EXECUTABLE}
      ARGS    ${RAGEL_COMPILE_FLAGS}
              -o${RAGEL_OUTPUT} ${RAGEL_INPUTS}
      DEPENDS ${RAGEL_INPUTS} ${RAGEL_DEPENDS}
      COMMENT
          "[RAGEL][${Name}] Compiling state machine with Ragel ${RAGEL_VERSION} -> ${RAGEL_OUTPUT}"
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    get_filename_component(src_target ${RAGEL_INPUTS} NAME_WE)
    add_custom_target(ragel_${src_target} DEPENDS ${RAGEL_OUTPUT})
    set_source_files_properties(${RAGEL_OUTPUT} PROPERTIES GENERATED TRUE)

    set(RAGEL_${Name}_DEFINED       TRUE)
    set(RAGEL_${Name}_OUTPUTS       ${RAGEL_OUTPUT})
    set(RAGEL_${Name}_INPUT         ${RAGEL_INPUTS})
    set(RAGEL_${Name}_COMPILE_FLAGS ${RAGEL_COMPILE_FLAGS})
  endmacro()

endif()

# use this include when module file is located under /usr/share/cmake/Modules
#include(${CMAKE_CURRENT_LIST_DIR}/FindPackageHandleStandardArgs.cmake)
# use this include when module file is located in build tree
include(FindPackageHandleStandardArgs)
FIND_PACKAGE_HANDLE_STANDARD_ARGS(RAGEL REQUIRED_VARS  RAGEL_EXECUTABLE
                                        VERSION_VAR    RAGEL_VERSION)