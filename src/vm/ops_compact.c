/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * ops_compact.c - the compact base instructions of 1.667 and 1.685
 *
 * The newer script compilers pack immediates as signed LEB128, local
 * references as a 14-bit reverse offset plus a two-bit access size, and
 * frequently used instruction sequences into single opcodes.  The 1.685
 * compiler adds more combined forms and uses EF for a call through a
 * pointer; older profiles keep EF for OpenBGI's own extension family.
 */
#include "bgi/vm.h"
#include "bgi/sys.h"
#include "bgi/error.h"
#include "bgi/msg.h"

// ---- compact operands ----------------------------------------------------

// signed LEB128, seven payload bits per byte and bit 6 of the last byte as the sign
static int64_t RdSleb64(Thread_t* t)
{
	int64_t v = 0;
	unsigned shift = 0, i;
	for(i = 0; i < 10; i++)
	{
		uint8_t b = RdU8(t);
		v |= (int64_t)(b & 0x7f) << shift;
		shift += 7;
		if(!(b & 0x80))
		{
			if((b & 0x40) && shift < 64)
				v |= -((int64_t)1 << shift);
			return v;
		}
	}
	return v;
}

static int32_t RdSleb(Thread_t* t)
{
	return (int32_t)RdSleb64(t);
}

static uint64_t RdU64(Thread_t* t)
{
	uint64_t v = 0;
	unsigned i;
	for(i = 0; i < 8; i++)
		v |= (uint64_t)RdU8(t) << (i * 8);
	return v;
}

/* A typed immediate is (value << 2) | size in one signed LEB128.  Compact
 * memory operations use size 2 for a dword, 1 for a signed word, and both
 * 0 and 3 for a signed byte. */
static int32_t RdTyped(Thread_t* t, uint32_t* size)
{
	int64_t v = RdSleb64(t);
	*size = (uint32_t)v & 3;
	return (int32_t)(v >> 2);
}

static uint32_t LoadCompact(const void* ptr, uint32_t size)
{
	const uint8_t* p = (const uint8_t*)ptr;
	if(size == 2)
	{
		uint32_t v;
		memcpy(&v, p, 4);
		return v;
	}
	if(size == 1)
	{
		int16_t v;
		memcpy(&v, p, 2);
		return (uint32_t)(int32_t)v;
	}
	return (uint32_t)(int32_t)(int8_t)*p;
}

static void StoreCompact(void* ptr, uint32_t size, uint32_t value)
{
	uint8_t* p = (uint8_t*)ptr;
	if(size == 2)
		memcpy(p, &value, 4);
	else if(size == 1)
	{
		uint16_t v = (uint16_t)value;
		memcpy(p, &v, 2);
	}
	else
		*p = (uint8_t)value;
}

// a packed local: bits 0 .. 13 the offset below fp, bits 14 .. 15 the access size
static uint8_t* LocalMasked(Thread_t* t, uint32_t local)
{
	return (uint8_t*)ResolvePtr((Thread_GetFP(t) - (local & 0x3fff)) | VM_TAG_DATA, t);
}

// some combined forms take an untyped local offset and the access size from another local
static uint8_t* LocalFull(Thread_t* t, uint32_t local)
{
	return (uint8_t*)ResolvePtr((Thread_GetFP(t) - local) | VM_TAG_DATA, t);
}

static uint32_t LocalLoad(Thread_t* t, uint32_t local)
{
	return LoadCompact(LocalMasked(t, local), local >> 14);
}

static void LocalStore(Thread_t* t, uint32_t local, uint32_t value)
{
	StoreCompact(LocalMasked(t, local), local >> 14, value);
}

// the pointer held by a packed local: always the full dword; the size bits are the access size of the value it points to
static uint32_t LocalPointer(Thread_t* t, uint32_t local)
{
	uint32_t v;
	memcpy(&v, LocalMasked(t, local), 4);
	return v;
}

// ---- arithmetic shared by the combined instructions ----------------------

