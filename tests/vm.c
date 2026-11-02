/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * tests/vm.c - unit test of the base instruction set of the script VM
 *              (src/vm/ops_base.c and src/vm/thread.c, declared in
 *              inc/bgi/vm.h); built as bin/test_vm by `make test`
 *
 * Small programs are assembled into a thread's code area and run through
 * the dispatcher exactly as the scheduler does it; the evaluation stack,
 * the data area and the local heap are then inspected.  Script errors
 * are caught with the engine's own try frames (the error box goes to
 * stderr with BGI_MSGBOX_STDERR, which this test sets).  Opcodes are
 * written as their byte values; the mnemonic of each is in the comments
 * and in src/vm/ops_base.c.
 *
 * The groups of tests, in the order they run:
 *
 *   push         - "00" .. "02" with sign extension, the tagged local
 *                  address of "04" and the code addresses of "05" / "06"
 *                  relative to the opcode
 *   arithmetic   - every binary operator "20" .. "39" on chosen operands
 *                  (truncating division, division by zero, masked shift
 *                  counts, signed comparisons), not / lnot / select,
 *                  muldiv with its 64-bit intermediate, sin / cos in 16.16
 *   memory       - load and store in the three sizes with sign extension,
 *                  store-keep, inline bytes, store-multi, and memset /
 *                  memcpy / memcmp / memclr between locals
 *   strings      - the string instructions "68" .. "6F" on code and data
 *                  area strings, including the character classifier "6C"
 *                  and the argument order of "6F"
 *   control flow - jcc taken and not taken, call / ret through the frame
 *                  stack, ret at the outermost frame ending the thread
 *   heap         - lalloc / lfree: tagged, non-overlapping blocks
 *   errors       - a jump to NULL raises a script error; an undefined
 *                  opcode has no handler
 *   extensions   - the "EF xx" family of OpenBGI's own instructions exists
 *                  in the profile, reports its level and sets the text
 *                  switches it is there for
 *   later builds - the services that instructions of the later builds rely
 *                  on: the priority limit by generation, the kept error of
 *                  "81 6A" / "81 6B", the gamepad table of "81 1B" and the
 *                  clock settings of "81 68" / "81 04"
 *   borrowed areas - the threads of "81 44" in parts of another thread's
 *                  code and data areas: where the parts go, what the
 *                  lender keeps, frames and modules of a borrower, the
 *                  order in which they end, the instruction itself
 *   files        - "80 27" moving a file, the file accessor "81 28" ..
 *                  "81 2B" in both of its generations, the result codes
 *                  of the file dialog "81 38"
 *
 * A failure means an instruction computes, pushes or pops differently
 * from the original, which every script would notice.
 */
#include "bgi/vm.h"
#include "bgi/sys.h"
#include "bgi/error.h"
#include "bgi/strutil.h"
#include "bgi/input.h"
#include "bgi/gfx/text.h"
#include "bgi/os.h"
#include "bgi/version.h"

#include <stdio.h>
#include <math.h>

static int gFails; // the number of failed checks so far
// evaluate a condition; print it with its location and count a failure when it is false
#define CHECK(c)                                                  \
	do                                                            \
	{                                                             \
		if(!(c))                                                  \
		{                                                         \
			printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
			gFails++;                                             \
		}                                                         \
	} while(0)

// ---- a tiny assembler ------------------------------------------------------

// a program under construction: raw bytes appended by the helpers below
typedef struct Asm
{
	uint8_t code[0x1000]; // the bytes
	uint32_t len;         // how many are used
} Asm_t;

// append a byte
static void A8(Asm_t* a, uint32_t v)
{
	a->code[a->len++] = (uint8_t)v;
}

// append a little-endian 16-bit value
static void A16(Asm_t* a, uint32_t v)
{
	A8(a, v & 0xff);
	A8(a, v >> 8);
}

// append a little-endian 32-bit value
static void A32(Asm_t* a, uint32_t v)
{
	A16(a, v & 0xffff);
	A16(a, v >> 16);
}

static void PushI8(Asm_t* a, int v) // 00
{
	A8(a, 0x00);
	A8(a, (uint32_t)v);
}

static void PushI16(Asm_t* a, int v) // 01
{
	A8(a, 0x01);
	A16(a, (uint32_t)v);
}

static void PushI32(Asm_t* a, uint32_t v) // 02
{
	A8(a, 0x02);
	A32(a, v);
}

// append a bare opcode
static void Op(Asm_t* a, uint32_t op)
{
	A8(a, op);
}

// 17 with the frame pointer reset: a return at fp 0 ends the program
static void Ret(Asm_t* a)
{
	PushI32(a, 0);
	A8(a, 0x11);
	A8(a, 0x17);
}

// ---- running ------------------------------------------------------------------

static Thread_t* gT; // the one thread every program runs on (made by main)

/* Load the program into the thread's code area and step it from offset 0
 * with the frame pointer and stack reset, dispatching through
 * vm_optable_main as the scheduler does, until a handler returns a
 * non-zero scheduler code (4: the thread ended) or 100000 steps have
 * passed.  Returns that code, or -1 when an opcode has no handler. */
static int Run(const Asm_t* a)
{
	int rc = 0, steps = 0;
	Thread_t* t = gT;
	memcpy(t->code, a->code, a->len);
	t->codeUsed = a->len;
	Thread_SetIP(t, 0);
	Thread_SetFP(t, 0);
	t->sp = 0;
	while(rc == 0 && steps++ < 100000)
	{
		uint8_t op = Thread_FetchOp(t);
		if(!vm_optable_main[op])
			return -1; // the scheduler raises MSG_UNDEFINED_OP here
		rc = vm_optable_main[op](t);
	}
	return rc;
}

/* the value on top of the stack, without popping (the stack wraps: on an
 * empty stack this reads the last entry) */
static uint32_t Top(void)
{
	Thread_t* t = gT;
	uint32_t i = t->sp ? t->sp - 1 : t->stackEntries - 1;
	return t->stack[i];
}

// the number of values on the evaluation stack
static uint32_t Depth(void)
{
	return gT->sp;
}

/* append the final return, run the program (which has to end normally with exactly one
 * value on the stack) and return that value */
static uint32_t Eval(Asm_t* a)
{
	Ret(a);
	CHECK(Run(a) == 4);
	CHECK(Depth() == 1);
	return Top();
}

// ---- the tests -----------------------------------------------------------------

/* The push instructions: "00" and "01" sign-extend their immediate, "02"
 * pushes it as is; "04" pushes fp - n with the data tag; "05" pushes the
 * code address opcode offset + n with the code tag and "06" the untagged
 * offset, both relative to the opcode's own position. */
static void TestPush(void)
{
	Asm_t a;
	printf("push\n");
	a.len = 0;
	PushI8(&a, -5);
	CHECK(Eval(&a) == 0xfffffffbu); // sign extended
	a.len = 0;
	PushI16(&a, 0x8000);
	CHECK(Eval(&a) == 0xffff8000u);
	a.len = 0;
	PushI32(&a, 0x12345678);
	CHECK(Eval(&a) == 0x12345678u);
	// 04: a local's address is fp - n with the data tag; 05 / 06 are relative to the
	// opcode
	a.len = 0;
	Op(&a, 0x04);
	A16(&a, 8);
	CHECK(Eval(&a) == ((0u - 8u) | VM_TAG_DATA));
	a.len = 0;
	Op(&a, 0x05);
	A16(&a, 0x10); // code address: opIP (0) + 0x10, tagged
	CHECK(Eval(&a) == (0x10u | VM_TAG_CODE));
	a.len = 0;
	Op(&a, 0x7f); // nop first, so the opcode is at 1
	Op(&a, 0x06);
	A16(&a, 0xfffe); // -2 -> 1 - 2
	CHECK(Eval(&a) == 0xffffffffu);
}

