# Writes a C++ source defining a file's bytes as an array. Run as a script:
#   cmake -DINPUT=<file> -DOUTPUT=<file.cpp> -DSYMBOL=<name> [-DNULL_TERMINATE=ON] -P NoxEmbedFile.cmake
# The source defines `extern const unsigned char <SYMBOL>[]` and `<SYMBOL>Size`. NULL_TERMINATE
# (off unless given) adds a zero byte after the file's, counted in the size, for text read as a
# C string.
file(READ "${INPUT}" hex HEX)
string(LENGTH "${hex}" hexLength)
math(EXPR size "${hexLength} / 2")
if(NULL_TERMINATE)
    string(APPEND hex "00")
    math(EXPR size "${size} + 1")
endif()
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
# Break the initializer into lines of 32 bytes (160 characters) so the file stays editor-friendly.
string(REGEX REPLACE "((0x..,){32})" "\\1\n" bytes "${bytes}")
file(WRITE "${OUTPUT}" "// Generated from ${INPUT} by NoxEmbedFile.cmake. Do not edit.\n#include <cstddef>\n\n"
    "extern const unsigned char ${SYMBOL}[] = {\n${bytes}\n};\n"
    "extern const std::size_t ${SYMBOL}Size = ${size};\n")