static uint32_t EvalCompare(uint32_t op, int32_t a, int32_t b)
{
	switch(op)
	{
		case 0: return a == b;
		case 1: return a != b;
		case 2: return a < b;
		case 3: return a <= b;
		case 4: return a >= b;
		case 5: return a > b;
		case 6: return (uint32_t)a & (uint32_t)b;
		case 7: return (uint32_t)a | (uint32_t)b;
		case 8: return a != 0 && b != 0;
		case 9: return a != 0 || b != 0;
		default: return 0;
	}
}

static uint32_t EvalArithmetic(uint32_t op, int32_t a, int32_t b)
{
	switch(op)
	{
		case 0: return (uint32_t)a + (uint32_t)b;
		case 1: return (uint32_t)a - (uint32_t)b;
		case 2: return (uint32_t)a * (uint32_t)b;
		case 3: return b ? (uint32_t)(a / b) : 0x80000000u;
		case 4: return b ? (uint32_t)(a % b) : 0x80000000u;
		case 5: return (uint32_t)a & (uint32_t)b;
		case 6: return (uint32_t)a | (uint32_t)b;
		case 7: return (uint32_t)a ^ (uint32_t)b;
		case 8: return (uint32_t)a << (b & 31);
		case 9: return (uint32_t)a >> (b & 31);
		case 10: return (uint32_t)(a >> (b & 31));
		default: return 0x80000000u;
	}
}

static int JumpRelative(Thread_t* t, int32_t target)
{
	char msg[0x100];
	if(target == 0)
		ScriptError(MSG_JUMP_TO_NULL, t);
	if((uint32_t)target >= t->codeSize)
	{
		sprintf(msg, MSG_IP_OUT_OF_RANGE, (unsigned)target);
		ScriptError(msg, t);
	}
	Thread_SetIP(t, (uint32_t)target);
	return 0;
}

static int CallRelative(Thread_t* t, int32_t target, uint32_t returnIP)
{
	if(Thread_GetFP(t) + 4 > t->dataSize)
		ScriptError(MSG_STACK_FULL, t);
	JumpRelative(t, target);
	FramePush(t, returnIP);
	return 0;
}

// ---- 1.667 compact instructions -----------------------------------------

static int Opcode_StoreLocalCompact(Thread_t* t) // 0F: v →
{
	uint32_t local = RdU16(t);
	LocalStore(t, local, Thread_Pop(t));
	return 0;
}

static int Opcode_AddFpCompact(Thread_t* t) // 12: →
{
	Thread_SetFP(t, (uint32_t)(Thread_GetFP(t) + RdSleb(t)));
	return 0;
}

static int Opcode_JccCompact(Thread_t* t) // 15 (1.667 on): v[, addr] →
{
	uint32_t control = RdU8(t);
	uint32_t target;
	int32_t v;
	int take;
	if(control & 8)
		target = (uint32_t)((int32_t)Thread_CurOpIP(t) + (int16_t)RdU16(t));
	else
		target = Thread_Pop(t);
	v = (int32_t)Thread_Pop(t);
	switch(control & 7)
	{
		case 0: take = v != 0; break;
		case 1: take = v == 0; break;
		case 2: take = v > 0; break;
		case 3: take = v >= 0; break;
		case 4: take = v <= 0; break;
		case 5: take = v < 0; break;
		default: take = 0; break;
	}
	if(take)
		JumpRelative(t, (int32_t)target);
	return 0;
}

static int Opcode_LoadAbsCompact(Thread_t* t) // 18: → v
{
	uint32_t ptr = RdU32(t);
	Thread_Push(t, LoadCompact(ResolvePtr(ptr & 0x3fffffffu, t), ptr >> 30));
	return 0;
}

static int Opcode_LoadLocalCompact(Thread_t* t) // 19: → v
{
	Thread_Push(t, LocalLoad(t, RdU16(t)));
	return 0;
}

static int Opcode_LoadLocalAddCompact(Thread_t* t) // 1A: → v
{
	uint32_t local = RdU16(t);
	int32_t add = RdSleb(t);
	Thread_Push(t, LocalLoad(t, local) + (uint32_t)add);
	return 0;
}

