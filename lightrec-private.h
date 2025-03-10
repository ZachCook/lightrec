/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Copyright (C) 2016-2021 Paul Cercueil <paul@crapouillou.net>
 */

#ifndef __LIGHTREC_PRIVATE_H__
#define __LIGHTREC_PRIVATE_H__

#include "lightning-wrapper.h"
#include "lightrec-config.h"
#include "disassembler.h"
#include "lightrec.h"
#include "regcache.h"

#if ENABLE_THREADED_COMPILER
#include <stdatomic.h>
#endif

#define ARRAY_SIZE(x) (sizeof(x) ? sizeof(x) / sizeof((x)[0]) : 0)

#ifdef __GNUC__
#	define likely(x)       __builtin_expect(!!(x),1)
#	define unlikely(x)     __builtin_expect(!!(x),0)
#else
#	define likely(x)       (x)
#	define unlikely(x)     (x)
#endif

#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#	define LE32TOH(x)	__builtin_bswap32(x)
#	define HTOLE32(x)	__builtin_bswap32(x)
#	define LE16TOH(x)	__builtin_bswap16(x)
#	define HTOLE16(x)	__builtin_bswap16(x)
#else
#	define LE32TOH(x)	(x)
#	define HTOLE32(x)	(x)
#	define LE16TOH(x)	(x)
#	define HTOLE16(x)	(x)
#endif

#if HAS_DEFAULT_ELM
#define SET_DEFAULT_ELM(table, value) [0 ... ARRAY_SIZE(table) - 1] = value
#else
#define SET_DEFAULT_ELM(table, value) [0] = NULL
#endif

#define fallthrough do {} while (0) /* fall-through */

#define container_of(ptr, type, member) \
	((type *)((void *)(ptr) - offsetof(type, member)))

#ifdef _MSC_BUILD
#	define popcount32(x)	__popcnt(x)
#	define ffs32(x)		(31 - __lzcnt(x))
#else
#	define popcount32(x)	__builtin_popcount(x)
#	define ffs32(x)		(__builtin_ffs(x) - 1)
#endif

/* Flags for (struct block *)->flags */
#define BLOCK_NEVER_COMPILE	BIT(0)
#define BLOCK_SHOULD_RECOMPILE	BIT(1)
#define BLOCK_FULLY_TAGGED	BIT(2)
#define BLOCK_IS_DEAD		BIT(3)
#define BLOCK_IS_MEMSET		BIT(4)
#define BLOCK_NO_OPCODE_LIST	BIT(5)

#define RAM_SIZE	0x200000
#define BIOS_SIZE	0x80000

#define CODE_LUT_SIZE	((RAM_SIZE + BIOS_SIZE) >> 2)

#define REG_LO 32
#define REG_HI 33

/* Definition of jit_state_t (avoids inclusion of <lightning.h>) */
struct jit_node;
struct jit_state;
typedef struct jit_state jit_state_t;

struct blockcache;
struct recompiler;
struct regcache;
struct opcode;
struct reaper;

struct u16x2 {
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
	u16 h, l;
#else
	u16 l, h;
#endif
};

struct block {
	jit_state_t *_jit;
	struct opcode *opcode_list;
	void (*function)(void);
	const u32 *code;
	struct block *next;
	u32 pc;
	u32 hash;
	u32 precompile_date;
	unsigned int code_size;
	u16 nb_ops;
#if ENABLE_THREADED_COMPILER
	_Atomic u8 flags;
#else
	u8 flags;
#endif
};

struct lightrec_branch {
	struct jit_node *branch;
	u32 target;
};

struct lightrec_branch_target {
	struct jit_node *label;
	u32 offset;
};

enum c_wrappers {
	C_WRAPPER_RW,
	C_WRAPPER_RW_GENERIC,
	C_WRAPPER_MFC,
	C_WRAPPER_MTC,
	C_WRAPPER_CP,
	C_WRAPPERS_COUNT,
};

struct lightrec_cstate {
	struct lightrec_state *state;

	struct lightrec_branch local_branches[512];
	struct lightrec_branch_target targets[512];
	unsigned int nb_local_branches;
	unsigned int nb_targets;
	unsigned int cycles;

	struct regcache *reg_cache;
};

struct lightrec_state {
	struct lightrec_registers regs;
	uintptr_t wrapper_regs[NUM_TEMPS];
	u32 next_pc;
	u32 current_cycle;
	u32 target_cycle;
	u32 exit_flags;
	u32 old_cycle_counter;
	struct block *dispatcher, *c_wrapper_block;
	void *c_wrappers[C_WRAPPERS_COUNT];
	void *wrappers_eps[C_WRAPPERS_COUNT];
	struct blockcache *block_cache;
	struct recompiler *rec;
	struct lightrec_cstate *cstate;
	struct reaper *reaper;
	void *tlsf;
	void (*eob_wrapper_func)(void);
	void (*memset_func)(void);
	void (*get_next_block)(void);
	struct lightrec_ops ops;
	unsigned int nb_precompile;
	unsigned int nb_maps;
	const struct lightrec_mem_map *maps;
	uintptr_t offset_ram, offset_bios, offset_scratch, offset_io;
	_Bool with_32bit_lut;
	_Bool mirrors_mapped;
	_Bool invalidate_from_dma_only;
	void *code_lut[];
};

