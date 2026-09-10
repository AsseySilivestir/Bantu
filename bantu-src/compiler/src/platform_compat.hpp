#pragma once
// ════════════════════════════════════════════════════════════════════════════
//  platform_compat.hpp — the two POSIX-isms MSVC does not have.
//
//  Found by adding CI that builds Windows on every push. Before that the only
//  workflow that touched Windows ran on version tags, so the MSVC build had
//  been failing on these for a long time without anyone seeing it:
//
//    * `ssize_t`  — POSIX, not ISO C or C++. MSVC ships `SSIZE_T` instead.
//                   Used by every recv/send loop in the server.
//    * `__attribute__((packed))` — a GCC/Clang extension. MSVC spells the same
//                   idea `#pragma pack`, and silently treated the attribute as
//                   an undeclared identifier.
//
//  Kept in one header so the shims exist exactly once and every file that needs
//  them says so, rather than each growing its own #ifdef.
// ════════════════════════════════════════════════════════════════════════════

#if defined(_MSC_VER)

  #include <BaseTsd.h>
  // Guarded: some third-party headers (parts of curl, zlib) define it too, and
  // a second typedef is an error rather than a benign repeat.
  #if !defined(_SSIZE_T_DEFINED)
    typedef SSIZE_T ssize_t;
    #define _SSIZE_T_DEFINED
  #endif

  // Struct packing. Used as:
  //     BANTU_PACKED_BEGIN
  //     struct Wire { ... } BANTU_PACKED;
  //     BANTU_PACKED_END
  // which is correct under both compilers: GCC/Clang take the trailing
  // attribute and ignore the (empty) pragmas, MSVC takes the pragmas and
  // ignores the (empty) trailing macro.
  #define BANTU_PACKED_BEGIN __pragma(pack(push, 1))
  #define BANTU_PACKED_END   __pragma(pack(pop))
  #define BANTU_PACKED

#else

  #include <sys/types.h>          // ssize_t
  #define BANTU_PACKED_BEGIN
  #define BANTU_PACKED_END
  #define BANTU_PACKED __attribute__((packed))

#endif