static int Opcode_LoadLocalIndexCompact(Thread_t* t) // 1B: add → v
{
	uint32_t local = RdU16(t);
	int32_t mul = RdSleb(t);
	uint32_t add = Thread_Pop(t);
	Thread_Push(t, LocalLoad(t, local) * (uint32_t)mul + add);
	return 0;
}

static int Opcode_LoadLocalShiftCompact(Thread_t* t) // 1C: → v
{
	uint32_t local = RdU16(t);
	int shift = (int8_t)RdU8(t);
	int32_t v = (int32_t)LocalLoad(t, local);
	Thread_Push(t, shift < 0 ? (uint32_t)(v >> ((-shift) & 31)) : (uint32_t)v << (shift & 31));
	return 0;
}

static int Opcode_MulLocalCompact(Thread_t* t) // 1D: v → result
{
	uint32_t local = RdU16(t);
	Thread_Push(t, LocalLoad(t, local) * Thread_Pop(t));
	return 0;
}

static int Opcode_DivLocalCompact(Thread_t* t) // 1E: dividend → result
{
	uint32_t local = RdU16(t);
	int32_t divisor = (int32_t)LocalLoad(t, local);
	int32_t dividend = (int32_t)Thread_Pop(t);
	Thread_Push(t, divisor ? (uint32_t)(dividend / divisor) : 0x80000000u);
	return 0;
}

static int Opcode_LoadRelativeCompact(Thread_t* t) // 1F: ptr → v
{
	uint32_t size;
	int32_t off = RdTyped(t, &size);
	uint32_t ptr = Thread_Pop(t);
	Thread_Push(t, LoadCompact(ResolvePtr(ptr + (uint32_t)off, t), size));
	return 0;
}

static int Opcode_AddImmCompact(Thread_t* t) // 2C: v → result
{
	int32_t imm = RdSleb(t);
	Thread_Push(t, Thread_Pop(t) + (uint32_t)imm);
	return 0;
}

static int Opcode_MulImmCompact(Thread_t* t) // 2D: v → result
{
	int32_t imm = RdSleb(t);
	Thread_Push(t, Thread_Pop(t) * (uint32_t)imm);
	return 0;
}

static int Opcode_MulAddImmCompact(Thread_t* t) // 2E: a, b → a + b * imm
{
	int32_t imm = RdSleb(t);
	uint32_t b = Thread_Pop(t), a = Thread_Pop(t);
	Thread_Push(t, a + b * (uint32_t)imm);
	return 0;
}

static int Opcode_DivImmCompact(Thread_t* t) // 2F: dividend → result
{
	int32_t divisor = RdSleb(t);
	int32_t dividend = (int32_t)Thread_Pop(t);
	Thread_Push(t, divisor ? (uint32_t)(dividend / divisor) : 0x80000000u);
	return 0;
}

static int Opcode_JumpCompareCompact(Thread_t* t) // 3B: a, b →
{
	uint32_t control = RdU8(t);
	int32_t rel = (int16_t)RdU16(t);
	uint32_t b = Thread_Pop(t), a = Thread_Pop(t);
	uint32_t take = EvalCompare(control & 0x0f, (int32_t)a, (int32_t)b);
	if(!(control & 0x80))
		take = !take;
	if(take)
		JumpRelative(t, (int32_t)Thread_CurOpIP(t) + rel);
	return 0;
}

static int Opcode_ShiftImmCompact(Thread_t* t) // 3C: v → result
{
	int shift = (int8_t)RdU8(t);
	int32_t v = (int32_t)Thread_Pop(t);
	Thread_Push(t, shift < 0 ? (uint32_t)(v >> ((-shift) & 31)) : (uint32_t)v << (shift & 31));
	return 0;
}

static int Opcode_PopCompact(Thread_t* t) // 73: v →
{
	Thread_Pop(t);
	return 0;
}

// ---- 1.685 compact instructions -----------------------------------------

