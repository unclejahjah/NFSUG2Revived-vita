// Weak imports of kubridge.skprx (github.com/bythos14/kubridge), the kernel plugin many Vita ports need: only the
// exception handler calls of the crash reporter (crashlog.cpp). The library is imported weakly (flag 0x8), so the game
// still loads without the plugin; then each call returns -1 (the unresolved stub is "mvn r0, #0; bx lr").
// Library NID: SHA-256 of "kubridge" (its exports.yml gives none); function NIDs from that exports.yml. The words are
// those vita-libs-gen-2 writes for a weak stub: version 1 << 16 | flags, library NID, function NID.

#ifdef __vita__
#define NFSU_KUBRIDGE_STUB(name, nid)                                                                                  \
    __asm__(".section .vitalink.fstubs.kubridge,\"ax\",%progbits\n"                                                    \
            "\t.align 4\n"                                                                                             \
            "\t.global " #name "\n"                                                                                    \
            "\t.type " #name ", %function\n" #name ":\n"                                                               \
            "\t.word 0x00010008\n"                                                                                     \
            "\t.word 0x6DB9389B\n"                                                                                     \
            "\t.word " #nid "\n"                                                                                       \
            "\t.align 4\n"                                                                                             \
            "\t.text\n");

NFSU_KUBRIDGE_STUB(kuKernelRegisterExceptionHandler, 0xF9025D19)
NFSU_KUBRIDGE_STUB(kuKernelReleaseExceptionHandler, 0xDF9F3BED)
NFSU_KUBRIDGE_STUB(kuKernelRegisterAbortHandler, 0x799F5648)
NFSU_KUBRIDGE_STUB(kuKernelReleaseAbortHandler, 0xB188292D)
#endif
