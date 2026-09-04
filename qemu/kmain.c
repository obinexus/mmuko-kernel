/* SPDX-License-Identifier: LicenseRef-OBINexus-1.0
 *
 * kmain.c -- the MMUKO ABI resolver, at ring 0, under QEMU.
 *
 * Why this exists
 * ---------------
 * The hosted test suites prove the resolver is CORRECT.  This proves it is
 * FREESTANDING, which is a different claim and not implied by the first.  A
 * library can pass every userspace test and still be unusable in a kernel
 * because it quietly calls malloc through a library routine, touches the FPU,
 * relies on a libc string function the linker supplied, or needs a
 * relocation-time initialiser to have run.
 *
 * This image links ONLY:
 *
 *   src/mmuko_abi.c   src/mmuko_semverx.c   src/mmuko_desc.c
 *   src/mmuko_trident.c
 *
 * with -ffreestanding -nostdlib -nostdinc++ -fno-builtin, no libc, no libgcc
 * on the 32-bit path, no allocator, no SSE and no x87.  It boots on bare metal
 * as a multiboot kernel, runs the consensus test vectors before any memory
 * manager or interrupt table exists, writes the results to COM1, and exits
 * through QEMU's isa-debug-exit device with a code the harness checks.
 *
 * If any of the four translation units above ever acquires a hosted
 * dependency, this image fails to LINK, which is the earliest and loudest
 * place for that to be noticed.
 */

#include "mmuko/mmuko_trident.h"
#include "mmuko/mmuko_desc.h"

#define MMUKO_TEST_FREESTANDING 1
#include "mmuko_test.h"

MMUKO_TEST_STATE_DEFS

/* ------------------------------------------------------------------------ */
/* Port I/O                                                                  */
/* ------------------------------------------------------------------------ */

static inline void outb(unsigned short port, unsigned char v)
{
    __asm__ __volatile__("outb %0, %1" : : "a"(v), "Nd"(port));
}