static int Opcode_PushCompact(Thread_t* t) // 03: → values…
{
	uint32_t control = RdU8(t);
	if(control < 0x80)
	{
		uint32_t i;
		for(i = 0; i <= control; i++)
			Thread_Push(t, (uint32_t)RdSleb(t));
	}
	else if((control & 0x7c) == 0)
	{
		switch(control & 3)
		{
			case 0: Thread_Push(t, (uint32_t)(int32_t)(int8_t)RdU8(t)); break;
			case 1: Thread_Push(t, (uint32_t)(int32_t)(int16_t)RdU16(t)); break;
			case 2: Thread_Push(t, RdU32(t)); break;
			default: Thread_Push(t, (uint32_t)RdU64(t)); break;
		}
	}
	return 0;
}

static int Opcode_StoreTypedCompact(Thread_t* t) // 0D: ptr →
{
	uint32_t size;
	uint32_t value = (uint32_t)RdTyped(t, &size);
	StoreCompact(PopPtr(t), size, value);
	return 0;
}

// the typed immediate carries the access size, so the local is a full 16-bit offset below fp, not a packed reference
static int Opcode_StoreLocalTypedCompact(Thread_t* t) // 0E: →
{
	uint32_t local = RdU16(t), size;
	uint32_t value = (uint32_t)RdTyped(t, &size);
	StoreCompact(LocalFull(t, local), size, value);
	return 0;
}

static int Opcode_JumpRelativeCompact(Thread_t* t) // 13: →
{
	Thread_SetIP(t, (uint32_t)((int32_t)Thread_CurOpIP(t) + (int16_t)RdU16(t)));
	return 0;
}

static int Opcode_CompareImmCompact(Thread_t* t) // 36: v → result
{
	uint32_t control = RdU8(t);
	int32_t imm = RdSleb(t);
	Thread_Push(t, EvalCompare(control, (int32_t)Thread_Pop(t), imm));
	return 0;
}

static int Opcode_JumpCompareImmCompact(Thread_t* t) // 37: v →
{
	uint32_t control = RdU8(t);
	int32_t rel = (int16_t)RdU16(t);
	int32_t imm = RdSleb(t);
	uint32_t take = EvalCompare(control & 0x0f, (int32_t)Thread_Pop(t), imm);
	if(!(control & 0x80))
		take = !take;
	if(take)
		JumpRelative(t, (int32_t)Thread_CurOpIP(t) + rel);
	return 0;
}

static int Opcode_OperationCompact(Thread_t* t) // 3E: a, b, ptr →
{
	uint32_t control = RdU8(t);
	int32_t b = (int32_t)Thread_Pop(t), a = (int32_t)Thread_Pop(t);
	void* dest = PopPtr(t);
	uint32_t op = control & 0x1f;
	uint32_t value = control & 0x20 ? EvalCompare(op, a, b) : EvalArithmetic(op, a, b);
	StoreCompact(dest, control >> 6, value);
	return 0;
}

static int Opcode_OperationImmCompact(Thread_t* t) // 3F: v, ptr →
{
	uint32_t control = RdU8(t);
	int32_t imm = RdSleb(t);
	int32_t a = (int32_t)Thread_Pop(t);
	void* dest = PopPtr(t);
	uint32_t op = control & 0x1f;
	uint32_t value = control & 0x20 ? EvalCompare(op, a, imm) : EvalArithmetic(op, a, imm);
	StoreCompact(dest, control >> 6, value);
	return 0;
}

static int Opcode_StoreMultiLocalCompact(Thread_t* t) // E2: values… →
{
	uint32_t count = RdU8(t) + 1, i;
	for(i = 0; i < count; i++)
	{
		uint32_t local = RdU16(t);
		LocalStore(t, local, Thread_Pop(t));
	}
	return 0;
}

static int Opcode_LoadMultiLocalCompact(Thread_t* t) // E3: → values…
{
	uint32_t count = RdU8(t) + 1, i;
	for(i = 0; i < count; i++)
		Thread_Push(t, LocalLoad(t, RdU16(t)));
	return 0;
}

static int Opcode_LoadLocalOffsetCompact(Thread_t* t) // E4: → v
{
	uint32_t local = RdU16(t), size;
	int32_t off = RdTyped(t, &size);
	uint32_t ptr = LocalLoad(t, local) + (uint32_t)off;
	Thread_Push(t, LoadCompact(ResolvePtr(ptr, t), size));
	return 0;
}

