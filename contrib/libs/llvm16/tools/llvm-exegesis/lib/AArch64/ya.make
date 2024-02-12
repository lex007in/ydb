# Generated by devtools/yamaker.

LIBRARY()

LICENSE(Apache-2.0 WITH LLVM-exception)

LICENSE_TEXTS(.yandex_meta/licenses.list.txt)

PEERDIR(
    contrib/libs/llvm16
    contrib/libs/llvm16/include
    contrib/libs/llvm16/lib/IR
    contrib/libs/llvm16/lib/MC
    contrib/libs/llvm16/lib/Support
    contrib/libs/llvm16/lib/Target/AArch64
    contrib/libs/llvm16/lib/Target/AArch64/AsmParser
    contrib/libs/llvm16/lib/Target/AArch64/Disassembler
    contrib/libs/llvm16/lib/Target/AArch64/MCTargetDesc
    contrib/libs/llvm16/lib/Target/AArch64/TargetInfo
    contrib/libs/llvm16/lib/Target/AArch64/Utils
    contrib/libs/llvm16/tools/llvm-exegesis/lib
)

ADDINCL(
    ${ARCADIA_BUILD_ROOT}/contrib/libs/llvm16/lib/Target/AArch64
    contrib/libs/llvm16/lib/Target/AArch64
    contrib/libs/llvm16/tools/llvm-exegesis/lib/AArch64
)

NO_COMPILER_WARNINGS()

NO_UTIL()

SRCS(
    Target.cpp
)

END()