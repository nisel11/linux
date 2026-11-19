// SPDX-License-Identifier: GPL-2.0
/*
 * Common direct-MMIO hardware control layer for MediaTek VDEC.
 */

#include <linux/errno.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>

#include "../../common/mtk_vcodec_intr.h"
#include "../../common/mtk_vcodec_util.h"
#include "../mtk_vcodec_dec.h"
#include "vdec_direct_hw.h"
#include "vdec_direct_regs.h"

#define VDEC_DIRECT_POLL_SLEEP_US	1
#define VDEC_DIRECT_RESET_TIMEOUT_US	50000
#define VDEC_DIRECT_FETCH_TIMEOUT_US	10000
#define VDEC_DIRECT_SC_TIMEOUT_US	100000

const struct vdec_direct_hw_cfg vdec_direct_gen1_cfg = {
	.vdecsys_clk_en_reg = VDEC_SYS_CLK_EN,
	.vdecsys_clk_en_val = VDEC_SYS_CLK_EN_VDEC,
	.vdecsys_misc_cfg_reg = VDEC_SYS_MISC_CFG,
	.vdecsys_misc_cfg_val = VDEC_SYS_MISC_CFG_EN,
	.misc_ctrl0_mask = 0xff400000,
	.misc_ctrl0_val = 0x00801ff0,
	.misc_ctrl_c8_val = 0x0000fff7,
	.misc_ctrl_cc_val = 0x51e30182,
	.misc_ctrl_178_val = 0xfffffe60,
	.misc_sram_ctrl_val = 0x0065ff10,
	.misc_sram_ctrl_uhd_val = 0x001eff10,
	.bs_tail_bytes = 0x180,
	.mv_slots = 32,
	.barrel_buf_size = 0x8000,
	.vld_wrap_size = 0xf000,
	.pp_wrap_size = 0x1e000,
	.uhd_pixel_threshold = 0x1fe000,
	.reduced_mv_buf = false,
};

const struct vdec_direct_hw_cfg *
vdec_direct_get_cfg(struct mtk_vcodec_dec_ctx *ctx)
{
	if (ctx->dev->vdec_pdata->direct_hw_cfg)
		return ctx->dev->vdec_pdata->direct_hw_cfg;

	return &vdec_direct_gen1_cfg;
}

static int vdec_direct_wait_vld_nop(struct mtk_vcodec_dec_dev *dev)
{
	u32 val;

	if (!(vdec_direct_read(dev, VDEC_LD, VDEC_LD_PROC_STATUS) &
	      VDEC_LD_PROC_ACTIVE))
		return 0;

	return readl_poll_timeout_atomic(dev->reg_base[VDEC_LD] +
					 VDEC_LD_PROC_STATUS,
					 val, val & VDEC_LD_PROC_NOP,
					 VDEC_DIRECT_POLL_SLEEP_US,
					 VDEC_DIRECT_RESET_TIMEOUT_US);
}

static void vdec_direct_safe_break(struct mtk_vcodec_dec_dev *dev)
{
	u32 val;

	val = vdec_direct_read(dev, VDEC_MISC, VDEC_MISC_SAFE_BREAK_REQ);
	vdec_direct_write(dev, VDEC_MISC, VDEC_MISC_SAFE_BREAK_REQ,
			  val | VDEC_MISC_SAFE_BREAK_REQ_BIT);

	readl_poll_timeout_atomic(dev->reg_base[VDEC_MISC] +
				  VDEC_MISC_SAFE_BREAK_ACK,
				  val,
				  (val & VDEC_MISC_SAFE_BREAK_ACK_MASK) ==
				  VDEC_MISC_SAFE_BREAK_ACK_MASK,
				  VDEC_DIRECT_POLL_SLEEP_US,
				  VDEC_DIRECT_RESET_TIMEOUT_US);
}