/* The arithmetic, logic and comparison instructions on a table of operand
 * pairs (a pushed first, then b, so b is on top): the quirks a script
 * relies on are division truncating toward zero and answering -1 for a
 * zero divisor, shift counts masked to 5 bits, a logical "2A" and an
 * arithmetic "2B" right shift, and signed comparisons.  Then the unary
 * not and lnot, select with a true and a false condition, muldiv keeping
 * a 64-bit intermediate, and sin / cos of 16.16 degrees giving 16.16
 * results (cos 60 may land on either side of 0.5 after truncation). */
static void TestArith(void)
{
	static const struct
	{
		uint32_t op, a, b, r; // the opcode, both operands and the expected result
	} cases[] = {
		{0x20, 7, 5, 12},
		{0x21, 7, 9, 0xfffffffeu},
		{0x22, 0xffffffffu, 3, 0xfffffffdu},
		{0x23, 0xfffffff9u, 2, 0xfffffffdu}, // -7 / 2 = -3 (truncation)
		{0x23, 5, 0, 0xffffffffu},           // division by zero: -1
		{0x24, 0xfffffff9u, 2, 0xffffffffu}, // -7 % 2 = -1
		{0x24, 5, 0, 0xffffffffu},
		{0x25, 0xf0f0, 0xff00, 0xf000},
		{0x26, 0xf0f0, 0x0f0f, 0xffff},
		{0x27, 0xf0f0, 0xffff, 0x0f0f},
		{0x29, 1, 35, 8},                     // the shift count is masked to 5 bits
		{0x2a, 0x80000000u, 31, 1},           // logical right shift
		{0x2b, 0x80000000u, 31, 0xffffffffu}, // arithmetic right shift
		{0x30, 3, 3, 1},
		{0x31, 3, 4, 1},
		{0x32, 0xffffffffu, 0, 1}, // -1 <= 0, signed
		{0x33, 0xffffffffu, 0, 0},
		{0x34, 0xffffffffu, 0, 1},
		{0x35, 0x7fffffffu, 0x80000000u, 1}, // INT_MAX > INT_MIN
		{0x38, 2, 0, 0},
		{0x39, 0, 2, 1},
	};
	size_t i;
	Asm_t a;
	printf("arithmetic\n");
	for(i = 0; i < sizeof cases / sizeof cases[0]; i++)
	{
		uint32_t r;
		a.len = 0;
		PushI32(&a, cases[i].a);
		PushI32(&a, cases[i].b);
		Op(&a, cases[i].op);
		r = Eval(&a);
		if(r != cases[i].r)
			printf("  op %02x: %08x, %08x -> %08x, expected %08x\n", cases[i].op, cases[i].a, cases[i].b, r, cases[i].r);
		CHECK(r == cases[i].r);
	}
	// unary and ternary
	a.len = 0;
	PushI32(&a, 0x0f0f0f0f);
	Op(&a, 0x28);
	CHECK(Eval(&a) == 0xf0f0f0f0u);
	a.len = 0;
	PushI32(&a, 0);
	Op(&a, 0x3a);
	CHECK(Eval(&a) == 1);
	a.len = 0;
	PushI32(&a, 1);  // c
	PushI32(&a, 10); // tv
	PushI32(&a, 20); // f
	Op(&a, 0x40);
	CHECK(Eval(&a) == 10);
	a.len = 0;
	PushI32(&a, 0);
	PushI32(&a, 10);
	PushI32(&a, 20);
	Op(&a, 0x40);
	CHECK(Eval(&a) == 20);
	// muldiv with a 64-bit intermediate: 0x10000 * 0x10000 / 0x100
	a.len = 0;
	PushI32(&a, 0x10000);
	PushI32(&a, 0x10000);
	PushI32(&a, 0x100);
	Op(&a, 0x42);
	CHECK(Eval(&a) == 0x1000000);
	// sin / cos of 16.16 degrees in 16.16
	a.len = 0;
	PushI32(&a, 90 << 16);
	Op(&a, 0x48);
	CHECK(Eval(&a) == 0x10000);
	a.len = 0;
	PushI32(&a, 60 << 16);
	Op(&a, 0x49);
	{
		uint32_t c = Eval(&a); // cos 60 = 0.5, truncated towards zero from the double
		CHECK(c == 0x8000 || c == 0x7fff);
	}
	a.len = 0;
	PushI32(&a, (uint32_t)-(90 << 16));
	Op(&a, 0x48);
	CHECK(Eval(&a) == 0xffff0000u);
}

/* The memory instructions with the frame pointer set so that locals land
 * at known data offsets: "0A" stores a dword and "08" loads it back as a
 * sign-extended byte, a sign-extended word and a dword; "09" stores and
 * keeps the value on the stack; "0B" copies the inline bytes that follow
 * the opcode; "0C" stores several values, the first pushed at the lowest
 * address; "60" .. "63" are memcpy, memclr, memset (low byte only) and
 * memcmp (1 when equal) between locals.  The data area is inspected
 * directly after each program. */
static void TestMemory(void)
{
	Asm_t a;
	Thread_t* t = gT;
	printf("memory\n");
	// store a dword at data[0x20] (fp = 0x40, local address fp - 0x20) and load it back
	// in each size
	a.len = 0;
	PushI32(&a, 0x40);
	Op(&a, 0x11); // set fp
	PushI32(&a, 0x8081fffe);
	Op(&a, 0x04);
	A16(&a, 0x20);
	Op(&a, 0x0a); // store: the pointer on top, the value under it
	A8(&a, 2);
	Op(&a, 0x04);
	A16(&a, 0x20);
	Op(&a, 0x08);
	A8(&a, 0); // s8: 0xfe -> -2
	Ret(&a);
	CHECK(Run(&a) == 4);
	CHECK(Top() == 0xfffffffeu);
	CHECK(memcmp(t->data + 0x20, "\xfe\xff\x81\x80", 4) == 0);
	a.len = 0;
	PushI32(&a, 0x40);
	Op(&a, 0x11);
	Op(&a, 0x04);
	A16(&a, 0x20);
	Op(&a, 0x08);
	A8(&a, 1); // s16: 0xfffe -> -2
	CHECK(Eval(&a) == 0xfffffffeu);
	a.len = 0;
	PushI32(&a, 0x40);
	Op(&a, 0x11);
	Op(&a, 0x04);
	A16(&a, 0x20);
	Op(&a, 0x08);
	A8(&a, 2); // dword
	CHECK(Eval(&a) == 0x8081fffeu);

	// 09 keeps the value; 0B copies inline bytes; 0C stores several values (first pushed
	// lowest)
	a.len = 0;
	PushI32(&a, 0x40);
	Op(&a, 0x11);
	Op(&a, 0x04);
	A16(&a, 0x10); // local address fp - 0x10 = data[0x30]
	PushI32(&a, 0x1234);
	Op(&a, 0x09);
	A8(&a, 1); // a word
	CHECK(Eval(&a) == 0x1234);
	CHECK(memcmp(t->data + 0x30, "\x34\x12", 2) == 0);
	a.len = 0;
	PushI32(&a, 0x40);
	Op(&a, 0x11);
	Op(&a, 0x04);
	A16(&a, 0x10);
	Op(&a, 0x0b);
	A8(&a, 3); // three inline bytes follow
	A8(&a, 'a');
	A8(&a, 'b');
	A8(&a, 0);
	Ret(&a);
	CHECK(Run(&a) == 4);
	CHECK(strcmp((char*)t->data + 0x30, "ab") == 0);
	a.len = 0;
	PushI32(&a, 0x40);
	Op(&a, 0x11);
	Op(&a, 0x04);
	A16(&a, 0x10);
	PushI32(&a, 1);
	PushI32(&a, 2);
	PushI32(&a, 3);
	Op(&a, 0x0c);
	A8(&a, 2); // dwords
	A8(&a, 3); // three of them
	Ret(&a);
	CHECK(Run(&a) == 4);
	CHECK(memcmp(t->data + 0x30, "\x01\0\0\0\x02\0\0\0\x03\0\0\0", 12) == 0);

	// memset / memcpy / memcmp / memclr between locals (fp = 0x80)
	a.len = 0;
	PushI32(&a, 0x80);
	Op(&a, 0x11);
	Op(&a, 0x04);
	A16(&a, 0x40); // dst = data[0x40]
	PushI32(&a, 8);
	PushI32(&a, 0x1ab); // only the low byte
	Op(&a, 0x62);
	Op(&a, 0x04);
	A16(&a, 0x20); // dst = data[0x60]
	Op(&a, 0x04);
	A16(&a, 0x40); // src = data[0x40]
	PushI32(&a, 8);
	Op(&a, 0x60);
	Op(&a, 0x04);
	A16(&a, 0x40);
	Op(&a, 0x04);
	A16(&a, 0x20);
	PushI32(&a, 8);
	Op(&a, 0x63); // equal -> 1
	Ret(&a);
	CHECK(Run(&a) == 4);
	CHECK(Top() == 1);
	CHECK(memcmp(t->data + 0x40, "\xab\xab\xab\xab\xab\xab\xab\xab", 8) == 0);
	CHECK(memcmp(t->data + 0x60, "\xab\xab\xab\xab\xab\xab\xab\xab", 8) == 0);
	a.len = 0;
	PushI32(&a, 0x80);
	Op(&a, 0x11);
	Op(&a, 0x04);
	A16(&a, 0x40);
	PushI32(&a, 4);
	Op(&a, 0x61); // memclr: the first four bytes only
	Ret(&a);
	CHECK(Run(&a) == 4);
	CHECK(memcmp(t->data + 0x40, "\0\0\0\0\xab\xab\xab\xab", 8) == 0);
}

