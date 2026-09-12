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

  // stat(2) mode test macros. MSVC's <sys/stat.h> defines _S_IFMT/_S_IFDIR but
  // not the POSIX S_IS*() predicates built on them.
  #include <sys/stat.h>
  #if !defined(S_ISDIR)
    #define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
  #endif
  #if !defined(S_ISREG)
    #define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
  #endif

  // chmod(2). MSVC provides _chmod, and its mode bits are only _S_IREAD /
  // _S_IWRITE -- there is no execute bit on Windows, so a POSIX mode like 0755
  // cannot be honoured exactly. Mapping "owner-writable" to _S_IWRITE keeps the
  // one distinction NTFS actually models; the executable bit is meaningless
  // there because Windows decides executability by file extension.
  #include <io.h>
  inline int bantu_compat_chmod(const char* path, int mode) {
    return _chmod(path, (mode & 0200) ? (_S_IREAD | _S_IWRITE) : _S_IREAD);
  }
  #if !defined(chmod)
    #define chmod(p, m) bantu_compat_chmod((p), (m))
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
