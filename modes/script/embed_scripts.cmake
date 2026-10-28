# The built-in modes' Luau sources made into C++ (builtin_scripts.hpp), so the program needs no
# script files on disk. Run at build time by modes/CMakeLists.txt whenever a listed file changes:
#   cmake -DLIST=<list file> -DOUT=<builtin_scripts.cpp> -P embed_scripts.cmake
# The list file holds one "<mode id>|<path in the mode's folder>|<source file>" a line. Each file's
# bytes go in as they are (an unsigned char array: no string literal's size limit).
file(STRINGS "${LIST}" entries)

set(arrays "")
set(rows "")
set(index 0)
foreach(entry IN LISTS entries)
    string(REPLACE "|" ";" parts "${entry}")
    list(GET parts 0 mode)
    list(GET parts 1 rel)
    list(GET parts 2 src)
    file(READ "${src}" hex HEX)
    string(LENGTH "${hex}" digits)
    math(EXPR size "${digits} / 2")
    if(size EQUAL 0)
        set(bytes "0")
    else()
        string(REGEX REPLACE "(..)" "0x\\1," bytes "${hex}")
        string(REGEX REPLACE "((0x..,){24})" "\\1\n" bytes "${bytes}")
    endif()
    string(APPEND arrays "// ${mode}: ${rel}\nconst unsigned char kFile${index}[] = {\n${bytes}\n};\n")
    string(APPEND rows "        {\"${mode}\", \"${rel}\", {reinterpret_cast<const char*>(kFile${index}), ${size}}},\n")
    math(EXPR index "${index} + 1")
endforeach()

if(index EQUAL 0)
    set(body "    return {};\n")
else()
    set(body "    static const BuiltinScript kScripts[] = {\n${rows}    };\n    return kScripts;\n")
endif()

file(WRITE "${OUT}.tmp" "// Made by modes/script/embed_scripts.cmake from the built-in modes' scripts: don't edit.
#include \"noxcore/modes/script/builtin_scripts.hpp\"

namespace nox::modes::script {

namespace {

${arrays}
} // namespace

std::span<const BuiltinScript> builtinScripts()
{
${body}}

} // namespace nox::modes::script
")
file(COPY_FILE "${OUT}.tmp" "${OUT}" ONLY_IF_DIFFERENT)
file(REMOVE "${OUT}.tmp")