/* The string instructions: "6A" strcpy from a literal in the code area
 * (reached through a tagged code address) and "68" strlen; "69" streq,
 * "6D" strlwr in place, "6B" the concatenation of two strings into a
 * third and "6E" the quoting with a given character, all on data-area
 * strings; "6C" pushes a character's code, whether it is double-byte and
 * whether it is a kinsoku character (one that may not start a line);
 * "6F" sprintf, whose arguments are popped in order of appearance after
 * the format and the destination, so a script pushes them in reverse.
 * The destination may also be a source (`strcat(buf, buf, "x")` appends),
 * as the scripts rely on. */
static void TestStrings(void)
{
	Asm_t a;
	Thread_t* t = gT;
	uint32_t strAt;
	printf("strings\n");
	// a literal in the code area, reached through a tagged code address
	a.len = 0;
	PushI32(&a, 0x100);
	Op(&a, 0x11);
	Op(&a, 0x04);
	A16(&a, 0x80); // dst = data[0x80]
	Op(&a, 0x05);
	A16(&a, 0); // placeholder: patched to point at the literal
	strAt = a.len - 2;
	Op(&a, 0x6a); // strcpy
	Op(&a, 0x04);
	A16(&a, 0x80);
	Op(&a, 0x68); // strlen
	Ret(&a);
	{ // the literal after the code; the operand of "05" is relative to the opcode's own offset
		uint32_t lit = a.len, opAt = strAt - 1;
		memcpy(a.code + a.len, "Hello, BGI", 11);
		a.len += 11;
		a.code[strAt] = (uint8_t)(lit - opAt);
		a.code[strAt + 1] = (uint8_t)((lit - opAt) >> 8);
	}
	CHECK(Run(&a) == 4);
	CHECK(Top() == 10);
	CHECK(strcmp((char*)t->data + 0x80, "Hello, BGI") == 0);

	// streq, strcat2, strlwr, strquote on data-area strings (fp = 0x100: local 0x80 is
	// data[0x80], local 0x70 data[0x90])
	strcpy((char*)t->data + 0x80, "Abc");
	strcpy((char*)t->data + 0x90, "abc");
	a.len = 0;
	PushI32(&a, 0x100);
	Op(&a, 0x11);
	Op(&a, 0x04);
	A16(&a, 0x80);
	Op(&a, 0x04);
	A16(&a, 0x70);
	Op(&a, 0x69); // "Abc" == "abc"? no
	Ret(&a);
	CHECK(Run(&a) == 4 && Top() == 0);
	a.len = 0;
	PushI32(&a, 0x100);
	Op(&a, 0x11);
	Op(&a, 0x04);
	A16(&a, 0x80);
	Op(&a, 0x6d); // lower-case in place
	Op(&a, 0x04);
	A16(&a, 0x80);
	Op(&a, 0x04);
	A16(&a, 0x70);
	Op(&a, 0x69); // now equal
	Ret(&a);
	CHECK(Run(&a) == 4 && Top() == 1);
	a.len = 0;
	PushI32(&a, 0x100);
	Op(&a, 0x11);
	Op(&a, 0x04);
	A16(&a, 0x60); // dst = data[0xa0]
	Op(&a, 0x04);
	A16(&a, 0x80); // a
	Op(&a, 0x04);
	A16(&a, 0x70); // b
	Op(&a, 0x6b);  // dst = a + b
	Ret(&a);
	CHECK(Run(&a) == 4);
	CHECK(strcmp((char*)t->data + 0xa0, "abcabc") == 0);
	a.len = 0;
	PushI32(&a, 0x100);
	Op(&a, 0x11);
	Op(&a, 0x04);
	A16(&a, 0x50); // dst = data[0xb0]
	Op(&a, 0x04);
	A16(&a, 0x80);
	PushI8(&a, '"');
	Op(&a, 0x6e); // dst = the string between two quote characters
	Ret(&a);
	CHECK(Run(&a) == 4);
	CHECK(strcmp((char*)t->data + 0xb0, "\"abc\"") == 0);

	// 6C: a character, whether it is double-byte, whether it is a kinsoku character
	strcpy((char*)t->data + 0x80, "\x82\xa0x"); // あ (the hiragana "a") followed by an "x"
	a.len = 0;
	PushI32(&a, 0x100);
	Op(&a, 0x11);
	Op(&a, 0x04);
	A16(&a, 0x80);
	Op(&a, 0x6c);
	Ret(&a);
	CHECK(Run(&a) == 4);
	CHECK(Depth() == 3);
	CHECK(Top() == 0); // あ is not a kinsoku character
	CHECK(t->stack[1] == 1);
	CHECK(t->stack[0] == 0x82a0);
	strcpy((char*)t->data + 0x80, "\x81\x42"); // 。 (the full stop): kinsoku
	a.len = 0;
	PushI32(&a, 0x100);
	Op(&a, 0x11);
	Op(&a, 0x04);
	A16(&a, 0x80);
	Op(&a, 0x6c);
	Ret(&a);
	CHECK(Run(&a) == 4);
	CHECK(Top() == 1 && t->stack[1] == 1 && t->stack[0] == 0x8142);

	// 6F: the arguments are popped in order of appearance after the format and
	// the destination, so the script pushes them in reverse
	strcpy((char*)t->data + 0x80, "%d-%04x-%s-%%-%c");
	strcpy((char*)t->data + 0xc0, "str");
	a.len = 0;
	PushI32(&a, 0x100);
	Op(&a, 0x11);
	PushI8(&a, 'Z'); // %c
	Op(&a, 0x04);
	A16(&a, 0x40);              // %s = data[0xc0]
	PushI32(&a, 0xbeef);        // %04x
	PushI32(&a, (uint32_t)-12); // %d
	Op(&a, 0x04);
	A16(&a, 0x60); // dst = data[0xa0]
	Op(&a, 0x04);
	A16(&a, 0x80); // fmt
	Op(&a, 0x6f);
	Ret(&a);
	CHECK(Run(&a) == 4);
	CHECK(Depth() == 0); // every argument was consumed
	CHECK(strcmp((char*)t->data + 0xa0, "-12-beef-str-%-Z") == 0);

	/* the destination as a source: `strcat(buf, buf, "x")` appends (what the
	 * scenario libraries build every file name with), `strcpy(s, s)` keeps
	 * the string, a sprintf argument may be the destination, and so may the
	 * quoted string - all as the original's CRT did it, whatever this one does */
	strcpy((char*)t->data + 0x80, "bs_a11");
	strcpy((char*)t->data + 0x90, "_N");
	a.len = 0;
	PushI32(&a, 0x100);
	Op(&a, 0x11);
	Op(&a, 0x04);
	A16(&a, 0x80); // dst = data[0x80]
	Op(&a, 0x04);
	A16(&a, 0x80); // a = the same
	Op(&a, 0x04);
	A16(&a, 0x70); // b
	Op(&a, 0x6b);
	Op(&a, 0x04);
	A16(&a, 0x80);
	Op(&a, 0x04);
	A16(&a, 0x80);
	Op(&a, 0x6a); // strcpy(s, s)
	Op(&a, 0x04);
	A16(&a, 0x80);
	Op(&a, 0x04);
	A16(&a, 0x80);
	PushI8(&a, '\'');
	Op(&a, 0x6e); // quote in place
	Ret(&a);
	CHECK(Run(&a) == 4);
	CHECK(strcmp((char*)t->data + 0x80, "'bs_a11_N'") == 0);
	strcpy((char*)t->data + 0x80, "%s+%d");
	strcpy((char*)t->data + 0xa0, "one");
	a.len = 0;
	PushI32(&a, 0x100);
	Op(&a, 0x11);
	PushI8(&a, 2); // %d
	Op(&a, 0x04);
	A16(&a, 0x60); // %s = the destination
	Op(&a, 0x04);
	A16(&a, 0x60); // dst = data[0xa0]
	Op(&a, 0x04);
	A16(&a, 0x80); // fmt
	Op(&a, 0x6f);
	Ret(&a);
	CHECK(Run(&a) == 4);
	CHECK(strcmp((char*)t->data + 0xa0, "one+2") == 0);
}

