/*
 * stubs/ios_base_library_initv.c
 *
 * Provides a no-op definition of:
 *   _ZSt21ios_base_library_initv
 *
 * Why this exists:
 *   GCC 14's libstdc++ headers (used on Debian 13 / Ubuntu 24.04) emit a
 *   call to _ZSt21ios_base_library_initv (a new init helper for
 *   std::ios_base::Init) tagged at GLIBCXX_3.4.32. Ubuntu 22.04's
 *   libstdc++.so.6 (built from GCC 11) does NOT export this symbol, so
 *   any binary compiled with GCC 14 + iostream fails to load on 22.04:
 *
 *     ./bantu: /lib/x86_64-linux-gnu/libstdc++.so.6: version
 *              `GLIBCXX_3.4.32' not found (required by ./bantu)
 *
 * By defining the symbol ourselves in the link, the call is resolved
 * locally and no dynamic GLIBCXX_3.4.32 dependency is recorded. The
 * function is a no-op because the older libstdc++ does the equivalent
 * work inside its own ios_base::Init constructor.
 *
 * Build: gcc -c stubs/ios_base_library_initv.c -o build/ios_stub.o
 * Link:  add build/ios_stub.o to the link line.
 */

void _ZSt21ios_base_library_initv(void) {
    /* no-op */
}