u32 lightrec_rw(struct lightrec_state *state, union code op,
		u32 addr, u32 data, u32 *flags,
		struct block *block);

void lightrec_free_block(struct lightrec_state *state, struct block *block);

void remove_from_code_lut(struct blockcache *cache, struct block *block);

enum psx_map
lightrec_get_map_idx(struct lightrec_state *state, u32 kaddr);

const struct lightrec_mem_map *
lightrec_get_map(struct lightrec_state *state, void **host, u32 kaddr);

static inline u32 kunseg(u32 addr)
{
	if (unlikely(addr >= 0xa0000000))
		return addr - 0xa0000000;
	else
		return addr &~ 0x80000000;
}

static inline u32 lut_offset(u32 pc)
{
	if (pc & BIT(28))
		return ((pc & (BIOS_SIZE - 1)) + RAM_SIZE) >> 2; // BIOS
	else
		return (pc & (RAM_SIZE - 1)) >> 2; // RAM
}

static inline _Bool is_big_endian(void)
{
	return __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__;
}

static inline _Bool lut_is_32bit(const struct lightrec_state *state)
{
	return __WORDSIZE == 32 ||
		(ENABLE_CODE_BUFFER && state->with_32bit_lut);
}

static inline size_t lut_elm_size(const struct lightrec_state *state)
{
	return lut_is_32bit(state) ? 4 : sizeof(void *);
}

static inline void ** lut_address(struct lightrec_state *state, u32 offset)
{
	if (lut_is_32bit(state))
		return (void **) ((uintptr_t) state->code_lut + offset * 4);
	else
		return &state->code_lut[offset];
}

static inline void * lut_read(struct lightrec_state *state, u32 offset)
{
	void **lut_entry = lut_address(state, offset);

	if (lut_is_32bit(state))
		return (void *)(uintptr_t) *(u32 *) lut_entry;
	else
		return *lut_entry;
}

static inline void lut_write(struct lightrec_state *state, u32 offset, void *ptr)
{
	void **lut_entry = lut_address(state, offset);

	if (lut_is_32bit(state))
		*(u32 *) lut_entry = (u32)(uintptr_t) ptr;
	else
		*lut_entry = ptr;
}

static inline u32 get_ds_pc(const struct block *block, u16 offset, s16 imm)
{
	u16 flags = block->opcode_list[offset].flags;

	offset += op_flag_no_ds(flags);

	return block->pc + (offset + imm << 2);
}

static inline u32 get_branch_pc(const struct block *block, u16 offset, s16 imm)
{
	u16 flags = block->opcode_list[offset].flags;

	offset -= op_flag_no_ds(flags);

	return block->pc + (offset + imm << 2);
}

void lightrec_mtc(struct lightrec_state *state, union code op, u32 data);
u32 lightrec_mfc(struct lightrec_state *state, union code op);
void lightrec_rfe(struct lightrec_state *state);
void lightrec_cp(struct lightrec_state *state, union code op);

struct lightrec_cstate * lightrec_create_cstate(struct lightrec_state *state);
void lightrec_free_cstate(struct lightrec_cstate *cstate);

union code lightrec_read_opcode(struct lightrec_state *state, u32 pc);

int lightrec_compile_block(struct lightrec_cstate *cstate, struct block *block);
void lightrec_free_opcode_list(struct lightrec_state *state,
			       struct opcode *list);

unsigned int lightrec_cycles_of_opcode(union code code);

static inline u8 get_mult_div_lo(union code c)
{
	return (OPT_FLAG_MULT_DIV && c.r.rd) ? c.r.rd : REG_LO;
}

static inline u8 get_mult_div_hi(union code c)
{
	return (OPT_FLAG_MULT_DIV && c.r.imm) ? c.r.imm : REG_HI;
}

static inline s16 s16_max(s16 a, s16 b)
{
	return a > b ? a : b;
}

static inline _Bool block_has_flag(struct block *block, u8 flag)
{
#if ENABLE_THREADED_COMPILER
	return atomic_load_explicit(&block->flags, memory_order_relaxed) & flag;
#else
	return block->flags & flag;
#endif
}

static inline u8 block_set_flags(struct block *block, u8 mask)
{
#if ENABLE_THREADED_COMPILER
	return atomic_fetch_or_explicit(&block->flags, mask,
					memory_order_relaxed);
#else
	u8 flags = block->flags;

	block->flags |= mask;

	return flags;
#endif
}

static inline u8 block_clear_flags(struct block *block, u8 mask)
{
#if ENABLE_THREADED_COMPILER
	return atomic_fetch_and_explicit(&block->flags, ~mask,
					 memory_order_relaxed);
#else
	u8 flags = block->flags;

	block->flags &= ~mask;

	return flags;
#endif
}

#endif /* __LIGHTREC_PRIVATE_H__ */