static int Opcode_LoadLocalIndexOffsetCompact(Thread_t* t) // E5: off → v
{
	uint32_t local = RdU16(t), size;
	int32_t scale = RdTyped(t, &size);
	uint32_t off = Thread_Pop(t);
	uint32_t ptr = LocalLoad(t, local) * (uint32_t)scale + off;
	Thread_Push(t, LoadCompact(ResolvePtr(ptr, t), size));
	return 0;
}

static int Opcode_StoreLocalOffsetCompact(Thread_t* t) // E6: ptr →
{
	uint32_t local = RdU16(t), size;
	int32_t off = RdTyped(t, &size);
	void* dest = PopPtr(t);
	StoreCompact(dest, size, LocalLoad(t, local) + (uint32_t)off);
	return 0;
}

static int Opcode_StoreLocalIndexOffsetCompact(Thread_t* t) // E7: off, ptr →
{
	uint32_t local = RdU16(t), size;
	int32_t scale = RdTyped(t, &size);
	uint32_t off = Thread_Pop(t);
	void* dest = PopPtr(t);
	StoreCompact(dest, size, LocalLoad(t, local) * (uint32_t)scale + off);
	return 0;
}

static int Opcode_SelectLocalCompact(Thread_t* t) // E8: c, tv, f →
{
	uint32_t local = RdU16(t);
	uint32_t f = Thread_Pop(t), tv = Thread_Pop(t), c = Thread_Pop(t);
	LocalStore(t, local, c ? tv : f);
	return 0;
}

static int Opcode_StoreIndirectLocalCompact(Thread_t* t) // E9: v →
{
	uint32_t local = RdU16(t), off = RdU16(t);
	uint32_t value = Thread_Pop(t);
	StoreCompact(ResolvePtr(LocalPointer(t, local) + off, t), local >> 14, value);
	return 0;
}

static int Opcode_StoreIndirectLocalImmCompact(Thread_t* t) // EA: →
{
	uint32_t local = RdU16(t), off = RdU16(t);
	int32_t value = RdSleb(t);
	StoreCompact(ResolvePtr(LocalPointer(t, local) + off, t), local >> 14, (uint32_t)value);
	return 0;
}

static int Opcode_CopyCompact(Thread_t* t) // EC: dst, src →
{
	uint32_t size = (uint32_t)RdSleb(t);
	void* src = PopPtr(t);
	void* dst = PopPtr(t);
	memcpy(dst, src, size);
	return 0;
}

static int Opcode_CopyLocalCompact(Thread_t* t) // ED: dst →
{
	uint32_t local = RdU16(t);
	uint32_t size = (uint32_t)RdSleb(t);
	void* dst = PopPtr(t);
	memcpy(dst, LocalFull(t, local), size);
	return 0;
}

static int Opcode_CallRelativeCompact(Thread_t* t) // EE: →
{
	int32_t target = (int32_t)Thread_CurOpIP(t) + (int16_t)RdU16(t);
	return CallRelative(t, target, Thread_CurOpIP(t) + 3);
}

static int Opcode_CallPointerCompact(Thread_t* t) // EF (1.685 on): →
{
	uint32_t ptr = RdU32(t);
	uint32_t target = LoadCompact(ResolvePtr(ptr & 0x3fffffffu, t), ptr >> 30);
	return CallRelative(t, (int32_t)target, Thread_CurOpIP(t) + 5);
}

static int Opcode_StoreAbsLocalCompact(Thread_t* t) // F0: →
{
	uint32_t dest = RdU32(t), local = RdU16(t);
	int32_t add = RdSleb(t);
	StoreCompact(ResolvePtr(dest, t), local >> 14, LocalLoad(t, local) + (uint32_t)add);
	return 0;
}

static int Opcode_StoreLocalLocalCompact(Thread_t* t) // F1: →
{
	uint32_t dest = RdU16(t), src = RdU16(t);
	int32_t add = RdSleb(t);
	StoreCompact(LocalFull(t, dest), src >> 14, LocalLoad(t, src) + (uint32_t)add);
	return 0;
}

