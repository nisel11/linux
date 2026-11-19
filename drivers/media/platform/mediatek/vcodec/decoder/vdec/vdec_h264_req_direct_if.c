// SPDX-License-Identifier: GPL-2.0
/*
 * Direct-MMIO H.264 stateless decoder interface for MediaTek VDEC.
 */

#include <linux/dma-mapping.h>
#include <linux/errno.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/overflow.h>
#include <linux/slab.h>
#include <linux/swab.h>
#include <media/v4l2-h264.h>
#include <media/videobuf2-dma-contig.h>

#include "../../common/mtk_vcodec_util.h"
#include "../mtk_vcodec_dec.h"
#include "../vdec_drv_base.h"
#include "../vdec_drv_if.h"
#include "vdec_direct_hw.h"
#include "vdec_direct_regs.h"
#include "vdec_h264_req_common.h"

#define H264_NAL_SLICE_MAX		5
#define H264_NAL_IDR			5
#define H264_MAX_MMCO_OPS		32
#define H264_REORDER_TIMEOUT_US		2000
#define H264_WPT_TIMEOUT_US		2000
#define H264_STATUS_TIMEOUT_US		10000
#define H264_PIC_STRUCT_FRAME		3

struct vdec_h264_direct_mv {
	struct mtk_vcodec_mem mem;
	bool initialized;
};

struct vdec_h264_direct_slice_state {
	u8 nal_ref_idc;
	u8 nal_unit_type;
	u32 first_mb_in_slice;
	u32 slice_type;
	u32 frame_num;
	u8 field_pic_flag;
	u8 bottom_field_flag;
	u8 direct_spatial_mv_pred_flag;
	u8 num_ref_idx_l0_active_minus1;
	u8 num_ref_idx_l1_active_minus1;
	u8 cabac_init_idc;
	s32 slice_qp_delta;
	u8 disable_deblocking_filter_idc;
	s8 slice_alpha_c0_offset_div2;
	s8 slice_beta_offset_div2;
};

struct vdec_h264_direct_inst {
	struct mtk_vcodec_dec_ctx *ctx;
	struct mtk_vcodec_mem bs;
	struct mtk_vcodec_mem barrel;
	struct mtk_vcodec_mem vld_wrap;
	struct mtk_vcodec_mem pp_wrap;
	struct mtk_vcodec_mem fb[VIDEO_MAX_FRAME];
	size_t fb_size;
	struct vdec_h264_direct_mv mv[VIDEO_MAX_FRAME];
	size_t mv_size;
	struct vdec_pic_info pic;
	struct v4l2_rect crop;
	u8 *stage_bs;
	size_t stage_len;
	size_t stage_cap;
	unsigned int stage_slices;
	bool decoded_frame;
};

static int vdec_h264_direct_alloc(struct vdec_h264_direct_inst *inst,
				  struct mtk_vcodec_mem *mem, size_t size)
{
	int ret;

	if (mem->size == size)
		return 0;

	if (mem->va)
		mtk_vcodec_mem_free(inst->ctx, mem);

	memset(mem, 0, sizeof(*mem));
	mem->size = size;
	ret = mtk_vcodec_mem_alloc(inst->ctx, mem);
	if (ret) {
		memset(mem, 0, sizeof(*mem));
		return ret;
	}

	return 0;
}

static void vdec_h264_direct_free(struct vdec_h264_direct_inst *inst)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(inst->fb); i++) {
		if (inst->fb[i].va)
			mtk_vcodec_mem_free(inst->ctx, &inst->fb[i]);
	}
	inst->fb_size = 0;
	for (i = 0; i < ARRAY_SIZE(inst->mv); i++) {
		if (inst->mv[i].mem.va)
			mtk_vcodec_mem_free(inst->ctx, &inst->mv[i].mem);
		inst->mv[i].initialized = false;
	}
	if (inst->pp_wrap.va)
		mtk_vcodec_mem_free(inst->ctx, &inst->pp_wrap);
	if (inst->vld_wrap.va)
		mtk_vcodec_mem_free(inst->ctx, &inst->vld_wrap);
	if (inst->barrel.va)
		mtk_vcodec_mem_free(inst->ctx, &inst->barrel);
	if (inst->bs.va)
		mtk_vcodec_mem_free(inst->ctx, &inst->bs);
	kfree(inst->stage_bs);
	inst->stage_bs = NULL;
	inst->stage_len = 0;
	inst->stage_cap = 0;
	inst->stage_slices = 0;
}

static int vdec_h264_direct_init(struct mtk_vcodec_dec_ctx *ctx)
{
	const struct vdec_direct_hw_cfg *cfg = vdec_direct_get_cfg(ctx);
	struct vdec_h264_direct_inst *inst;
	int ret;

	inst = kzalloc(sizeof(*inst), GFP_KERNEL);
	if (!inst)
		return -ENOMEM;

	inst->ctx = ctx;
	ret = vdec_h264_direct_alloc(inst, &inst->barrel, cfg->barrel_buf_size);
	if (!ret)
		ret = vdec_h264_direct_alloc(inst, &inst->vld_wrap,
					     cfg->vld_wrap_size);
	if (!ret)
		ret = vdec_h264_direct_alloc(inst, &inst->pp_wrap,
					     cfg->pp_wrap_size);
	if (ret) {
		vdec_h264_direct_free(inst);
		kfree(inst);
		return ret;
	}

	ctx->drv_handle = inst;
	return 0;
}

static void vdec_h264_direct_deinit(void *handle)
{
	struct vdec_h264_direct_inst *inst = handle;

	vdec_h264_direct_free(inst);
	kfree(inst);
}

static bool vdec_h264_direct_addr_ok(dma_addr_t addr)
{
	return !upper_32_bits(addr) && IS_ALIGNED(addr, 16);
}

static bool vdec_h264_direct_dims_valid(struct mtk_vcodec_dec_ctx *ctx,
					u32 w, u32 h)
{
	u32 max_w = MTK_VDEC_MAX_W;
	u32 max_h = MTK_VDEC_MAX_H;

	if (!(ctx->dev->dec_capability & VCODEC_CAPABILITY_4K_DISABLED)) {
		max_w = VCODEC_DEC_4K_CODED_WIDTH;
		max_h = VCODEC_DEC_4K_CODED_HEIGHT;
	}

	return w >= MTK_VDEC_MIN_W && w <= max_w &&
	       h >= MTK_VDEC_MIN_H && h <= max_h;
}

static int vdec_h264_direct_resize(struct vdec_h264_direct_inst *inst,
				   const struct v4l2_ctrl_h264_sps *sps)
{
	struct vdec_pic_info *p = &inst->pic;
	u32 w = (sps->pic_width_in_mbs_minus1 + 1) * MB_UNIT_LEN;
	u32 h = (sps->pic_height_in_map_units_minus1 + 1) * MB_UNIT_LEN;
	u32 bw = ALIGN(w, 64);
	u32 bh = ALIGN(h, 64);
	size_t fb_size;
	size_t mv_size;
	unsigned int i;

	if (!vdec_h264_direct_dims_valid(inst->ctx, w, h))
		return -EINVAL;

	fb_size = ALIGN((size_t)bw * bh + ((size_t)bw * bh) / 2, 0x1000);
	if (inst->fb_size != fb_size) {
		for (i = 0; i < ARRAY_SIZE(inst->fb); i++) {
			if (inst->fb[i].va) {
				mtk_vcodec_mem_free(inst->ctx, &inst->fb[i]);
				memset(&inst->fb[i], 0, sizeof(inst->fb[i]));
			}
		}
		inst->fb_size = fb_size;
	}

	mv_size = ALIGN((size_t)mtk_vdec_h264_get_mv_buf_size(bw, bh), 0x1000);
	if (inst->mv_size != mv_size) {
		for (i = 0; i < ARRAY_SIZE(inst->mv); i++) {
			if (inst->mv[i].mem.va) {
				mtk_vcodec_mem_free(inst->ctx, &inst->mv[i].mem);
				memset(&inst->mv[i].mem, 0,
				       sizeof(inst->mv[i].mem));
			}
			inst->mv[i].initialized = false;
		}
		inst->mv_size = mv_size;
	}

	p->pic_w = w;
	p->pic_h = h;
	p->buf_w = bw;
	p->buf_h = bh;
	p->fb_sz[0] = bw * bh;
	p->fb_sz[1] = p->fb_sz[0] / 2;
	p->cap_fourcc = V4L2_PIX_FMT_MM21;
	inst->crop.left = 0;
	inst->crop.top = 0;
	inst->crop.width = w;
	inst->crop.height = h;

	return 0;
}

