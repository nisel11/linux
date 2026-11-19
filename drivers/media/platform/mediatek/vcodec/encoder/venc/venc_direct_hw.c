// SPDX-License-Identifier: GPL-2.0
/*
 * Common direct-MMIO hardware control layer for MediaTek VENC.
 */

#include <linux/errno.h>
#include <linux/kernel.h>

#include "../../common/mtk_vcodec_intr.h"
#include "../../common/mtk_vcodec_util.h"
#include "venc_direct_hw.h"
#include "venc_direct_regs.h"

const struct venc_direct_hw_cfg venc_direct_gen1_cfg = {
	.reset_assert_us = 10,
	/* H264_AllocWorkBuf(): rRCInfo is 0x800 and rRCCode is 0x8040. */
	.rc_info_size = 0x800,
	.rc_code_size = 0x8040,
	.work_align = 0x80,
};

const struct venc_direct_hw_cfg *
venc_direct_get_cfg(struct mtk_vcodec_enc_ctx *ctx)
{
	return &venc_direct_gen1_cfg;
}

int venc_direct_reset_hw(struct mtk_vcodec_enc_ctx *ctx)
{
	struct mtk_vcodec_enc_dev *dev = ctx->dev;

	if (!dev->reg_base[VENC_SYS] || !dev->reg_base[VENC_CORE])
		return -ENODEV;

	/* H264_ResetVENC() in the vendor HAL: 0 -> readback -> 1 -> readback. */
	venc_direct_write(dev, VENC_SYS_SW_RESET, 0);
	(void)venc_direct_read(dev, VENC_SYS_SW_RESET);
	venc_direct_write(dev, VENC_SYS_SW_RESET, 1);
	(void)venc_direct_read(dev, VENC_SYS_SW_RESET);

	/* The HAL's reset routine is only a write/readback pair; no explicit
	 * polling was recovered for it.  Read the status register once so the
	 * reset write cannot be reordered past the following setup writes.
	 */
	(void)venc_direct_read(dev, VENC_SYS_STATUS);

	venc_direct_write(dev, VENC_SYS_ENABLE, 0);
	venc_direct_write(dev, VENC_SYS_IRQ_ACK, VENC_SYS_IRQ_ALL);

	return 0;
}

int venc_direct_wait_done(struct mtk_vcodec_enc_ctx *ctx, u32 *irq_status,
			  u32 *bs_size)
{
	struct mtk_vcodec_enc_dev *dev = ctx->dev;
	u32 status;
	int ret;

	ret = mtk_vcodec_wait_for_done_ctx(ctx, MTK_INST_IRQ_RECEIVED,
					   WAIT_INTR_TIMEOUT_MS, 0);
	if (ret) {
		venc_direct_write(dev, VENC_SYS_ENABLE, 0);
		venc_direct_write(dev, VENC_SYS_IRQ_ACK, VENC_SYS_IRQ_ALL);
		return -ETIMEDOUT;
	}

	/* The shared VENC IRQ handler captures and acknowledges the status. */
	status = ctx->irq_status;
	if (!(status & (VENC_SYS_IRQ_DONE | VENC_SYS_IRQ_BS_FULL))) {
		venc_direct_write(dev, VENC_SYS_ENABLE, 0);
		return -EIO;
	}

	if (irq_status)
		*irq_status = status;

	/* H264_EncodeFrameEnc_ReadResult(): byte count lives in 0x98 and the
	 * HAL clears HW enable (0xec) after reading it.
	 */
	if (bs_size)
		*bs_size = venc_direct_read(dev, VENC_SYS_BITSTREAM_BYTE_CNT);
	venc_direct_write(dev, VENC_SYS_ENABLE, 0);
	if (status & VENC_SYS_IRQ_BS_FULL)
		return -ENOSPC;

	return 0;
}
