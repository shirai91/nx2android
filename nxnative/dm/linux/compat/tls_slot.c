// Linux (glibc, aarch64): the guest thread slots' anchor.
// The rewritten guest code (DM_TARGET=android) reads a thread's slot block through tpidr_el0[2], which is Bionic's TLS_SLOT_APP. glibc's thread pointer is the
// two-word TCB (dtv, private) followed by the executable's static TLS block, so tpidr_el0[2] is the first word of the executable's own TLS: this variable,
// provided it is the first thread-local in the link (this file is linked first; loader.c checks the address at start-up and stops if it is not).
__thread void *dm_tls_app __attribute__((section(".tdata")));   // zero, but in .tdata so that it comes before the other (initialised) thread-locals