static int vdec_h264_direct_get_mv(struct vdec_h264_direct_inst *inst,
				   unsigned int slot_idx, bool reset,
				   dma_addr_t *mv_addr)
{
	struct vdec_h264_direct_mv *slot;
	dma_addr_t limit;
	int ret;

	if (slot_idx >= ARRAY_SIZE(inst->mv))
		return -EINVAL;

	slot = &inst->mv[slot_idx];
	if (!slot->mem.va) {
		ret = vdec_h264_direct_alloc(inst, &slot->mem, inst->mv_size);
		if (ret)
			return ret;
		slot->initialized = false;
	}

	if (!vdec_h264_direct_addr_ok(slot->mem.dma_addr) ||
	    check_add_overflow(slot->mem.dma_addr, (dma_addr_t)inst->mv_size,
			       &limit) ||
	    upper_32_bits(limit))
		return -ERANGE;

	if (reset || !slot->initialized) {
		memset(slot->mem.va, 0, inst->mv_size);
		slot->initialized = true;
	}

	*mv_addr = slot->mem.dma_addr;
	return 0;
}

/*
 * MT6768 VDEC_CM reads reference chroma at REF_Y + VDEC_CM_CHROMA_OFFSET
 * instead of VDEC_CM_*_REF_C, so non-contiguous MM21 capture buffers need a
 * per-slot contiguous bounce buffer where C == Y + fb_sz[0].
 */
static int vdec_h264_direct_get_fb(struct vdec_h264_direct_inst *inst,
				   unsigned int slot_idx, dma_addr_t raw_y,
				   dma_addr_t raw_c, dma_addr_t *hw_y,
				   dma_addr_t *hw_c, bool *use_bounce)
{
	struct mtk_vcodec_mem *slot;
	dma_addr_t limit;
	int ret;

	if (raw_c == raw_y + inst->pic.fb_sz[0]) {
		*hw_y = raw_y;
		*hw_c = raw_c;
		*use_bounce = false;
		return 0;
	}

	if (slot_idx >= ARRAY_SIZE(inst->fb))
		return -EINVAL;

	slot = &inst->fb[slot_idx];
	if (!slot->va) {
		ret = vdec_h264_direct_alloc(inst, slot, inst->fb_size);
		if (ret)
			return ret;
	}

	if (!vdec_h264_direct_addr_ok(slot->dma_addr) ||
	    !IS_ALIGNED(slot->dma_addr, 512) ||
	    check_add_overflow(slot->dma_addr, (dma_addr_t)inst->fb_size,
			       &limit) ||
	    upper_32_bits(limit))
		return -ERANGE;

	*hw_y = slot->dma_addr;
	*hw_c = slot->dma_addr + inst->pic.fb_sz[0];
	*use_bounce = true;
	return 0;
}

static bool vdec_h264_find_vcl_nal(const u8 *data, size_t len, size_t *offset)
{
	size_t i = 0;

	while (i + 3 < len) {
		size_t prefix = 0;

		if (!data[i] && !data[i + 1] && data[i + 2] == 1)
			prefix = 3;
		else if (i + 4 < len && !data[i] && !data[i + 1] &&
			 !data[i + 2] && data[i + 3] == 1)
			prefix = 4;

		if (prefix) {
			u8 nal_type = data[i + prefix] & 0x1f;

			if (nal_type >= 1 && nal_type <= H264_NAL_SLICE_MAX) {
				*offset = i + prefix;
				return true;
			}
			i += prefix + 1;
			continue;
		}
		i++;
	}

	return false;
}

static void vdec_h264_write_scaling_list(struct mtk_vcodec_dec_dev *dev,
					 u32 list_idx, const u8 *list)
{
	u32 base, len, i;

	if (list_idx < 6) {
		base = list_idx * 16;
		len = 16;
	} else {
		base = (list_idx == 6) ? 0x60 : (list_idx * 16 + 0x30);
		len = 64;
	}

	for (i = 0; i < len; i += 4) {
		u32 word;

		memcpy(&word, list + i, sizeof(word));
		vdec_direct_write(dev, VDEC_LD, VDEC_LD_SCL_ADDR,
				  base + i + VDEC_LD_SCL_ADDR_WRITE);
		vdec_direct_write(dev, VDEC_LD, VDEC_LD_SCL_DATA, swab32(word));
		vdec_direct_write(dev, VDEC_LD, VDEC_LD_SCL_ADDR,
				  base + i + VDEC_LD_SCL_ADDR_LATCH);
	}
}

static void vdec_h264_set_sps_avld(struct mtk_vcodec_dec_dev *dev,
				   const struct v4l2_ctrl_h264_sps *sps)
{
	u32 reg;

	reg = VDEC_AV_SPS_ENABLE_BIT | VDEC_AV_SPS_CHROMA_420_BIT |
	      ((sps->flags & V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY) ?
	       VDEC_AV_SPS_FRAME_MBS_ONLY : 0) |
	      ((sps->log2_max_frame_num_minus4 & 0xf) <<
	       VDEC_AV_SPS_LOG2_FRAME_NUM_SHIFT) |
	      ((sps->pic_order_cnt_type & 3) << VDEC_AV_SPS_POC_TYPE_SHIFT) |
	      ((sps->log2_max_pic_order_cnt_lsb_minus4 & 0xf) <<
	       VDEC_AV_SPS_LOG2_POC_LSB_SHIFT) |
	      ((sps->flags & V4L2_H264_SPS_FLAG_DELTA_PIC_ORDER_ALWAYS_ZERO) ?
	       VDEC_AV_SPS_DELTA_POC_ZERO_BIT : 0) |
	      ((sps->max_num_ref_frames & 0x1f) << VDEC_AV_SPS_MAX_REF_SHIFT) |
	      ((sps->flags & V4L2_H264_SPS_FLAG_DIRECT_8X8_INFERENCE) ?
	       VDEC_AV_SPS_DIRECT_8X8_BIT : 0);

	vdec_direct_write(dev, VDEC_AV, VDEC_AV_SPS_CFG, reg);
	vdec_direct_write(dev, VDEC_TOP, VDEC_TOP_PIC_MB_SIZE,
			  sps->pic_width_in_mbs_minus1 |
			  (sps->pic_height_in_map_units_minus1 << 16));
	vdec_direct_write(dev, VDEC_TOP, VDEC_TOP_BIT_DEPTH,
			  (sps->bit_depth_luma_minus8 & 0xf) |
			  ((sps->bit_depth_chroma_minus8 & 0xf) << 4));
}

static void vdec_h264_set_pps_avld(struct mtk_vcodec_dec_dev *dev,
				   const struct v4l2_ctrl_h264_pps *pps,
				   const struct v4l2_ctrl_h264_scaling_matrix *sm)
{
	u32 pps0, pps1;
	u32 i;

	pps0 = ((pps->flags & V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE) ?
		VDEC_AV_PPS_CABAC_BIT : 0) |
	       ((pps->flags &
		 V4L2_H264_PPS_FLAG_BOTTOM_FIELD_PIC_ORDER_IN_FRAME_PRESENT) ?
		VDEC_AV_PPS_BOTTOM_FIELD_POC_BIT : 0) |
	       ((pps->flags & V4L2_H264_PPS_FLAG_WEIGHTED_PRED) ?
		VDEC_AV_PPS_WEIGHTED_PRED_BIT : 0) |
	       ((pps->weighted_bipred_idc & 3) <<
		VDEC_AV_PPS_WEIGHTED_BIPRED_SHIFT) |
	       ((pps->pic_init_qp_minus26 & 0x3f) <<
		VDEC_AV_PPS_INIT_QP_SHIFT) |
	       ((pps->chroma_qp_index_offset & 0x1f) <<
		VDEC_AV_PPS_CHROMA_QP_SHIFT) |
	       ((pps->flags &
		 V4L2_H264_PPS_FLAG_DEBLOCKING_FILTER_CONTROL_PRESENT) ?
		VDEC_AV_PPS_DEBLOCK_CTRL_BIT : 0) |
	       ((pps->flags & V4L2_H264_PPS_FLAG_CONSTRAINED_INTRA_PRED) ?
		VDEC_AV_PPS_CONSTRAINED_INTRA_BIT : 0) |
	       ((pps->flags & V4L2_H264_PPS_FLAG_TRANSFORM_8X8_MODE) ?
		VDEC_AV_PPS_TRANSFORM_8X8_BIT : 0) |
	       ((pps->second_chroma_qp_index_offset & 0x1f) <<
		VDEC_AV_PPS_SECOND_CHROMA_QP_SHIFT);

	vdec_direct_write(dev, VDEC_AV, VDEC_AV_PPS_0, pps0);

	pps1 = (pps->num_ref_idx_l0_default_active_minus1 & 0x1f) |
	       ((pps->num_ref_idx_l1_default_active_minus1 & 0x1f) <<
		VDEC_AV_PPS_1_L1_SHIFT);

	if ((pps->flags & V4L2_H264_PPS_FLAG_SCALING_MATRIX_PRESENT) && sm) {
		for (i = 0; i < 6; i++)
			vdec_h264_write_scaling_list(dev, i,
						     sm->scaling_list_4x4[i]);
		for (i = 0; i < 2; i++)
			vdec_h264_write_scaling_list(dev, 6 + i,
						     sm->scaling_list_8x8[i]);

		pps1 |= VDEC_AV_PPS_1_SCL_LIST_MASK |
			VDEC_AV_PPS_1_SCL_PRESENT_BIT;
	}

	vdec_direct_write(dev, VDEC_AV, VDEC_AV_PPS_1, pps1);
}