int vdec_direct_reset_hw(struct mtk_vcodec_dec_ctx *ctx)
{
	const struct vdec_direct_hw_cfg *cfg = vdec_direct_get_cfg(ctx);
	struct mtk_vcodec_dec_dev *dev = ctx->dev;
	u32 val;
	int ret;

	if (!dev->reg_base[VDEC_MISC] || !dev->reg_base[VDEC_LD] ||
	    !dev->reg_base[VDEC_TOP] || !dev->reg_base[VDEC_CM] ||
	    !dev->reg_base[VDEC_AD] || !dev->reg_base[VDEC_AV] ||
	    !dev->reg_base[VDEC_PP])
		return -ENODEV;

	if (mtk_vcodec_write_vdecsys(ctx, cfg->vdecsys_clk_en_reg,
				     cfg->vdecsys_clk_en_val))
		return -EIO;

	vdec_direct_write(dev, VDEC_MISC, VDEC_MISC_PROCESS_FLAG, 0);
	ret = vdec_direct_wait_vld_nop(dev);
	if (ret)
		vdec_direct_safe_break(dev);

	vdec_direct_write(dev, VDEC_LD, VDEC_LD_RESET, VDEC_LD_RESET_ASSERT);
	vdec_direct_write(dev, VDEC_MISC, VDEC_MISC_IRQ_CLR,
			  VDEC_MISC_IRQ_CLR_VAL);

	val = vdec_direct_read(dev, VDEC_MISC, VDEC_MISC_CTRL0);
	vdec_direct_write(dev, VDEC_MISC, VDEC_MISC_CTRL0,
			  (val & cfg->misc_ctrl0_mask) | cfg->misc_ctrl0_val);
	vdec_direct_write(dev, VDEC_MISC, VDEC_MISC_CTRL_C8,
			  cfg->misc_ctrl_c8_val);
	vdec_direct_write(dev, VDEC_MISC, VDEC_MISC_CTRL_CC,
			  cfg->misc_ctrl_cc_val);
	vdec_direct_write(dev, VDEC_MISC, VDEC_MISC_CTRL_178,
			  cfg->misc_ctrl_178_val);
	vdec_direct_write(dev, VDEC_MISC, VDEC_MISC_IRQ_CLR,
			  VDEC_MISC_IRQ_CLR_VAL);
	vdec_direct_write(dev, VDEC_LD, VDEC_LD_RESET, 0);

	if (mtk_vcodec_write_vdecsys(ctx, cfg->vdecsys_misc_cfg_reg,
				     cfg->vdecsys_misc_cfg_val))
		return -EIO;

	vdec_direct_write(dev, VDEC_MISC, VDEC_MISC_CTRL_EC,
			  VDEC_MISC_CTRL_EC_VAL);
	vdec_direct_write(dev, VDEC_MISC, VDEC_MISC_CTRL_04,
			  VDEC_MISC_CTRL_04_VAL);
	vdec_direct_write(dev, VDEC_MISC, VDEC_MISC_SRAM_CTRL,
			  cfg->misc_sram_ctrl_val);

	return 0;
}

