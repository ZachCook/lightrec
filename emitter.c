// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * Copyright (C) 2014-2021 Paul Cercueil <paul@crapouillou.net>
 */

#include "blockcache.h"
#include "debug.h"
#include "disassembler.h"
#include "emitter.h"
#include "lightning-wrapper.h"
#include "optimizer.h"
#include "regcache.h"

#include <stdbool.h>
#include <stddef.h>

typedef void (*lightrec_rec_func_t)(struct lightrec_cstate *, const struct block *, u16);

/* Forward declarations */
static void rec_SPECIAL(struct lightrec_cstate *state, const struct block *block, u16 offset);
static void rec_REGIMM(struct lightrec_cstate *state, const struct block *block, u16 offset);
static void rec_CP0(struct lightrec_cstate *state, const struct block *block, u16 offset);
static void rec_CP2(struct lightrec_cstate *state, const struct block *block, u16 offset);

static void unknown_opcode(struct lightrec_cstate *state, const struct block *block, u16 offset)
{
	pr_warn("Unknown opcode: 0x%08x at PC 0x%08x\n",
		block->opcode_list[offset].c.opcode,
		block->pc + (offset << 2));
}

static void
lightrec_jump_to_eob(struct lightrec_cstate *state, jit_state_t *_jit)
{
	/* Prevent jit_jmpi() from using our cycles register as a temporary */
	jit_live(LIGHTREC_REG_CYCLE);

	jit_patch_abs(jit_jmpi(), state->state->eob_wrapper_func);
}

static void lightrec_emit_end_of_block(struct lightrec_cstate *state,
				       const struct block *block, u16 offset,
				       s8 reg_new_pc, u32 imm, u8 ra_reg,
				       u32 link, bool update_cycles)
{
	struct regcache *reg_cache = state->reg_cache;
	jit_state_t *_jit = block->_jit;
	const struct opcode *op = &block->opcode_list[offset],
			    *next = &block->opcode_list[offset + 1];
	u32 cycles = state->cycles + lightrec_cycles_of_opcode(op->c);

	jit_note(__FILE__, __LINE__);

	if (link) {
		/* Update the $ra register */
		u8 link_reg = lightrec_alloc_reg_out(reg_cache, _jit, ra_reg, 0);
		jit_movi(link_reg, link);
		lightrec_free_reg(reg_cache, link_reg);
	}

	if (reg_new_pc < 0) {
		reg_new_pc = lightrec_alloc_reg(reg_cache, _jit, JIT_V0);
		lightrec_lock_reg(reg_cache, _jit, reg_new_pc);

		jit_movi(reg_new_pc, imm);
	}

	if (has_delay_slot(op->c) &&
	    !op_flag_no_ds(op->flags) && !op_flag_local_branch(op->flags)) {
		cycles += lightrec_cycles_of_opcode(next->c);

		/* Recompile the delay slot */
		if (next->c.opcode)
			lightrec_rec_opcode(state, block, offset + 1);
	}

	/* Clean the remaining registers */
	lightrec_clean_regs(reg_cache, _jit);

	jit_movr(JIT_V0, reg_new_pc);

	if (cycles && update_cycles) {
		jit_subi(LIGHTREC_REG_CYCLE, LIGHTREC_REG_CYCLE, cycles);
		pr_debug("EOB: %u cycles\n", cycles);
	}

	lightrec_jump_to_eob(state, _jit);
}

void lightrec_emit_eob(struct lightrec_cstate *state, const struct block *block,
		       u16 offset, bool after_op)
{
	struct regcache *reg_cache = state->reg_cache;
	jit_state_t *_jit = block->_jit;
	union code c = block->opcode_list[offset].c;
	u32 cycles = state->cycles;

	if (after_op)
		cycles += lightrec_cycles_of_opcode(c);

	lightrec_clean_regs(reg_cache, _jit);

	jit_movi(JIT_V0, block->pc + (offset << 2));
	jit_subi(LIGHTREC_REG_CYCLE, LIGHTREC_REG_CYCLE, cycles);

	lightrec_jump_to_eob(state, _jit);
}

static u8 get_jr_jalr_reg(struct lightrec_cstate *state, const struct block *block, u16 offset)
{
	struct regcache *reg_cache = state->reg_cache;
	jit_state_t *_jit = block->_jit;
	const struct opcode *op = &block->opcode_list[offset];
	u8 rs;

	rs = lightrec_request_reg_in(reg_cache, _jit, op->r.rs, JIT_V0);
	lightrec_lock_reg(reg_cache, _jit, rs);

	return rs;
}

static void rec_special_JR(struct lightrec_cstate *state, const struct block *block, u16 offset)
{
	u8 rs = get_jr_jalr_reg(state, block, offset);

	_jit_name(block->_jit, __func__);
	lightrec_emit_end_of_block(state, block, offset, rs, 0, 31, 0, true);
}

static void rec_special_JALR(struct lightrec_cstate *state, const struct block *block, u16 offset)
{
	u8 rs = get_jr_jalr_reg(state, block, offset);
	union code c = block->opcode_list[offset].c;

	_jit_name(block->_jit, __func__);
	lightrec_emit_end_of_block(state, block, offset, rs, 0, c.r.rd,
				   get_branch_pc(block, offset, 2), true);
}

static void rec_J(struct lightrec_cstate *state, const struct block *block, u16 offset)
{
	union code c = block->opcode_list[offset].c;

	_jit_name(block->_jit, __func__);
	lightrec_emit_end_of_block(state, block, offset, -1,
				   (block->pc & 0xf0000000) | (c.j.imm << 2),
				   31, 0, true);
}

static void rec_JAL(struct lightrec_cstate *state, const struct block *block, u16 offset)
{
	union code c = block->opcode_list[offset].c;

	_jit_name(block->_jit, __func__);
	lightrec_emit_end_of_block(state, block, offset, -1,
				   (block->pc & 0xf0000000) | (c.j.imm << 2),
				   31, get_branch_pc(block, offset, 2), true);
}

static void lightrec_do_early_unload(struct lightrec_cstate *state,
				     const struct block *block, u16 offset)
{
	struct regcache *reg_cache = state->reg_cache;
	const struct opcode *op = &block->opcode_list[offset];
	jit_state_t *_jit = block->_jit;
	unsigned int i;
	u8 reg;
	struct {
		u8 reg, op;
	} reg_ops[3] = {
		{ op->r.rd, LIGHTREC_FLAGS_GET_RD(op->flags), },
		{ op->i.rt, LIGHTREC_FLAGS_GET_RT(op->flags), },
		{ op->i.rs, LIGHTREC_FLAGS_GET_RS(op->flags), },
	};

	for (i = 0; i < ARRAY_SIZE(reg_ops); i++) {
		reg = reg_ops[i].reg;

		switch (reg_ops[i].op) {
		case LIGHTREC_REG_UNLOAD:
			lightrec_clean_reg_if_loaded(reg_cache, _jit, reg, true);
			break;

		case LIGHTREC_REG_DISCARD:
			lightrec_discard_reg_if_loaded(reg_cache, reg);
			break;

		case LIGHTREC_REG_CLEAN:
			lightrec_clean_reg_if_loaded(reg_cache, _jit, reg, false);
			break;
		default:
			break;
		};
	}
}

static void rec_b(struct lightrec_cstate *state, const struct block *block, u16 offset,
		  jit_code_t code, jit_code_t code2, u32 link, bool unconditional, bool bz)
{
	struct regcache *reg_cache = state->reg_cache;
	struct native_register *regs_backup;
	jit_state_t *_jit = block->_jit;
	struct lightrec_branch *branch;
	const struct opcode *op = &block->opcode_list[offset],
			    *next = &block->opcode_list[offset + 1];
	jit_node_t *addr;
	u8 link_reg, rs, rt;
	bool is_forward = (s16)op->i.imm >= -1;
	int op_cycles = lightrec_cycles_of_opcode(op->c);
	u32 target_offset, cycles = state->cycles + op_cycles;
	bool no_indirection = false;
	u32 next_pc;

	jit_note(__FILE__, __LINE__);

	if (!op_flag_no_ds(op->flags))
		cycles += lightrec_cycles_of_opcode(next->c);

	state->cycles = -op_cycles;

	if (!unconditional) {
		rs = lightrec_alloc_reg_in(reg_cache, _jit, op->i.rs, REG_EXT);
		rt = bz ? 0 : lightrec_alloc_reg_in(reg_cache,
						    _jit, op->i.rt, REG_EXT);

		/* Unload dead registers before evaluating the branch */
		if (OPT_EARLY_UNLOAD)
			lightrec_do_early_unload(state, block, offset);

		if (op_flag_local_branch(op->flags) &&
		    (op_flag_no_ds(op->flags) || !next->opcode) &&
		    is_forward && !lightrec_has_dirty_regs(reg_cache))
			no_indirection = true;

		if (no_indirection)
			pr_debug("Using no indirection for branch at offset 0x%hx\n", offset << 2);
	}

	if (cycles)
		jit_subi(LIGHTREC_REG_CYCLE, LIGHTREC_REG_CYCLE, cycles);

	if (!unconditional) {
		/* Generate the branch opcode */
		if (!no_indirection)
			addr = jit_new_node_pww(code, NULL, rs, rt);

		lightrec_free_regs(reg_cache);
		regs_backup = lightrec_regcache_enter_branch(reg_cache);
	}

	if (op_flag_local_branch(op->flags)) {
		/* Recompile the delay slot */
		if (next && next->opcode && !op_flag_no_ds(op->flags))
			lightrec_rec_opcode(state, block, offset + 1);

		if (link) {
			/* Update the $ra register */
			link_reg = lightrec_alloc_reg_out(reg_cache, _jit, 31, 0);
			jit_movi(link_reg, link);
			lightrec_free_reg(reg_cache, link_reg);
		}

		/* Clean remaining registers */
		lightrec_clean_regs(reg_cache, _jit);

		target_offset = offset + 1 + (s16)op->i.imm
			- !!op_flag_no_ds(op->flags);
		pr_debug("Adding local branch to offset 0x%x\n",
			 target_offset << 2);
		branch = &state->local_branches[
			state->nb_local_branches++];

		branch->target = target_offset;

		if (no_indirection)
			branch->branch = jit_new_node_pww(code2, NULL, rs, rt);
		else if (is_forward)
			branch->branch = jit_b();
		else
			branch->branch = jit_bgti(LIGHTREC_REG_CYCLE, 0);
	}

	if (!op_flag_local_branch(op->flags) || !is_forward) {
		next_pc = get_branch_pc(block, offset, 1 + (s16)op->i.imm);
		lightrec_emit_end_of_block(state, block, offset, -1, next_pc,
					   31, link, false);
	}

	if (!unconditional) {
		if (!no_indirection)
			jit_patch(addr);

		lightrec_regcache_leave_branch(reg_cache, regs_backup);

		if (bz && link) {
			/* Update the $ra register */
			link_reg = lightrec_alloc_reg_out(reg_cache, _jit,
							  31, REG_EXT);
			jit_movi(link_reg, (s32)link);
			lightrec_free_reg(reg_cache, link_reg);
		}

		if (!op_flag_no_ds(op->flags) && next->opcode)
			lightrec_rec_opcode(state, block, offset + 1);
	}
}

static void rec_BNE(struct lightrec_cstate *state,
		    const struct block *block, u16 offset)
{
	union code c = block->opcode_list[offset].c;

	_jit_name(block->_jit, __func__);

	if (c.i.rt == 0)
		rec_b(state, block, offset, jit_code_beqi, jit_code_bnei, 0, false, true);
	else
		rec_b(state, block, offset, jit_code_beqr, jit_code_bner, 0, false, false);
}

static void rec_BEQ(struct lightrec_cstate *state,
		    const struct block *block, u16 offset)
{
	union code c = block->opcode_list[offset].c;

	_jit_name(block->_jit, __func__);

	if (c.i.rt == 0)
		rec_b(state, block, offset, jit_code_bnei, jit_code_beqi, 0, c.i.rs == 0, true);
	else
		rec_b(state, block, offset, jit_code_bner, jit_code_beqr, 0, c.i.rs == c.i.rt, false);
}