/* Control flow: "15" with condition 5 ("< 0") jumps over a push and with
 * condition 2 ("> 0") falls through, the target being a code offset
 * pushed by "06"; "16" pushes the return address on the frame stack in
 * the data area and "17" returns to it, while a return at fp 0 ends the
 * thread.  The targets are patched into the code after the skipped part
 * is assembled. */
static void TestControl(void)
{
	Asm_t a;
	printf("control flow\n");
	// jcc: 5 = "< 0" taken, 2 = "> 0" not taken; the target is a code offset
	a.len = 0;
	PushI32(&a, (uint32_t)-1); // v
	Op(&a, 0x06);
	A16(&a, 0); // placeholder, patched below
	{
		uint32_t at = a.len - 2, opAt = at - 1;
		Op(&a, 0x15);
		A8(&a, 5);
		PushI32(&a, 111); // skipped
		Ret(&a);
		a.code[at] = (uint8_t)(a.len - opAt);
		a.code[at + 1] = (uint8_t)((a.len - opAt) >> 8);
		PushI32(&a, 222);
		Ret(&a);
	}
	CHECK(Run(&a) == 4 && Top() == 222 && Depth() == 1);
	a.len = 0;
	PushI32(&a, (uint32_t)-1);
	Op(&a, 0x06);
	A16(&a, 0);
	{
		uint32_t at = a.len - 2, opAt = at - 1;
		Op(&a, 0x15);
		A8(&a, 2); // > 0: not taken
		PushI32(&a, 111);
		Ret(&a);
		a.code[at] = (uint8_t)(a.len - opAt);
		a.code[at + 1] = (uint8_t)((a.len - opAt) >> 8);
		PushI32(&a, 222);
		Ret(&a);
	}
	CHECK(Run(&a) == 4 && Top() == 111 && Depth() == 1);

	// call / ret: the return address goes on the frame stack in the data area; ret at fp
	// 0 ends the thread
	a.len = 0;
	PushI32(&a, 0x40);
	Op(&a, 0x11);
	Op(&a, 0x06);
	A16(&a, 0);
	{
		uint32_t at = a.len - 2, opAt = at - 1;
		Op(&a, 0x16);
		PushI32(&a, 5);
		Op(&a, 0x20); // 7 + 5
		Ret(&a);
		a.code[at] = (uint8_t)(a.len - opAt);
		a.code[at + 1] = (uint8_t)((a.len - opAt) >> 8);
		PushI32(&a, 7);
		A8(&a, 0x17); // a plain return: back to the instruction after the call
	}
	CHECK(Run(&a) == 4 && Top() == 12 && Depth() == 1);
}

/* The thread-local heap: "70" returns a pointer with the heap tag that
 * ResolvePtr maps to the block, a second block does not overlap the
 * first, and "71" frees each with 1. */
static void TestHeap(void)
{
	Asm_t a;
	Thread_t* t = gT;
	uint32_t p;
	printf("heap\n");
	a.len = 0;
	PushI32(&a, 100);
	Op(&a, 0x70);
	p = Eval(&a);
	CHECK((p & 0xfe000000u) == VM_TAG_HEAP);
	CHECK(ResolvePtr(p, t) == Thread_HeapPtr(t, p & VM_OFFSET_MASK));
	// a second block does not overlap the first; freeing both succeeds, freeing again
	// fails
	a.len = 0;
	PushI32(&a, 100);
	Op(&a, 0x70);
	{
		uint32_t q = Eval(&a);
		CHECK(q != p);
		CHECK((q & VM_OFFSET_MASK) >= (p & VM_OFFSET_MASK) + 100 || (p & VM_OFFSET_MASK) >= (q & VM_OFFSET_MASK) + 100);
		a.len = 0;
		PushI32(&a, q);
		Op(&a, 0x71);
		CHECK(Eval(&a) == 1);
	}
	a.len = 0;
	PushI32(&a, p);
	Op(&a, 0x71);
	CHECK(Eval(&a) == 1);
}

/* Script errors: a jump to address 0 ("14" with a NULL target) raises a
 * script error, which arrives in the try frame as the thrown value
 * BGI_SCRIPT_ERROR_VALUE; an undefined base opcode ("03") has no handler,
 * which Run reports as -1 (the scheduler raises MSG_UNDEFINED_OP
 * instead).  A failure means bad scripts would run on instead of stopping
 * with an error box. */
static void TestErrors(void)
{
	Asm_t a;
	BgiTryFrame_t f;
	printf("errors\n");
	a.len = 0;
	PushI32(&a, 0);
	Op(&a, 0x14); // jump to NULL
	Ret(&a);
	if(BGI_TRY(f))
	{
		Run(&a);
		BGI_END_TRY(f);
		CHECK(!"a jump to NULL must raise");
	}
	else
	{
		CHECK(f.value == BGI_SCRIPT_ERROR_VALUE);
	}
	a.len = 0;
	Op(&a, 0x03); // undefined base instruction
	Ret(&a);
	if(BGI_TRY(f))
	{
		int rc = Run(&a);
		BGI_END_TRY(f);
		CHECK(rc == -1); // Run stops on the NULL handler; the scheduler raises MSG_UNDEFINED_OP
	}
	else
	{
		CHECK(f.value == BGI_SCRIPT_ERROR_VALUE);
	}
}

