#----------------------------------------------------------------
# Generated CMake target import file.
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "serial" for configuration ""
set_property(TARGET serial APPEND PROPERTY IMPORTED_CONFIGURATIONS NOCONFIG)
set_target_properties(serial PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_NOCONFIG "CXX"
  IMPORTED_LINK_INTERFACE_LIBRARIES_NOCONFIG "rt;pthread"
  IMPORTED_LOCATION_NOCONFIG "/mnt/c/Users/banhq/Documents/Projects/DMX.chug/serial/lib/libserial.a"
  )

list(APPEND _cmake_import_check_targets serial )
list(APPEND _cmake_import_check_files_for_serial "/mnt/c/Users/banhq/Documents/Projects/DMX.chug/serial/lib/libserial.a" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