static void rec_BLEZ(struct lightrec_cstate *state,
		     const struct block *block, u16 offset)
{
	union code c = block->opcode_list[offset].c;

	_jit_name(block->_jit, __func__);
	rec_b(state, block, offset, jit_code_bgti, jit_code_blei, 0, c.i.rs == 0, true);
}

static void rec_BGTZ(struct lightrec_cstate *state,
		     const struct block *block, u16 offset)
{
	_jit_name(block->_jit, __func__);
	rec_b(state, block, offset, jit_code_blei, jit_code_bgti, 0, false, true);
}

static void rec_regimm_BLTZ(struct lightrec_cstate *state,
			    const struct block *block, u16 offset)
{
	_jit_name(block->_jit, __func__);
	rec_b(state, block, offset, jit_code_bgei, jit_code_blti, 0, false, true);
}

static void rec_regimm_BLTZAL(struct lightrec_cstate *state,
			      const struct block *block, u16 offset)
{
	_jit_name(block->_jit, __func__);
	rec_b(state, block, offset, jit_code_bgei, jit_code_blti,
	      get_branch_pc(block, offset, 2), false, true);
}

static void rec_regimm_BGEZ(struct lightrec_cstate *state,
			    const struct block *block, u16 offset)
{
	union code c = block->opcode_list[offset].c;

	_jit_name(block->_jit, __func__);
	rec_b(state, block, offset, jit_code_blti, jit_code_bgei, 0, !c.i.rs, true);
}

static void rec_regimm_BGEZAL(struct lightrec_cstate *state,
			      const struct block *block, u16 offset)
{
	const struct opcode *op = &block->opcode_list[offset];
	_jit_name(block->_jit, __func__);
	rec_b(state, block, offset, jit_code_blti, jit_code_bgei,
	      get_branch_pc(block, offset, 2),
	      !op->i.rs, true);
}

static void rec_alu_imm(struct lightrec_cstate *state, const struct block *block,
			u16 offset, jit_code_t code, bool slti)
{
	struct regcache *reg_cache = state->reg_cache;
	union code c = block->opcode_list[offset].c;
	jit_state_t *_jit = block->_jit;
	u8 rs, rt, out_flags = REG_EXT;

	if (slti)
		out_flags |= REG_ZEXT;

	jit_note(__FILE__, __LINE__);
	rs = lightrec_alloc_reg_in(reg_cache, _jit, c.i.rs, REG_EXT);
	rt = lightrec_alloc_reg_out(reg_cache, _jit, c.i.rt, out_flags);

	jit_new_node_www(code, rt, rs, (s32)(s16) c.i.imm);

	lightrec_free_reg(reg_cache, rs);
	lightrec_free_reg(reg_cache, rt);
}

static void rec_alu_special(struct lightrec_cstate *state, const struct block *block,
			    u16 offset, jit_code_t code, bool out_ext)
{
	struct regcache *reg_cache = state->reg_cache;
	union code c = block->opcode_list[offset].c;
	jit_state_t *_jit = block->_jit;
	u8 rd, rt, rs;

	jit_note(__FILE__, __LINE__);
	rs = lightrec_alloc_reg_in(reg_cache, _jit, c.r.rs, REG_EXT);
	rt = lightrec_alloc_reg_in(reg_cache, _jit, c.r.rt, REG_EXT);
	rd = lightrec_alloc_reg_out(reg_cache, _jit, c.r.rd,
				    out_ext ? REG_EXT | REG_ZEXT : 0);

	jit_new_node_www(code, rd, rs, rt);

	lightrec_free_reg(reg_cache, rs);
	lightrec_free_reg(reg_cache, rt);
	lightrec_free_reg(reg_cache, rd);
}

static void rec_alu_shiftv(struct lightrec_cstate *state, const struct block *block,
			   u16 offset, jit_code_t code)
{
	struct regcache *reg_cache = state->reg_cache;
	union code c = block->opcode_list[offset].c;
	jit_state_t *_jit = block->_jit;
	u8 rd, rt, rs, temp, flags = 0;

	jit_note(__FILE__, __LINE__);
	rs = lightrec_alloc_reg_in(reg_cache, _jit, c.r.rs, 0);

	if (code == jit_code_rshr)
		flags = REG_EXT;
	else if (code == jit_code_rshr_u)
		flags = REG_ZEXT;

	rt = lightrec_alloc_reg_in(reg_cache, _jit, c.r.rt, flags);
	rd = lightrec_alloc_reg_out(reg_cache, _jit, c.r.rd, flags);

	if (rs != rd && rt != rd) {
		jit_andi(rd, rs, 0x1f);
		jit_new_node_www(code, rd, rt, rd);
	} else {
		temp = lightrec_alloc_reg_temp(reg_cache, _jit);
		jit_andi(temp, rs, 0x1f);
		jit_new_node_www(code, rd, rt, temp);
		lightrec_free_reg(reg_cache, temp);
	}

	lightrec_free_reg(reg_cache, rs);
	lightrec_free_reg(reg_cache, rt);
	lightrec_free_reg(reg_cache, rd);
}

static void rec_movi(struct lightrec_cstate *state,
		     const struct block *block, u16 offset)
{
	struct regcache *reg_cache = state->reg_cache;
	union code c = block->opcode_list[offset].c;
	jit_state_t *_jit = block->_jit;
	u16 flags = REG_EXT;
	u8 rt;

	if (!(c.i.imm & 0x8000))
		flags |= REG_ZEXT;

	rt = lightrec_alloc_reg_out(reg_cache, _jit, c.i.rt, flags);

	jit_movi(rt, (s32)(s16) c.i.imm);

	lightrec_free_reg(reg_cache, rt);
}

static void rec_ADDIU(struct lightrec_cstate *state,
		      const struct block *block, u16 offset)
{
	_jit_name(block->_jit, __func__);

	if (block->opcode_list[offset].c.i.rs)
		rec_alu_imm(state, block, offset, jit_code_addi, false);
	else
		rec_movi(state, block, offset);
}

static void rec_ADDI(struct lightrec_cstate *state,
		     const struct block *block, u16 offset)
{
	/* TODO: Handle the exception? */
	_jit_name(block->_jit, __func__);
	rec_ADDIU(state, block, offset);
}

static void rec_SLTIU(struct lightrec_cstate *state,
		      const struct block *block, u16 offset)
{
	_jit_name(block->_jit, __func__);
	rec_alu_imm(state, block, offset, jit_code_lti_u, true);
}

static void rec_SLTI(struct lightrec_cstate *state,
		     const struct block *block, u16 offset)
{
	_jit_name(block->_jit, __func__);
	rec_alu_imm(state, block, offset, jit_code_lti, true);
}

static void rec_ANDI(struct lightrec_cstate *state,
		     const struct block *block, u16 offset)
{
	struct regcache *reg_cache = state->reg_cache;
	union code c = block->opcode_list[offset].c;
	jit_state_t *_jit = block->_jit;
	u8 rs, rt;

	_jit_name(block->_jit, __func__);
	jit_note(__FILE__, __LINE__);
	rs = lightrec_alloc_reg_in(reg_cache, _jit, c.i.rs, 0);
	rt = lightrec_alloc_reg_out(reg_cache, _jit, c.i.rt,
				    REG_EXT | REG_ZEXT);

	/* PSX code uses ANDI 0xff / ANDI 0xffff a lot, which are basically
	 * casts to uint8_t / uint16_t. */
	if (c.i.imm == 0xff)
		jit_extr_uc(rt, rs);
	else if (c.i.imm == 0xffff)
		jit_extr_us(rt, rs);
	else
		jit_andi(rt, rs, (u32)(u16) c.i.imm);

	lightrec_free_reg(reg_cache, rs);
	lightrec_free_reg(reg_cache, rt);
}

static void rec_alu_or_xor(struct lightrec_cstate *state, const struct block *block,
			   u16 offset, jit_code_t code)
{
	struct regcache *reg_cache = state->reg_cache;
	union code c = block->opcode_list[offset].c;
	jit_state_t *_jit = block->_jit;
	u8 rs, rt, flags;

	jit_note(__FILE__, __LINE__);
	rs = lightrec_alloc_reg_in(reg_cache, _jit, c.i.rs, 0);
	rt = lightrec_alloc_reg_out(reg_cache, _jit, c.i.rt, 0);

	flags = lightrec_get_reg_in_flags(reg_cache, rs);
	lightrec_set_reg_out_flags(reg_cache, rt, flags);

	jit_new_node_www(code, rt, rs, (u32)(u16) c.i.imm);

	lightrec_free_reg(reg_cache, rs);
	lightrec_free_reg(reg_cache, rt);
}


static void rec_ORI(struct lightrec_cstate *state,
		    const struct block *block, u16 offset)
{
	_jit_name(block->_jit, __func__);
	rec_alu_or_xor(state, block, offset, jit_code_ori);
}

static void rec_XORI(struct lightrec_cstate *state,
		     const struct block *block, u16 offset)
{
	_jit_name(block->_jit, __func__);
	rec_alu_or_xor(state, block, offset, jit_code_xori);
}

static void rec_LUI(struct lightrec_cstate *state,
		    const struct block *block, u16 offset)
{
	struct regcache *reg_cache = state->reg_cache;
	union code c = block->opcode_list[offset].c;
	jit_state_t *_jit = block->_jit;
	u8 rt, flags = REG_EXT;

	jit_name(__func__);
	jit_note(__FILE__, __LINE__);

	if (!(c.i.imm & BIT(15)))
		flags |= REG_ZEXT;

	rt = lightrec_alloc_reg_out(reg_cache, _jit, c.i.rt, flags);

	jit_movi(rt, (s32)(c.i.imm << 16));

	lightrec_free_reg(reg_cache, rt);
}

static void rec_special_ADDU(struct lightrec_cstate *state,
			     const struct block *block, u16 offset)
{
	_jit_name(block->_jit, __func__);
	rec_alu_special(state, block, offset, jit_code_addr, false);
}

static void rec_special_ADD(struct lightrec_cstate *state,
			    const struct block *block, u16 offset)
{
	/* TODO: Handle the exception? */
	_jit_name(block->_jit, __func__);
	rec_alu_special(state, block, offset, jit_code_addr, false);
}

static void rec_special_SUBU(struct lightrec_cstate *state,
			     const struct block *block, u16 offset)
{
	_jit_name(block->_jit, __func__);
	rec_alu_special(state, block, offset, jit_code_subr, false);
}

static void rec_special_SUB(struct lightrec_cstate *state,
			    const struct block *block, u16 offset)
{
	/* TODO: Handle the exception? */
	_jit_name(block->_jit, __func__);
	rec_alu_special(state, block, offset, jit_code_subr, false);
}

static void rec_special_AND(struct lightrec_cstate *state,
			    const struct block *block, u16 offset)
{
	struct regcache *reg_cache = state->reg_cache;
	union code c = block->opcode_list[offset].c;
	jit_state_t *_jit = block->_jit;
	u8 rd, rt, rs, flags_rs, flags_rt, flags_rd;

	_jit_name(block->_jit, __func__);
	jit_note(__FILE__, __LINE__);
	rs = lightrec_alloc_reg_in(reg_cache, _jit, c.r.rs, 0);
	rt = lightrec_alloc_reg_in(reg_cache, _jit, c.r.rt, 0);
	rd = lightrec_alloc_reg_out(reg_cache, _jit, c.r.rd, 0);

	flags_rs = lightrec_get_reg_in_flags(reg_cache, rs);
	flags_rt = lightrec_get_reg_in_flags(reg_cache, rt);

	/* Z(rd) = Z(rs) | Z(rt) */
	flags_rd = REG_ZEXT & (flags_rs | flags_rt);

	/* E(rd) = (E(rt) & Z(rt)) | (E(rs) & Z(rs)) | (E(rs) & E(rt)) */
	if (((flags_rs & REG_EXT) && (flags_rt & REG_ZEXT)) ||
	    ((flags_rt & REG_EXT) && (flags_rs & REG_ZEXT)) ||
	    (REG_EXT & flags_rs & flags_rt))
		flags_rd |= REG_EXT;

	lightrec_set_reg_out_flags(reg_cache, rd, flags_rd);

	jit_andr(rd, rs, rt);

	lightrec_free_reg(reg_cache, rs);
	lightrec_free_reg(reg_cache, rt);
	lightrec_free_reg(reg_cache, rd);
}

