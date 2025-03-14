/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Copyright (C) 2016-2021 Paul Cercueil <paul@crapouillou.net>
 */

#ifndef __LIGHTREC_H__
#define __LIGHTREC_H__

#ifdef __cplusplus
#define _Bool bool
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#   ifdef lightrec_EXPORTS
#	define __api __declspec(dllexport)
#   elif !defined(LIGHTREC_STATIC)
#	define __api __declspec(dllimport)
#   else
#	define __api
#   endif
#elif __GNUC__ >= 4
#   define __api __attribute__((visibility ("default")))
#else
#   define __api
#endif

typedef uint64_t u64;
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t  u8;

typedef int64_t s64;
typedef int32_t s32;
typedef int16_t s16;
typedef int8_t  s8;

struct lightrec_state;
struct lightrec_mem_map;

/* Exit flags */
#define LIGHTREC_EXIT_NORMAL	(0)
#define LIGHTREC_EXIT_CHECK_INTERRUPT	(1 << 0)
#define LIGHTREC_EXIT_BREAK	(1 << 1)
#define LIGHTREC_EXIT_SYSCALL	(1 << 2)
#define LIGHTREC_EXIT_SEGFAULT	(1 << 3)
#define LIGHTREC_EXIT_NOMEM	(1 << 4)

enum psx_map {
	PSX_MAP_KERNEL_USER_RAM,
	PSX_MAP_BIOS,
	PSX_MAP_SCRATCH_PAD,
	PSX_MAP_PARALLEL_PORT,
	PSX_MAP_HW_REGISTERS,
	PSX_MAP_CACHE_CONTROL,
	PSX_MAP_MIRROR1,
	PSX_MAP_MIRROR2,
	PSX_MAP_MIRROR3,
	PSX_MAP_CODE_BUFFER,

	PSX_MAP_UNKNOWN,
};

struct lightrec_mem_map_ops {
	void (*sb)(struct lightrec_state *, u32 opcode,
		   void *host, u32 addr, u8 data);
	void (*sh)(struct lightrec_state *, u32 opcode,
		   void *host, u32 addr, u16 data);
	void (*sw)(struct lightrec_state *, u32 opcode,
		   void *host, u32 addr, u32 data);
	u8 (*lb)(struct lightrec_state *, u32 opcode, void *host, u32 addr);
	u16 (*lh)(struct lightrec_state *, u32 opcode, void *host, u32 addr);
	u32 (*lw)(struct lightrec_state *, u32 opcode, void *host, u32 addr);
};

struct lightrec_mem_map {
	u32 pc;
	u32 length;
	void *address;
	const struct lightrec_mem_map_ops *ops;
	const struct lightrec_mem_map *mirror_of;
};

struct lightrec_ops {
	void (*cop2_notify)(struct lightrec_state *state, u32 op, u32 data);
	void (*cop2_op)(struct lightrec_state *state, u32 op);
	void (*enable_ram)(struct lightrec_state *state, _Bool enable);
	_Bool (*hw_direct)(u32 kaddr, _Bool is_write, u8 size);
};

struct lightrec_registers {
	u32 gpr[34];
	u32 cp0[32];
	u32 cp2d[32];
	u32 cp2c[32];
};

__api struct lightrec_state *lightrec_init(char *argv0,
					   const struct lightrec_mem_map *map,
					   size_t nb,
					   const struct lightrec_ops *ops);

__api void lightrec_destroy(struct lightrec_state *state);

__api u32 lightrec_execute(struct lightrec_state *state,
			   u32 pc, u32 target_cycle);
__api u32 lightrec_run_interpreter(struct lightrec_state *state,
				   u32 pc, u32 target_cycle);

__api void lightrec_invalidate(struct lightrec_state *state, u32 addr, u32 len);
__api void lightrec_invalidate_all(struct lightrec_state *state);
__api void lightrec_set_invalidate_mode(struct lightrec_state *state,
					_Bool dma_only);

__api void lightrec_set_exit_flags(struct lightrec_state *state, u32 flags);
__api u32 lightrec_exit_flags(struct lightrec_state *state);

__api struct lightrec_registers * lightrec_get_registers(struct lightrec_state *state);

__api u32 lightrec_current_cycle_count(const struct lightrec_state *state);
__api void lightrec_reset_cycle_count(struct lightrec_state *state, u32 cycles);
__api void lightrec_set_target_cycle_count(struct lightrec_state *state,
					   u32 cycles);

#ifdef __cplusplus
};
#endif

#endif /* __LIGHTREC_H__ */
