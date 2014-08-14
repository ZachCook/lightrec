/*
 * Copyright (C) 2014 Paul Cercueil <paul@crapouillou.net>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 */

#ifndef __REGCACHE_H__
#define __REGCACHE_H__

#include "lightrec.h"

#include <stdbool.h>

struct register_value {
	bool known;
	u32 value;
};

extern struct register_value lightrec_rvals[];

u8 lightrec_alloc_reg_temp(jit_state_t *_jit);
u8 lightrec_alloc_reg_temp_with_value(jit_state_t *_jit, u32 value);
u8 lightrec_alloc_reg_out(jit_state_t *_jit, u8 reg);
u8 lightrec_alloc_reg_in(jit_state_t *_jit, u8 reg);

void lightrec_free_regs(void);

/* Store back caller-saved registers */
void lightrec_storeback_regs(jit_state_t *_jit);

/* Store back all registers */
void lightrec_storeback_all_regs(jit_state_t *_jit);

#endif /* __REGCACHE_H__ */