static void rec_special_or_nor(struct lightrec_cstate *state,
			       const struct block *block, u16 offset, bool nor)
{
	struct regcache *reg_cache = state->reg_cache;
	union code c = block->opcode_list[offset].c;
	jit_state_t *_jit = block->_jit;
	u8 rd, rt, rs, flags_rs, flags_rt, flags_rd = 0;

	jit_note(__FILE__, __LINE__);
	rs = lightrec_alloc_reg_in(reg_cache, _jit, c.r.rs, 0);
	rt = lightrec_alloc_reg_in(reg_cache, _jit, c.r.rt, 0);
	rd = lightrec_alloc_reg_out(reg_cache, _jit, c.r.rd, 0);

	flags_rs = lightrec_get_reg_in_flags(reg_cache, rs);
	flags_rt = lightrec_get_reg_in_flags(reg_cache, rt);

	/* or: Z(rd) = Z(rs) & Z(rt)
	 * nor: Z(rd) = 0 */
	if (!nor)
		flags_rd = REG_ZEXT & flags_rs & flags_rt;

	/* E(rd) = E(rs) & E(rt) */
	if (REG_EXT & flags_rs & flags_rt)
		flags_rd |= REG_EXT;

	lightrec_set_reg_out_flags(reg_cache, rd, flags_rd);

	jit_orr(rd, rs, rt);

	if (nor)
		jit_comr(rd, rd);

	lightrec_free_reg(reg_cache, rs);
	lightrec_free_reg(reg_cache, rt);
	lightrec_free_reg(reg_cache, rd);
}

static void rec_special_OR(struct lightrec_cstate *state,
			   const struct block *block, u16 offset)
{
	_jit_name(block->_jit, __func__);
	rec_special_or_nor(state, block, offset, false);
}

static void rec_special_NOR(struct lightrec_cstate *state,
			    const struct block *block, u16 offset)
{
	_jit_name(block->_jit, __func__);
	rec_special_or_nor(state, block, offset, true);
}

static void rec_special_XOR(struct lightrec_cstate *state,
			    const struct block *block, u16 offset)
{
	struct regcache *reg_cache = state->reg_cache;
	union code c = block->opcode_list[offset].c;
	jit_state_t *_jit = block->_jit;
	u8 rd, rt, rs, flags_rs, flags_rt, flags_rd;

	_jit_name(block->_jit, __func__);

	jit_note(__FILE__, __LINE__);
	rs = lightrec_alloc_reg_in(reg_cache, _jit, c.r.rs, 0);
	rt = lightrec_alloc_reg_in(reg_cache, _jit, c.r.rt, 0);
	rd = lightrec_alloc_reg_out(reg_cache, _jit, c.r.rd, 0);

	flags_rs = lightrec_get_reg_in_flags(reg_cache, rs);
	flags_rt = lightrec_get_reg_in_flags(reg_cache, rt);

	/* Z(rd) = Z(rs) & Z(rt) */
	flags_rd = REG_ZEXT & flags_rs & flags_rt;

	/* E(rd) = E(rs) & E(rt) */
	flags_rd |= REG_EXT & flags_rs & flags_rt;

	lightrec_set_reg_out_flags(reg_cache, rd, flags_rd);

	jit_xorr(rd, rs, rt);

	lightrec_free_reg(reg_cache, rs);
	lightrec_free_reg(reg_cache, rt);
	lightrec_free_reg(reg_cache, rd);
}

static void rec_special_SLTU(struct lightrec_cstate *state,
			     const struct block *block, u16 offset)
{
	_jit_name(block->_jit, __func__);
	rec_alu_special(state, block, offset, jit_code_ltr_u, true);
}

static void rec_special_SLT(struct lightrec_cstate *state,
			    const struct block *block, u16 offset)
{
	_jit_name(block->_jit, __func__);
	rec_alu_special(state, block, offset, jit_code_ltr, true);
}

static void rec_special_SLLV(struct lightrec_cstate *state,
			     const struct block *block, u16 offset)
{
	_jit_name(block->_jit, __func__);
	rec_alu_shiftv(state, block, offset, jit_code_lshr);
}

static void rec_special_SRLV(struct lightrec_cstate *state,
			     const struct block *block, u16 offset)
{
	_jit_name(block->_jit, __func__);
	rec_alu_shiftv(state, block, offset, jit_code_rshr_u);
}

static void rec_special_SRAV(struct lightrec_cstate *state,
			     const struct block *block, u16 offset)
{
	_jit_name(block->_jit, __func__);
	rec_alu_shiftv(state, block, offset, jit_code_rshr);
}

static void rec_alu_shift(struct lightrec_cstate *state, const struct block *block,
			  u16 offset, jit_code_t code)
{
	struct regcache *reg_cache = state->reg_cache;
	union code c = block->opcode_list[offset].c;
	jit_state_t *_jit = block->_jit;
	u8 rd, rt, flags = 0;

	jit_note(__FILE__, __LINE__);

	if (code == jit_code_rshi)
		flags = REG_EXT;
	else if (code == jit_code_rshi_u)
		flags = REG_ZEXT;

	rt = lightrec_alloc_reg_in(reg_cache, _jit, c.r.rt, flags);

	/* Input reg is zero-extended, if we SRL at least by one bit, we know
	 * the output reg will be both zero-extended and sign-extended. */
	if (code == jit_code_rshi_u && c.r.imm)
		flags |= REG_EXT;
	rd = lightrec_alloc_reg_out(reg_cache, _jit, c.r.rd, flags);

	jit_new_node_www(code, rd, rt, c.r.imm);

	lightrec_free_reg(reg_cache, rt);
	lightrec_free_reg(reg_cache, rd);
}

static void rec_special_SLL(struct lightrec_cstate *state,
			    const struct block *block, u16 offset)
{
	_jit_name(block->_jit, __func__);
	rec_alu_shift(state, block, offset, jit_code_lshi);
}

static void rec_special_SRL(struct lightrec_cstate *state,
			    const struct block *block, u16 offset)
{
	_jit_name(block->_jit, __func__);
	rec_alu_shift(state, block, offset, jit_code_rshi_u);
}

static void rec_special_SRA(struct lightrec_cstate *state,
			    const struct block *block, u16 offset)
{
	_jit_name(block->_jit, __func__);
	rec_alu_shift(state, block, offset, jit_code_rshi);
}

static void rec_alu_mult(struct lightrec_cstate *state,
			 const struct block *block, u16 offset, bool is_signed)
{
	struct regcache *reg_cache = state->reg_cache;
	union code c = block->opcode_list[offset].c;
	u32 flags = block->opcode_list[offset].flags;
	u8 reg_lo = get_mult_div_lo(c);
	u8 reg_hi = get_mult_div_hi(c);
	jit_state_t *_jit = block->_jit;
	u8 lo, hi, rs, rt, rflags = 0;

	jit_note(__FILE__, __LINE__);

	if (is_signed)
		rflags = REG_EXT;
	else
		rflags = REG_ZEXT;

	rs = lightrec_alloc_reg_in(reg_cache, _jit, c.r.rs, rflags);
	rt = lightrec_alloc_reg_in(reg_cache, _jit, c.r.rt, rflags);

	if (!op_flag_no_lo(flags))
		lo = lightrec_alloc_reg_out(reg_cache, _jit, reg_lo, 0);
	else if (__WORDSIZE == 32)
		lo = lightrec_alloc_reg_temp(reg_cache, _jit);

	if (!op_flag_no_hi(flags))
		hi = lightrec_alloc_reg_out(reg_cache, _jit, reg_hi, REG_EXT);

	if (__WORDSIZE == 32) {
		/* On 32-bit systems, do a 32*32->64 bit operation, or a 32*32->32 bit
		 * operation if the MULT was detected a 32-bit only. */
		if (!op_flag_no_hi(flags)) {
			if (is_signed)
				jit_qmulr(lo, hi, rs, rt);
			else
				jit_qmulr_u(lo, hi, rs, rt);
		} else {
			jit_mulr(lo, rs, rt);
		}
	} else {
		/* On 64-bit systems, do a 64*64->64 bit operation. */
		if (op_flag_no_lo(flags)) {
			jit_mulr(hi, rs, rt);
			jit_rshi(hi, hi, 32);
		} else {
			jit_mulr(lo, rs, rt);

			/* The 64-bit output value is in $lo, store the upper 32 bits in $hi */
			if (!op_flag_no_hi(flags))
				jit_rshi(hi, lo, 32);
		}
	}

	lightrec_free_reg(reg_cache, rs);
	lightrec_free_reg(reg_cache, rt);
	if (!op_flag_no_lo(flags) || __WORDSIZE == 32)
		lightrec_free_reg(reg_cache, lo);
	if (!op_flag_no_hi(flags))
		lightrec_free_reg(reg_cache, hi);
}

static void rec_alu_div(struct lightrec_cstate *state,
			const struct block *block, u16 offset, bool is_signed)
{
	struct regcache *reg_cache = state->reg_cache;
	union code c = block->opcode_list[offset].c;
	u32 flags = block->opcode_list[offset].flags;
	bool no_check = op_flag_no_div_check(flags);
	u8 reg_lo = get_mult_div_lo(c);
	u8 reg_hi = get_mult_div_hi(c);
	jit_state_t *_jit = block->_jit;
	jit_node_t *branch, *to_end;
	u8 lo = 0, hi = 0, rs, rt, rflags = 0;

	jit_note(__FILE__, __LINE__);

	if (is_signed)
		rflags = REG_EXT;
	else
		rflags = REG_ZEXT;

	rs = lightrec_alloc_reg_in(reg_cache, _jit, c.r.rs, rflags);
	rt = lightrec_alloc_reg_in(reg_cache, _jit, c.r.rt, rflags);

	if (!op_flag_no_lo(flags))
		lo = lightrec_alloc_reg_out(reg_cache, _jit, reg_lo, 0);

	if (!op_flag_no_hi(flags))
		hi = lightrec_alloc_reg_out(reg_cache, _jit, reg_hi, 0);

	/* Jump to special handler if dividing by zero  */
	if (!no_check)
		branch = jit_beqi(rt, 0);

	if (op_flag_no_lo(flags)) {
		if (is_signed)
			jit_remr(hi, rs, rt);
		else
			jit_remr_u(hi, rs, rt);
	} else if (op_flag_no_hi(flags)) {
		if (is_signed)
			jit_divr(lo, rs, rt);
		else
			jit_divr_u(lo, rs, rt);
	} else {
		if (is_signed)
			jit_qdivr(lo, hi, rs, rt);
		else
			jit_qdivr_u(lo, hi, rs, rt);
	}

	if (!no_check) {
		/* Jump above the div-by-zero handler */
		to_end = jit_b();

		jit_patch(branch);

		if (!op_flag_no_lo(flags)) {
			if (is_signed) {
				jit_lti(lo, rs, 0);
				jit_lshi(lo, lo, 1);
				jit_subi(lo, lo, 1);
			} else {
				jit_movi(lo, 0xffffffff);
			}
		}

		if (!op_flag_no_hi(flags))
			jit_movr(hi, rs);

		jit_patch(to_end);
	}

	lightrec_free_reg(reg_cache, rs);
	lightrec_free_reg(reg_cache, rt);

	if (!op_flag_no_lo(flags))
		lightrec_free_reg(reg_cache, lo);

	if (!op_flag_no_hi(flags))
		lightrec_free_reg(reg_cache, hi);
}

static void rec_special_MULT(struct lightrec_cstate *state,
			     const struct block *block, u16 offset)
{
	_jit_name(block->_jit, __func__);
	rec_alu_mult(state, block, offset, true);
}

