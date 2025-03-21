# Generated by devtools/yamaker from nixpkgs 24.05.

LIBRARY()

LICENSE(
    BSD-2-Clause AND
    BSD-2-Clause-Views AND
    BSD-3-Clause AND
    MIT
)

LICENSE_TEXTS(.yandex_meta/licenses.list.txt)

VERSION(2024-10-14)

ORIGINAL_SOURCE(https://github.com/libcxxrt/libcxxrt/archive/76435c4451aeb5e04e9500b090293347a38cef8d.tar.gz)

PEERDIR(
    contrib/libs/libunwind
    library/cpp/sanitizer/include
)

NO_COMPILER_WARNINGS()

NO_RUNTIME()

NO_UTIL()

CXXFLAGS(-nostdinc++)

IF (SANITIZER_TYPE == undefined OR FUZZING)
    NO_SANITIZE()
    NO_SANITIZE_COVERAGE()
ENDIF()

SRCS(
    auxhelper.cc
    dynamic_cast.cc
    exception.cc
    guard.cc
    memory.cc
    stdexcept.cc
    typeinfo.cc
)

END()