/* The "EF xx" family (src/vm/ops_obgi.c, docs/obgi.md): "EF 00" pushes
 * the level of the extensions; the switches take their arguments off the
 * stack, push nothing, and land in the text engine's variables - a
 * direction other than 1 or 2 is 0, an encoding other than 0 or 1 changes
 * nothing, a scroll time of 0 keeps the one that is set.  A number the
 * family does not have is an undefined instruction, which the dispatcher
 * raises as a script error. */
static void TestExtensions(void)
{
	Asm_t a;
	BgiTryFrame_t f;
	int mode;
	static const int dirs[][2] = {{1, 1}, {2, 2}, {0, 0}, {5, 0}, {-1, 0}};
	printf("extensions\n");
	a.len = 0;
	Op(&a, 0xef);
	A8(&a, 0x00); // obgi.version
	CHECK(Eval(&a) == 1);
	for(mode = 0; mode < 5; mode++)
	{
		a.len = 0;
		PushI8(&a, dirs[mode][0]);
		Op(&a, 0xef);
		A8(&a, 0x01);  // obgi.text_direction
		PushI8(&a, 7); // (what Eval expects to find: the instruction itself pushes nothing)
		CHECK(Eval(&a) == 7);
		CHECK(gTextDirection == dirs[mode][1]);
	}
	a.len = 0;
	PushI8(&a, 1);
	Op(&a, 0xef);
	A8(&a, 0x02); // obgi.text_encoding(1): UTF-8
	PushI8(&a, 7);
	CHECK(Eval(&a) == 7);
	CHECK(gTextUtf8 == 1);
	a.len = 0;
	PushI8(&a, 9); // not an encoding: nothing changes
	Op(&a, 0xef);
	A8(&a, 0x02);
	PushI8(&a, 0);
	Op(&a, 0xef);
	A8(&a, 0x02); // and back to Shift-JIS
	PushI8(&a, 7);
	CHECK(Eval(&a) == 7);
	CHECK(gTextUtf8 == 0);
	a.len = 0;
	PushI8(&a, 1);
	Op(&a, 0xef);
	A8(&a, 0x03); // obgi.text_word_wrap(1)
	PushI8(&a, 1);
	PushI16(&a, 200);
	Op(&a, 0xef);
	A8(&a, 0x08); // obgi.text_scroll(1, 200)
	PushI8(&a, 7);
	CHECK(Eval(&a) == 7);
	CHECK(gTextWordWrap == 1 && gTextScroll == 1 && gTextScrollMs == 200);
	a.len = 0;
	PushI8(&a, 0);
	Op(&a, 0xef);
	A8(&a, 0x03);
	PushI8(&a, 0);
	PushI8(&a, 0); // a time of 0 keeps the 200
	Op(&a, 0xef);
	A8(&a, 0x08);
	PushI8(&a, 7);
	CHECK(Eval(&a) == 7);
	CHECK(gTextWordWrap == 0 && gTextScroll == 0 && gTextScrollMs == 200);
	a.len = 0;
	Op(&a, 0xef);
	A8(&a, 0x7f); // no such instruction in the family
	Ret(&a);
	if(BGI_TRY(f))
	{
		Run(&a);
		BGI_END_TRY(f);
		CHECK(!"an undefined EF instruction must raise");
	}
	else
	{
		CHECK(f.value == BGI_SCRIPT_ERROR_VALUE);
	}
}

/* 1 when CheckPriority accepts prio, 0 when it raises the script error */
static int PriorityAccepted(uint32_t prio)
{
	BgiTryFrame_t f;
	if(BGI_TRY(f))
	{
		CheckPriority(prio, gT);
		BGI_END_TRY(f);
		return 1;
	}
	CHECK(f.value == BGI_SCRIPT_ERROR_VALUE);
	return 0;
}

/* ThrowScriptError(text) in a try frame; 1 when it threw the script error */
static int Throws(const char* text)
{
	BgiTryFrame_t f;
	if(BGI_TRY(f))
	{
		ThrowScriptError(text);
		BGI_END_TRY(f);
		return 0;
	}
	return f.value == BGI_SCRIPT_ERROR_VALUE;
}

/* The services the instructions of the later builds rely on.  A display
 * priority is below 0x1000, from 1.529 on below 0x10000.  While "81 6A"
 * keeps errors the first one's message is kept and a later error does
 * not replace it ("81 6B" reads it: its length + 1, 0 when none).  The
 * gamepad table of "81 1B" has 36 entries; "81 04" takes a step limit of
 * 50 .. 60000 ms and "81 68" answers the previous setting. */
static void TestLaterBuilds(void)
{
	char buf[32];
	printf("later builds\n");
	CHECK(PriorityAccepted(0xfff));
	CHECK(!PriorityAccepted(0x1000));
	CHECK(Engine_SelectProfile("1.636"));
	CHECK(PriorityAccepted(0xffff));
	CHECK(!PriorityAccepted(0x10000));
	CHECK(Error_KeptMessage(NULL) == 0);
	Error_SetKeep(1);
	CHECK(Throws("first"));
	CHECK(Throws("the second one"));
	CHECK(Error_KeptMessage(NULL) == 6);
	memset(buf, 0x55, sizeof buf);
	CHECK(Error_KeptMessage(buf) == 6 && strcmp(buf, "first") == 0);
	Error_FreeKept();
	Error_SetKeep(0);
	CHECK(Error_KeptMessage(NULL) == 0);
	CHECK(Pad_SetKey(35, 0x0d) == 0 && gPadKeys[35] == 0x0d);
	CHECK(Pad_SetKey(36, 0x0d) == 0x80000002u);
	CHECK(Clock_SetMaxStep(49) == 0 && Clock_SetMaxStep(60001) == 0);
	CHECK(Clock_SetMaxStep(60000) == 1 && Clock_SetMaxStep(500) == 1);
	CHECK(Clock_SetStopWhenInactive(1) == 0 && Clock_SetStopWhenInactive(0) == 1);
	CHECK(Engine_SelectProfile("1.69/444"));
}

// step thread t from where it is until a handler returns a non-zero scheduler code; -1 for an opcode without a handler
static int Step(Thread_t* t)
{
	int rc = 0, steps = 0;
	while(rc == 0 && steps++ < 100000)
	{
		uint8_t op = Thread_FetchOp(t);
		if(!vm_optable_main[op])
			return -1;
		rc = vm_optable_main[op](t);
	}
	return rc;
}

// load the program into t's code area at offset 0 and step it from there with an empty stack
static int RunOn(Thread_t* t, const Asm_t* a)
{
	memcpy(t->code, a->code, a->len);
	t->codeUsed = a->len;
	Thread_SetIP(t, 0);
	t->sp = 0;
	return Step(t);
}

/* "81 44": threads in parts of another thread's areas.  The parts are cut
 * from the tops of the lender's code and data areas, and what the lender
 * may use itself ends below them; a second borrower goes below the first,
 * and a borrower's own borrower borrows from the first lender.  A code
 * part that would reach into the lender's modules fails with 2, a data
 * part larger than the room left with 3.  A borrower shares the lender's
 * buffers and heap, loads its modules into its part (and may unload them
 * all with "80 41", which is an error for a thread's main program), keeps
 * its frames in its part of the data area and ends with a return at the
 * start of that part.  A borrower that ends gives its parts back (the lender's limits
 * stay until its next loan); a lender that ends takes its borrowers with
 * it.  Through the instruction: the id is pushed, an ip past the part is
 * a script error. */