static void rec_special_MULTU(struct lightrec_cstate *state,
			      const struct block *block, u16 offset)
{
	_jit_name(block->_jit, __func__);
	rec_alu_mult(state, block, offset, false);
}

static void rec_special_DIV(struct lightrec_cstate *state,
			    const struct block *block, u16 offset)
{
	_jit_name(block->_jit, __func__);
	rec_alu_div(state, block, offset, true);
}

static void rec_special_DIVU(struct lightrec_cstate *state,
			     const struct block *block, u16 offset)
{
	_jit_name(block->_jit, __func__);
	rec_alu_div(state, block, offset, false);
}

static void rec_alu_mv_lo_hi(struct lightrec_cstate *state,
			     const struct block *block, u8 dst, u8 src)
{
	struct regcache *reg_cache = state->reg_cache;
	jit_state_t *_jit = block->_jit;

	jit_note(__FILE__, __LINE__);
	src = lightrec_alloc_reg_in(reg_cache, _jit, src, 0);
	dst = lightrec_alloc_reg_out(reg_cache, _jit, dst, REG_EXT);

	jit_extr_i(dst, src);

	lightrec_free_reg(reg_cache, src);
	lightrec_free_reg(reg_cache, dst);
}

static void rec_special_MFHI(struct lightrec_cstate *state,
			     const struct block *block, u16 offset)
{
	union code c = block->opcode_list[offset].c;

	_jit_name(block->_jit, __func__);
	rec_alu_mv_lo_hi(state, block, c.r.rd, REG_HI);
}

static void rec_special_MTHI(struct lightrec_cstate *state,
			     const struct block *block, u16 offset)
{
	union code c = block->opcode_list[offset].c;

	_jit_name(block->_jit, __func__);
	rec_alu_mv_lo_hi(state, block, REG_HI, c.r.rs);
}

static void rec_special_MFLO(struct lightrec_cstate *state,
			     const struct block *block, u16 offset)
{
	union code c = block->opcode_list[offset].c;

	_jit_name(block->_jit, __func__);
	rec_alu_mv_lo_hi(state, block, c.r.rd, REG_LO);
}

static void rec_special_MTLO(struct lightrec_cstate *state,
			     const struct block *block, u16 offset)
{
	union code c = block->opcode_list[offset].c;

	_jit_name(block->_jit, __func__);
	rec_alu_mv_lo_hi(state, block, REG_LO, c.r.rs);
}

static void call_to_c_wrapper(struct lightrec_cstate *state,
			      const struct block *block, u32 arg,
			      enum c_wrappers wrapper)
{
	struct regcache *reg_cache = state->reg_cache;
	jit_state_t *_jit = block->_jit;
	s8 tmp, tmp2;

	/* Make sure JIT_R1 is not mapped; it will be used in the C wrapper. */
	tmp2 = lightrec_alloc_reg(reg_cache, _jit, JIT_R1);

	tmp = lightrec_get_reg_with_value(reg_cache,
					  (intptr_t) state->state->wrappers_eps[wrapper]);
	if (tmp < 0) {
		tmp = lightrec_alloc_reg_temp(reg_cache, _jit);
		jit_ldxi(tmp, LIGHTREC_REG_STATE,
			 offsetof(struct lightrec_state, wrappers_eps[wrapper]));

		lightrec_temp_set_value(reg_cache, tmp,
					(intptr_t) state->state->wrappers_eps[wrapper]);
	}

	lightrec_free_reg(reg_cache, tmp2);

#ifdef __mips__
	/* On MIPS, register t9 is always used as the target register for JALR.
	 * Therefore if it does not contain the target address we must
	 * invalidate it. */
	if (tmp != _T9)
		lightrec_unload_reg(reg_cache, _jit, _T9);
#endif

	jit_prepare();
	jit_pushargi(arg);

	lightrec_regcache_mark_live(reg_cache, _jit);
	jit_callr(tmp);

	lightrec_free_reg(reg_cache, tmp);
	lightrec_regcache_mark_live(reg_cache, _jit);
}

static void rec_io(struct lightrec_cstate *state,
		   const struct block *block, u16 offset,
		   bool load_rt, bool read_rt)
{
	struct regcache *reg_cache = state->reg_cache;
	jit_state_t *_jit = block->_jit;
	union code c = block->opcode_list[offset].c;
	u32 flags = block->opcode_list[offset].flags;
	bool is_tagged = LIGHTREC_FLAGS_GET_IO_MODE(flags);
	u32 lut_entry;

	jit_note(__FILE__, __LINE__);

	lightrec_clean_reg_if_loaded(reg_cache, _jit, c.i.rs, false);

	if (read_rt && likely(c.i.rt))
		lightrec_clean_reg_if_loaded(reg_cache, _jit, c.i.rt, true);
	else if (load_rt)
		lightrec_clean_reg_if_loaded(reg_cache, _jit, c.i.rt, false);

	if (is_tagged) {
		call_to_c_wrapper(state, block, c.opcode, C_WRAPPER_RW);
	} else {
		lut_entry = lightrec_get_lut_entry(block);
		call_to_c_wrapper(state, block, (lut_entry << 16) | offset,
				  C_WRAPPER_RW_GENERIC);
	}
}

static u32 rec_ram_mask(struct lightrec_state *state)
{
	return (RAM_SIZE << (state->mirrors_mapped * 2)) - 1;
}

static void rec_store_memory(struct lightrec_cstate *cstate,
			     const struct block *block,
			     u16 offset, jit_code_t code,
			     jit_code_t swap_code,
			     uintptr_t addr_offset, u32 addr_mask,
			     bool invalidate)
{
	const struct lightrec_state *state = cstate->state;
	struct regcache *reg_cache = cstate->reg_cache;
	struct opcode *op = &block->opcode_list[offset];
	jit_state_t *_jit = block->_jit;
	union code c = op->c;
	u8 rs, rt, tmp, tmp2, tmp3, addr_reg, addr_reg2;
	s16 imm = (s16)c.i.imm;
	s32 simm = (s32)imm << (1 - lut_is_32bit(state));
	s32 lut_offt = offsetof(struct lightrec_state, code_lut);
	bool no_mask = op_flag_no_mask(op->flags);
	bool add_imm = c.i.imm &&
		((!state->mirrors_mapped && !no_mask) || (invalidate &&
		((imm & 0x3) || simm + lut_offt != (s16)(simm + lut_offt))));
	bool need_tmp = !no_mask || addr_offset || add_imm || invalidate;

	rt = lightrec_alloc_reg_in(reg_cache, _jit, c.i.rt, 0);
	rs = lightrec_alloc_reg_in(reg_cache, _jit, c.i.rs, 0);
	if (need_tmp)
		tmp = lightrec_alloc_reg_temp(reg_cache, _jit);

	addr_reg = rs;

	if (add_imm) {
		jit_addi(tmp, addr_reg, (s16)c.i.imm);
		addr_reg = tmp;
		imm = 0;
	} else if (simm) {
		lut_offt += simm;
	}

	if (!no_mask) {
		jit_andi(tmp, addr_reg, addr_mask);
		addr_reg = tmp;
	}

	if (addr_offset) {
		tmp2 = lightrec_alloc_reg_temp(reg_cache, _jit);
		jit_addi(tmp2, addr_reg, addr_offset);
		addr_reg2 = tmp2;
	} else {
		addr_reg2 = addr_reg;
	}

	if (is_big_endian() && swap_code && c.i.rt) {
		tmp3 = lightrec_alloc_reg_temp(reg_cache, _jit);

		jit_new_node_ww(swap_code, tmp3, rt);
		jit_new_node_www(code, imm, addr_reg2, tmp3);

		lightrec_free_reg(reg_cache, tmp3);
	} else {
		jit_new_node_www(code, imm, addr_reg2, rt);
	}

	lightrec_free_reg(reg_cache, rt);

	if (invalidate) {
		tmp3 = lightrec_alloc_reg_in(reg_cache, _jit, 0, 0);

		if (c.i.op != OP_SW) {
			jit_andi(tmp, addr_reg, ~3);
			addr_reg = tmp;
		}

		if (!lut_is_32bit(state)) {
			jit_lshi(tmp, addr_reg, 1);
			addr_reg = tmp;
		}

		if (addr_reg == rs && c.i.rs == 0) {
			addr_reg = LIGHTREC_REG_STATE;
		} else {
			jit_addr(tmp, addr_reg, LIGHTREC_REG_STATE);
			addr_reg = tmp;
		}

		if (lut_is_32bit(state))
			jit_stxi_i(lut_offt, addr_reg, tmp3);
		else
			jit_stxi(lut_offt, addr_reg, tmp3);

		lightrec_free_reg(reg_cache, tmp3);
	}

	if (addr_offset)
		lightrec_free_reg(reg_cache, tmp2);
	if (need_tmp)
		lightrec_free_reg(reg_cache, tmp);
	lightrec_free_reg(reg_cache, rs);
}

static void rec_store_ram(struct lightrec_cstate *cstate,
			  const struct block *block,
			  u16 offset, jit_code_t code,
			  jit_code_t swap_code, bool invalidate)
{
	struct lightrec_state *state = cstate->state;

	_jit_note(block->_jit, __FILE__, __LINE__);

	return rec_store_memory(cstate, block, offset, code, swap_code,
				state->offset_ram, rec_ram_mask(state),
				invalidate);
}

static void rec_store_scratch(struct lightrec_cstate *cstate,
			      const struct block *block, u16 offset,
			      jit_code_t code, jit_code_t swap_code)
{
	_jit_note(block->_jit, __FILE__, __LINE__);

	return rec_store_memory(cstate, block, offset, code, swap_code,
				cstate->state->offset_scratch,
				0x1fffffff, false);
}

static void rec_store_io(struct lightrec_cstate *cstate,
			 const struct block *block, u16 offset,
			 jit_code_t code, jit_code_t swap_code)
{
	_jit_note(block->_jit, __FILE__, __LINE__);

	return rec_store_memory(cstate, block, offset, code, swap_code,
				cstate->state->offset_io,
				0x1fffffff, false);
}

static void rec_store_direct_no_invalidate(struct lightrec_cstate *cstate,
					   const struct block *block,
					   u16 offset, jit_code_t code,
					   jit_code_t swap_code)
{
	struct lightrec_state *state = cstate->state;
	struct regcache *reg_cache = cstate->reg_cache;
	union code c = block->opcode_list[offset].c;
	jit_state_t *_jit = block->_jit;
	jit_node_t *to_not_ram, *to_end;
	u8 tmp, tmp2, rs, rt;
	s16 imm;

	jit_note(__FILE__, __LINE__);
	rs = lightrec_alloc_reg_in(reg_cache, _jit, c.i.rs, 0);
	tmp = lightrec_alloc_reg_temp(reg_cache, _jit);

	if (state->offset_ram || state->offset_scratch)
		tmp2 = lightrec_alloc_reg_temp(reg_cache, _jit);

	/* Convert to KUNSEG and avoid RAM mirrors */
	if (state->mirrors_mapped) {
		imm = (s16)c.i.imm;
		jit_andi(tmp, rs, 0x1f800000 | (4 * RAM_SIZE - 1));
	} else if (c.i.imm) {
		imm = 0;
		jit_addi(tmp, rs, (s16)c.i.imm);
		jit_andi(tmp, tmp, 0x1f800000 | (RAM_SIZE - 1));
	} else {
		imm = 0;
		jit_andi(tmp, rs, 0x1f800000 | (RAM_SIZE - 1));
	}

	lightrec_free_reg(reg_cache, rs);

	if (state->offset_ram != state->offset_scratch) {
		to_not_ram = jit_bmsi(tmp, BIT(28));

		jit_movi(tmp2, state->offset_ram);

		to_end = jit_b();
		jit_patch(to_not_ram);

		jit_movi(tmp2, state->offset_scratch);
		jit_patch(to_end);
	} else if (state->offset_ram) {
		jit_movi(tmp2, state->offset_ram);
	}

	if (state->offset_ram || state->offset_scratch) {
		jit_addr(tmp, tmp, tmp2);
		lightrec_free_reg(reg_cache, tmp2);
	}

	rt = lightrec_alloc_reg_in(reg_cache, _jit, c.i.rt, 0);

	if (is_big_endian() && swap_code && c.i.rt) {
		tmp2 = lightrec_alloc_reg_temp(reg_cache, _jit);

		jit_new_node_ww(swap_code, tmp2, rt);
		jit_new_node_www(code, imm, tmp, tmp2);

		lightrec_free_reg(reg_cache, tmp2);
	} else {
		jit_new_node_www(code, imm, tmp, rt);
	}

	lightrec_free_reg(reg_cache, rt);
	lightrec_free_reg(reg_cache, tmp);
}