static inline unsigned char inb(unsigned short port)
{
    unsigned char v;
    __asm__ __volatile__("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void outl(unsigned short port, unsigned int v)
{
    __asm__ __volatile__("outl %0, %1" : : "a"(v), "Nd"(port));
}

/* ------------------------------------------------------------------------ */
/* COM1                                                                      */
/* ------------------------------------------------------------------------ */

#define COM1 0x3F8

static void serial_init(void)
{
    outb(COM1 + 1, 0x00);   /* interrupts off */
    outb(COM1 + 3, 0x80);   /* DLAB */
    outb(COM1 + 0, 0x03);   /* divisor 3 -> 38400 baud */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);   /* 8N1, DLAB off */
    outb(COM1 + 2, 0xC7);   /* FIFO on, clear, 14-byte threshold */
    outb(COM1 + 4, 0x0B);   /* DTR, RTS, OUT2 */
}

static void serial_putc(char c)
{
    while ((inb(COM1 + 5) & 0x20) == 0) { }
    outb(COM1, (unsigned char)c);
    if (c == '\n') serial_putc('\r');
}

/* The one symbol mmuko_test.h needs from the platform. */
void mmuko_test_print(const char *s)
{
    if (!s) return;
    while (*s) serial_putc(*s++);
}

static void print_u64(unsigned long long v)
{
    char tmp[24];
    int i = 0;
    if (v == 0) { serial_putc('0'); return; }
    while (v && i < (int)sizeof(tmp)) { tmp[i++] = (char)('0' + (v % 10u)); v /= 10u; }
    while (i-- > 0) serial_putc(tmp[i]);
}

/* ------------------------------------------------------------------------ */
/* QEMU isa-debug-exit                                                       */
/* ------------------------------------------------------------------------ */

/* -device isa-debug-exit,iobase=0xf4,iosize=0x04 makes QEMU exit with
 * (value << 1) | 1.  0x10 therefore means exit code 33, and 0x11 means 35. */
#define DEBUG_EXIT_PORT   0xf4
#define EXIT_PASS_VALUE   0x10   /* -> process exit code 33 */
#define EXIT_FAIL_VALUE   0x11   /* -> process exit code 35 */

static void kexit(unsigned int value)
{
    outl(DEBUG_EXIT_PORT, value);
    /* If isa-debug-exit is absent, halt rather than fall off the end. */
    for (;;) __asm__ __volatile__("cli; hlt");
}

/* ------------------------------------------------------------------------ */
/* Test vectors                                                              */
/* ------------------------------------------------------------------------ */

/* The transcript's two shapes.
 *
 * `add_i32` is the real thing: the resolver binds it and the test calls it.
 *
 * `add_f64_stub` stands in for `double add(double, double)`. Its body is
 * deliberately not a floating-point addition, for two reasons that point the
 * same way. First, this image is built with -mno-sse and -mno-80387 -- no FPU
 * has been initialised at this point in boot and the resolver contains no
 * floating point -- so a genuine f64 function could not be compiled into it at
 * all. Second, and more to the point, it does not need to be: the resolver
 * decides entirely from the DESCRIPTOR, and the property under test is that
 * this address is never reached. A fixture that would misbehave if entered is
 * a more honest stand-in than one that would quietly work. */
static int32_t add_i32(int32_t a, int32_t b) { return a + b; }

static volatile int g_stub_entered = 0;
static void add_f64_stub(void) { g_stub_entered = 1; }

MMUKO_SIG(sig_ii_i, MMUKO_CC_CDECL, MMUKO_I32, MMUKO_I32, MMUKO_I32);
MMUKO_SIG(sig_dd_d, MMUKO_CC_CDECL, MMUKO_F64, MMUKO_F64, MMUKO_F64);

static const mmuko_export_desc_t req_add  =
    MMUKO_REQUIRE("add", sig_ii_i, MMUKO_VER(1, STABLE, 0, STABLE, 0, STABLE));
static const mmuko_export_desc_t prov_v1  =
    MMUKO_EXPORT("add", sig_ii_i, MMUKO_VER(1, STABLE, 0, STABLE, 0, STABLE), add_i32);
static const mmuko_export_desc_t prov_v11 =
    MMUKO_EXPORT("add", sig_ii_i, MMUKO_VER(1, STABLE, 0, STABLE, 0, STABLE), add_i32);
static const mmuko_export_desc_t prov_v2  =
    MMUKO_EXPORT("add", sig_dd_d, MMUKO_VER(2, EXPERIMENTAL, 0, STABLE, 0, STABLE),
                 add_f64_stub);

typedef int32_t (*add_fn_t)(int32_t, int32_t);

static void vectors(void)
{
    mmuko_policy_t pol = mmuko_policy_default();
    mmuko_trident_t t;

    MMUKO_CASE("ring 0: the encoder runs with no allocator and no libc");
    {
        mmuko_fingerprint_t fp;
        MMUKO_CHECK(mmuko_fp_export("add", &sig_ii_i, MMUKO_ARCH_NATIVE, &fp) == 0,
                    "fingerprinting succeeds before any memory manager exists");
        MMUKO_CHECK(!mmuko_fp_is_zero(fp), "and produces a non-zero identity");
    }

    MMUKO_CASE("ring 0: consensus binds identical contracts");
    mmuko_trident_init(&t, "add", &req_add);
    MMUKO_CHECK(t.w_slot == mmuko_trident_trap_address(),
                "an unresolved slot points at the trap");
    MMUKO_CHECK(mmuko_trident_swap(&t, &prov_v1, &pol) == MMUKO_BIND_BOUND,
                "u1 == u2 binds");
    {
        add_fn_t f = (add_fn_t)mmuko_trident_address(&t);
        MMUKO_CHECK(f && f(2, 3) == 5, "and the bound slot computes 5");
    }

    MMUKO_CASE("ring 0: THE BREAK -- int32 add meets double add");
    MMUKO_CHECK(mmuko_trident_swap(&t, &prov_v2, &pol) == MMUKO_BIND_FAULT,
                "the double-shaped provider is refused");
    MMUKO_CHECK(t.fault == MMUKO_FAULT_FINGERPRINT, "named: abi-fingerprint-mismatch");
    MMUKO_CHECK(mmuko_trident_address(&t) == NULL, "no callable address is exposed");
    MMUKO_CHECK(t.w_slot != (mmuko_fnptr_t)add_f64_stub,
                "the mis-shaped function is unreachable through the slot");
    MMUKO_CHECK(g_stub_entered == 0,
                "and it was never entered");

    MMUKO_CASE("ring 0: the trap is entered and counted, and the kernel survives");
    {
        uint64_t before = mmuko_trident_trap_count();
        add_fn_t f = (add_fn_t)t.w_slot;
        (void)f(2, 3);
        MMUKO_CHECK(mmuko_trident_trap_count() == before + 1u,
                    "calling through a faulted slot traps rather than corrupting");
    }

    MMUKO_CASE("ring 0: hot-swap heals with no restart");
    MMUKO_CHECK(mmuko_trident_swap(&t, &prov_v11, &pol) == MMUKO_BIND_BOUND,
                "publishing a correct provider re-binds");
    MMUKO_CHECK(t.generation == 2u, "the generation advanced");
    {
        add_fn_t f = (add_fn_t)mmuko_trident_address(&t);
        MMUKO_CHECK(f && f(20, 22) == 42, "and the healed slot computes 42");
    }

    MMUKO_CASE("ring 0: fault containment across a three-node chain");
    {
        static mmuko_trident_t a, b, c;
        static mmuko_trident_t *deps_b[1];
        static mmuko_trident_t *deps_c[1];
        mmuko_trident_t *nodes[3];
        uint32_t bound = 0, faulted = 0;

        mmuko_trident_init(&a, "A", &req_add);
        mmuko_trident_init(&b, "B", &req_add);
        mmuko_trident_init(&c, "C", &req_add);
        deps_b[0] = &a; mmuko_trident_set_deps(&b, deps_b, 1);
        deps_c[0] = &b; mmuko_trident_set_deps(&c, deps_c, 1);
        nodes[0] = &a; nodes[1] = &b; nodes[2] = &c;

        a.u2_provided = &prov_v1;
        b.u2_provided = &prov_v1;
        c.u2_provided = &prov_v1;
        MMUKO_CHECK(mmuko_trident_graph_resolve(nodes, 3, &pol, &bound, &faulted) == 0
                    && bound == 3u,
                    "a consistent chain binds end to end");

        a.u2_provided = &prov_v2;
        MMUKO_CHECK(mmuko_trident_graph_resolve(nodes, 3, &pol, &bound, &faulted) != 0
                    && bound == 0u,
                    "breaking the root faults the whole chain");
        MMUKO_CHECK(c.fault == MMUKO_FAULT_UPSTREAM,
                    "and the far end reports containment, not a cause of its own");

        a.u2_provided = &prov_v11;
        MMUKO_CHECK(mmuko_trident_graph_resolve(nodes, 3, &pol, &bound, &faulted) == 0
                    && bound == 3u,
                    "healing the root heals the whole chain");
    }

    MMUKO_CASE("ring 0: SemVerX parses and formats without libc");
    {
        mmuko_semverx_t v;
        char out[MMUKO_SEMVERX_STRLEN];
        MMUKO_CHECK(mmuko_semverx_parse("4.stable.17.beta.2.stable", &v) == 0,
                    "the six-field form parses");
        mmuko_semverx_format(&v, out);
        MMUKO_CHECK(mmuko_streq(out, "4.stable.17.beta.2.stable"),
                    "and round-trips exactly");
        MMUKO_CHECK(mmuko_semverx_parse("4.17.2", &v) != 0,
                    "and a partial form is still refused at ring 0");
    }
}

/* ------------------------------------------------------------------------ */
/* Entry                                                                     */
/* ------------------------------------------------------------------------ */

void kmain(void)
{
    serial_init();
    mmuko_test_print("\n=== MMUKO ABI resolver, ring 0, ");
    mmuko_test_print(mmuko_arch_name(MMUKO_ARCH_NATIVE));
    mmuko_test_print(" ===\n");

    vectors();

    mmuko_test_print("\n");
    print_u64(mmuko_test_passed);
    mmuko_test_print(" passed, ");
    print_u64(mmuko_test_failed);
    mmuko_test_print(" failed  [freestanding, no libc, no allocator]\n");

    if (mmuko_test_failed == 0) {
        mmuko_test_print("MMUKO-QEMU-RESULT: PASS\n");
        kexit(EXIT_PASS_VALUE);
    } else {
        mmuko_test_print("MMUKO-QEMU-RESULT: FAIL\n");
        kexit(EXIT_FAIL_VALUE);
    }
}
