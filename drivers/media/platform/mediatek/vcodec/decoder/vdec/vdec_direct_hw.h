/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Common direct-MMIO hardware control interface for MediaTek VDEC.
 */

#ifndef _VDEC_DIRECT_HW_H_
#define _VDEC_DIRECT_HW_H_

#include <linux/io.h>
#include <linux/types.h>

#include "../mtk_vcodec_dec_drv.h"

/**
 * struct vdec_direct_hw_cfg - hardware-generation parameters for direct MMIO
 * @vdecsys_clk_en_reg: VDEC_SYS clock enable register offset
 * @vdecsys_clk_en_val: value written to enable VDEC core clock
 * @vdecsys_misc_cfg_reg: VDEC_SYS misc config register offset
 * @vdecsys_misc_cfg_val: value written to configure VDEC_SYS misc
 * @misc_ctrl0_mask: preserved bits mask for VDEC_MISC_CTRL0
 * @misc_ctrl0_val: configured bits for VDEC_MISC_CTRL0
 * @misc_ctrl_c8_val: value for VDEC_MISC_CTRL_C8
 * @misc_ctrl_cc_val: value for VDEC_MISC_CTRL_CC
 * @misc_ctrl_178_val: value for VDEC_MISC_CTRL_178
 * @misc_sram_ctrl_val: default value for VDEC_MISC_SRAM_CTRL
 * @misc_sram_ctrl_uhd_val: UHD (>1080p) value for VDEC_MISC_SRAM_CTRL
 * @bs_tail_bytes: padding tail bytes appended to bitstream ring buffer
 * @mv_slots: maximum MV buffer slots
 * @barrel_buf_size: barrel shifter scratch buffer size
 * @vld_wrap_size: VLD wrapper scratch buffer size
 * @pp_wrap_size: PP wrapper scratch buffer size
 * @uhd_pixel_threshold: pixel count above which UHD SRAM/PP settings apply
 * @reduced_mv_buf: whether reduced MV buffer spacing (2 vs 4) is active
 */
struct vdec_direct_hw_cfg {
	u32 vdecsys_clk_en_reg;
	u32 vdecsys_clk_en_val;
	u32 vdecsys_misc_cfg_reg;
	u32 vdecsys_misc_cfg_val;
	u32 misc_ctrl0_mask;
	u32 misc_ctrl0_val;
	u32 misc_ctrl_c8_val;
	u32 misc_ctrl_cc_val;
	u32 misc_ctrl_178_val;
	u32 misc_sram_ctrl_val;
	u32 misc_sram_ctrl_uhd_val;
	size_t bs_tail_bytes;
	unsigned int mv_slots;
	size_t barrel_buf_size;
	size_t vld_wrap_size;
	size_t pp_wrap_size;
	u32 uhd_pixel_threshold;
	bool reduced_mv_buf;
};

extern const struct vdec_direct_hw_cfg vdec_direct_gen1_cfg;

static inline void vdec_direct_write(struct mtk_vcodec_dec_dev *dev,
				     unsigned int bank, u32 reg, u32 val)
{
	writel(val, dev->reg_base[bank] + reg);
}

static inline u32 vdec_direct_read(struct mtk_vcodec_dec_dev *dev,
				   unsigned int bank, u32 reg)
{
	return readl(dev->reg_base[bank] + reg);
}

const struct vdec_direct_hw_cfg *
vdec_direct_get_cfg(struct mtk_vcodec_dec_ctx *ctx);

int vdec_direct_reset_hw(struct mtk_vcodec_dec_ctx *ctx);
int vdec_direct_vld_init(struct mtk_vcodec_dec_ctx *ctx, dma_addr_t bs_dma,
			 size_t bs_size, dma_addr_t barrel_dma);
void vdec_direct_vld_shift(struct mtk_vcodec_dec_ctx *ctx, u32 bit_count);
u32 vdec_direct_vld_get_bits(struct mtk_vcodec_dec_ctx *ctx, u32 bit_count);
u32 vdec_direct_vld_ue(struct mtk_vcodec_dec_ctx *ctx);
s32 vdec_direct_vld_se(struct mtk_vcodec_dec_ctx *ctx);
int vdec_direct_vld_find_start_code(struct mtk_vcodec_dec_ctx *ctx);
int vdec_direct_wait_done(struct mtk_vcodec_dec_ctx *ctx);

#endif /* _VDEC_DIRECT_HW_H_ */