static void rec_store_direct(struct lightrec_cstate *cstate, const struct block *block,
			     u16 offset, jit_code_t code, jit_code_t swap_code)
{
	struct lightrec_state *state = cstate->state;
	u32 ram_size = state->mirrors_mapped ? RAM_SIZE * 4 : RAM_SIZE;
	struct regcache *reg_cache = cstate->reg_cache;
	union code c = block->opcode_list[offset].c;
	jit_state_t *_jit = block->_jit;
	jit_node_t *to_not_ram, *to_end;
	u8 tmp, tmp2, tmp3, rs, rt;

	jit_note(__FILE__, __LINE__);

	rs = lightrec_alloc_reg_in(reg_cache, _jit, c.i.rs, 0);
	tmp2 = lightrec_alloc_reg_temp(reg_cache, _jit);
	tmp3 = lightrec_alloc_reg_in(reg_cache, _jit, 0, 0);

	/* Convert to KUNSEG and avoid RAM mirrors */
	if (c.i.imm) {
		jit_addi(tmp2, rs, (s16)c.i.imm);
		jit_andi(tmp2, tmp2, 0x1f800000 | (ram_size - 1));
	} else {
		jit_andi(tmp2, rs, 0x1f800000 | (ram_size - 1));
	}

	lightrec_free_reg(reg_cache, rs);
	tmp = lightrec_alloc_reg_temp(reg_cache, _jit);

	to_not_ram = jit_bgti(tmp2, ram_size);

	/* Compute the offset to the code LUT */
	jit_andi(tmp, tmp2, (RAM_SIZE - 1) & ~3);
	if (!lut_is_32bit(state))
		jit_lshi(tmp, tmp, 1);
	jit_addr(tmp, LIGHTREC_REG_STATE, tmp);

	/* Write NULL to the code LUT to invalidate any block that's there */
	if (lut_is_32bit(state))
		jit_stxi_i(offsetof(struct lightrec_state, code_lut), tmp, tmp3);
	else
		jit_stxi(offsetof(struct lightrec_state, code_lut), tmp, tmp3);

	if (state->offset_ram != state->offset_scratch) {
		jit_movi(tmp, state->offset_ram);

		to_end = jit_b();
	}

	jit_patch(to_not_ram);

	if (state->offset_ram || state->offset_scratch)
		jit_movi(tmp, state->offset_scratch);

	if (state->offset_ram != state->offset_scratch)
		jit_patch(to_end);

	if (state->offset_ram || state->offset_scratch)
		jit_addr(tmp2, tmp2, tmp);

	lightrec_free_reg(reg_cache, tmp);
	lightrec_free_reg(reg_cache, tmp3);

	rt = lightrec_alloc_reg_in(reg_cache, _jit, c.i.rt, 0);

	if (is_big_endian() && swap_code && c.i.rt) {
		tmp = lightrec_alloc_reg_temp(reg_cache, _jit);

		jit_new_node_ww(swap_code, tmp, rt);
		jit_new_node_www(code, 0, tmp2, tmp);

		lightrec_free_reg(reg_cache, tmp);
	} else {
		jit_new_node_www(code, 0, tmp2, rt);
	}

	lightrec_free_reg(reg_cache, rt);
	lightrec_free_reg(reg_cache, tmp2);
}

static void rec_store(struct lightrec_cstate *state,
		      const struct block *block, u16 offset,
		      jit_code_t code, jit_code_t swap_code)
{
	u32 flags = block->opcode_list[offset].flags;
	bool no_invalidate = op_flag_no_invalidate(flags) ||
		state->state->invalidate_from_dma_only;

	switch (LIGHTREC_FLAGS_GET_IO_MODE(flags)) {
	case LIGHTREC_IO_RAM:
		rec_store_ram(state, block, offset, code,
			      swap_code, !no_invalidate);
		break;
	case LIGHTREC_IO_SCRATCH:
		rec_store_scratch(state, block, offset, code, swap_code);
		break;
	case LIGHTREC_IO_DIRECT:
		if (no_invalidate) {
			rec_store_direct_no_invalidate(state, block, offset,
						       code, swap_code);
		} else {
			rec_store_direct(state, block, offset, code, swap_code);
		}
		break;
	case LIGHTREC_IO_DIRECT_HW:
		rec_store_io(state, block, offset, code, swap_code);
		break;
	default:
		rec_io(state, block, offset, true, false);
		break;
	}
}

static void rec_SB(struct lightrec_cstate *state,
		   const struct block *block, u16 offset)
{
	_jit_name(block->_jit, __func__);
	rec_store(state, block, offset, jit_code_stxi_c, 0);
}

static void rec_SH(struct lightrec_cstate *state,
		   const struct block *block, u16 offset)
{
	_jit_name(block->_jit, __func__);
	rec_store(state, block, offset,
		  jit_code_stxi_s, jit_code_bswapr_us);
}

static void rec_SW(struct lightrec_cstate *state,
		   const struct block *block, u16 offset)

{
	_jit_name(block->_jit, __func__);
	rec_store(state, block, offset,
		  jit_code_stxi_i, jit_code_bswapr_ui);
}

static void rec_SWL(struct lightrec_cstate *state,
		    const struct block *block, u16 offset)
{
	_jit_name(block->_jit, __func__);
	rec_io(state, block, offset, true, false);
}

static void rec_SWR(struct lightrec_cstate *state,
		    const struct block *block, u16 offset)
{
	_jit_name(block->_jit, __func__);
	rec_io(state, block, offset, true, false);
}

static void rec_SWC2(struct lightrec_cstate *state,
		     const struct block *block, u16 offset)
{
	_jit_name(block->_jit, __func__);
	rec_io(state, block, offset, false, false);
}

static void rec_load_memory(struct lightrec_cstate *cstate,
			    const struct block *block, u16 offset,
			    jit_code_t code, jit_code_t swap_code, bool is_unsigned,
			    uintptr_t addr_offset, u32 addr_mask)
{
	struct regcache *reg_cache = cstate->reg_cache;
	struct opcode *op = &block->opcode_list[offset];
	jit_state_t *_jit = block->_jit;
	u8 rs, rt, addr_reg, flags = REG_EXT;
	bool no_mask = op_flag_no_mask(op->flags);
	union code c = op->c;
	s16 imm;

	if (!c.i.rt)
		return;

	if (is_unsigned)
		flags |= REG_ZEXT;

	rs = lightrec_alloc_reg_in(reg_cache, _jit, c.i.rs, 0);
	rt = lightrec_alloc_reg_out(reg_cache, _jit, c.i.rt, flags);

	if (!cstate->state->mirrors_mapped && c.i.imm && !no_mask) {
		jit_addi(rt, rs, (s16)c.i.imm);
		addr_reg = rt;
		imm = 0;
	} else {
		addr_reg = rs;
		imm = (s16)c.i.imm;
	}

	if (!no_mask) {
		jit_andi(rt, addr_reg, addr_mask);
		addr_reg = rt;
	}

	if (addr_offset) {
		jit_addi(rt, addr_reg, addr_offset);
		addr_reg = rt;
	}

	jit_new_node_www(code, rt, addr_reg, imm);

	if (is_big_endian() && swap_code) {
		jit_new_node_ww(swap_code, rt, rt);

		if (c.i.op == OP_LH)
			jit_extr_s(rt, rt);
		else if (c.i.op == OP_LW && __WORDSIZE == 64)
			jit_extr_i(rt, rt);
	}

	lightrec_free_reg(reg_cache, rs);
	lightrec_free_reg(reg_cache, rt);
}

static void rec_load_ram(struct lightrec_cstate *cstate,
			 const struct block *block, u16 offset,
			 jit_code_t code, jit_code_t swap_code, bool is_unsigned)
{
	_jit_note(block->_jit, __FILE__, __LINE__);

	rec_load_memory(cstate, block, offset, code, swap_code, is_unsigned,
			cstate->state->offset_ram, rec_ram_mask(cstate->state));
}

static void rec_load_bios(struct lightrec_cstate *cstate,
			  const struct block *block, u16 offset,
			  jit_code_t code, jit_code_t swap_code, bool is_unsigned)
{
	_jit_note(block->_jit, __FILE__, __LINE__);

	rec_load_memory(cstate, block, offset, code, swap_code, is_unsigned,
			cstate->state->offset_bios, 0x1fffffff);
}

static void rec_load_scratch(struct lightrec_cstate *cstate,
			     const struct block *block, u16 offset,
			     jit_code_t code, jit_code_t swap_code, bool is_unsigned)
{
	_jit_note(block->_jit, __FILE__, __LINE__);

	rec_load_memory(cstate, block, offset, code, swap_code, is_unsigned,
			cstate->state->offset_scratch, 0x1fffffff);
}

static void rec_load_io(struct lightrec_cstate *cstate,
			const struct block *block, u16 offset,
			jit_code_t code, jit_code_t swap_code, bool is_unsigned)
{
	_jit_note(block->_jit, __FILE__, __LINE__);

	rec_load_memory(cstate, block, offset, code, swap_code, is_unsigned,
			cstate->state->offset_io, 0x1fffffff);
}

static void rec_load_direct(struct lightrec_cstate *cstate,
			    const struct block *block, u16 offset,
			    jit_code_t code, jit_code_t swap_code,
			    bool is_unsigned)
{
	struct lightrec_state *state = cstate->state;
	struct regcache *reg_cache = cstate->reg_cache;
	union code c = block->opcode_list[offset].c;
	jit_state_t *_jit = block->_jit;
	jit_node_t *to_not_ram, *to_not_bios, *to_end, *to_end2;
	u8 tmp, rs, rt, addr_reg, flags = REG_EXT;
	s16 imm;

	if (!c.i.rt)
		return;

	if (is_unsigned)
		flags |= REG_ZEXT;

	jit_note(__FILE__, __LINE__);
	rs = lightrec_alloc_reg_in(reg_cache, _jit, c.i.rs, 0);
	rt = lightrec_alloc_reg_out(reg_cache, _jit, c.i.rt, flags);

	if ((state->offset_ram == state->offset_bios &&
	    state->offset_ram == state->offset_scratch &&
	    state->mirrors_mapped) || !c.i.imm) {
		addr_reg = rs;
		imm = (s16)c.i.imm;
	} else {
		jit_addi(rt, rs, (s16)c.i.imm);
		addr_reg = rt;
		imm = 0;

		if (c.i.rs != c.i.rt)
			lightrec_free_reg(reg_cache, rs);
	}

	tmp = lightrec_alloc_reg_temp(reg_cache, _jit);

	if (state->offset_ram == state->offset_bios &&
	    state->offset_ram == state->offset_scratch) {
		if (!state->mirrors_mapped) {
			jit_andi(tmp, addr_reg, BIT(28));
			jit_rshi_u(tmp, tmp, 28 - 22);
			jit_ori(tmp, tmp, 0x1f800000 | (RAM_SIZE - 1));
			jit_andr(rt, addr_reg, tmp);
		} else {
			jit_andi(rt, addr_reg, 0x1fffffff);
		}

		if (state->offset_ram)
			jit_movi(tmp, state->offset_ram);
	} else {
		to_not_ram = jit_bmsi(addr_reg, BIT(28));

		/* Convert to KUNSEG and avoid RAM mirrors */
		jit_andi(rt, addr_reg, RAM_SIZE - 1);

		if (state->offset_ram)
			jit_movi(tmp, state->offset_ram);

		to_end = jit_b();

		jit_patch(to_not_ram);

		if (state->offset_bios != state->offset_scratch)
			to_not_bios = jit_bmci(addr_reg, BIT(22));

		/* Convert to KUNSEG */
		jit_andi(rt, addr_reg, 0x1fc00000 | (BIOS_SIZE - 1));

		jit_movi(tmp, state->offset_bios);

		if (state->offset_bios != state->offset_scratch) {
			to_end2 = jit_b();

			jit_patch(to_not_bios);

			/* Convert to KUNSEG */
			jit_andi(rt, addr_reg, 0x1f800fff);

			if (state->offset_scratch)
				jit_movi(tmp, state->offset_scratch);

			jit_patch(to_end2);
		}

		jit_patch(to_end);
	}

	if (state->offset_ram || state->offset_bios || state->offset_scratch)
		jit_addr(rt, rt, tmp);

	jit_new_node_www(code, rt, rt, imm);

	if (is_big_endian() && swap_code) {
		jit_new_node_ww(swap_code, rt, rt);

		if (c.i.op == OP_LH)
			jit_extr_s(rt, rt);
		else if (c.i.op == OP_LW && __WORDSIZE == 64)
			jit_extr_i(rt, rt);
	}

	lightrec_free_reg(reg_cache, addr_reg);
	lightrec_free_reg(reg_cache, rt);
	lightrec_free_reg(reg_cache, tmp);
}