static void TestBorrower(void)
{
	Thread_t* root = Thread_New(0, 0, 0);
	Thread_t *l, *a, *b, *g = NULL;
	uint8_t prog[16 + 0x40];
	ProgramHeader_t* h = (ProgramHeader_t*)prog;
	Asm_t s;
	BgiTryFrame_t f;
	printf("borrowed areas\n");
	CHECK(Engine_SelectProfile("1.636"));
	Vm_TablesInit();
	l = Thread_SpawnChild(root, 0x100, 0x1000, 0x1000);
	memset(prog, 0, sizeof prog);
	h->codeOffset = 16;
	h->codeSize = 0x40;
	CHECK(Thread_LoadModule(l, prog, "lender") == 0 && l->codeUsed == 0x40);
	CHECK(Thread_SpawnBorrower(l, 0x40, 0x400, 0x300, 0x10, &a) == 0);
	CHECK(a->lender == l && a->code == l->code && a->data == l->data && a->heap == l->heap);
	CHECK(a->codeBase == 0xc00 && a->codeUsed == 0xc00 && a->codeSize == 0x1000);
	CHECK(a->dataBase == 0xd00 && a->fp == 0xd00 && a->dataSize == 0x1000 && a->ip == 0x10);
	CHECK(l->codeSize == 0xc00 && l->dataSize == 0xd00 && l->loanCount == 1 && l->next == a);
	CHECK(a->id == l->id + 1 && a->stackEntries == 0x40);
	CHECK(Thread_SpawnBorrower(a, 0x40, 0x200, 0x100, 0x20, &b) == 0); // from a's lender
	CHECK(b->lender == l && b->codeBase == 0xa00 && b->dataBase == 0xc00 && a->next == b);
	CHECK(l->codeSize == 0xa00 && l->dataSize == 0xc00 && l->loanCount == 2);
	CHECK(Thread_SpawnBorrower(l, 0x40, 0x9c1, 0x10, 0, &g) == 2 && g == NULL);                      // into the lender's module
	CHECK(Thread_SpawnBorrower(l, 0x40, 0x10, 0xc01, 0, &g) == 3 && g == NULL && l->loanCount == 2); // more data than is left
	CHECK(Thread_LoadModule(a, prog, "borrower") == 0xc00 && a->codeUsed == 0xc40);
	CHECK(Thread_LoadModule(l, prog, "lender2") == 0x40); // the lender's own loads stay below its loans

	// a call and the returns of a borrower: its frames are in the lender's data area
	s.len = 0;
	PushI32(&s, 0xa0);
	Op(&s, 0x16); // call 0xa0
	PushI32(&s, 9);
	Op(&s, 0x17); // the outermost return: the thread ends
	while(s.len < 0x20)
		Op(&s, 0xff);
	PushI32(&s, 7);
	Op(&s, 0x17);
	memcpy(l->code + 0x80, s.code, s.len);
	Thread_SetIP(b, 0x80);
	b->sp = 0;
	CHECK(Step(b) == 4 && b->sp == 2 && b->stack[0] == 7 && b->stack[1] == 9 && b->fp == 0xc00);
	CHECK(l->data[0xc00] == 0x86); // the return address, at the start of b's part

	// a borrower that ends gives its parts back, the lender's limits stay; a lender takes its borrowers along
	CHECK(Thread_SpawnBorrower(l, 0x40, 0x100, 0x100, 0, &g) == 0 && g->codeBase == 0x900 && l->loanCount == 3);
	CHECK(Thread_KillChild(root, g) && l->loanCount == 2 && l->codeSize == 0x900);
	CHECK(Thread_SpawnBorrower(l, 0x40, 0x80, 0x80, 0, &g) == 0 && g->codeBase == 0x980 && g->dataBase == 0xb80);
	CHECK(l->codeSize == 0x980 && l->dataSize == 0xb80);
	CHECK(Thread_KillChild(root, l) && root->next == NULL);
	Thread_Delete(root);

	// the instruction: stackEntries, codeSize, dataSize, ip -> id; an ip past the new part is a script error
	l = Thread_New(0x100, 0x1000, 0x1000);
	s.len = 0;
	PushI32(&s, 0x40);
	PushI32(&s, 0x200);
	PushI32(&s, 0x200);
	PushI32(&s, 0x10);
	A8(&s, 0x81);
	A8(&s, 0x44);
	Op(&s, 0x17);
	CHECK(RunOn(l, &s) == 4 && l->sp == 1 && l->next && l->stack[0] == l->next->id);
	CHECK(l->next && l->next->codeBase == 0xe00 && l->next->ip == 0x10);
	// "80 41": a borrower may unload its last module (its main program is the lender's), the lender may not
	g = l->next;
	CHECK(Thread_LoadModule(g, prog, "sub") == 0xe00);
	s.len = 0;
	A8(&s, 0x80);
	A8(&s, 0x41);
	A8(&s, 0x80);
	A8(&s, 0x41);
	Op(&s, 0x17);
	memcpy(l->code + 0x100, s.code, s.len);
	Thread_SetIP(g, 0x100);
	g->sp = 0;
	CHECK(Step(g) == 4 && g->sp == 2 && g->stack[0] == 0 && g->stack[1] == 0x80000001u && g->codeUsed == 0xe00);
	CHECK(Thread_LoadModule(l, prog, "main") != 0x80000000u);
	if(BGI_TRY(f))
	{
		RunOn(l, &s);
		BGI_END_TRY(f);
		CHECK(!"no script error for unloading the main program");
	}
	else
		CHECK(f.value == BGI_SCRIPT_ERROR_VALUE);
	s.len = 0;
	PushI32(&s, 0x40);
	PushI32(&s, 0x200);
	PushI32(&s, 0x200);
	PushI32(&s, 0xe00); // the end of the new part, 0xc00 .. 0xe00
	A8(&s, 0x81);
	A8(&s, 0x44);
	Op(&s, 0x17);
	if(BGI_TRY(f))
	{
		RunOn(l, &s);
		BGI_END_TRY(f);
		CHECK(!"no script error for an ip past the part");
	}
	else
		CHECK(f.value == BGI_SCRIPT_ERROR_VALUE && l->next && l->next->next && l->next->next->codeBase == 0xc00);
	while(l->next)
		Thread_KillChild(l, l->next);
	CHECK(l->loanCount == 0 && !l->codeLoans && !l->dataLoans);
	Thread_Delete(l);
	CHECK(Engine_SelectProfile("1.69/444"));
	Vm_TablesInit();
}

// a pointer to byte `off` of the data area, with the selected profile's tag
static uint32_t DataPtr(uint32_t off)
{
	return off | (gEngine->tagData << VM_TAG_SHIFT);
}

// signed LEB128, used by the compact instruction tests
static void Sleb(Asm_t* a, int32_t value)
{
	for(;;)
	{
		uint8_t b = value & 0x7f;
		int32_t next = value >> 7;
		if((next == 0 && !(b & 0x40)) || (next == -1 && (b & 0x40)))
		{
			A8(a, b);
			return;
		}
		A8(a, b | 0x80);
		value = next;
	}
}

static void Typed(Asm_t* a, int32_t value, uint32_t size)
{
	Sleb(a, (value << 2) | (int32_t)size);
}

/* The compact base instructions of 1.667 and 1.685.  In particular, 2E is
 * a + b * imm (not a * b + imm), EA adds its 16-bit offset before storing,
 * and 15 reads an inline target when condition bit 3 is set. */
