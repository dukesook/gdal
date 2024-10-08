# depends from gpkg, osm
add_gdal_driver(
  TARGET ogr_SQLite
  BUILTIN
  SOURCES ogr_sqlite.h
          ogrsqliteexecutesql.h
          ogrsqlitesqlfunctions.h
          ogrsqlitevirtualogr.h
          ogrsqliteutility.h
          ogrsqlitedatasource.cpp
          ogrsqlitedriver.cpp
          ogrsqlitelayer.cpp
          ogrsqliteselectlayer.cpp
          ogrsqlitesinglefeaturelayer.cpp
          ogrsqlitetablelayer.cpp
          ogrsqlitevfs.cpp
          ogrsqliteviewlayer.cpp
          ogrsqliteexecutesql.cpp
          ogrsqlitevirtualogr.cpp
          ogrsqliteutility.cpp
          ogrsqliteapiroutines.c
          rasterlite2_header.h
          rasterlite2.cpp
          sqlite_rtree_bulk_load/wrapper.cpp
   STRONG_CXX_WFLAGS
)
gdal_standard_includes(ogr_SQLite)

# I'm not completely sure why this is needed, but otherwise
# autotest/cpp/gdal_unit_test crashes in test_ogr.GDALDatasetSetQueryLoggerFunc
# This may be related to the sqlite3 extension stuff in ogrsqliteapiroutines.c
# and ogrsqlitevirtualogr.cpp
set_property(TARGET ogr_SQLite PROPERTY UNITY_BUILD OFF)

target_compile_definitions(ogr_SQLite PRIVATE -DHAVE_SQLITE)
gdal_target_link_libraries(ogr_SQLite PRIVATE SQLite::SQLite3)
gdal_target_link_libraries(ogr_SQLite PRIVATE PROJ::proj)

include(CMakePushCheckState)
include(CheckIncludeFile)
cmake_push_check_state(RESET)
set(CMAKE_REQUIRED_INCLUDES "${SQLite3_INCLUDE_DIRS}")
check_include_file("sqlite3ext.h" HAVE_SQLITE3EXT_H)
cmake_pop_check_state()

if (HAVE_SQLITE3EXT_H)
  target_compile_definitions(ogr_SQLite PRIVATE -DHAVE_SQLITE3EXT_H)
else()
  message(WARNING "sqlite3ext.h is missing. SQLite VirtualOGR and SQLite SQL dialect will be missing")
endif()

if (SQLite3_HAS_LOAD_EXTENSION)
  option(OGR_SQLITE_ALLOW_LOAD_EXTENSIONS "Whether SQLite extension loading is enabled through the OGR_SQLITE_LOAD_EXTENSIONS configuration option" ON)
  if (OGR_SQLITE_ALLOW_LOAD_EXTENSIONS)
    target_compile_definitions(ogr_SQLite PRIVATE -DOGR_SQLITE_ALLOW_LOAD_EXTENSIONS)
  endif()
endif()

if (GDAL_USE_PCRE2)
  target_compile_definitions(ogr_SQLite PRIVATE -DHAVE_PCRE2)
  gdal_target_link_libraries(ogr_SQLite PRIVATE PCRE2::PCRE2-8)
elseif (GDAL_USE_PCRE)
  target_compile_definitions(ogr_SQLite PRIVATE -DHAVE_PCRE)
  gdal_target_link_libraries(ogr_SQLite PRIVATE PCRE::PCRE)
endif ()

if (SQLite3_HAS_COLUMN_METADATA)
  target_compile_definitions(ogr_SQLite PRIVATE -DSQLITE_HAS_COLUMN_METADATA)
endif ()

if (SQLite3_HAS_NON_DEPRECATED_AUTO_EXTENSION)
  target_compile_definitions(ogr_SQLite PRIVATE -DSQLITE_HAS_NON_DEPRECATED_AUTO_EXTENSION)
endif ()

if (GDAL_USE_SPATIALITE)
  check_sqlite3_rtree("Spatialite")
  target_compile_definitions(ogr_SQLite PRIVATE -DHAVE_SPATIALITE)
  gdal_target_link_libraries(ogr_SQLite PRIVATE SPATIALITE::SPATIALITE)
endif ()
if (GDAL_USE_RASTERLITE2)
  target_compile_definitions(ogr_SQLite PRIVATE -DHAVE_RASTERLITE2)
  gdal_target_link_libraries(ogr_SQLite PRIVATE RASTERLITE2::RASTERLITE2)
endif ()

# Test utility
add_executable(test_load_virtual_ogr EXCLUDE_FROM_ALL test_load_virtual_ogr.c)
gdal_standard_includes(test_load_virtual_ogr)
target_link_libraries(test_load_virtual_ogr PRIVATE SQLite::SQLite3)
if (GDAL_USE_SPATIALITE)
  target_include_directories(test_load_virtual_ogr PRIVATE ${SPATIALITE_INCLUDE_DIRS})
  target_compile_definitions(test_load_virtual_ogr PRIVATE -DHAVE_SPATIALITE)
  target_link_libraries(test_load_virtual_ogr PRIVATE SPATIALITE::SPATIALITE)
  if (SPATIALITE_AMALGAMATION)
    target_compile_definitions(test_load_virtual_ogr PRIVATE -DSPATIALITE_AMALGAMATION)
  endif ()
endif ()

# Test sqlite3 extension
if (HAVE_SQLITE3EXT_H AND BUILD_SHARED_LIBS)
    add_library(my_test_sqlite3_ext MODULE my_test_sqlite3_ext.c)
    gdal_standard_includes(my_test_sqlite3_ext)
    get_target_property(PLUGIN_OUTPUT_DIR ${GDAL_LIB_TARGET_NAME} PLUGIN_OUTPUT_DIR)
    set_target_properties(my_test_sqlite3_ext
                          PROPERTIES
                          PREFIX ""
                          LIBRARY_OUTPUT_DIRECTORY ${PLUGIN_OUTPUT_DIR}
                          SKIP_BUILD_RPATH YES
                          )
    target_link_libraries(my_test_sqlite3_ext PRIVATE SQLite::SQLite3)
endif()