static void rec_load(struct lightrec_cstate *state, const struct block *block,
		     u16 offset, jit_code_t code, jit_code_t swap_code,
		     bool is_unsigned)
{
	u32 flags = block->opcode_list[offset].flags;

	switch (LIGHTREC_FLAGS_GET_IO_MODE(flags)) {
	case LIGHTREC_IO_RAM:
		rec_load_ram(state, block, offset, code, swap_code, is_unsigned);
		break;
	case LIGHTREC_IO_BIOS:
		rec_load_bios(state, block, offset, code, swap_code, is_unsigned);
		break;
	case LIGHTREC_IO_SCRATCH:
		rec_load_scratch(state, block, offset, code, swap_code, is_unsigned);
		break;
	case LIGHTREC_IO_DIRECT_HW:
		rec_load_io(state, block, offset, code, swap_code, is_unsigned);
		break;
	case LIGHTREC_IO_DIRECT:
		rec_load_direct(state, block, offset, code, swap_code, is_unsigned);
		break;
	default:
		rec_io(state, block, offset, false, true);
		break;
	}
}

static void rec_LB(struct lightrec_cstate *state, const struct block *block, u16 offset)
{
	_jit_name(block->_jit, __func__);
	rec_load(state, block, offset, jit_code_ldxi_c, 0, false);
}

static void rec_LBU(struct lightrec_cstate *state, const struct block *block, u16 offset)
{
	_jit_name(block->_jit, __func__);
	rec_load(state, block, offset, jit_code_ldxi_uc, 0, true);
}

static void rec_LH(struct lightrec_cstate *state, const struct block *block, u16 offset)
{
	jit_code_t code = is_big_endian() ? jit_code_ldxi_us : jit_code_ldxi_s;

	_jit_name(block->_jit, __func__);
	rec_load(state, block, offset, code, jit_code_bswapr_us, false);
}

static void rec_LHU(struct lightrec_cstate *state, const struct block *block, u16 offset)
{
	_jit_name(block->_jit, __func__);
	rec_load(state, block, offset, jit_code_ldxi_us, jit_code_bswapr_us, true);
}

static void rec_LWL(struct lightrec_cstate *state, const struct block *block, u16 offset)
{
	_jit_name(block->_jit, __func__);
	rec_io(state, block, offset, true, true);
}

static void rec_LWR(struct lightrec_cstate *state, const struct block *block, u16 offset)
{
	_jit_name(block->_jit, __func__);
	rec_io(state, block, offset, true, true);
}

static void rec_LW(struct lightrec_cstate *state, const struct block *block, u16 offset)
{
	jit_code_t code;

	if (is_big_endian() && __WORDSIZE == 64)
		code = jit_code_ldxi_ui;
	else
		code = jit_code_ldxi_i;

	_jit_name(block->_jit, __func__);
	rec_load(state, block, offset, code, jit_code_bswapr_ui, false);
}

static void rec_LWC2(struct lightrec_cstate *state, const struct block *block, u16 offset)
{
	_jit_name(block->_jit, __func__);
	rec_io(state, block, offset, false, false);
}

static void rec_break_syscall(struct lightrec_cstate *state,
			      const struct block *block, u16 offset,
			      u32 exit_code)
{
	struct regcache *reg_cache = state->reg_cache;
	jit_state_t *_jit = block->_jit;
	u8 tmp;

	_jit_note(block->_jit, __FILE__, __LINE__);

	tmp = lightrec_alloc_reg_temp(reg_cache, _jit);

	jit_movi(tmp, exit_code);
	jit_stxi_i(offsetof(struct lightrec_state, exit_flags),
		   LIGHTREC_REG_STATE, tmp);

	lightrec_free_reg(reg_cache, tmp);

	/* TODO: the return address should be "pc - 4" if we're a delay slot */
	lightrec_emit_end_of_block(state, block, offset, -1,
				   get_ds_pc(block, offset, 0),
				   31, 0, true);
}

static void rec_special_SYSCALL(struct lightrec_cstate *state,
				const struct block *block, u16 offset)
{
	_jit_name(block->_jit, __func__);
	rec_break_syscall(state, block, offset, LIGHTREC_EXIT_SYSCALL);
}

static void rec_special_BREAK(struct lightrec_cstate *state,
			      const struct block *block, u16 offset)
{
	_jit_name(block->_jit, __func__);
	rec_break_syscall(state, block, offset, LIGHTREC_EXIT_BREAK);
}

static void rec_mfc(struct lightrec_cstate *state, const struct block *block, u16 offset)
{
	struct regcache *reg_cache = state->reg_cache;
	union code c = block->opcode_list[offset].c;
	jit_state_t *_jit = block->_jit;

	jit_note(__FILE__, __LINE__);
	lightrec_clean_reg_if_loaded(reg_cache, _jit, c.i.rt, true);

	call_to_c_wrapper(state, block, c.opcode, C_WRAPPER_MFC);
}

static void rec_mtc(struct lightrec_cstate *state, const struct block *block, u16 offset)
{
	struct regcache *reg_cache = state->reg_cache;
	union code c = block->opcode_list[offset].c;
	jit_state_t *_jit = block->_jit;

	jit_note(__FILE__, __LINE__);
	lightrec_clean_reg_if_loaded(reg_cache, _jit, c.i.rs, false);
	lightrec_clean_reg_if_loaded(reg_cache, _jit, c.i.rt, false);

	call_to_c_wrapper(state, block, c.opcode, C_WRAPPER_MTC);

	if (c.i.op == OP_CP0 &&
	    !op_flag_no_ds(block->opcode_list[offset].flags) &&
	    (c.r.rd == 12 || c.r.rd == 13))
		lightrec_emit_end_of_block(state, block, offset, -1,
					   get_ds_pc(block, offset, 1),
					   0, 0, true);
}

static void
rec_mfc0(struct lightrec_cstate *state, const struct block *block, u16 offset)
{
	struct regcache *reg_cache = state->reg_cache;
	union code c = block->opcode_list[offset].c;
	jit_state_t *_jit = block->_jit;
	u8 rt;

	jit_note(__FILE__, __LINE__);

	rt = lightrec_alloc_reg_out(reg_cache, _jit, c.i.rt, REG_EXT);

	jit_ldxi_i(rt, LIGHTREC_REG_STATE,
		   offsetof(struct lightrec_state, regs.cp0[c.r.rd]));

	lightrec_free_reg(reg_cache, rt);
}

static bool block_in_bios(const struct lightrec_cstate *state,
			  const struct block *block)
{
	const struct lightrec_mem_map *bios = &state->state->maps[PSX_MAP_BIOS];
	u32 pc = kunseg(block->pc);

	return pc >= bios->pc && pc < bios->pc + bios->length;
}

static void
rec_mtc0(struct lightrec_cstate *state, const struct block *block, u16 offset)
{
	struct regcache *reg_cache = state->reg_cache;
	const union code c = block->opcode_list[offset].c;
	jit_state_t *_jit = block->_jit;
	u8 rt, tmp = 0, tmp2, status;

	jit_note(__FILE__, __LINE__);

	switch(c.r.rd) {
	case 1:
	case 4:
	case 8:
	case 14:
	case 15:
		/* Those registers are read-only */
		return;
	default:
		break;
	}

	if (block_in_bios(state, block) && c.r.rd == 12) {
		/* If we are running code from the BIOS, handle writes to the
		 * Status register in C. BIOS code may toggle bit 16 which will
		 * map/unmap the RAM, while game code cannot do that. */
		rec_mtc(state, block, offset);
		return;
	}

	rt = lightrec_alloc_reg_in(reg_cache, _jit, c.i.rt, 0);

	if (c.r.rd != 13) {
		jit_stxi_i(offsetof(struct lightrec_state, regs.cp0[c.r.rd]),
			   LIGHTREC_REG_STATE, rt);
	}

	if (c.r.rd == 12 || c.r.rd == 13) {
		tmp = lightrec_alloc_reg_temp(reg_cache, _jit);
		jit_ldxi_i(tmp, LIGHTREC_REG_STATE,
			   offsetof(struct lightrec_state, regs.cp0[13]));

		tmp2 = lightrec_alloc_reg_temp(reg_cache, _jit);
	}

	if (c.r.rd == 12) {
		status = rt;
	} else if (c.r.rd == 13) {
		/* Cause = (Cause & ~0x0300) | (value & 0x0300) */
		jit_andi(tmp2, rt, 0x0300);
		jit_ori(tmp, tmp, 0x0300);
		jit_xori(tmp, tmp, 0x0300);
		jit_orr(tmp, tmp, tmp2);
		jit_ldxi_i(tmp2, LIGHTREC_REG_STATE,
			   offsetof(struct lightrec_state, regs.cp0[12]));
		jit_stxi_i(offsetof(struct lightrec_state, regs.cp0[13]),
			   LIGHTREC_REG_STATE, tmp);
		status = tmp2;
	}

	if (c.r.rd == 12 || c.r.rd == 13) {
		/* Exit dynarec in case there's a software interrupt.
		 * exit_flags = !!(status & tmp & 0x0300) & status; */
		jit_andr(tmp, tmp, status);
		jit_andi(tmp, tmp, 0x0300);
		jit_nei(tmp, tmp, 0);
		jit_andr(tmp, tmp, status);
	}

	if (c.r.rd == 12) {
		/* Exit dynarec in case we unmask a hardware interrupt.
		 * exit_flags = !(~status & 0x401) */

		jit_comr(tmp2, status);
		jit_andi(tmp2, tmp2, 0x401);
		jit_eqi(tmp2, tmp2, 0);
		jit_orr(tmp, tmp, tmp2);
	}

	if (c.r.rd == 12 || c.r.rd == 13) {
		jit_stxi_i(offsetof(struct lightrec_state, exit_flags),
			   LIGHTREC_REG_STATE, tmp);

		lightrec_free_reg(reg_cache, tmp);
		lightrec_free_reg(reg_cache, tmp2);
	}

	lightrec_free_reg(reg_cache, rt);

	if (!op_flag_no_ds(block->opcode_list[offset].flags) &&
	    (c.r.rd == 12 || c.r.rd == 13))
		lightrec_emit_eob(state, block, offset + 1, true);
}

static void rec_cp0_MFC0(struct lightrec_cstate *state,
			 const struct block *block, u16 offset)
{
	_jit_name(block->_jit, __func__);
	rec_mfc0(state, block, offset);
}

static void rec_cp0_CFC0(struct lightrec_cstate *state,
			 const struct block *block, u16 offset)
{
	_jit_name(block->_jit, __func__);
	rec_mfc0(state, block, offset);
}

static void rec_cp0_MTC0(struct lightrec_cstate *state,
			 const struct block *block, u16 offset)
{
	_jit_name(block->_jit, __func__);
	rec_mtc0(state, block, offset);
}

