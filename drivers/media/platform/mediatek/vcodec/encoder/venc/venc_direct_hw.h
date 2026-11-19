/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Common direct-MMIO hardware control layer for MediaTek VENC.
 */

#ifndef _VENC_DIRECT_HW_H_
#define _VENC_DIRECT_HW_H_

#include <linux/io.h>
#include <linux/types.h>

#include "../mtk_vcodec_enc_drv.h"
#include "venc_direct_regs.h"

/**
 * struct venc_direct_hw_cfg - hardware parameters for direct MMIO encoding
 * @reset_assert_us: delay between reset assert and deassert
 * @rc_info_size: size of one RC-info descriptor
 * @rc_code_size: size of one RC-code table
 * @work_align: required alignment of all DMA work buffers
 */
struct venc_direct_hw_cfg {
	unsigned int reset_assert_us;
	size_t rc_info_size;
	size_t rc_code_size;
	size_t work_align;
};

extern const struct venc_direct_hw_cfg venc_direct_gen1_cfg;

static inline void venc_direct_write(struct mtk_vcodec_enc_dev *dev,
				     u32 reg, u32 val)
{
	writel(val, dev->reg_base[VENC_CORE] + reg);
}

static inline u32 venc_direct_read(struct mtk_vcodec_enc_dev *dev, u32 reg)
{
	return readl(dev->reg_base[VENC_CORE] + reg);
}

const struct venc_direct_hw_cfg *
venc_direct_get_cfg(struct mtk_vcodec_enc_ctx *ctx);

int venc_direct_reset_hw(struct mtk_vcodec_enc_ctx *ctx);
int venc_direct_wait_done(struct mtk_vcodec_enc_ctx *ctx, u32 *irq_status,
			  u32 *bs_size);

#endif /* _VENC_DIRECT_HW_H_ */
