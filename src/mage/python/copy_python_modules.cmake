# Copies the MAGE Python query modules into the dist directory, skipping
# development-only files and purging removed or renamed Python modules first
# while leaving C++/Rust shared libraries, license directories, and hidden
# stamp files in ${DST} intact. Run as: cmake -DSRC=<dir> -DDST=<dir> -P <this>.
#
# Keep the exclude list in sync with the install(DIRECTORY ...) rule in
# CMakeLists.txt next to this script.
if(EXISTS "${DST}")
    file(GLOB _stale_top_py "${DST}/*.py")
    if(_stale_top_py)
        file(REMOVE ${_stale_top_py})
    endif()
    file(REMOVE "${DST}/requirements-gpu.txt")
    file(REMOVE_RECURSE "${DST}/mage" "${DST}/__pycache__")
endif()

file(COPY "${SRC}/" DESTINATION "${DST}"
     PATTERN "__pycache__"       EXCLUDE
     PATTERN "tests"             EXCLUDE
     PATTERN "htmlcov"           EXCLUDE
     PATTERN "pytest.ini"        EXCLUDE
     PATTERN "requirements.txt"  EXCLUDE
     PATTERN "requirements*.lock" EXCLUDE
     PATTERN "CMakeLists.txt"    EXCLUDE
     PATTERN "copy_python_modules.cmake" EXCLUDE
     REGEX "/\\.[^/]*$"          EXCLUDE)