static void rec_cp0_CTC0(struct lightrec_cstate *state,
			 const struct block *block, u16 offset)
{
	_jit_name(block->_jit, __func__);
	rec_mtc0(state, block, offset);
}

static unsigned int cp2d_i_offset(u8 reg)
{
	return offsetof(struct lightrec_state, regs.cp2d[reg]);
}

static unsigned int cp2d_s_offset(u8 reg)
{
	return cp2d_i_offset(reg) + is_big_endian() * 2;
}

static unsigned int cp2c_i_offset(u8 reg)
{
	return offsetof(struct lightrec_state, regs.cp2c[reg]);
}

static unsigned int cp2c_s_offset(u8 reg)
{
	return cp2c_i_offset(reg) + is_big_endian() * 2;
}

static void rec_cp2_basic_MFC2(struct lightrec_cstate *state,
			       const struct block *block, u16 offset)
{
	struct regcache *reg_cache = state->reg_cache;
	const union code c = block->opcode_list[offset].c;
	jit_state_t *_jit = block->_jit;
	const u32 zext_regs = 0x300f0080;
	u8 rt, tmp, tmp2, tmp3, out, flags;
	u8 reg = c.r.rd == 15 ? 14 : c.r.rd;
	unsigned int i;

	_jit_name(block->_jit, __func__);

	if (state->state->ops.cop2_notify) {
		/* We must call cop2_notify, handle that in C. */
		rec_mfc(state, block, offset);
		return;
	}

	flags = (zext_regs & BIT(reg)) ? REG_ZEXT : REG_EXT;
	rt = lightrec_alloc_reg_out(reg_cache, _jit, c.r.rt, flags);

	switch (reg) {
	case 1:
	case 3:
	case 5:
	case 8:
	case 9:
	case 10:
	case 11:
		jit_ldxi_s(rt, LIGHTREC_REG_STATE, cp2d_s_offset(reg));
		break;
	case 7:
	case 16:
	case 17:
	case 18:
	case 19:
		jit_ldxi_us(rt, LIGHTREC_REG_STATE, cp2d_s_offset(reg));
		break;
	case 28:
	case 29:
		tmp = lightrec_alloc_reg_temp(reg_cache, _jit);
		tmp2 = lightrec_alloc_reg_temp(reg_cache, _jit);
		tmp3 = lightrec_alloc_reg_temp(reg_cache, _jit);

		for (i = 0; i < 3; i++) {
			out = i == 0 ? rt : tmp;

			jit_ldxi_s(tmp, LIGHTREC_REG_STATE, cp2d_s_offset(9 + i));
			jit_movi(tmp2, 0x1f);
			jit_rshi(out, tmp, 7);

			jit_ltr(tmp3, tmp2, out);
			jit_movnr(out, tmp2, tmp3);

			jit_gei(tmp2, out, 0);
			jit_movzr(out, tmp2, tmp2);

			if (i > 0) {
				jit_lshi(tmp, tmp, 5 * i);
				jit_orr(rt, rt, tmp);
			}
		}


		lightrec_free_reg(reg_cache, tmp);
		lightrec_free_reg(reg_cache, tmp2);
		lightrec_free_reg(reg_cache, tmp3);
		break;
	default:
		jit_ldxi_i(rt, LIGHTREC_REG_STATE, cp2d_i_offset(reg));
		break;
	}

	lightrec_free_reg(reg_cache, rt);
}

static void rec_cp2_basic_CFC2(struct lightrec_cstate *state,
			       const struct block *block, u16 offset)
{
	struct regcache *reg_cache = state->reg_cache;
	const union code c = block->opcode_list[offset].c;
	jit_state_t *_jit = block->_jit;
	u8 rt;

	_jit_name(block->_jit, __func__);

	if (state->state->ops.cop2_notify) {
		/* We must call cop2_notify, handle that in C. */
		rec_mfc(state, block, offset);
		return;
	}

	switch (c.r.rd) {
	case 4:
	case 12:
	case 20:
	case 26:
	case 27:
	case 29:
	case 30:
		rt = lightrec_alloc_reg_out(reg_cache, _jit, c.r.rt, REG_EXT);
		jit_ldxi_s(rt, LIGHTREC_REG_STATE, cp2c_s_offset(c.r.rd));
		break;
	default:
		rt = lightrec_alloc_reg_out(reg_cache, _jit, c.r.rt, REG_ZEXT);
		jit_ldxi_i(rt, LIGHTREC_REG_STATE, cp2c_i_offset(c.r.rd));
		break;
	}

	lightrec_free_reg(reg_cache, rt);
}

static void rec_cp2_basic_MTC2(struct lightrec_cstate *state,
			       const struct block *block, u16 offset)
{
	struct regcache *reg_cache = state->reg_cache;
	const union code c = block->opcode_list[offset].c;
	jit_state_t *_jit = block->_jit;
	jit_node_t *loop, *to_loop;
	u8 rt, tmp, tmp2, flags = 0;

	_jit_name(block->_jit, __func__);

	if (state->state->ops.cop2_notify) {
		/* We must call cop2_notify, handle that in C. */
		rec_mtc(state, block, offset);
		return;
	}

	if (c.r.rd == 31)
		return;

	if (c.r.rd == 30)
		flags |= REG_EXT;

	rt = lightrec_alloc_reg_in(reg_cache, _jit, c.r.rt, flags);

	switch (c.r.rd) {
	case 15:
		tmp = lightrec_alloc_reg_temp(reg_cache, _jit);
		jit_ldxi_i(tmp, LIGHTREC_REG_STATE, cp2d_i_offset(13));

		tmp2 = lightrec_alloc_reg_temp(reg_cache, _jit);
		jit_ldxi_i(tmp2, LIGHTREC_REG_STATE, cp2d_i_offset(14));

		jit_stxi_i(cp2d_i_offset(12), LIGHTREC_REG_STATE, tmp);
		jit_stxi_i(cp2d_i_offset(13), LIGHTREC_REG_STATE, tmp2);
		jit_stxi_i(cp2d_i_offset(14), LIGHTREC_REG_STATE, rt);

		lightrec_free_reg(reg_cache, tmp);
		lightrec_free_reg(reg_cache, tmp2);
		break;
	case 28:
		tmp = lightrec_alloc_reg_temp(reg_cache, _jit);

		jit_lshi(tmp, rt, 7);
		jit_andi(tmp, tmp, 0xf80);
		jit_stxi_s(cp2d_s_offset(9), LIGHTREC_REG_STATE, tmp);

		jit_lshi(tmp, rt, 2);
		jit_andi(tmp, tmp, 0xf80);
		jit_stxi_s(cp2d_s_offset(10), LIGHTREC_REG_STATE, tmp);

		jit_rshi(tmp, rt, 3);
		jit_andi(tmp, tmp, 0xf80);
		jit_stxi_s(cp2d_s_offset(11), LIGHTREC_REG_STATE, tmp);

		lightrec_free_reg(reg_cache, tmp);
		break;
	case 30:
		tmp = lightrec_alloc_reg_temp(reg_cache, _jit);
		tmp2 = lightrec_alloc_reg_temp(reg_cache, _jit);

		/* if (rt < 0) rt = ~rt; */
		jit_rshi(tmp, rt, 31);
		jit_xorr(tmp, rt, tmp);

		/* We know the sign bit is 0. Left-shift by 1 to start the algorithm */
		jit_lshi(tmp, tmp, 1);
		jit_movi(tmp2, 33);

		/* Decrement tmp2 and right-shift the value by 1 until it equals zero */
		loop = jit_label();
		jit_subi(tmp2, tmp2, 1);
		jit_rshi_u(tmp, tmp, 1);
		to_loop = jit_bnei(tmp, 0);

		jit_patch_at(to_loop, loop);

		jit_stxi_i(cp2d_i_offset(31), LIGHTREC_REG_STATE, tmp2);
		jit_stxi_i(cp2d_i_offset(30), LIGHTREC_REG_STATE, rt);

		lightrec_free_reg(reg_cache, tmp);
		lightrec_free_reg(reg_cache, tmp2);
		break;
	default:
		jit_stxi_i(cp2d_i_offset(c.r.rd), LIGHTREC_REG_STATE, rt);
		break;
	}

	lightrec_free_reg(reg_cache, rt);
}

static void rec_cp2_basic_CTC2(struct lightrec_cstate *state,
			       const struct block *block, u16 offset)
{
	struct regcache *reg_cache = state->reg_cache;
	const union code c = block->opcode_list[offset].c;
	jit_state_t *_jit = block->_jit;
	u8 rt, tmp, tmp2;

	_jit_name(block->_jit, __func__);

	if (state->state->ops.cop2_notify) {
		/* We must call cop2_notify, handle that in C. */
		rec_mtc(state, block, offset);
		return;
	}

	rt = lightrec_alloc_reg_in(reg_cache, _jit, c.r.rt, 0);

	switch (c.r.rd) {
	case 4:
	case 12:
	case 20:
	case 26:
	case 27:
	case 29:
	case 30:
		jit_stxi_s(cp2c_s_offset(c.r.rd), LIGHTREC_REG_STATE, rt);
		break;
	case 31:
		tmp = lightrec_alloc_reg_temp(reg_cache, _jit);
		tmp2 = lightrec_alloc_reg_temp(reg_cache, _jit);

		jit_andi(tmp, rt, 0x7f87e000);
		jit_nei(tmp, tmp, 0);
		jit_lshi(tmp, tmp, 31);

		jit_andi(tmp2, rt, 0x7ffff000);
		jit_orr(tmp, tmp2, tmp);

		jit_stxi_i(cp2c_i_offset(31), LIGHTREC_REG_STATE, tmp);

		lightrec_free_reg(reg_cache, tmp);
		lightrec_free_reg(reg_cache, tmp2);
		break;

	default:
		jit_stxi_i(cp2c_i_offset(c.r.rd), LIGHTREC_REG_STATE, rt);
	}

	lightrec_free_reg(reg_cache, rt);
}

static void rec_cp0_RFE(struct lightrec_cstate *state,
			const struct block *block, u16 offset)
{
	struct regcache *reg_cache = state->reg_cache;
	jit_state_t *_jit = block->_jit;
	u8 status, tmp;

	jit_name(__func__);
	jit_note(__FILE__, __LINE__);

	status = lightrec_alloc_reg_temp(reg_cache, _jit);
	jit_ldxi_i(status, LIGHTREC_REG_STATE,
		   offsetof(struct lightrec_state, regs.cp0[12]));

	tmp = lightrec_alloc_reg_temp(reg_cache, _jit);

	/* status = ((status >> 2) & 0xf) | status & ~0xf; */
	jit_rshi(tmp, status, 2);
	jit_andi(tmp, tmp, 0xf);
	jit_andi(status, status, ~0xful);
	jit_orr(status, status, tmp);

	jit_ldxi_i(tmp, LIGHTREC_REG_STATE,
		   offsetof(struct lightrec_state, regs.cp0[13]));
	jit_stxi_i(offsetof(struct lightrec_state, regs.cp0[12]),
		   LIGHTREC_REG_STATE, status);

	/* Exit dynarec in case there's a software interrupt.
	 * exit_flags = !!(status & cause & 0x0300) & status; */
	jit_andr(tmp, tmp, status);
	jit_andi(tmp, tmp, 0x0300);
	jit_nei(tmp, tmp, 0);
	jit_andr(tmp, tmp, status);
	jit_stxi_i(offsetof(struct lightrec_state, exit_flags),
		   LIGHTREC_REG_STATE, tmp);

	lightrec_free_reg(reg_cache, status);
	lightrec_free_reg(reg_cache, tmp);
}

static void rec_CP(struct lightrec_cstate *state,
		   const struct block *block, u16 offset)
{
	union code c = block->opcode_list[offset].c;
	jit_state_t *_jit = block->_jit;

	jit_name(__func__);
	jit_note(__FILE__, __LINE__);

	call_to_c_wrapper(state, block, c.opcode, C_WRAPPER_CP);
}