int vdec_direct_vld_init(struct mtk_vcodec_dec_ctx *ctx, dma_addr_t bs_dma,
			 size_t bs_size, dma_addr_t barrel_dma)
{
	const struct vdec_direct_hw_cfg *cfg = vdec_direct_get_cfg(ctx);
	struct mtk_vcodec_dec_dev *dev = ctx->dev;
	u32 start_addr = lower_32_bits(bs_dma);
	u32 end_addr = lower_32_bits(bs_dma + bs_size + cfg->bs_tail_bytes);
	u32 val;
	int ret;

	vdec_direct_write(dev, VDEC_LD, VDEC_LD_MODE_CFG, VDEC_LD_MODE_H264);
	vdec_direct_write(dev, VDEC_AV, VDEC_AV_VLD_CTRL,
			  VDEC_AV_VLD_CTRL_DEFAULT);

	val = vdec_direct_read(dev, VDEC_TOP, VDEC_TOP_CTRL_54);
	vdec_direct_write(dev, VDEC_TOP, VDEC_TOP_CTRL_54,
			  val | VDEC_TOP_CTRL_54_EN);
	vdec_direct_write(dev, VDEC_CM, VDEC_CM_BURST_MODE,
			  VDEC_CM_BURST_MODE_VAL);

	vdec_direct_write(dev, VDEC_LD, VDEC_LD_VFIFO_SA, start_addr >> 6);
	vdec_direct_write(dev, VDEC_LD, VDEC_LD_VFIFO_EA, end_addr >> 6);

	val = vdec_direct_read(dev, VDEC_LD, VDEC_LD_RPTR_CTRL);
	vdec_direct_write(dev, VDEC_LD, VDEC_LD_RPTR_CTRL,
			  val | VDEC_LD_RPTR_RING_EN);

	vdec_direct_write(dev, VDEC_TOP, VDEC_TOP_BARREL_ADDR,
			  lower_32_bits(barrel_dma));
	vdec_direct_write(dev, VDEC_AV, VDEC_AV_BS_RESET, 1);
	vdec_direct_write(dev, VDEC_AV, VDEC_AV_BS_RESET, 0);

	ret = vdec_direct_wait_vld_nop(dev);
	if (ret)
		return ret;

	val = vdec_direct_read(dev, VDEC_LD, VDEC_LD_WPTR);
	vdec_direct_write(dev, VDEC_LD, VDEC_LD_WPTR,
			  val | VDEC_LD_WPTR_INIT_BIT);
	vdec_direct_write(dev, VDEC_LD, VDEC_LD_RPTR, start_addr);
	vdec_direct_write(dev, VDEC_LD, VDEC_LD_RPTR, start_addr);
	vdec_direct_write(dev, VDEC_LD, VDEC_LD_WPTR, end_addr);

	vdec_direct_write(dev, VDEC_AV, VDEC_AV_BS_RESET, 1);
	vdec_direct_write(dev, VDEC_LD, VDEC_LD_RESET, VDEC_LD_RESET_VLD_ONLY);
	vdec_direct_write(dev, VDEC_LD, VDEC_LD_RESET, 0);
	vdec_direct_write(dev, VDEC_LD, VDEC_LD_VLD_CTRL, 0);
	vdec_direct_write(dev, VDEC_LD, VDEC_LD_FETCH_CTRL,
			  VDEC_LD_FETCH_START);

	ret = readl_poll_timeout_atomic(dev->reg_base[VDEC_LD] +
					VDEC_LD_FETCH_STATUS,
					val, val & VDEC_LD_INI_FETCH_RDY,
					VDEC_DIRECT_POLL_SLEEP_US,
					VDEC_DIRECT_FETCH_TIMEOUT_US);
	if (ret)
		return ret;

	vdec_direct_write(dev, VDEC_LD, VDEC_LD_FETCH_CTRL, VDEC_LD_FETCH_DONE);
	vdec_direct_write(dev, VDEC_AV, VDEC_AV_BS_RESET, 0);

	return 0;
}

void vdec_direct_vld_shift(struct mtk_vcodec_dec_ctx *ctx, u32 bit_count)
{
	struct mtk_vcodec_dec_dev *dev = ctx->dev;

	while (bit_count) {
		u32 shift = min(bit_count, 32U);

		vdec_direct_read(dev, VDEC_AV, VDEC_AV_SHIFT_BITS(shift));
		bit_count -= shift;
	}
}

u32 vdec_direct_vld_get_bits(struct mtk_vcodec_dec_ctx *ctx, u32 bit_count)
{
	struct mtk_vcodec_dec_dev *dev = ctx->dev;
	u32 val;

	if (!bit_count || bit_count > 32)
		return 0;

	val = vdec_direct_read(dev, VDEC_AV, VDEC_AV_INPUT_WINDOW);
	vdec_direct_read(dev, VDEC_AV, VDEC_AV_SHIFT_BITS(bit_count));

	return val >> (32U - bit_count);
}