static u32 vdec_h264_pack_mv_poc(s32 poc, s32 other_poc, bool is_bot,
				  bool is_long_term)
{
	u32 val = (poc & VDEC_AD_POC_MASK) |
		  (H264_PIC_STRUCT_FRAME << VDEC_AD_MV_FRAME_STRUCT_SHIFT);

	if (is_bot)
		val |= VDEC_AD_MV_BOT_FIELD_BIT;
	if (is_long_term)
		val |= VDEC_AD_MV_LONG_TERM_BIT;
	if (poc != S32_MAX && !(other_poc < poc))
		val |= VDEC_AD_MV_TOP_GE_BOT_BIT;

	return val;
}

static bool vdec_h264_lookup_dpb_ref(struct vdec_h264_direct_inst *inst,
				     const struct v4l2_ctrl_h264_decode_params *dec,
				     u8 dpb_idx, dma_addr_t *y_addr,
				     dma_addr_t *c_addr, dma_addr_t *mv_addr,
				     u32 *pic_num, s32 *top_poc, s32 *bot_poc,
				     bool *is_lt)
{
	struct vb2_queue *vq;
	struct vb2_buffer *vb;
	const struct v4l2_h264_dpb_entry *entry;

	if (dpb_idx >= V4L2_H264_NUM_DPB_ENTRIES)
		return false;

	entry = &dec->dpb[dpb_idx];
	if (!(entry->flags & V4L2_H264_DPB_ENTRY_FLAG_ACTIVE))
		return false;

	vq = v4l2_m2m_get_vq(inst->ctx->m2m_ctx,
			     V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
	vb = vb2_find_buffer(vq, entry->reference_ts);
	if (!vb)
		return false;

	*y_addr = vb2_dma_contig_plane_dma_addr(vb, 0);
	*c_addr = vb2_dma_contig_plane_dma_addr(vb, 1);
	if (*c_addr != *y_addr + inst->pic.fb_sz[0]) {
		if (vb->index >= ARRAY_SIZE(inst->fb) ||
		    !inst->fb[vb->index].va)
			return false;
		*y_addr = inst->fb[vb->index].dma_addr;
		*c_addr = *y_addr + inst->pic.fb_sz[0];
	}
	if (!vdec_h264_direct_addr_ok(*y_addr) ||
	    !vdec_h264_direct_addr_ok(*c_addr))
		return false;

	if (mv_addr &&
	    vdec_h264_direct_get_mv(inst, vb->index, false, mv_addr))
		return false;

	*pic_num = entry->pic_num;
	*top_poc = entry->top_field_order_cnt;
	*bot_poc = entry->bottom_field_order_cnt;
	*is_lt = !!(entry->flags & V4L2_H264_DPB_ENTRY_FLAG_LONG_TERM);

	return true;
}

static void vdec_h264_modify_ref_list(struct mtk_vcodec_dec_ctx *ctx,
				      const struct v4l2_ctrl_h264_sps *sps,
				      const struct v4l2_ctrl_h264_decode_params *dec,
				      const struct vdec_h264_direct_slice_state *sh,
				      struct v4l2_h264_reference *list,
				      u32 num_valid, u32 num_active)
{
	u32 max_pic_num, curr_pic_num, pic_num_pred, ref_idx_lx = 0;
	u32 list_len = min_t(u32, max(num_valid, num_active),
			     V4L2_H264_REF_LIST_LEN - 1);
	u32 i;

	if (!vdec_direct_vld_get_bits(ctx, 1))
		return;

	max_pic_num = 1U << (sps->log2_max_frame_num_minus4 + 4 +
			     (sh->field_pic_flag ? 1 : 0));
	curr_pic_num = sh->field_pic_flag ? (2 * sh->frame_num + 1) :
					    sh->frame_num;
	pic_num_pred = curr_pic_num;

	for (i = 0; i < V4L2_H264_REF_LIST_LEN; i++) {
		u32 op = vdec_direct_vld_ue(ctx);
		struct v4l2_h264_reference target = { .index = 0xff };
		bool found = false;
		u32 j, c_idx, n_idx;

		if (op == 3)
			break;

		if (op == 0 || op == 1) {
			u32 diff = vdec_direct_vld_ue(ctx) + 1;
			s32 pic_num_no_wrap;
			s32 pic_num;

			if (op == 0) {
				pic_num_no_wrap = (s32)pic_num_pred - (s32)diff;
				if (pic_num_no_wrap < 0)
					pic_num_no_wrap += max_pic_num;
			} else {
				pic_num_no_wrap = (s32)pic_num_pred + (s32)diff;
				if (pic_num_no_wrap >= (s32)max_pic_num)
					pic_num_no_wrap -= max_pic_num;
			}
			pic_num_pred = pic_num_no_wrap;
			pic_num = (pic_num_no_wrap > (s32)curr_pic_num) ?
				  pic_num_no_wrap - (s32)max_pic_num :
				  pic_num_no_wrap;

			for (j = 0; j < V4L2_H264_NUM_DPB_ENTRIES; j++) {
				const struct v4l2_h264_dpb_entry *e =
					&dec->dpb[j];

				if (!(e->flags &
				      V4L2_H264_DPB_ENTRY_FLAG_ACTIVE) ||
				    (e->flags &
				     V4L2_H264_DPB_ENTRY_FLAG_LONG_TERM))
					continue;
				if ((s32)e->pic_num == pic_num) {
					target.index = j;
					target.fields = V4L2_H264_FRAME_REF;
					found = true;
					break;
				}
			}
		} else if (op == 2) {
			u32 lt_pic_num = vdec_direct_vld_ue(ctx);

			for (j = 0; j < V4L2_H264_NUM_DPB_ENTRIES; j++) {
				const struct v4l2_h264_dpb_entry *e =
					&dec->dpb[j];

				if (!(e->flags &
				      V4L2_H264_DPB_ENTRY_FLAG_ACTIVE) ||
				    !(e->flags &
				      V4L2_H264_DPB_ENTRY_FLAG_LONG_TERM))
					continue;
				if (e->pic_num == lt_pic_num) {
					target.index = j;
					target.fields = V4L2_H264_FRAME_REF;
					found = true;
					break;
				}
			}
		}

		if (!found || ref_idx_lx >= num_active)
			continue;

		for (c_idx = list_len; c_idx > ref_idx_lx; c_idx--)
			list[c_idx] = list[c_idx - 1];
		list[ref_idx_lx++] = target;
		n_idx = ref_idx_lx;
		for (c_idx = ref_idx_lx; c_idx <= list_len; c_idx++) {
			if (list[c_idx].index != target.index ||
			    list[c_idx].fields != target.fields)
				list[n_idx++] = list[c_idx];
		}
	}
}

static int vdec_h264_set_ref_lists(struct vdec_h264_direct_inst *inst,
				   const struct v4l2_ctrl_h264_sps *sps,
				   const struct v4l2_ctrl_h264_decode_params *dec,
				   const struct vdec_h264_direct_slice_state *sh,
				   dma_addr_t cur_y, dma_addr_t cur_c,
				   dma_addr_t cur_mv)
{
	struct mtk_vcodec_dec_ctx *ctx = inst->ctx;
	struct mtk_vcodec_dec_dev *dev = ctx->dev;
	const struct vdec_direct_hw_cfg *cfg = vdec_direct_get_cfg(ctx);
	struct v4l2_h264_reflist_builder reflist_builder;
	struct v4l2_h264_reference p0_list[V4L2_H264_REF_LIST_LEN];
	struct v4l2_h264_reference b0_list[V4L2_H264_REF_LIST_LEN];
	struct v4l2_h264_reference b1_list[V4L2_H264_REF_LIST_LEN];
	u32 slice_type = sh->slice_type % 5;
	u32 mv_step = cfg->reduced_mv_buf ? 2 : 4;
	u32 val, i;
	int ret;

	vdec_direct_write(dev, VDEC_CM, VDEC_CM_P_REF_LT_MASK, 0);
	vdec_direct_write(dev, VDEC_AV, VDEC_AV_REF_LIST_INIT,
			  VDEC_AV_REF_LIST_INIT_TRIG);
	ret = readl_poll_timeout_atomic(dev->reg_base[VDEC_AV] +
					VDEC_AV_REF_LIST_INIT,
					val, !(val & VDEC_AV_REF_LIST_INIT_TRIG),
					1, H264_STATUS_TIMEOUT_US);
	if (ret)
		return ret;

	vdec_direct_write(dev, VDEC_AV, VDEC_AV_MAX_PIC_NUM,
			  BIT(sps->log2_max_frame_num_minus4 + 4 +
			      (sh->field_pic_flag ? 1 : 0)));
	vdec_direct_write(dev, VDEC_AV, VDEC_AV_CURR_PIC_NUM,
			  sh->field_pic_flag ? (2 * sh->frame_num + 1) :
					       sh->frame_num);

	for (i = 0; i < 32; i++) {
		vdec_direct_write(dev, VDEC_CM, VDEC_CM_P_REF_Y(i),
				  lower_32_bits(cur_y));
		vdec_direct_write(dev, VDEC_CM, VDEC_CM_P_REF_C(i),
				  lower_32_bits(cur_c));
		vdec_direct_write(dev, VDEC_AD, VDEC_AD_P_REF_POC(i), 0);
		vdec_direct_write(dev, VDEC_CM, VDEC_CM_B_L0_REF_Y(i),
				  lower_32_bits(cur_y));
		vdec_direct_write(dev, VDEC_CM, VDEC_CM_B_L1_REF_Y(i),
				  lower_32_bits(cur_y));
		vdec_direct_write(dev, VDEC_CM, VDEC_CM_B_L0_REF_C(i),
				  lower_32_bits(cur_c));
		vdec_direct_write(dev, VDEC_CM, VDEC_CM_B_L1_REF_C(i),
				  lower_32_bits(cur_c));
		vdec_direct_write(dev, VDEC_AD, VDEC_AD_B_L0_REF_POC(i), 0);
		vdec_direct_write(dev, VDEC_AD, VDEC_AD_B_L1_REF_POC(i), 0);
		vdec_direct_write(dev, VDEC_AD, VDEC_AD_REF_MV_ADDR(i),
				  lower_32_bits(cur_mv) >> 4);
	}
	vdec_direct_write(dev, VDEC_CM, VDEC_CM_B_L0_REF_MASK, 0);
	vdec_direct_write(dev, VDEC_CM, VDEC_CM_B_L1_REF_MASK, 0);

	vdec_direct_write(dev, VDEC_AD, VDEC_AD_CURR_POC,
			  dec->top_field_order_cnt & VDEC_AD_POC_MASK);
	vdec_direct_write(dev, VDEC_AD, VDEC_AD_TOP_POC,
			  dec->top_field_order_cnt & VDEC_AD_POC_MASK);
	vdec_direct_write(dev, VDEC_AD, VDEC_AD_BOT_POC,
			  dec->bottom_field_order_cnt & VDEC_AD_POC_MASK);

	if (slice_type == V4L2_H264_SLICE_TYPE_I ||
	    slice_type == V4L2_H264_SLICE_TYPE_SI)
		return 0;

	v4l2_h264_init_reflist_builder(&reflist_builder, dec, sps, dec->dpb);

	if (slice_type == V4L2_H264_SLICE_TYPE_P ||
	    slice_type == V4L2_H264_SLICE_TYPE_SP) {
		u32 active0 = sh->num_ref_idx_l0_active_minus1 + 1;
		u32 count = min_t(u32,
				  max_t(u32, reflist_builder.num_valid,
					active0),
				  16);

		memset(p0_list, 0xff, sizeof(p0_list));
		v4l2_h264_build_p_ref_list(&reflist_builder, p0_list);
		vdec_h264_modify_ref_list(ctx, sps, dec, sh, p0_list,
					  reflist_builder.num_valid, active0);
		for (i = 0; i < count; i++) {
			dma_addr_t ry, rc;
			u32 pic_num;
			s32 tp, bp;
			bool is_lt;

			if (!vdec_h264_lookup_dpb_ref(inst, dec,
						      p0_list[i].index,
						      &ry, &rc, NULL, &pic_num,
						      &tp, &bp, &is_lt))
				continue;

			vdec_direct_write(dev, VDEC_CM, VDEC_CM_P_REF_Y(i),
					  lower_32_bits(ry));
			vdec_direct_write(dev, VDEC_CM, VDEC_CM_P_REF_C(i),
					  lower_32_bits(rc));
			vdec_direct_write(dev, VDEC_AV, VDEC_AV_P_PIC_NUM(i),
					  (pic_num & VDEC_AV_PIC_NUM_MASK) |
					  (is_lt ? VDEC_AV_PIC_NUM_LT_BIT : 0));
			vdec_direct_write(dev, VDEC_AD,
					  VDEC_AD_P_REF_POC(i * 2),
					  vdec_h264_pack_mv_poc(tp, bp, false,
								is_lt));
			vdec_direct_write(dev, VDEC_AD,
					  VDEC_AD_P_REF_POC(i * 2 + 1),
					  vdec_h264_pack_mv_poc(bp, tp, true,
								is_lt));
		}
		return 0;
	}

	/* B-slice: program reordered L0 and L1 lists */
	{
		u32 active0 = sh->num_ref_idx_l0_active_minus1 + 1;
		u32 active1 = sh->num_ref_idx_l1_active_minus1 + 1;
		u32 count0 = min_t(u32,
				   max_t(u32, reflist_builder.num_valid,
					 active0),
				   16);
		u32 count1 = min_t(u32,
				   max_t(u32, reflist_builder.num_valid,
					 active1),
				   16);

		memset(b0_list, 0xff, sizeof(b0_list));
		memset(b1_list, 0xff, sizeof(b1_list));
		v4l2_h264_build_b_ref_lists(&reflist_builder, b0_list, b1_list);
		vdec_h264_modify_ref_list(ctx, sps, dec, sh, b0_list,
					  reflist_builder.num_valid, active0);
		vdec_h264_modify_ref_list(ctx, sps, dec, sh, b1_list,
					  reflist_builder.num_valid, active1);

		for (i = 0; i < count0; i++) {
			dma_addr_t ry, rc;
			u32 pic_num;
			s32 tp, bp;
			bool is_lt;

			if (!vdec_h264_lookup_dpb_ref(inst, dec,
						      b0_list[i].index,
						      &ry, &rc, NULL, &pic_num,
						      &tp, &bp, &is_lt))
				continue;

			vdec_direct_write(dev, VDEC_CM, VDEC_CM_B_L0_REF_Y(i),
					  lower_32_bits(ry));
			vdec_direct_write(dev, VDEC_CM, VDEC_CM_B_L0_REF_C(i),
					  lower_32_bits(rc));
			vdec_direct_write(dev, VDEC_AV, VDEC_AV_B_L0_PIC_NUM(i),
					  (pic_num & VDEC_AV_PIC_NUM_MASK) |
					  (is_lt ? VDEC_AV_PIC_NUM_LT_BIT : 0));
			vdec_direct_write(dev, VDEC_AD,
					  VDEC_AD_B_L0_REF_POC(i * 2),
					  vdec_h264_pack_mv_poc(tp, bp, false,
								is_lt));
			vdec_direct_write(dev, VDEC_AD,
					  VDEC_AD_B_L0_REF_POC(i * 2 + 1),
					  vdec_h264_pack_mv_poc(bp, tp, true,
								is_lt));
		}

		for (i = 0; i < count1; i++) {
			dma_addr_t ry, rc, rmv;
			u32 pic_num;
			s32 tp, bp;
			bool is_lt;

			if (!vdec_h264_lookup_dpb_ref(inst, dec,
						      b1_list[i].index,
						      &ry, &rc, &rmv, &pic_num,
						      &tp, &bp, &is_lt))
				continue;

			vdec_direct_write(dev, VDEC_CM, VDEC_CM_B_L1_REF_Y(i),
					  lower_32_bits(ry));
			vdec_direct_write(dev, VDEC_CM, VDEC_CM_B_L1_REF_C(i),
					  lower_32_bits(rc));
			vdec_direct_write(dev, VDEC_AV, VDEC_AV_B_L1_PIC_NUM(i),
					  (pic_num & VDEC_AV_PIC_NUM_MASK) |
					  (is_lt ? VDEC_AV_PIC_NUM_LT_BIT : 0));
			vdec_direct_write(dev, VDEC_AD,
					  VDEC_AD_B_L1_REF_POC(i * 2),
					  vdec_h264_pack_mv_poc(tp, bp, false,
								is_lt));
			vdec_direct_write(dev, VDEC_AD,
					  VDEC_AD_B_L1_REF_POC(i * 2 + 1),
					  vdec_h264_pack_mv_poc(bp, tp, true,
								is_lt));
			vdec_direct_write(dev, VDEC_AD,
					  VDEC_AD_REF_MV_ADDR(i * 2),
					  lower_32_bits(rmv) >> 4);
			vdec_direct_write(dev, VDEC_AD,
					  VDEC_AD_REF_MV_ADDR(i * 2 + 1),
					  (lower_32_bits(rmv) >> 4) + mv_step);
		}
	}

	return 0;
}

static void vdec_h264_set_shdr_avld1(struct mtk_vcodec_dec_dev *dev,
				     const struct vdec_h264_direct_slice_state *sh)
{
	u32 reg;

	reg = vdec_direct_read(dev, VDEC_AV, VDEC_AV_SHDR_0);
	vdec_direct_write(dev, VDEC_AV, VDEC_AV_SHDR_0,
			  (reg & VDEC_AV_SHDR_0_MASK) |
			  (sh->first_mb_in_slice & 0x1fff));

	reg = vdec_direct_read(dev, VDEC_AV, VDEC_AV_SHDR_1);
	reg &= VDEC_AV_SHDR_1_KEEP_MASK;
	reg |= ((sh->slice_type & 0xf) << VDEC_AV_SHDR_1_SLICE_TYPE_SHIFT) |
	       (sh->field_pic_flag ? VDEC_AV_SHDR_1_FIELD_PIC_BIT : 0) |
	       (sh->bottom_field_flag ? VDEC_AV_SHDR_1_BOT_FIELD_BIT : 0) |
	       (sh->direct_spatial_mv_pred_flag ?
		VDEC_AV_SHDR_1_DIRECT_SPATIAL_BIT : 0) |
	       ((sh->num_ref_idx_l0_active_minus1 & 0x1f) <<
		VDEC_AV_SHDR_1_L0_ACTIVE_SHIFT) |
	       ((sh->num_ref_idx_l1_active_minus1 & 0x1f) <<
		VDEC_AV_SHDR_1_L1_ACTIVE_SHIFT);
	vdec_direct_write(dev, VDEC_AV, VDEC_AV_SHDR_1, reg);
}

static int vdec_h264_hw_pred_weight_table(struct mtk_vcodec_dec_dev *dev)
{
	u32 misc_f0, val;
	int ret;

	misc_f0 = vdec_direct_read(dev, VDEC_MISC, VDEC_MISC_WPT_CTRL_F0);
	vdec_direct_write(dev, VDEC_AV, VDEC_AV_PRED_WEIGHT,
			  VDEC_AV_PRED_WEIGHT_TRIG);
	ret = readl_poll_timeout_atomic(dev->reg_base[VDEC_AV] +
					VDEC_AV_PRED_WEIGHT,
					val, val & VDEC_AV_PRED_WEIGHT_TRIG,
					1, H264_WPT_TIMEOUT_US);
	vdec_direct_write(dev, VDEC_AV, VDEC_AV_WPT_CTRL_F0,
			  misc_f0 & ~BIT(0));
	return ret;
}

static int vdec_h264_parse_slice_hw(struct vdec_h264_direct_inst *inst,
				    const struct v4l2_ctrl_h264_sps *sps,
				    const struct v4l2_ctrl_h264_pps *pps,
				    const struct v4l2_ctrl_h264_decode_params *dec,
				    struct vdec_h264_direct_slice_state *sh,
				    dma_addr_t cur_y, dma_addr_t cur_c,
				    dma_addr_t cur_mv)
{
	struct mtk_vcodec_dec_ctx *ctx = inst->ctx;
	struct mtk_vcodec_dec_dev *dev = ctx->dev;
	u8 nal_hdr;
	u32 slice_type;
	int ret;

	nal_hdr = vdec_direct_vld_get_bits(ctx, 8);
	sh->nal_ref_idc = (nal_hdr >> 5) & 3;
	sh->nal_unit_type = nal_hdr & 0x1f;

	sh->first_mb_in_slice = vdec_direct_vld_ue(ctx);
	sh->slice_type = vdec_direct_vld_ue(ctx);
	if (sh->slice_type > 9)
		return -EINVAL;
	slice_type = sh->slice_type % 5;

	vdec_direct_vld_ue(ctx); /* pic_parameter_set_id */
	if (sps->flags & V4L2_H264_SPS_FLAG_SEPARATE_COLOUR_PLANE)
		vdec_direct_vld_get_bits(ctx, 2);

	sh->frame_num = vdec_direct_vld_get_bits(ctx,
						 sps->log2_max_frame_num_minus4 + 4);
	if (!(sps->flags & V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY)) {
		sh->field_pic_flag = vdec_direct_vld_get_bits(ctx, 1);
		if (sh->field_pic_flag)
			sh->bottom_field_flag = vdec_direct_vld_get_bits(ctx, 1);
	}
	if (sh->nal_unit_type == H264_NAL_IDR)
		vdec_direct_vld_ue(ctx); /* idr_pic_id */

	if (sps->pic_order_cnt_type == 0) {
		vdec_direct_vld_get_bits(ctx,
					 sps->log2_max_pic_order_cnt_lsb_minus4 + 4);
		if ((pps->flags &
		     V4L2_H264_PPS_FLAG_BOTTOM_FIELD_PIC_ORDER_IN_FRAME_PRESENT) &&
		    !sh->field_pic_flag)
			vdec_direct_vld_se(ctx);
	} else if (sps->pic_order_cnt_type == 1 &&
		   !(sps->flags &
		     V4L2_H264_SPS_FLAG_DELTA_PIC_ORDER_ALWAYS_ZERO)) {
		vdec_direct_vld_se(ctx);
		if ((pps->flags &
		     V4L2_H264_PPS_FLAG_BOTTOM_FIELD_PIC_ORDER_IN_FRAME_PRESENT) &&
		    !sh->field_pic_flag)
			vdec_direct_vld_se(ctx);
	}

	if (pps->flags & V4L2_H264_PPS_FLAG_REDUNDANT_PIC_CNT_PRESENT)
		vdec_direct_vld_ue(ctx);

	if (slice_type == V4L2_H264_SLICE_TYPE_B)
		sh->direct_spatial_mv_pred_flag = vdec_direct_vld_get_bits(ctx, 1);

	sh->num_ref_idx_l0_active_minus1 =
		pps->num_ref_idx_l0_default_active_minus1;
	sh->num_ref_idx_l1_active_minus1 =
		pps->num_ref_idx_l1_default_active_minus1;
	if (slice_type == V4L2_H264_SLICE_TYPE_P ||
	    slice_type == V4L2_H264_SLICE_TYPE_SP ||
	    slice_type == V4L2_H264_SLICE_TYPE_B) {
		if (vdec_direct_vld_get_bits(ctx, 1)) {
			sh->num_ref_idx_l0_active_minus1 =
				min_t(u32, vdec_direct_vld_ue(ctx), 31);
			if (slice_type == V4L2_H264_SLICE_TYPE_B)
				sh->num_ref_idx_l1_active_minus1 =
					min_t(u32, vdec_direct_vld_ue(ctx), 31);
		}
	}
	if (slice_type != V4L2_H264_SLICE_TYPE_B)
		sh->num_ref_idx_l1_active_minus1 = 0;

	ret = vdec_h264_set_ref_lists(inst, sps, dec, sh, cur_y, cur_c, cur_mv);
	if (ret)
		return ret;

	vdec_h264_set_shdr_avld1(dev, sh);

	if (((pps->flags & V4L2_H264_PPS_FLAG_WEIGHTED_PRED) &&
	     (slice_type == V4L2_H264_SLICE_TYPE_P ||
	      slice_type == V4L2_H264_SLICE_TYPE_SP)) ||
	    (pps->weighted_bipred_idc == 1 &&
	     slice_type == V4L2_H264_SLICE_TYPE_B)) {
		ret = vdec_h264_hw_pred_weight_table(dev);
		if (ret)
			return ret;
	}

	if (sh->nal_ref_idc) {
		if (sh->nal_unit_type == H264_NAL_IDR) {
			vdec_direct_vld_get_bits(ctx, 2);
		} else if (vdec_direct_vld_get_bits(ctx, 1)) {
			unsigned int op;

			for (op = 0; op < H264_MAX_MMCO_OPS; op++) {
				u32 mmco = vdec_direct_vld_ue(ctx);

				if (!mmco)
					break;
				if (mmco == 1 || mmco == 2 || mmco == 4 ||
				    mmco == 6)
					vdec_direct_vld_ue(ctx);
				else if (mmco == 3) {
					vdec_direct_vld_ue(ctx);
					vdec_direct_vld_ue(ctx);
				}
			}
		}
	}

	if ((pps->flags & V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE) &&
	    slice_type != V4L2_H264_SLICE_TYPE_I &&
	    slice_type != V4L2_H264_SLICE_TYPE_SI)
		sh->cabac_init_idc = vdec_direct_vld_ue(ctx) & 3;

	sh->slice_qp_delta = vdec_direct_vld_se(ctx);
	if (slice_type == V4L2_H264_SLICE_TYPE_SP ||
	    slice_type == V4L2_H264_SLICE_TYPE_SI) {
		if (slice_type == V4L2_H264_SLICE_TYPE_SP)
			vdec_direct_vld_get_bits(ctx, 1);
		vdec_direct_vld_se(ctx); /* slice_qs_delta */
	}

	if (pps->flags & V4L2_H264_PPS_FLAG_DEBLOCKING_FILTER_CONTROL_PRESENT) {
		sh->disable_deblocking_filter_idc = vdec_direct_vld_ue(ctx);
		if (sh->disable_deblocking_filter_idc != 1) {
			sh->slice_alpha_c0_offset_div2 = vdec_direct_vld_se(ctx);
			sh->slice_beta_offset_div2 = vdec_direct_vld_se(ctx);
		}
	}

	return 0;
}

static int vdec_h264_dec_start(struct vdec_h264_direct_inst *inst,
			       const struct v4l2_ctrl_h264_sps *sps,
			       const struct v4l2_ctrl_h264_pps *pps,
			       const struct vdec_h264_direct_slice_state *sh,
			       dma_addr_t y, dma_addr_t c, dma_addr_t mv,
			       dma_addr_t fb_min, dma_addr_t fb_max,
			       dma_addr_t mv_min, dma_addr_t mv_max,
			       bool is_first_slice)
{
	struct mtk_vcodec_dec_ctx *ctx = inst->ctx;
	struct mtk_vcodec_dec_dev *dev = ctx->dev;
	const struct vdec_direct_hw_cfg *cfg = vdec_direct_get_cfg(ctx);
	u32 width = inst->pic.pic_w;
	u32 height = inst->pic.pic_h;
	u32 mb_stride = (inst->pic.buf_w + 15) >> 4;
	u32 val;
	int ret;

	if (is_first_slice) {
		vdec_direct_write(dev, VDEC_CM, VDEC_CM_PIC_STRUCT, 4);
		vdec_direct_write(dev, VDEC_TOP, VDEC_TOP_CTRL_58, 0xffffffff);
		vdec_direct_write(dev, VDEC_CM, VDEC_CM_PIC_WIDTH, width);
		vdec_direct_write(dev, VDEC_CM, VDEC_CM_PIC_HEIGHT, height);
		vdec_direct_write(dev, VDEC_MISC, VDEC_MISC_CTRL_04,
				  VDEC_MISC_CTRL_04_VAL);
		vdec_direct_write(dev, VDEC_TOP, VDEC_TOP_PIC_SIZE,
				  width | (height << 16));
		vdec_direct_write(dev, VDEC_AV, VDEC_AV_ERR_MASK,
				  VDEC_AV_ERR_MASK_DEFAULT);
		vdec_direct_write(dev, VDEC_AV, VDEC_AV_ERR_CFG,
				  VDEC_AV_ERR_MASK_DEFAULT);
		vdec_direct_write(dev, VDEC_CM, VDEC_CM_MM_CFG,
				  VDEC_CM_MM_CFG_EN);
		vdec_direct_write(dev, VDEC_CM, VDEC_CM_CHROMA_OFFSET,
				  inst->pic.fb_sz[0]);
		vdec_direct_write(dev, VDEC_CM, VDEC_CM_LUMA_BASE,
				  lower_32_bits(y) >> 9);
		vdec_direct_write(dev, VDEC_CM, VDEC_CM_CHROMA_BASE,
				  lower_32_bits(c) >> 8);
		vdec_direct_write(dev, VDEC_CM, VDEC_CM_MC_CFG_250,
				  VDEC_CM_MC_CFG_250_VAL);
		vdec_direct_write(dev, VDEC_CM, VDEC_CM_MC_CFG_238,
				  VDEC_CM_MC_CFG_238_VAL);
		vdec_direct_write(dev, VDEC_CM, VDEC_CM_MB_X_MAX,
				  ((width + 15) >> 4) - 1);
		vdec_direct_write(dev, VDEC_CM, VDEC_CM_MB_Y_MAX,
				  ((height + 15) >> 4) - 1);
		vdec_direct_write(dev, VDEC_CM, VDEC_CM_STRIDE_MB, mb_stride);

		vdec_direct_write(dev, VDEC_CM, VDEC_CM_FB_MAX_ADDR,
				  lower_32_bits(fb_max) >> 4);
		vdec_direct_write(dev, VDEC_CM, VDEC_CM_FB_MIN_ADDR,
				  lower_32_bits(fb_min) >> 4);
		vdec_direct_write(dev, VDEC_CM, VDEC_CM_FB_BOUND_EN, 1);

		vdec_direct_write(dev, VDEC_CM, VDEC_CM_MC_CTRL_5E4,
				  VDEC_CM_MC_CTRL_5E4_INIT);
		vdec_direct_write(dev, VDEC_CM, VDEC_CM_CURR_Y_ADDR,
				  lower_32_bits(y));
		vdec_direct_write(dev, VDEC_CM, VDEC_CM_PP_STRIDE_MB,
				  mb_stride);
		vdec_direct_write(dev, VDEC_AV, VDEC_AV_IRQ_MASK_2B4,
				  0xffffffff);
		vdec_direct_write(dev, VDEC_AV, VDEC_AV_IRQ_MASK_2B8,
				  0xffffffff);
		vdec_direct_write(dev, VDEC_AD, VDEC_AD_CURR_MV_ADDR,
				  lower_32_bits(mv) >> 4);
	}

	vdec_direct_write(dev, VDEC_AV, VDEC_AV_VLD_CFG_2C8,
			  VDEC_AV_VLD_CFG_2C8_VAL);
	vdec_direct_write(dev, VDEC_AV, VDEC_AV_VLD_CFG_2D0,
			  VDEC_AV_VLD_CFG_2D0_INIT);

	vdec_direct_write(dev, VDEC_AV, VDEC_AV_SHDR_2,
			  (sh->slice_qp_delta & 0x7f) |
			  ((sh->disable_deblocking_filter_idc & 3) <<
			   VDEC_AV_SHDR_2_DEBLOCK_IDC_SHIFT) |
			  ((sh->slice_alpha_c0_offset_div2 & 0xf) <<
			   VDEC_AV_SHDR_2_ALPHA_SHIFT) |
			  ((sh->slice_beta_offset_div2 & 0xf) <<
			   VDEC_AV_SHDR_2_BETA_SHIFT) |
			  ((sh->nal_ref_idc & 3) <<
			   VDEC_AV_SHDR_2_NAL_REF_IDC_SHIFT) |
			  ((sh->nal_unit_type == H264_NAL_IDR) ?
			   VDEC_AV_SHDR_2_FRAME_BIT : 0) |
			  ((sh->cabac_init_idc & 3) <<
			   VDEC_AV_SHDR_2_CABAC_INIT_SHIFT));

	vdec_direct_write(dev, VDEC_AV, VDEC_AV_CABAC_EN,
			  (pps->flags & V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE) ?
			  VDEC_AV_CABAC_EN_BIT : 0);

	ret = readl_poll_timeout_atomic(dev->reg_base[VDEC_AV] + VDEC_AV_STATUS,
					val, !(val & VDEC_AV_STATUS_BUSY),
					1, H264_STATUS_TIMEOUT_US);
	if (ret)
		return ret;

	if (is_first_slice) {
		val = vdec_direct_read(dev, VDEC_CM, VDEC_CM_MC_CTRL_5E4);
		vdec_direct_write(dev, VDEC_CM, VDEC_CM_MC_CTRL_5E4,
				  val | VDEC_CM_MC_CTRL_5E4_START);
		val = vdec_direct_read(dev, VDEC_CM, VDEC_CM_MC_CTRL_920);
		vdec_direct_write(dev, VDEC_CM, VDEC_CM_MC_CTRL_920,
				  val | VDEC_CM_MC_CTRL_920_START);
		val = vdec_direct_read(dev, VDEC_CM, VDEC_CM_MC_CTRL_768);
		vdec_direct_write(dev, VDEC_CM, VDEC_CM_MC_CTRL_768,
				  (val & VDEC_CM_MC_CTRL_768_MASK) | 4);

		vdec_direct_write(dev, VDEC_TOP, VDEC_TOP_PIC_TOTAL_MBS,
				  (sps->pic_width_in_mbs_minus1 + 1) *
				  (sps->pic_height_in_map_units_minus1 + 1));
		vdec_direct_write(dev, VDEC_TOP, VDEC_TOP_IRQ_MASK, 0xffffffff);
		vdec_direct_write(dev, VDEC_CM, VDEC_CM_VLD_WRAP_ADDR,
				  lower_32_bits(inst->vld_wrap.dma_addr));
		vdec_direct_write(dev, VDEC_CM, VDEC_CM_PP_WRAP_ADDR,
				  lower_32_bits(inst->pp_wrap.dma_addr));
	}

	vdec_direct_write(dev, VDEC_AV, VDEC_AV_VLD_CFG_2D0,
			  VDEC_AV_VLD_CFG_2D0_DEC);
	vdec_direct_write(dev, VDEC_AV, VDEC_AV_VLD_CFG_2D4,
			  VDEC_AV_VLD_CFG_2D4_VAL);

	if (is_first_slice) {
		val = vdec_direct_read(dev, VDEC_TOP, VDEC_TOP_TIMEOUT_CTRL);
		vdec_direct_write(dev, VDEC_TOP, VDEC_TOP_TIMEOUT_CTRL,
				  (val & VDEC_TOP_TIMEOUT_MASK) |
				  VDEC_TOP_TIMEOUT_VAL);

		if (width * height > cfg->uhd_pixel_threshold) {
			vdec_direct_write(dev, VDEC_MISC, VDEC_MISC_SRAM_CTRL,
					  cfg->misc_sram_ctrl_uhd_val);
			val = vdec_direct_read(dev, VDEC_PP, VDEC_PP_CTRL_B80);
			vdec_direct_write(dev, VDEC_PP, VDEC_PP_CTRL_B80,
					  val | VDEC_PP_CTRL_B80_UHD_BIT);
		}

		val = vdec_direct_read(dev, VDEC_MISC, VDEC_MISC_ADDR_HIGH_EN);
		vdec_direct_write(dev, VDEC_MISC, VDEC_MISC_ADDR_HIGH_EN,
				  val | VDEC_MISC_ADDR_HIGH_EN_BIT);
		vdec_direct_write(dev, VDEC_MISC, VDEC_MISC_LUMA_ADDR,
				  lower_32_bits(y));
		vdec_direct_write(dev, VDEC_MISC, VDEC_MISC_CHROMA_ADDR,
				  lower_32_bits(c));
		vdec_direct_write(dev, VDEC_MISC, VDEC_MISC_MV_MIN_ADDR,
				  lower_32_bits(mv_min));
		vdec_direct_write(dev, VDEC_MISC, VDEC_MISC_MV_MAX_ADDR,
				  lower_32_bits(mv_max));
	}

	vdec_direct_write(dev, VDEC_AV, VDEC_AV_VLD_CTRL,
			  VDEC_AV_VLD_CTRL_DEFAULT);

	vdec_direct_write(dev, VDEC_AV, VDEC_AV_DEC_START,
			  VDEC_AV_DEC_START_TRIG);
	return 0;
}

static int vdec_h264_direct_stage_slice(struct vdec_h264_direct_inst *inst,
					const u8 *nal, size_t nal_len)
{
	size_t prefix_len = inst->stage_slices ? 4 : 0;
	size_t add_len, needed;
	u8 *buf;

	if (check_add_overflow(nal_len, prefix_len, &add_len) ||
	    check_add_overflow(inst->stage_len, add_len, &needed))
		return -EOVERFLOW;

	if (needed > inst->stage_cap) {
		size_t new_cap = max(needed, inst->stage_cap ?
				     inst->stage_cap * 2 : (size_t)SZ_64K);

		buf = krealloc(inst->stage_bs, new_cap, GFP_KERNEL);
		if (!buf)
			return -ENOMEM;
		inst->stage_bs = buf;
		inst->stage_cap = new_cap;
	}

	if (prefix_len) {
		static const u8 start_code[4] = { 0x00, 0x00, 0x00, 0x01 };

		memcpy(inst->stage_bs + inst->stage_len, start_code,
		       sizeof(start_code));
		inst->stage_len += sizeof(start_code);
	}
	memcpy(inst->stage_bs + inst->stage_len, nal, nal_len);
	inst->stage_len += nal_len;
	inst->stage_slices++;

	return 0;
}

static int vdec_h264_direct_decode(void *handle, struct mtk_vcodec_mem *bs,
				   struct vdec_fb *fb, bool *res_chg)
{
	struct vdec_h264_direct_inst *inst = handle;
	struct mtk_vcodec_dec_ctx *ctx = inst->ctx;
	struct mtk_vcodec_dec_dev *dev = ctx->dev;
	const struct vdec_direct_hw_cfg *cfg = vdec_direct_get_cfg(ctx);
	const struct v4l2_ctrl_h264_sps *sps;
	const struct v4l2_ctrl_h264_pps *pps;
	const struct v4l2_ctrl_h264_decode_params *dec;
	const struct v4l2_ctrl_h264_slice_params *slice_ctrl;
	const struct v4l2_ctrl_h264_scaling_matrix *sm = NULL;
	struct v4l2_ctrl *start_code_ctrl;
	struct mtk_video_dec_buf *dst, *src;
	struct vdec_h264_direct_slice_state sh = {};
	struct vdec_pic_info old_pic = inst->pic;
	u32 capture_fourcc = ctx->q_data[MTK_Q_DATA_DST].fmt ?
		ctx->q_data[MTK_Q_DATA_DST].fmt->fourcc : 0;
	dma_addr_t y, y_end, c, c_end, hw_y, hw_c, fb_min, fb_max;
	dma_addr_t mv, mv_min, mv_max, mv_limit, bs_end;
	size_t nal_offset = 0, bs_len, total_len, copy_size;
	bool is_idr, is_first_slice, is_held, use_bounce = false;
	u32 start_code = V4L2_STATELESS_H264_START_CODE_ANNEX_B;
	unsigned int i;
	int ret;

	if (!res_chg)
		return -EINVAL;
	*res_chg = false;
	if (!bs)
		return 0;
	if (!bs->va || !bs->size || capture_fourcc != V4L2_PIX_FMT_MM21)
		return -EINVAL;

	if (!fb)
		fb = ctx->dev->vdec_pdata->get_cap_buffer(ctx);
	if (!fb)
		return -ENOMEM;

	sps = mtk_vdec_h264_get_ctrl_ptr(ctx, V4L2_CID_STATELESS_H264_SPS);
	pps = mtk_vdec_h264_get_ctrl_ptr(ctx, V4L2_CID_STATELESS_H264_PPS);
	dec = mtk_vdec_h264_get_ctrl_ptr(ctx,
					 V4L2_CID_STATELESS_H264_DECODE_PARAMS);
	slice_ctrl = mtk_vdec_h264_get_ctrl_ptr(ctx,
						V4L2_CID_STATELESS_H264_SLICE_PARAMS);
	if (IS_ERR(sps) || IS_ERR(pps) || IS_ERR(dec) || IS_ERR(slice_ctrl))
		return -EOPNOTSUPP;

	if (sps->chroma_format_idc != 1 || sps->bit_depth_luma_minus8 ||
	    sps->bit_depth_chroma_minus8 ||
	    !(sps->flags & V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY) ||
	    (sps->flags & V4L2_H264_SPS_FLAG_MB_ADAPTIVE_FRAME_FIELD) ||
	    pps->num_slice_groups_minus1)
		return -EOPNOTSUPP;

	if (pps->flags & V4L2_H264_PPS_FLAG_SCALING_MATRIX_PRESENT) {
		sm = mtk_vdec_h264_get_ctrl_ptr(ctx,
						V4L2_CID_STATELESS_H264_SCALING_MATRIX);
		if (IS_ERR(sm))
			return -EOPNOTSUPP;
	}

	start_code_ctrl = v4l2_ctrl_find(&ctx->ctrl_hdl,
					 V4L2_CID_STATELESS_H264_START_CODE);
	if (start_code_ctrl)
		start_code = start_code_ctrl->val;

	if (start_code == V4L2_STATELESS_H264_START_CODE_ANNEX_B) {
		if (!vdec_h264_find_vcl_nal(bs->va, bs->size, &nal_offset))
			return -ENODATA;
	}

	bs_len = bs->size - nal_offset;
	if (!bs_len)
		return -EINVAL;

	ret = vdec_h264_direct_resize(inst, sps);
	if (ret)
		return ret;

	*res_chg = old_pic.pic_w && (old_pic.pic_w != inst->pic.pic_w ||
				     old_pic.pic_h != inst->pic.pic_h);

	y = fb->base_y.dma_addr;
	c = fb->base_c.dma_addr;
	if (!vdec_h264_direct_addr_ok(y) || !vdec_h264_direct_addr_ok(c) ||
	    !IS_ALIGNED(y, 512) || !IS_ALIGNED(c, 512) ||
	    upper_32_bits(inst->barrel.dma_addr) ||
	    upper_32_bits(inst->vld_wrap.dma_addr) ||
	    upper_32_bits(inst->pp_wrap.dma_addr) ||
	    check_add_overflow(y, (dma_addr_t)inst->pic.fb_sz[0], &y_end) ||
	    upper_32_bits(y_end) ||
	    check_add_overflow(c, (dma_addr_t)inst->pic.fb_sz[1], &c_end) ||
	    upper_32_bits(c_end))
		return -ERANGE;

	if (fb->base_y.size < inst->pic.fb_sz[0] ||
	    fb->base_c.size < inst->pic.fb_sz[1])
		return -ENOSPC;

	dst = container_of(fb, struct mtk_video_dec_buf, frame_buffer);
	src = container_of(bs, struct mtk_video_dec_buf, bs_buffer);

	is_idr = !!(dec->flags & V4L2_H264_DECODE_PARAM_FLAG_IDR_PIC);
	is_first_slice = !slice_ctrl->first_mb_in_slice;
	is_held = !!(src->m2m_buf.vb.flags & V4L2_BUF_FLAG_M2M_HOLD_CAPTURE_BUF);

	if (is_first_slice) {
		inst->stage_len = 0;
		inst->stage_slices = 0;
		v4l2_m2m_buf_copy_metadata(&src->m2m_buf.vb, &dst->m2m_buf.vb);
	}

	if (is_held)
		return vdec_h264_direct_stage_slice(inst,
						    (const u8 *)bs->va + nal_offset,
						    bs_len);

	if (inst->stage_slices) {
		ret = vdec_h264_direct_stage_slice(inst,
						   (const u8 *)bs->va + nal_offset,
						   bs_len);
		if (ret) {
			inst->stage_len = 0;
			inst->stage_slices = 0;
			return ret;
		}
		total_len = inst->stage_len;
	} else {
		total_len = bs_len;
	}

	ret = vdec_h264_direct_get_fb(inst, dst->m2m_buf.vb.vb2_buf.index,
				      y, c, &hw_y, &hw_c, &use_bounce);
	if (ret) {
		inst->stage_len = 0;
		inst->stage_slices = 0;
		return ret;
	}
	if (use_bounce && (!fb->base_y.va || !fb->base_c.va)) {
		inst->stage_len = 0;
		inst->stage_slices = 0;
		return -EINVAL;
	}

	ret = vdec_h264_direct_get_mv(inst, dst->m2m_buf.vb.vb2_buf.index,
				      is_idr, &mv);
	if (ret) {
		inst->stage_len = 0;
		inst->stage_slices = 0;
		return ret;
	}

	mv_min = mv;
	if (check_add_overflow(mv, (dma_addr_t)inst->mv_size, &mv_max) ||
	    upper_32_bits(mv_max)) {
		inst->stage_len = 0;
		inst->stage_slices = 0;
		return -ERANGE;
	}
	for (i = 0; i < ARRAY_SIZE(inst->mv); i++) {
		dma_addr_t addr = inst->mv[i].mem.dma_addr;

		if (!inst->mv[i].mem.va)
			continue;
		if (check_add_overflow(addr, (dma_addr_t)inst->mv_size,
				       &mv_limit) ||
		    upper_32_bits(mv_limit)) {
			inst->stage_len = 0;
			inst->stage_slices = 0;
			return -ERANGE;
		}
		mv_min = min(mv_min, addr);
		mv_max = max(mv_max, mv_limit);
	}

	if (check_add_overflow(total_len, cfg->bs_tail_bytes, &copy_size)) {
		inst->stage_len = 0;
		inst->stage_slices = 0;
		return -EOVERFLOW;
	}

	ret = vdec_h264_direct_alloc(inst, &inst->bs, ALIGN(copy_size, 64));
	if (ret) {
		inst->stage_len = 0;
		inst->stage_slices = 0;
		return ret;
	}

	if (upper_32_bits(inst->bs.dma_addr) ||
	    !IS_ALIGNED(inst->bs.dma_addr, 64) ||
	    check_add_overflow(inst->bs.dma_addr, (dma_addr_t)copy_size,
			       &bs_end) ||
	    upper_32_bits(bs_end)) {
		inst->stage_len = 0;
		inst->stage_slices = 0;
		return -ERANGE;
	}

	if (inst->stage_slices) {
		memcpy(inst->bs.va, inst->stage_bs, total_len);
		inst->stage_len = 0;
		inst->stage_slices = 0;
	} else {
		memcpy(inst->bs.va, (const u8 *)bs->va + nal_offset, total_len);
	}
	memset((u8 *)inst->bs.va + total_len, 0, inst->bs.size - total_len);

	if (check_add_overflow(hw_y, (dma_addr_t)inst->pic.fb_sz[0], &y_end) ||
	    upper_32_bits(y_end) ||
	    check_add_overflow(hw_c, (dma_addr_t)inst->pic.fb_sz[1], &c_end) ||
	    upper_32_bits(c_end))
		return -ERANGE;

	fb_min = min(hw_y, hw_c);
	fb_max = max(y_end, c_end);
	for (i = 0; i < ARRAY_SIZE(dec->dpb); i++) {
		dma_addr_t ry, rc, r_y_end, r_c_end;
		u32 pic_num;
		s32 tp, bp;
		bool is_lt;

		if (!vdec_h264_lookup_dpb_ref(inst, dec, i, &ry, &rc, NULL,
					      &pic_num, &tp, &bp, &is_lt))
			continue;
		if (check_add_overflow(ry, (dma_addr_t)inst->pic.fb_sz[0],
				       &r_y_end) ||
		    upper_32_bits(r_y_end) ||
		    check_add_overflow(rc, (dma_addr_t)inst->pic.fb_sz[1],
				       &r_c_end) ||
		    upper_32_bits(r_c_end))
			return -ERANGE;
		fb_min = min3(fb_min, ry, rc);
		fb_max = max3(fb_max, r_y_end, r_c_end);
	}

	memset(inst->barrel.va, 0, inst->barrel.size);
	memset(inst->vld_wrap.va, 0, inst->vld_wrap.size);
	memset(inst->pp_wrap.va, 0, inst->pp_wrap.size);

	ret = vdec_direct_reset_hw(ctx);
	if (ret)
		return ret;

	ret = vdec_direct_vld_init(ctx, inst->bs.dma_addr, total_len,
				   inst->barrel.dma_addr);
	if (ret)
		return ret;

	vdec_h264_set_sps_avld(dev, sps);
	vdec_h264_set_pps_avld(dev, pps, sm);

	ret = vdec_h264_parse_slice_hw(inst, sps, pps, dec, &sh,
				       hw_y, hw_c, mv);
	if (ret)
		return ret;

	ret = vdec_h264_dec_start(inst, sps, pps, &sh, hw_y, hw_c, mv,
				  fb_min, fb_max, mv_min, mv_max, true);
	if (ret)
		return ret;

	ret = vdec_direct_wait_done(ctx);
	if (ret)
		return ret;

	if (use_bounce) {
		const u8 *src_va =
			inst->fb[dst->m2m_buf.vb.vb2_buf.index].va;

		memcpy(fb->base_y.va, src_va, inst->pic.fb_sz[0]);
		memcpy(fb->base_c.va, src_va + inst->pic.fb_sz[0],
		       inst->pic.fb_sz[1]);
	}
	inst->decoded_frame = true;
	return 0;
}

static int vdec_h264_direct_get_param(void *handle,
				      enum vdec_get_param_type type, void *out)
{
	struct vdec_h264_direct_inst *inst = handle;

	switch (type) {
	case GET_PARAM_PIC_INFO: {
		struct vdec_pic_info *pic = out;

		if (!inst->decoded_frame) {
			u32 width = pic->pic_w;
			u32 height = pic->pic_h;

			if (!vdec_h264_direct_dims_valid(inst->ctx, width,
							 height))
				return -EINVAL;

			inst->pic.pic_w = width;
			inst->pic.pic_h = height;
			inst->pic.buf_w = ALIGN(width, 64);
			inst->pic.buf_h = ALIGN(height, 64);
			inst->pic.fb_sz[0] = inst->pic.buf_w * inst->pic.buf_h;
			inst->pic.fb_sz[1] = inst->pic.fb_sz[0] / 2;
			inst->pic.cap_fourcc = V4L2_PIX_FMT_MM21;
			inst->crop.width = width;
			inst->crop.height = height;
		}
		*pic = inst->pic;
		return 0;
	}
	case GET_PARAM_CROP_INFO:
		*(struct v4l2_rect *)out = inst->crop;
		return 0;
	case GET_PARAM_DPB_SIZE:
		*(unsigned int *)out = V4L2_H264_NUM_DPB_ENTRIES;
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

const struct vdec_common_if vdec_h264_direct_if = {
	.init = vdec_h264_direct_init,
	.decode = vdec_h264_direct_decode,
	.get_param = vdec_h264_direct_get_param,
	.deinit = vdec_h264_direct_deinit,
};