static void TestCompact(void)
{
	Asm_t a;
	printf("compact\n");
	CHECK(Engine_SelectProfile("1.685"));
	CHECK(gEngine->tagShift == 28 && gEngine->dynFirst == 4);
	Vm_TablesInit();

	a.len = 0;
	PushI32(&a, 0x50000000);
	PushI32(&a, 2);
	Op(&a, 0x2e);
	Sleb(&a, 4);
	CHECK(Eval(&a) == 0x50000008u);

	a.len = 0;
	Op(&a, 0x03);
	A8(&a, 2);
	Sleb(&a, 13);
	Sleb(&a, 1280);
	Sleb(&a, 960);
	Ret(&a);
	CHECK(Run(&a) == 4);
	CHECK(Depth() == 3);
	CHECK(gT->stack[0] == 13 && gT->stack[1] == 1280 && Top() == 960);

	a.len = 0;
	Op(&a, 0x13);
	A16(&a, 3);
	PushI32(&a, 0x1234);
	CHECK(Eval(&a) == 0x1234);

	a.len = 0;
	PushI32(&a, 1);
	Op(&a, 0x15);
	A8(&a, 8);
	A16(&a, 9);
	PushI32(&a, 0xdead);
	PushI32(&a, 0x2468);
	CHECK(Eval(&a) == 0x2468);

	a.len = 0;
	PushI32(&a, 0x100);
	Op(&a, 0x11);
	PushI32(&a, 11);
	PushI32(&a, 22);
	Op(&a, 0xe2);
	A8(&a, 1);
	A16(&a, 0x8040);
	A16(&a, 0x8044);
	Op(&a, 0xe3);
	A8(&a, 1);
	A16(&a, 0x8040);
	A16(&a, 0x8044);
	Ret(&a);
	CHECK(Run(&a) == 4);
	CHECK(Depth() == 2);
	CHECK(gT->stack[0] == 22 && Top() == 11);

	a.len = 0;
	PushI32(&a, DataPtr(0x40));
	Op(&a, 0x0d);
	Typed(&a, 123, 2);
	Ret(&a);
	CHECK(Run(&a) == 4 && *(uint32_t*)(gT->data + 0x40) == 123);
	a.len = 0;
	PushI32(&a, 0x100);
	Op(&a, 0x11);
	Op(&a, 0x0e);
	A16(&a, 0x40);
	Typed(&a, 456, 2);
	Ret(&a);
	CHECK(Run(&a) == 4 && *(uint32_t*)(gT->data + 0xc0) == 456);

	/* 0E's local is a full 16-bit offset (the size is in the immediate):
	 * cnfgwnd._bp stores its result at fp - 0x46F0 in an 18336-byte frame */
	{
		Thread_t* small = gT;
		gT = Thread_New(0x100, 0x1000, 0x8000);
		CHECK(gT != NULL && gT->valid);
		a.len = 0;
		PushI32(&a, 0x4800);
		Op(&a, 0x11);
		Op(&a, 0x0e);
		A16(&a, 0x46f0);
		Typed(&a, -1, 2);
		Ret(&a);
		CHECK(Run(&a) == 4 && *(int32_t*)(gT->data + 0x110) == -1 && *(int32_t*)(gT->data + 0x4110) == 0);
		Thread_Delete(gT);
		gT = small;
	}

	memcpy(gT->data + 0x80, "MakerLogo", 10);
	*(uint32_t*)(gT->data + 0x40) = DataPtr(0x80);
	a.len = 0;
	PushI32(&a, 0x100);
	Op(&a, 0x11);
	Op(&a, 0xea);
	A16(&a, 0x80c0);
	A16(&a, 4);
	Sleb(&a, 0);
	Ret(&a);
	CHECK(Run(&a) == 4 && gT->data[0x84] == 0 && memcmp(gT->data + 0x80, "Make", 4) == 0);

	/* the size bits are the access size only: the pointer is still a dword
	 * (cnfgwnd._bp's Default writes word and byte settings this way) */
	memset(gT->data + 0x80, 0x55, 0x30);
	a.len = 0;
	PushI32(&a, 0x100);
	Op(&a, 0x11);
	Op(&a, 0xea); // word 4 at +10
	A16(&a, 0x40c0);
	A16(&a, 10);
	Sleb(&a, 4);
	Op(&a, 0xea); // byte 1 at +36
	A16(&a, 0x00c0);
	A16(&a, 36);
	Sleb(&a, 1);
	Ret(&a);
	CHECK(Run(&a) == 4 && *(int16_t*)(gT->data + 0x8a) == 4 && gT->data[0x8c] == 0x55 && gT->data[0xa4] == 1 && gT->data[0xa5] == 0x55);

	/* FA: the index is a dword whatever the size bits say (they size the
	 * value read): bitmap._bp reads byte 0 of name slot 0x80 this way */
	*(uint32_t*)(gT->data + 0x40) = 0x80; // the index, at fp - 0xc0
	memset(gT->data + 0x200, 0, 0x100);
	gT->data[0x280] = 0x5a;
	a.len = 0;
	PushI32(&a, 0x100);
	Op(&a, 0x11);
	Op(&a, 0xfa);
	A32(&a, DataPtr(0x200)); // add
	A16(&a, 0x00c0);         // the index local, byte access
	A16(&a, 1);              // mul
	CHECK(Eval(&a) == 0x5a);

	*(uint32_t*)(gT->data + 0x40) = 18;
	a.len = 0;
	PushI32(&a, 0x100);
	Op(&a, 0x11);
	Op(&a, 0xef);
	A32(&a, DataPtr(0x40) | 0x80000000u);
	Ret(&a);
	PushI32(&a, 0x1357);
	Op(&a, 0x17);
	CHECK(Eval(&a) == 0x1357);

	CHECK(Engine_SelectProfile("1.69/444"));
	Vm_TablesInit();
}

/* push the n arguments, run family `fam` instruction `op` and return what it pushed */
static uint32_t Call(uint32_t fam, uint32_t op, const uint32_t* args, int n)
{
	Asm_t a;
	int i;
	a.len = 0;
	for(i = 0; i < n; i++)
		PushI32(&a, args[i]);
	A8(&a, fam);
	A8(&a, op);
	return Eval(&a);
}

// the contents of a file as a string (at most 63 bytes); "" when it cannot be read
static const char* Slurp(const char* path)
{
	static char buf[64];
	FILE* f = fopen(path, "rb");
	size_t n = 0;
	if(f)
	{
		n = fread(buf, 1, sizeof buf - 1, f);
		fclose(f);
	}
	buf[n] = 0;
	return buf;
}

// write a string to a file
static void Spit(const char* path, const char* text)
{
	FILE* f = fopen(path, "wb");
	if(f)
	{
		fputs(text, f);
		fclose(f);
	}
}

/* Files, through the instructions, with two files under bin/ (the test
 * runs from the project root).  "80 27" moves the operand pushed last to
 * the one pushed first and, as MoveFile, fails when the destination
 * exists.  "81 28" opens (0 read, 1 write from scratch, 2 write into the
 * file as it is) and refuses a bad mode (1), a path already open in
 * another case (2) and a missing file (3); the status words of "81 29" ..
 * "81 2B" get the bytes transferred, 1 for a seek or a close, -1 for
 * nothing transferred or a seek past the end, and an unknown handle is
 * 4.  In 1.553 there is no mode 2, "81 29" takes only the handle (-1 when
 * unknown), a transfer of 0 bytes is 6, one that transfers nothing gives
 * -2, and there is no "81 2B".  "81 38" answers 7 without filters, 4 for
 * an unknown kind (the path zeroed) and -1 when nothing was chosen. */