u32 vdec_direct_vld_ue(struct mtk_vcodec_dec_ctx *ctx)
{
	return vdec_direct_read(ctx->dev, VDEC_AV, VDEC_AV_UE_CODE);
}

s32 vdec_direct_vld_se(struct mtk_vcodec_dec_ctx *ctx)
{
	return (s32)vdec_direct_read(ctx->dev, VDEC_AV, VDEC_AV_SE_CODE);
}

int vdec_direct_vld_find_start_code(struct mtk_vcodec_dec_ctx *ctx)
{
	struct mtk_vcodec_dec_dev *dev = ctx->dev;
	u32 win, err_status, sc_ctrl, cfg_2c8;
	int ret;

	win = vdec_direct_read(dev, VDEC_AV, VDEC_AV_INPUT_WINDOW);
	err_status = vdec_direct_read(dev, VDEC_AV, VDEC_AV_VLD_CTRL);
	if ((win & 0xffffff00) != 0x00000100 || (err_status & BIT(31))) {
		vdec_direct_write(dev, VDEC_AV, VDEC_AV_GET_START_CODE,
				  VDEC_AV_GET_START_CODE_TRIG);
		ret = readl_poll_timeout_atomic(dev->reg_base[VDEC_AV] +
						VDEC_AV_GET_START_CODE,
						sc_ctrl,
						!(sc_ctrl & VDEC_AV_GET_START_CODE_TRIG),
						VDEC_DIRECT_POLL_SLEEP_US,
						VDEC_DIRECT_SC_TIMEOUT_US);
		cfg_2c8 = vdec_direct_read(dev, VDEC_AV, VDEC_AV_VLD_CFG_2C8);
		if (ret && !(cfg_2c8 & VDEC_AV_VLD_CFG_2C8_SC_FOUND))
			return -ENODATA;
	}

	vdec_direct_vld_shift(ctx, 24);
	return 0;
}

int vdec_direct_wait_done(struct mtk_vcodec_dec_ctx *ctx)
{
	struct mtk_vcodec_dec_dev *dev = ctx->dev;
	u32 av_err, av_fin, av_status, pic_complete;
	int ret;

	ret = mtk_vcodec_wait_for_done_ctx(ctx, MTK_INST_IRQ_RECEIVED,
					   WAIT_INTR_TIMEOUT_MS, 0);
	av_status = vdec_direct_read(dev, VDEC_AV, VDEC_AV_STATUS);
	av_fin = vdec_direct_read(dev, VDEC_AV, VDEC_AV_FIN);
	av_err = vdec_direct_read(dev, VDEC_AV, VDEC_AV_ERR_STATUS) &
		 vdec_direct_read(dev, VDEC_AV, VDEC_AV_ERR_MASK);
	pic_complete = vdec_direct_read(dev, VDEC_AV, VDEC_AV_PIC_COMPLETE) &
		       VDEC_AV_PIC_COMPLETE_BIT;

	if ((!ret || (av_fin && !(av_status & VDEC_AV_STATUS_BUSY))) &&
	    !(av_err & VDEC_AV_ERR_FATAL_MASK) &&
	    (!av_err || (av_err == BIT(3) && pic_complete)))
		return 0;

	mtk_v4l2_vdec_err(ctx,
			  "direct decode failed (ret=%d): misc_irq=%#x av_status=%#x av_fin=%#x av_err=%#x ld_status=%#x mc=(%u,%u)\n",
			  ret,
			  vdec_direct_read(dev, VDEC_MISC, VDEC_MISC_IRQ_STATUS),
			  av_status,
			  av_fin,
			  av_err,
			  vdec_direct_read(dev, VDEC_LD, VDEC_LD_FETCH_STATUS),
			  vdec_direct_read(dev, VDEC_CM, VDEC_CM_MB_X),
			  vdec_direct_read(dev, VDEC_CM, VDEC_CM_MB_Y));

	vdec_direct_safe_break(dev);
	vdec_direct_reset_hw(ctx);

	return ret ? ret : -EIO;
}