static void rec_meta_MOV(struct lightrec_cstate *state,
			 const struct block *block, u16 offset)
{
	struct regcache *reg_cache = state->reg_cache;
	union code c = block->opcode_list[offset].c;
	jit_state_t *_jit = block->_jit;
	u8 rs, rd;

	_jit_name(block->_jit, __func__);
	jit_note(__FILE__, __LINE__);
	if (c.r.rs)
		rs = lightrec_alloc_reg_in(reg_cache, _jit, c.r.rs, 0);
	rd = lightrec_alloc_reg_out(reg_cache, _jit, c.r.rd, REG_EXT);

	if (c.r.rs == 0)
		jit_movi(rd, 0);
	else
		jit_extr_i(rd, rs);

	if (c.r.rs)
		lightrec_free_reg(reg_cache, rs);
	lightrec_free_reg(reg_cache, rd);
}

static void rec_meta_EXTC_EXTS(struct lightrec_cstate *state,
			       const struct block *block,
			       u16 offset)
{
	struct regcache *reg_cache = state->reg_cache;
	union code c = block->opcode_list[offset].c;
	jit_state_t *_jit = block->_jit;
	u8 rs, rt;

	_jit_name(block->_jit, __func__);
	jit_note(__FILE__, __LINE__);

	rs = lightrec_alloc_reg_in(reg_cache, _jit, c.i.rs, 0);
	rt = lightrec_alloc_reg_out(reg_cache, _jit, c.i.rt, REG_EXT);

	if (c.i.op == OP_META_EXTC)
		jit_extr_c(rt, rs);
	else
		jit_extr_s(rt, rs);

	lightrec_free_reg(reg_cache, rs);
	lightrec_free_reg(reg_cache, rt);
}

static void rec_meta_MULT2(struct lightrec_cstate *state,
			   const struct block *block,
			   u16 offset)
{
	struct regcache *reg_cache = state->reg_cache;
	union code c = block->opcode_list[offset].c;
	jit_state_t *_jit = block->_jit;
	u8 reg_lo = get_mult_div_lo(c);
	u8 reg_hi = get_mult_div_hi(c);
	u32 flags = block->opcode_list[offset].flags;
	bool is_signed = c.i.op == OP_META_MULT2;
	u8 rs, lo, hi, rflags = 0, hiflags = 0;

	if (!op_flag_no_hi(flags) && c.r.op < 32) {
		rflags = is_signed ? REG_EXT : REG_ZEXT;
		hiflags = is_signed ? REG_EXT : (REG_EXT | REG_ZEXT);
	}

	_jit_name(block->_jit, __func__);
	jit_note(__FILE__, __LINE__);

	rs = lightrec_alloc_reg_in(reg_cache, _jit, c.i.rs, rflags);

	if (!op_flag_no_lo(flags)) {
		lo = lightrec_alloc_reg_out(reg_cache, _jit, reg_lo, 0);

		if (c.r.op < 32)
			jit_lshi(lo, rs, c.r.op);
		else
			jit_movi(lo, 0);

		lightrec_free_reg(reg_cache, lo);
	}

	if (!op_flag_no_hi(flags)) {
		hi = lightrec_alloc_reg_out(reg_cache, _jit, reg_hi, hiflags);

		if (c.r.op >= 32)
			jit_lshi(hi, rs, c.r.op - 32);
		else if (is_signed)
			jit_rshi(hi, rs, 32 - c.r.op);
		else
			jit_rshi_u(hi, rs, 32 - c.r.op);

		lightrec_free_reg(reg_cache, hi);
	}

	lightrec_free_reg(reg_cache, rs);

	_jit_name(block->_jit, __func__);
	jit_note(__FILE__, __LINE__);
}

static const lightrec_rec_func_t rec_standard[64] = {
	SET_DEFAULT_ELM(rec_standard, unknown_opcode),
	[OP_SPECIAL]		= rec_SPECIAL,
	[OP_REGIMM]		= rec_REGIMM,
	[OP_J]			= rec_J,
	[OP_JAL]		= rec_JAL,
	[OP_BEQ]		= rec_BEQ,
	[OP_BNE]		= rec_BNE,
	[OP_BLEZ]		= rec_BLEZ,
	[OP_BGTZ]		= rec_BGTZ,
	[OP_ADDI]		= rec_ADDI,
	[OP_ADDIU]		= rec_ADDIU,
	[OP_SLTI]		= rec_SLTI,
	[OP_SLTIU]		= rec_SLTIU,
	[OP_ANDI]		= rec_ANDI,
	[OP_ORI]		= rec_ORI,
	[OP_XORI]		= rec_XORI,
	[OP_LUI]		= rec_LUI,
	[OP_CP0]		= rec_CP0,
	[OP_CP2]		= rec_CP2,
	[OP_LB]			= rec_LB,
	[OP_LH]			= rec_LH,
	[OP_LWL]		= rec_LWL,
	[OP_LW]			= rec_LW,
	[OP_LBU]		= rec_LBU,
	[OP_LHU]		= rec_LHU,
	[OP_LWR]		= rec_LWR,
	[OP_SB]			= rec_SB,
	[OP_SH]			= rec_SH,
	[OP_SWL]		= rec_SWL,
	[OP_SW]			= rec_SW,
	[OP_SWR]		= rec_SWR,
	[OP_LWC2]		= rec_LWC2,
	[OP_SWC2]		= rec_SWC2,

	[OP_META_MOV]		= rec_meta_MOV,
	[OP_META_EXTC]		= rec_meta_EXTC_EXTS,
	[OP_META_EXTS]		= rec_meta_EXTC_EXTS,
	[OP_META_MULT2]		= rec_meta_MULT2,
	[OP_META_MULTU2]	= rec_meta_MULT2,
};

static const lightrec_rec_func_t rec_special[64] = {
	SET_DEFAULT_ELM(rec_special, unknown_opcode),
	[OP_SPECIAL_SLL]	= rec_special_SLL,
	[OP_SPECIAL_SRL]	= rec_special_SRL,
	[OP_SPECIAL_SRA]	= rec_special_SRA,
	[OP_SPECIAL_SLLV]	= rec_special_SLLV,
	[OP_SPECIAL_SRLV]	= rec_special_SRLV,
	[OP_SPECIAL_SRAV]	= rec_special_SRAV,
	[OP_SPECIAL_JR]		= rec_special_JR,
	[OP_SPECIAL_JALR]	= rec_special_JALR,
	[OP_SPECIAL_SYSCALL]	= rec_special_SYSCALL,
	[OP_SPECIAL_BREAK]	= rec_special_BREAK,
	[OP_SPECIAL_MFHI]	= rec_special_MFHI,
	[OP_SPECIAL_MTHI]	= rec_special_MTHI,
	[OP_SPECIAL_MFLO]	= rec_special_MFLO,
	[OP_SPECIAL_MTLO]	= rec_special_MTLO,
	[OP_SPECIAL_MULT]	= rec_special_MULT,
	[OP_SPECIAL_MULTU]	= rec_special_MULTU,
	[OP_SPECIAL_DIV]	= rec_special_DIV,
	[OP_SPECIAL_DIVU]	= rec_special_DIVU,
	[OP_SPECIAL_ADD]	= rec_special_ADD,
	[OP_SPECIAL_ADDU]	= rec_special_ADDU,
	[OP_SPECIAL_SUB]	= rec_special_SUB,
	[OP_SPECIAL_SUBU]	= rec_special_SUBU,
	[OP_SPECIAL_AND]	= rec_special_AND,
	[OP_SPECIAL_OR]		= rec_special_OR,
	[OP_SPECIAL_XOR]	= rec_special_XOR,
	[OP_SPECIAL_NOR]	= rec_special_NOR,
	[OP_SPECIAL_SLT]	= rec_special_SLT,
	[OP_SPECIAL_SLTU]	= rec_special_SLTU,
};

static const lightrec_rec_func_t rec_regimm[64] = {
	SET_DEFAULT_ELM(rec_regimm, unknown_opcode),
	[OP_REGIMM_BLTZ]	= rec_regimm_BLTZ,
	[OP_REGIMM_BGEZ]	= rec_regimm_BGEZ,
	[OP_REGIMM_BLTZAL]	= rec_regimm_BLTZAL,
	[OP_REGIMM_BGEZAL]	= rec_regimm_BGEZAL,
};

static const lightrec_rec_func_t rec_cp0[64] = {
	SET_DEFAULT_ELM(rec_cp0, rec_CP),
	[OP_CP0_MFC0]		= rec_cp0_MFC0,
	[OP_CP0_CFC0]		= rec_cp0_CFC0,
	[OP_CP0_MTC0]		= rec_cp0_MTC0,
	[OP_CP0_CTC0]		= rec_cp0_CTC0,
	[OP_CP0_RFE]		= rec_cp0_RFE,
};

static const lightrec_rec_func_t rec_cp2_basic[64] = {
	SET_DEFAULT_ELM(rec_cp2_basic, rec_CP),
	[OP_CP2_BASIC_MFC2]	= rec_cp2_basic_MFC2,
	[OP_CP2_BASIC_CFC2]	= rec_cp2_basic_CFC2,
	[OP_CP2_BASIC_MTC2]	= rec_cp2_basic_MTC2,
	[OP_CP2_BASIC_CTC2]	= rec_cp2_basic_CTC2,
};

static void rec_SPECIAL(struct lightrec_cstate *state,
			const struct block *block, u16 offset)
{
	union code c = block->opcode_list[offset].c;
	lightrec_rec_func_t f = rec_special[c.r.op];

	if (!HAS_DEFAULT_ELM && unlikely(!f))
		unknown_opcode(state, block, offset);
	else
		(*f)(state, block, offset);
}

static void rec_REGIMM(struct lightrec_cstate *state,
		       const struct block *block, u16 offset)
{
	union code c = block->opcode_list[offset].c;
	lightrec_rec_func_t f = rec_regimm[c.r.rt];

	if (!HAS_DEFAULT_ELM && unlikely(!f))
		unknown_opcode(state, block, offset);
	else
		(*f)(state, block, offset);
}

static void rec_CP0(struct lightrec_cstate *state,
		    const struct block *block, u16 offset)
{
	union code c = block->opcode_list[offset].c;
	lightrec_rec_func_t f = rec_cp0[c.r.rs];

	if (!HAS_DEFAULT_ELM && unlikely(!f))
		rec_CP(state, block, offset);
	else
		(*f)(state, block, offset);
}

static void rec_CP2(struct lightrec_cstate *state,
		    const struct block *block, u16 offset)
{
	union code c = block->opcode_list[offset].c;

	if (c.r.op == OP_CP2_BASIC) {
		lightrec_rec_func_t f = rec_cp2_basic[c.r.rs];

		if (HAS_DEFAULT_ELM || likely(f)) {
			(*f)(state, block, offset);
			return;
		}
	}

	rec_CP(state, block, offset);
}

void lightrec_rec_opcode(struct lightrec_cstate *state,
			 const struct block *block, u16 offset)
{
	struct regcache *reg_cache = state->reg_cache;
	struct lightrec_branch_target *target;
	const struct opcode *op = &block->opcode_list[offset];
	jit_state_t *_jit = block->_jit;
	lightrec_rec_func_t f;
	u16 unload_offset;

	if (op_flag_sync(op->flags)) {
		if (state->cycles)
			jit_subi(LIGHTREC_REG_CYCLE, LIGHTREC_REG_CYCLE, state->cycles);
		state->cycles = 0;

		lightrec_storeback_regs(reg_cache, _jit);
		lightrec_regcache_reset(reg_cache);

		pr_debug("Adding branch target at offset 0x%x\n", offset << 2);
		target = &state->targets[state->nb_targets++];
		target->offset = offset;
		target->label = jit_indirect();
	}

	if (likely(op->opcode)) {
		f = rec_standard[op->i.op];

		if (!HAS_DEFAULT_ELM && unlikely(!f))
			unknown_opcode(state, block, offset);
		else
			(*f)(state, block, offset);
	}

	if (OPT_EARLY_UNLOAD) {
		unload_offset = offset +
			(has_delay_slot(op->c) && !op_flag_no_ds(op->flags));

		lightrec_do_early_unload(state, block, unload_offset);
	}
}