static void TestFiles(void)
{
	static const char* A = "bin/test_vm_a.tmp";
	static const char* B = "bin/test_vm_b.tmp";
	uint8_t* d = gT->data;
	uint32_t pa, pb, pbUpper, out, st, buf, h, h2;
	int32_t status;
	printf("files\n");
	CHECK(Engine_SelectProfile("1.636"));
	Vm_TablesInit();
	pa = DataPtr(0x400), pb = DataPtr(0x480), pbUpper = DataPtr(0x500);
	out = DataPtr(0x200), st = DataPtr(0x210), buf = DataPtr(0x300);
	remove(A);
	remove(B);
	strcpy((char*)d + 0x400, A);
	strcpy((char*)d + 0x480, B);
	strcpy((char*)d + 0x500, "BIN/TEST_VM_B.TMP");
	Spit(A, "hello");
	{
		const uint32_t mv[] = {pb, pa}; // newPath, existing
		CHECK(Call(0x80, 0x27, mv, 2) == 1 && strcmp(Slurp(B), "hello") == 0 && strcmp(Slurp(A), "") == 0);
		Spit(A, "again");
		CHECK(Call(0x80, 0x27, mv, 2) == 0 && strcmp(Slurp(B), "hello") == 0); // the destination exists
		remove(A);
	}
	{
		const uint32_t badMode[] = {out, pb, 3}, missing[] = {out, pa, 0}, rd[] = {out, pb, 0}, again[] = {out, pbUpper, 0};
		CHECK(Call(0x81, 0x28, badMode, 3) == 1);
		CHECK(Call(0x81, 0x28, missing, 3) == 3);
		CHECK(Call(0x81, 0x28, rd, 3) == 0);
		memcpy(&h, d + 0x200, 4);
		CHECK(h != 0);
		CHECK(Call(0x81, 0x28, again, 3) == 2); // the same file in another case
	}
	{
		const uint32_t rd4[] = {st, h, buf, 4}, rd9[] = {st, h, buf + 4, 9}, rd1[] = {st, h, buf, 1};
		const uint32_t end[] = {st, h, 0xffffffffu}, past[] = {st, h, 6}, start[] = {st, h, 1}, close[] = {st, h};
		const uint32_t unknown[] = {st, h + 100, buf, 4};
		memset(d + 0x300, 0, 16);
		CHECK(Call(0x81, 0x2a, rd4, 4) == 0 && (memcpy(&status, d + 0x210, 4), status == 4) && memcmp(d + 0x300, "hell", 4) == 0);
		CHECK(Call(0x81, 0x2a, rd9, 4) == 0 && (memcpy(&status, d + 0x210, 4), status == 1) && d[0x304] == 'o');
		CHECK(Call(0x81, 0x2a, rd1, 4) == 0 && (memcpy(&status, d + 0x210, 4), status == -1)); // at the end
		CHECK(Call(0x81, 0x2a, unknown, 4) == 4);
		CHECK(Call(0x81, 0x2b, past, 3) == 0 && (memcpy(&status, d + 0x210, 4), status == -1));
		CHECK(Call(0x81, 0x2b, end, 3) == 0 && (memcpy(&status, d + 0x210, 4), status == 1));
		CHECK(Call(0x81, 0x2b, start, 3) == 0 && (memcpy(&status, d + 0x210, 4), status == 1));
		CHECK(Call(0x81, 0x2a, rd4, 4) == 0 && memcmp(d + 0x300, "ello", 4) == 0);
		CHECK(Call(0x81, 0x29, close, 2) == 0 && (memcpy(&status, d + 0x210, 4), status == 1));
		CHECK(Call(0x81, 0x29, close, 2) == 4);
	}
	{
		const uint32_t keep[] = {out, pb, 2}, create[] = {out, pb, 1};
		uint32_t w2[4], end[3], w1[4], close[2];
		memcpy(d + 0x300, "XYZ", 3);
		CHECK(Call(0x81, 0x28, keep, 3) == 0);
		memcpy(&h2, d + 0x200, 4);
		CHECK(h2 > h); // a new number
		w2[0] = st, w2[1] = h2, w2[2] = buf, w2[3] = 2;
		end[0] = st, end[1] = h2, end[2] = 0xffffffffu;
		w1[0] = st, w1[1] = h2, w1[2] = buf + 2, w1[3] = 1;
		close[0] = 0, close[1] = h2; // no status word
		CHECK(Call(0x81, 0x2a, w2, 4) == 0 && (memcpy(&status, d + 0x210, 4), status == 2));
		CHECK(Call(0x81, 0x2b, end, 3) == 0 && Call(0x81, 0x2a, w1, 4) == 0);
		CHECK(Call(0x81, 0x29, close, 2) == 0 && strcmp(Slurp(B), "XYlloZ") == 0);
		CHECK(Call(0x81, 0x28, create, 3) == 0);
		memcpy(&h2, d + 0x200, 4);
		close[1] = h2;
		CHECK(Call(0x81, 0x29, close, 2) == 0 && strcmp(Slurp(B), "") == 0); // truncated
	}
	{
		uint32_t list = DataPtr(0x700), str = DataPtr(0x780), path = DataPtr(0x800);
		const uint32_t none[] = {path, 0, list, list, 0, 0, 0}, kind[] = {path, 1, list, list, 0, 0, 2}, open[] = {path, 1, list, list, 0, 0, 0};
		memcpy(d + 0x700, &str, 4); // a one-entry list: the string at 0x780
		strcpy((char*)d + 0x780, "*.txt");
		memset(d + 0x800, 0x55, 0x30c);
		CHECK(Call(0x81, 0x38, none, 7) == 7 && d[0x800] == 0x55); // the path untouched
		CHECK(Call(0x81, 0x38, kind, 7) == 4 && d[0x800] == 0 && d[0x800 + 0x30b] == 0);
		CHECK(Call(0x81, 0x38, open, 7) == 0xffffffffu); // nothing chosen (no dialog here)
	}

	// 1.553: two modes, a close without a status word, no seek
	CHECK(Engine_SelectProfile("1.553"));
	Vm_TablesInit();
	pb = DataPtr(0x480), out = DataPtr(0x200), st = DataPtr(0x210), buf = DataPtr(0x300);
	CHECK(!vm_optable_81[0x2b]);
	Spit(B, "abc");
	{
		const uint32_t keep[] = {out, pb, 2}, rd[] = {out, pb, 0};
		uint32_t zero[4], all[4], more[4], close[1];
		CHECK(Call(0x81, 0x28, keep, 3) == 1);
		CHECK(Call(0x81, 0x28, rd, 3) == 0);
		memcpy(&h, d + 0x200, 4);
		zero[0] = st, zero[1] = h, zero[2] = buf, zero[3] = 0;
		all[0] = st, all[1] = h, all[2] = buf, all[3] = 8;
		more[0] = st, more[1] = h, more[2] = buf, more[3] = 8;
		close[0] = h;
		CHECK(Call(0x81, 0x2a, zero, 4) == 6);
		CHECK(Call(0x81, 0x2a, all, 4) == 0 && (memcpy(&status, d + 0x210, 4), status == 3));
		CHECK(Call(0x81, 0x2a, more, 4) == 0 && (memcpy(&status, d + 0x210, 4), status == -2));
		CHECK(Call(0x81, 0x29, close, 1) == 0);
		CHECK(Call(0x81, 0x29, close, 1) == 0xffffffffu);
	}
	FileAcc_CloseAll();
	remove(A);
	remove(B);
	CHECK(Engine_SelectProfile("1.69/444"));
	Vm_TablesInit();
}

// make the thread, run every group; exit status 1 when any check failed
int main(void)
{
#if defined(_WIN32)
	_putenv("BGI_MSGBOX_STDERR=1"); // error boxes go to stderr instead of a dialog
#else
	setenv("BGI_MSGBOX_STDERR", "1", 1);
#endif
	OS_Init();
	Vm_TablesInit();
	gT = Thread_New(0x100, 0x1000, 0x1000); // 0x100 stack entries, 0x1000 bytes of code and of data
	CHECK(gT != NULL && gT->valid);
	TestPush();
	TestArith();
	TestMemory();
	TestStrings();
	TestControl();
	TestHeap();
	TestErrors();
	TestExtensions();
	TestLaterBuilds();
	TestBorrower();
	TestCompact();
	TestFiles();
	Thread_Delete(gT);
	OS_Shutdown();
	if(gFails)
	{
		printf("%d failure(s)\n", gFails);
		return 1;
	}
	printf("ok\n");
	return 0;
}