static int Opcode_StoreIndirectLocalLocalCompact(Thread_t* t) // F2: →
{
	uint32_t dest = RdU16(t), off = RdU16(t), src = RdU16(t);
	int32_t add = RdSleb(t);
	uint32_t base;
	memcpy(&base, LocalFull(t, dest), 4);
	StoreCompact(ResolvePtr(base + off, t), src >> 14, LocalLoad(t, src) + (uint32_t)add);
	return 0;
}

static int Opcode_StoreLocalMulAddCompact(Thread_t* t) // F4: →
{
	uint32_t dest = RdU16(t), add = RdU32(t), src = RdU16(t);
	int32_t mul = (int16_t)RdU16(t);
	StoreCompact(LocalFull(t, dest), src >> 14, LocalLoad(t, src) * (uint32_t)mul + add);
	return 0;
}

static int Opcode_StoreLocalArrayCompact(Thread_t* t) // F5: →
{
	uint32_t dest = RdU16(t), baseLocal = RdU16(t);
	int32_t off = (int16_t)RdU16(t);
	uint32_t src = RdU16(t);
	int32_t mul = (int16_t)RdU16(t), base;
	memcpy(&base, LocalFull(t, baseLocal), 4);
	StoreCompact(LocalFull(t, dest), src >> 14, LocalLoad(t, src) * (uint32_t)mul + (uint32_t)off + (uint32_t)base);
	return 0;
}

static int Opcode_LoadIndirectLocalMulAddCompact(Thread_t* t) // F7: add → v
{
	uint32_t local = RdU16(t), off = RdU16(t);
	int32_t mul = (int16_t)RdU16(t);
	uint32_t add = Thread_Pop(t), base = LocalPointer(t, local);
	Thread_Push(t, LoadCompact(ResolvePtr(base + off, t), local >> 14) * (uint32_t)mul + add);
	return 0;
}

static int Opcode_LoadLocalMulAddCompact(Thread_t* t) // F8: → v
{
	uint32_t add = RdU32(t), local = RdU16(t);
	int32_t mul = (int16_t)RdU16(t), value;
	memcpy(&value, LocalFull(t, local), 4);
	Thread_Push(t, (uint32_t)value * (uint32_t)mul + add);
	return 0;
}

static int Opcode_LoadLocalArrayCompact(Thread_t* t) // F9: → v
{
	uint32_t baseLocal = RdU16(t);
	int32_t off = (int16_t)RdU16(t);
	uint32_t indexLocal = RdU16(t);
	int32_t mul = (int16_t)RdU16(t), base, index;
	memcpy(&base, LocalFull(t, baseLocal), 4);
	memcpy(&index, LocalFull(t, indexLocal), 4);
	Thread_Push(t, (uint32_t)base + (uint32_t)off + (uint32_t)index * (uint32_t)mul);
	return 0;
}

static int Opcode_LoadIndirectLocalMulCompact(Thread_t* t) // FA: → v
{
	uint32_t add = RdU32(t), local = RdU16(t);
	int32_t mul = (int16_t)RdU16(t);
	uint32_t ptr = LocalPointer(t, local) * (uint32_t)mul + add; // the index is a dword; the size bits size the value read
	Thread_Push(t, LoadCompact(ResolvePtr(ptr, t), local >> 14));
	return 0;
}

static int Opcode_LoadIndirectLocalAddressCompact(Thread_t* t) // FB: → v
{
	uint32_t dest = RdU16(t), src = RdU16(t);
	int32_t mul = (int16_t)RdU16(t);
	uint32_t ptr = LocalPointer(t, src) * (uint32_t)mul + ((Thread_GetFP(t) - dest) | VM_TAG_DATA);
	Thread_Push(t, LoadCompact(ResolvePtr(ptr, t), src >> 14));
	return 0;
}

/* Register the compact instructions after the ordinary base table has been
 * filled.  EF deliberately remains OpenBGI's extension dispatcher before
 * 1.685; the original 1.685 executable uses it for a call through a pointer. */
void Vm_OptableCompactInit(void)
{
	if(gEngine->gen < GEN_1_667)
		return;
	vm_optable_main[0x0f] = Opcode_StoreLocalCompact;
	vm_optable_main[0x12] = Opcode_AddFpCompact;
	vm_optable_main[0x15] = Opcode_JccCompact;
	vm_optable_main[0x18] = Opcode_LoadAbsCompact;
	vm_optable_main[0x19] = Opcode_LoadLocalCompact;
	vm_optable_main[0x1a] = Opcode_LoadLocalAddCompact;
	vm_optable_main[0x1b] = Opcode_LoadLocalIndexCompact;
	vm_optable_main[0x1c] = Opcode_LoadLocalShiftCompact;
	vm_optable_main[0x1d] = Opcode_MulLocalCompact;
	vm_optable_main[0x1e] = Opcode_DivLocalCompact;
	vm_optable_main[0x1f] = Opcode_LoadRelativeCompact;
	vm_optable_main[0x2c] = Opcode_AddImmCompact;
	vm_optable_main[0x2d] = Opcode_MulImmCompact;
	vm_optable_main[0x2e] = Opcode_MulAddImmCompact;
	vm_optable_main[0x2f] = Opcode_DivImmCompact;
	vm_optable_main[0x3b] = Opcode_JumpCompareCompact;
	vm_optable_main[0x3c] = Opcode_ShiftImmCompact;
	vm_optable_main[0x73] = Opcode_PopCompact;
	if(gEngine->gen < GEN_1_685)
		return;
	vm_optable_main[0x03] = Opcode_PushCompact;
	vm_optable_main[0x0d] = Opcode_StoreTypedCompact;
	vm_optable_main[0x0e] = Opcode_StoreLocalTypedCompact;
	vm_optable_main[0x13] = Opcode_JumpRelativeCompact;
	vm_optable_main[0x36] = Opcode_CompareImmCompact;
	vm_optable_main[0x37] = Opcode_JumpCompareImmCompact;
	vm_optable_main[0x3e] = Opcode_OperationCompact;
	vm_optable_main[0x3f] = Opcode_OperationImmCompact;
	vm_optable_main[0xe2] = Opcode_StoreMultiLocalCompact;
	vm_optable_main[0xe3] = Opcode_LoadMultiLocalCompact;
	vm_optable_main[0xe4] = Opcode_LoadLocalOffsetCompact;
	vm_optable_main[0xe5] = Opcode_LoadLocalIndexOffsetCompact;
	vm_optable_main[0xe6] = Opcode_StoreLocalOffsetCompact;
	vm_optable_main[0xe7] = Opcode_StoreLocalIndexOffsetCompact;
	vm_optable_main[0xe8] = Opcode_SelectLocalCompact;
	vm_optable_main[0xe9] = Opcode_StoreIndirectLocalCompact;
	vm_optable_main[0xea] = Opcode_StoreIndirectLocalImmCompact;
	vm_optable_main[0xec] = Opcode_CopyCompact;
	vm_optable_main[0xed] = Opcode_CopyLocalCompact;
	vm_optable_main[0xee] = Opcode_CallRelativeCompact;
	vm_optable_main[0xef] = Opcode_CallPointerCompact;
	vm_optable_main[0xf0] = Opcode_StoreAbsLocalCompact;
	vm_optable_main[0xf1] = Opcode_StoreLocalLocalCompact;
	vm_optable_main[0xf2] = Opcode_StoreIndirectLocalLocalCompact;
	vm_optable_main[0xf4] = Opcode_StoreLocalMulAddCompact;
	vm_optable_main[0xf5] = Opcode_StoreLocalArrayCompact;
	vm_optable_main[0xf7] = Opcode_LoadIndirectLocalMulAddCompact;
	vm_optable_main[0xf8] = Opcode_LoadLocalMulAddCompact;
	vm_optable_main[0xf9] = Opcode_LoadLocalArrayCompact;
	vm_optable_main[0xfa] = Opcode_LoadIndirectLocalMulCompact;
	vm_optable_main[0xfb] = Opcode_LoadIndirectLocalAddressCompact;
}
