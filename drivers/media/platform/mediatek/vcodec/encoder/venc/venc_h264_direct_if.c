// SPDX-License-Identifier: GPL-2.0
/*
 * Direct-MMIO H.264 stateless-style encoder interface for MediaTek VENC.
 *
 * The vendor H.264 firmware is replaced by direct register programming
 * following libvcodecdrv.so.c (H264_SeqLvlMMRSetting, H264_FrmLvlMMRSetting,
 * H264_EncodeFrameEnc_SetRC/SetBufs/SetMV, H264_SetHWModeSel,
 * H264_EncodeFrameEnc_TriggerHW and H264_EncodeFrameEnc_ReadResult).
 *
 * SPS and PPS are generated in software from the V4L2 encoder controls so
 * that the driver does not need the firmware's SPS/PPS encode passes.  The
 * hardware only emits the slice NAL(s), as it does for the vendor HAL.
 */

#include <linux/bits.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/overflow.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-dma-contig.h>

#include "../mtk_vcodec_enc.h"
#include "../venc_drv_base.h"
#include "../venc_drv_if.h"
#include "venc_direct_hw.h"
#include "venc_direct_regs.h"

#define VENC_H264_MAX_W		1920
#define VENC_H264_MAX_H		1080
#define VENC_H264_MIN_W		16
#define VENC_H264_MIN_H		16
#define VENC_H264_MAX_BS_SIZE	SZ_4M
#define VENC_H264_NUM_REFS	3
#define VENC_H264_RBSP_SIZE	128
#define VENC_H264_BS_ALIGN	128
#define VENC_H264_ADDR_ALIGN	16

/* Work buffers from H264_AllocWorkBuf(), HAL ~84701-85023. */
enum venc_h264_direct_buf {
	VENC_H264_BUF_RC_INFO,
	VENC_H264_BUF_RC_CODE,
	VENC_H264_BUF_BITSTREAM,
	VENC_H264_BUF_REC_LUMA,
	VENC_H264_BUF_REC_CHROMA,
	VENC_H264_BUF_REF_LUMA,
	VENC_H264_BUF_REF_CHROMA,
	VENC_H264_BUF_MV_1,
	VENC_H264_BUF_MV_2,
	VENC_H264_BUF_SUB_WRITE_1,
	VENC_H264_BUF_SUB_WRITE_2,
	VENC_H264_BUF_SUB_WRITE_DUMMY,
	VENC_H264_BUF_MAX,
};

struct venc_h264_direct_inst {
	struct mtk_vcodec_enc_ctx *ctx;
	struct mtk_vcodec_mem work[VENC_H264_BUF_MAX];
	struct venc_enc_param param;
	u32 buf_w;
	u32 buf_h;
	u32 mb_w;
	u32 mb_h;
	u32 profile_idc;
	u32 level_idc;
	u32 input_fourcc;
	u32 frm_cnt;
	bool work_allocated;
	bool prepend_hdr;
	struct mtk_vcodec_mem header;
	size_t header_len;
};

/* ---- tiny RBSP writer for the software-generated SPS/PPS ---- */

struct venc_h264_rbsp {
	u8 buf[VENC_H264_RBSP_SIZE];
	size_t len;
	unsigned int bit;
};

static void rbsp_put_bit(struct venc_h264_rbsp *r, u32 bit)
{
	if (r->len >= sizeof(r->buf))
		return;
	if (!r->bit)
		r->buf[r->len] = 0;
	r->buf[r->len] |= (bit & 1) << (7 - r->bit);
	r->bit++;
	if (r->bit == 8) {
		r->bit = 0;
		r->len++;
	}
}

static void rbsp_put_bits(struct venc_h264_rbsp *r, u32 val, unsigned int n)
{
	unsigned int i;


	for (i = n; i > 0; i--)
		rbsp_put_bit(r, val >> (i - 1));
}

static void rbsp_put_ue(struct venc_h264_rbsp *r, u32 val)
{
	u32 code = val + 1;
	unsigned int bits = fls(code);

	rbsp_put_bits(r, 0, bits - 1);
	rbsp_put_bits(r, code, bits);
}

static void rbsp_put_se(struct venc_h264_rbsp *r, int val)
{
	u32 code = val <= 0 ? (u32)(-2 * val) : (u32)(2 * val - 1);

	rbsp_put_ue(r, code);
}

static void rbsp_trailing(struct venc_h264_rbsp *r)
{
	rbsp_put_bit(r, 1);
	while (r->bit)
		rbsp_put_bit(r, 0);
}

static size_t emit_nal(u8 *dst, u8 nal_hdr, const struct venc_h264_rbsp *r)
{
	static const u8 start_code[4] = { 0, 0, 0, 1 };
	size_t out = 0;
	size_t i;
	unsigned int zeros = 0;

	memcpy(dst, start_code, sizeof(start_code));
	out += sizeof(start_code);
	dst[out++] = nal_hdr;

	for (i = 0; i < r->len; i++) {
		u8 byte = r->buf[i];

		if (zeros == 2 && byte <= 3) {
			dst[out++] = 3;
			zeros = 0;
		}
		dst[out++] = byte;
		zeros = byte ? 0 : zeros + 1;
	}

	return out;
}

static size_t venc_h264_make_sps(struct venc_h264_direct_inst *inst, u8 *dst)
{
	struct venc_h264_rbsp r = {};
	u32 crop_l, crop_r, crop_t, crop_b;
	u32 w = inst->param.width;
	u32 h = inst->param.height;

	crop_l = 0;
	crop_r = (inst->mb_w * 16 - w) / 2 * 2;
	crop_t = 0;
	crop_b = (inst->mb_h * 16 - h) / 2 * 2;

	rbsp_put_bits(&r, inst->profile_idc, 8);
	rbsp_put_bits(&r, 0, 8); /* constraint flags + reserved */
	rbsp_put_bits(&r, inst->level_idc, 8);
	rbsp_put_ue(&r, 0); /* seq_parameter_set_id */

	if (inst->profile_idc == 100) {
		rbsp_put_ue(&r, 1); /* chroma_format_idc = 4:2:0 */
		rbsp_put_ue(&r, 0); /* bit_depth_luma_minus8 */
		rbsp_put_ue(&r, 0); /* bit_depth_chroma_minus8 */
		rbsp_put_bit(&r, 0); /* qpprime_y_zero_transform_bypass */
		rbsp_put_bit(&r, 0); /* seq_scaling_matrix_present */
	}

	/* Keep the SPS in sync with the HAL register setup:
	 * log2_max_frame_num_minus4 = 0, pic_order_cnt_type = 0,
	 * log2_max_pic_order_cnt_lsb_minus4 = 0.
	 */
	rbsp_put_ue(&r, 0);
	rbsp_put_ue(&r, 0);
	rbsp_put_ue(&r, 0);
	rbsp_put_ue(&r, VENC_H264_NUM_REFS);
	rbsp_put_bit(&r, 0); /* gaps_in_frame_num_value_allowed */
	rbsp_put_ue(&r, inst->mb_w - 1);
	rbsp_put_ue(&r, inst->mb_h - 1);
	rbsp_put_bit(&r, 1); /* frame_mbs_only_flag */
	rbsp_put_bit(&r, 1); /* direct_8x8_inference_flag */

	if (crop_l || crop_r || crop_t || crop_b) {
		rbsp_put_bit(&r, 1);
		rbsp_put_ue(&r, crop_l);
		rbsp_put_ue(&r, crop_r);
		rbsp_put_ue(&r, crop_t);
		rbsp_put_ue(&r, crop_b);
	} else {
		rbsp_put_bit(&r, 0);
	}

	rbsp_put_bit(&r, 0); /* vui_parameters_present_flag */
	rbsp_trailing(&r);

	return emit_nal(dst, 0x67, &r);
}

static size_t venc_h264_make_pps(struct venc_h264_direct_inst *inst, u8 *dst)
{
	struct venc_h264_rbsp r = {};

	rbsp_put_ue(&r, 0); /* pic_parameter_set_id */
	rbsp_put_ue(&r, 0); /* seq_parameter_set_id */
	rbsp_put_bit(&r, 0); /* entropy_coding_mode_flag: CAVLC */
	rbsp_put_bit(&r, 0); /* bottom_field_pic_order_in_frame_present */
	rbsp_put_ue(&r, 0); /* num_slice_groups_minus1 */
	rbsp_put_ue(&r, VENC_H264_NUM_REFS - 1);
	rbsp_put_ue(&r, 0);
	rbsp_put_bit(&r, 0); /* weighted_pred_flag */
	rbsp_put_bits(&r, 0, 2); /* weighted_bipred_idc */
	rbsp_put_se(&r, 0); /* pic_init_qp_minus26 */
	rbsp_put_se(&r, 0); /* pic_init_qs_minus26 */
	rbsp_put_se(&r, 0); /* chroma_qp_index_offset */
	rbsp_put_bit(&r, 0); /* deblocking_filter_control_present */
	rbsp_put_bit(&r, 0); /* constrained_intra_pred_flag */
	rbsp_put_bit(&r, 0); /* redundant_pic_cnt_present_flag */
	rbsp_trailing(&r);

	return emit_nal(dst, 0x68, &r);
}

/* ---- DMA work buffers ---- */

static void venc_h264_direct_free_work(struct venc_h264_direct_inst *inst)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(inst->work); i++) {
		if (inst->work[i].va)
			mtk_vcodec_mem_free(inst->ctx, &inst->work[i]);
	}
	if (inst->header.va)
		mtk_vcodec_mem_free(inst->ctx, &inst->header);
	inst->work_allocated = false;
}

static int venc_h264_direct_alloc_one(struct venc_h264_direct_inst *inst,
				      enum venc_h264_direct_buf idx,
				      size_t size)
{
	struct mtk_vcodec_mem *mem = &inst->work[idx];
	int ret;

	if (mem->size == ALIGN(size, VENC_H264_BS_ALIGN) && mem->va)
		return 0;
	if (mem->va)
		mtk_vcodec_mem_free(inst->ctx, mem);

	memset(mem, 0, sizeof(*mem));
	mem->size = ALIGN(size, VENC_H264_BS_ALIGN);
	ret = mtk_vcodec_mem_alloc(inst->ctx, mem);
	if (ret) {
		memset(mem, 0, sizeof(*mem));
		return ret;
	}
	if (upper_32_bits(mem->dma_addr) ||
	    !IS_ALIGNED(mem->dma_addr, VENC_H264_ADDR_ALIGN)) {
		mtk_vcodec_mem_free(inst->ctx, mem);
		return -ERANGE;
	}

	return 0;
}

static int venc_h264_direct_alloc_work(struct venc_h264_direct_inst *inst)
{
	const struct venc_direct_hw_cfg *cfg =
		venc_direct_get_cfg(inst->ctx);
	size_t luma_sz, chroma_sz, mv_sz, sub_write_sz;
	int ret;

	/* H264_AllocWorkBuf(): 2 * ceil(visible H/32) *
	 * ceil(visible W/16) * {0x100,0x80}.
	 */
	luma_sz = (size_t)2 * DIV_ROUND_UP(inst->param.height, 32) *
		  inst->mb_w * 0x100;
	chroma_sz = (size_t)2 * DIV_ROUND_UP(inst->param.height, 32) *
		    inst->mb_w * 0x80;
	/* rMVInfo1/2: roughly (W/4) * (H/8) * 2, aligned down to 0x80. */
	mv_sz = ALIGN((size_t)DIV_ROUND_UP(inst->buf_w, 4) *
		      DIV_ROUND_UP(inst->buf_h, 8) * 2, cfg->work_align);
	/* H264_AllocWorkBuf(): rSubWrite1/2/Dummy use
	 * ceil(width / 16) * ALIGN(height, 32) * 4 bytes.
	 */
	sub_write_sz = (size_t)DIV_ROUND_UP(inst->param.width, 16) *
		       ALIGN(inst->param.height, 32) * 4;

	ret = venc_h264_direct_alloc_one(inst, VENC_H264_BUF_RC_INFO,
					 cfg->rc_info_size);
	if (!ret)
		ret = venc_h264_direct_alloc_one(inst, VENC_H264_BUF_RC_CODE,
						 cfg->rc_code_size);
	if (!ret)
		ret = venc_h264_direct_alloc_one(inst, VENC_H264_BUF_BITSTREAM,
						 VENC_H264_MAX_BS_SIZE);
	if (!ret)
		ret = venc_h264_direct_alloc_one(inst, VENC_H264_BUF_REC_LUMA,
						 luma_sz);
	if (!ret)
		ret = venc_h264_direct_alloc_one(inst, VENC_H264_BUF_REC_CHROMA,
						 chroma_sz);
	if (!ret)
		ret = venc_h264_direct_alloc_one(inst, VENC_H264_BUF_REF_LUMA,
						 luma_sz);
	if (!ret)
		ret = venc_h264_direct_alloc_one(inst, VENC_H264_BUF_REF_CHROMA,
						 chroma_sz);
	if (!ret)
		ret = venc_h264_direct_alloc_one(inst, VENC_H264_BUF_MV_1, mv_sz);
	if (!ret)
		ret = venc_h264_direct_alloc_one(inst, VENC_H264_BUF_MV_2, mv_sz);
	if (!ret)
		ret = venc_h264_direct_alloc_one(inst,
						 VENC_H264_BUF_SUB_WRITE_1,
						 sub_write_sz);
	if (!ret)
		ret = venc_h264_direct_alloc_one(inst,
						 VENC_H264_BUF_SUB_WRITE_2,
						 sub_write_sz);
	if (!ret)
		ret = venc_h264_direct_alloc_one(inst,
						 VENC_H264_BUF_SUB_WRITE_DUMMY,
						 sub_write_sz);
	if (ret)
		return ret;

	if (!inst->header.va) {
		inst->header.size = VENC_H264_RBSP_SIZE * 2;
		ret = mtk_vcodec_mem_alloc(inst->ctx, &inst->header);
		if (ret)
			return ret;
	}

	inst->work_allocated = true;
	return 0;
}

/* ---- register programming ---- */

static u32 venc_h264_profile_idc(unsigned int v4l2_profile)
{
	switch (v4l2_profile) {
	case V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE:
		return 66;
	case V4L2_MPEG_VIDEO_H264_PROFILE_MAIN:
		return 77;
	case V4L2_MPEG_VIDEO_H264_PROFILE_HIGH:
		return 100;
	default:
		return 100;
	}
}

static u32 venc_h264_level_idc(unsigned int v4l2_level)
{
	switch (v4l2_level) {
	case V4L2_MPEG_VIDEO_H264_LEVEL_1_0:
		return 10;
	case V4L2_MPEG_VIDEO_H264_LEVEL_1_1:
		return 11;
	case V4L2_MPEG_VIDEO_H264_LEVEL_1_2:
		return 12;
	case V4L2_MPEG_VIDEO_H264_LEVEL_1_3:
		return 13;
	case V4L2_MPEG_VIDEO_H264_LEVEL_2_0:
		return 20;
	case V4L2_MPEG_VIDEO_H264_LEVEL_2_1:
		return 21;
	case V4L2_MPEG_VIDEO_H264_LEVEL_2_2:
		return 22;
	case V4L2_MPEG_VIDEO_H264_LEVEL_3_0:
		return 30;
	case V4L2_MPEG_VIDEO_H264_LEVEL_3_1:
		return 31;
	case V4L2_MPEG_VIDEO_H264_LEVEL_3_2:
		return 32;
	case V4L2_MPEG_VIDEO_H264_LEVEL_4_0:
		return 40;
	case V4L2_MPEG_VIDEO_H264_LEVEL_4_1:
		return 41;
	case V4L2_MPEG_VIDEO_H264_LEVEL_4_2:
		return 42;
	case V4L2_MPEG_VIDEO_H264_LEVEL_5_0:
		return 50;
	case V4L2_MPEG_VIDEO_H264_LEVEL_5_1:
		return 51;
	default:
		return 40;
	}
}

static void venc_h264_direct_seq_setup(struct venc_h264_direct_inst *inst)
{
	struct mtk_vcodec_enc_dev *dev = inst->ctx->dev;
	u32 profile_code, bitrate_kbps, fps, buf_mb_w, pad;
	size_t luma_scratch;

	/* H264_SeqLvlMMRSetting() ~90106-90314. 0x24 and 0x2c use the
	 * visible frame size; 0x28 uses the coded buffer stride and scan type.
	 */
	buf_mb_w = DIV_ROUND_UP(inst->buf_w, 16);
	pad = inst->buf_w - inst->param.width;
	if (pad == 16)
		pad = 0;
	else if (pad > 16)
		pad = pad <= 32 ? pad - 16 : 0;

	venc_direct_write(dev, VENC_SYS_PIC_SIZE_0,
			  (inst->mb_h << VENC_SYS_PIC_HEIGHT_MB_SHIFT) |
			  inst->mb_w);
	venc_direct_write(dev, VENC_SYS_PIC_SIZE_1,
			  (buf_mb_w << VENC_SYS_PIC_BUF_WIDTH_MB_SHIFT) |
			  (buf_mb_w << VENC_SYS_PIC_UV_PITCH_SHIFT) |
			  (pad << VENC_SYS_PIC_HPAD_SHIFT) |
			  ((ALIGN(inst->param.height, 16) - inst->param.height) &
			   VENC_SYS_PIC_VPAD_MASK) |
			  (VENC_SYS_PIC_SCAN_TYPE_NV12 <<
			   VENC_SYS_PIC_SCAN_TYPE_SHIFT));
	venc_direct_write(dev, VENC_SYS_TOTAL_MBS, inst->mb_w * inst->mb_h);

	profile_code = inst->profile_idc == 66 ? 0 :
		       inst->profile_idc == 77 ? 1 : 2;
	venc_direct_write(dev, VENC_SYS_CODEC_CFG,
			  (profile_code << 29) |
			  (inst->level_idc << VENC_SYS_CODEC_LEVEL_SHIFT) |
			  VENC_SYS_CODEC_FLAG_1);

	bitrate_kbps = inst->param.bitrate / 1000;
	fps = inst->param.frm_rate ? inst->param.frm_rate : 30;

	/* POC type 0: the HAL omits the 0x20000 POC offset here. */
	venc_direct_write(dev, VENC_SYS_RATE_CTRL_0, bitrate_kbps);
	venc_direct_write(dev, VENC_SYS_RATE_CTRL_1,
			  (fps << VENC_SYS_RATE_CTRL_1_FPS_SHIFT) | 0x6a);
	venc_direct_write(dev, VENC_SYS_RATE_CTRL_2,
			  VENC_SYS_RATE_CTRL_2_MAGIC | (26 << 8));
	venc_direct_write(dev, VENC_SYS_RATE_CTRL_3,
			  VENC_SYS_RATE_CTRL_3_DEFAULT);

	venc_direct_write(dev, VENC_CORE_PSY_RD_LAMBDA, 6);
	venc_direct_write(dev, VENC_SYS_CFG_12C, 0x200);
	venc_direct_write(dev, VENC_SYS_CLOCK_GATE,
			  VENC_SYS_CLOCK_GATE_DEFAULT);

	luma_scratch = (size_t)3 * inst->buf_w * 2 + 0x50 * 0x200 + 0x37f;
	venc_direct_write(dev, VENC_CORE_SCRATCH_0, 0);
	venc_direct_write(dev, VENC_CORE_SCRATCH_LUMA,
			  luma_scratch >> 7);
	venc_direct_write(dev, VENC_CORE_SCRATCH_LUMA_2,
			  luma_scratch >> 7);
	venc_direct_write(dev, VENC_CORE_SCRATCH_CHROMA,
			  ((luma_scratch - 0x7f) / 2 + 0x7f) >> 7);
	venc_direct_write(dev, VENC_CORE_BUFFER_MODE, 2);
}

static void venc_h264_direct_frame_setup(struct venc_h264_direct_inst *inst)
{
	struct mtk_vcodec_enc_dev *dev = inst->ctx->dev;

	/* HAL H264_FrmLvlMMRSetting() ~90350-90472.  All frames are IDR in
	 * this minimal path, so POC/frame numbers stay at zero.
	 */
	venc_direct_write(dev, VENC_SYS_FRAME_CTRL,
			  VENC_SYS_FRAME_CTRL_BASE |
			  VENC_SYS_FRAME_CTRL_IDR |
			  VENC_SYS_FRAME_CTRL_SINGLE_SLICE |
			  VENC_SYS_FRAME_CTRL_QP_DELTA |
			  VENC_SYS_FRAME_CTRL_MODE_MAGIC | 2 | 0xc);
	venc_direct_write(dev, VENC_SYS_FRAME_COUNT, inst->frm_cnt);
	venc_direct_write(dev, VENC_SYS_FRAME_NUM_CURR, 0);
	venc_direct_write(dev, VENC_SYS_FRAME_NUM_REF, 0);
	venc_direct_write(dev, VENC_SYS_I_FRAME_SIZE, 0xff);
	venc_direct_write(dev, VENC_SYS_GOP_IDX, 0);
	venc_direct_write(dev, VENC_SYS_REF_P_DIST, 0);
	venc_direct_write(dev, VENC_SYS_FRAME_CFG_128, 0);
}

static void venc_h264_direct_set_buffers(struct venc_h264_direct_inst *inst,
					 struct venc_frm_buf *frm_buf)
{
	struct mtk_vcodec_enc_dev *dev = inst->ctx->dev;
	struct mtk_vcodec_mem *hw_bs =
		&inst->work[VENC_H264_BUF_BITSTREAM];
	dma_addr_t y, c;

	/* HAL H264_EncodeFrameEnc_SetBufs() ~90503-90547 writes the NV12
	 * chroma address to 0x70 and zero to 0x94.  Zero in the V address
	 * selects the semi-planar layout; passing the UV base to both registers
	 * makes the hardware treat the input as a three-plane image.
	 */
	y = frm_buf->fb_addr[0].dma_addr;
	c = frm_buf->fb_addr[1].dma_addr;

	venc_direct_write(dev, VENC_SYS_BS_ADDR,
			  hw_bs->dma_addr >> VENC_SYS_BS_ADDR_SHIFT);
	venc_direct_write(dev, VENC_SYS_BS_SIZE,
			  min_t(size_t, hw_bs->size, VENC_H264_MAX_BS_SIZE) >>
			  VENC_SYS_ADDR_SHIFT);
	venc_direct_write(dev, VENC_SYS_CURR_Y_ADDR,
			  y >> VENC_SYS_ADDR_SHIFT);
	venc_direct_write(dev, VENC_SYS_CURR_U_ADDR,
			  c >> VENC_SYS_ADDR_SHIFT);
	venc_direct_write(dev, VENC_SYS_CURR_V_ADDR, 0);

	/* HAL H264_EncodeFrameEnc_SetRC() ~90496-90497. */
	venc_direct_write(dev, VENC_SYS_RC_INFO_ADDR,
			  inst->work[VENC_H264_BUF_RC_INFO].dma_addr >>
			  VENC_SYS_ADDR_SHIFT);
	venc_direct_write(dev, VENC_SYS_RC_CODE_ADDR,
			  inst->work[VENC_H264_BUF_RC_CODE].dma_addr >>
			  VENC_SYS_ADDR_SHIFT);

	/* Reference picture, reconstructed picture, and motion-vector buffers. */
	venc_direct_write(dev, VENC_SYS_REF_BASE,
			  inst->work[VENC_H264_BUF_REF_LUMA].dma_addr >> VENC_SYS_ADDR_SHIFT);
	venc_direct_write(dev, VENC_SYS_REF_BASE + 4,
			  inst->work[VENC_H264_BUF_REF_CHROMA].dma_addr >> VENC_SYS_ADDR_SHIFT);
	venc_direct_write(dev, VENC_SYS_REF_BASE + 8,
			  inst->work[VENC_H264_BUF_REC_LUMA].dma_addr >> VENC_SYS_ADDR_SHIFT);
	venc_direct_write(dev, VENC_SYS_REF_BASE + 12,
			  inst->work[VENC_H264_BUF_REC_CHROMA].dma_addr >> VENC_SYS_ADDR_SHIFT);
	venc_direct_write(dev, VENC_SYS_REF_BASE + 16,
			  inst->work[VENC_H264_BUF_MV_1].dma_addr >> VENC_SYS_ADDR_SHIFT);
	venc_direct_write(dev, VENC_SYS_REF_BASE + 20,
			  inst->work[VENC_H264_BUF_MV_2].dma_addr >> VENC_SYS_ADDR_SHIFT);
}

/* ---- common-if callbacks ---- */

static int venc_h264_direct_init(struct mtk_vcodec_enc_ctx *ctx)
{
	struct venc_h264_direct_inst *inst;

	inst = kzalloc(sizeof(*inst), GFP_KERNEL);
	if (!inst)
		return -ENOMEM;

	inst->ctx = ctx;
	ctx->drv_handle = inst;
	return 0;
}

static int venc_h264_direct_set_param(void *handle,
				      enum venc_set_param_type type,
				      struct venc_enc_param *in)
{
	struct venc_h264_direct_inst *inst = handle;
	int ret;

	switch (type) {
	case VENC_SET_PARAM_ENC:
		if (!in || !in->width || !in->height)
			return -EINVAL;
		if (in->input_yuv_fmt != VENC_YUV_FORMAT_NV12)
			return -EINVAL;
		inst->param = *in;
		inst->buf_w = in->buf_width;
		inst->buf_h = in->buf_height;
		if (!inst->buf_w || !inst->buf_h) {
			inst->buf_w = ALIGN(in->width, 16);
			inst->buf_h = ALIGN(in->height, 16);
		}
		if (inst->buf_w < in->width || inst->buf_h < in->height ||
		    inst->buf_w > VENC_H264_MAX_W ||
		    inst->buf_h > VENC_H264_MAX_H ||
		    inst->buf_w < VENC_H264_MIN_W ||
		    inst->buf_h < VENC_H264_MIN_H)
			return -EINVAL;
		inst->mb_w = DIV_ROUND_UP(in->width, 16);
		inst->mb_h = DIV_ROUND_UP(in->height, 16);
		inst->profile_idc = venc_h264_profile_idc(in->h264_profile);
		inst->level_idc = venc_h264_level_idc(in->h264_level);
		inst->input_fourcc = inst->ctx->q_data[MTK_Q_DATA_SRC].fmt->fourcc;
		if (inst->input_fourcc != V4L2_PIX_FMT_MM21 &&
		    inst->input_fourcc != V4L2_PIX_FMT_NV12M)
			return -EINVAL;

		if (inst->work_allocated)
			venc_h264_direct_free_work(inst);
		ret = venc_h264_direct_alloc_work(inst);
		if (ret)
			return ret;

		/* Regenerate the software SPS/PPS whenever the stream
		 * configuration changes, so both the separate-header path and
		 * the prepend-header path use the same parameter set.
		 */
		inst->header_len = venc_h264_make_sps(inst, inst->header.va);
		inst->header_len += venc_h264_make_pps(inst,
						inst->header.va +
						inst->header_len);
		break;
	case VENC_SET_PARAM_PREPEND_HEADER:
		inst->prepend_hdr = true;
		break;
	case VENC_SET_PARAM_FORCE_INTRA:
	case VENC_SET_PARAM_GOP_SIZE:
	case VENC_SET_PARAM_INTRA_PERIOD:
		inst->frm_cnt = 0;
		break;
	default:
		break;
	}

	return 0;
}

static int venc_h264_direct_encode(void *handle, enum venc_start_opt opt,
				   struct venc_frm_buf *frm_buf,
				   struct mtk_vcodec_mem *bs_buf,
				   struct venc_done_result *result)
{
	struct venc_h264_direct_inst *inst = handle;
	struct mtk_vcodec_enc_ctx *ctx = inst->ctx;
	struct mtk_vcodec_enc_dev *dev = ctx->dev;
	u32 irq_status, bs_size;
	int ret;

	if (!bs_buf || !bs_buf->va || !result)
		return -EINVAL;

	if (opt == VENC_START_OPT_ENCODE_SEQUENCE_HEADER) {
		if (!inst->work_allocated)
			return -EINVAL;
		if (inst->header_len > bs_buf->size)
			return -ENOSPC;
		memcpy(bs_buf->va, inst->header.va, inst->header_len);
		result->bs_size = inst->header_len;
		result->is_key_frm = false;
		return 0;
	}

	if (opt != VENC_START_OPT_ENCODE_FRAME || !frm_buf ||
	    !frm_buf->fb_addr[0].dma_addr || !frm_buf->fb_addr[1].dma_addr)
		return -EINVAL;
	if (!inst->work_allocated)
		return -EINVAL;
	if (upper_32_bits(frm_buf->fb_addr[0].dma_addr) ||
	    upper_32_bits(frm_buf->fb_addr[1].dma_addr) ||
	    !IS_ALIGNED(frm_buf->fb_addr[0].dma_addr, VENC_H264_ADDR_ALIGN) ||
	    !IS_ALIGNED(frm_buf->fb_addr[1].dma_addr, VENC_H264_ADDR_ALIGN))
		return -ERANGE;

	ret = venc_direct_reset_hw(ctx);
	if (ret)
		return ret;

	venc_h264_direct_seq_setup(inst);
	venc_h264_direct_frame_setup(inst);
	venc_h264_direct_set_buffers(inst, frm_buf);

	/* H264_SetHWModeSel(h, 2) uses the base word for POC type 0.
	 * The SPS emitted above also sets pic_order_cnt_type to 0.  The
	 * 0xd200000 and bit-22/23 additions are conditional on the HAL's
	 * separate POC-managed mode and must not be forced for this stream.
	 */
	venc_direct_write(dev, VENC_SYS_IRQ_MASK,
			  VENC_SYS_IRQ_MASK_DEFAULT);
	venc_direct_write(dev, VENC_SYS_MODE_SEL, VENC_SYS_MODE_SEL_H264);

	/* H264_SeqLvlMMRSetting() programs the run-time clock gate at 0x1c0. */
	venc_direct_write(dev, VENC_SYS_CLOCK_GATE,
			  VENC_SYS_CLOCK_GATE_ON);

	/* IRQ_NOAUTOEN is set by the platform driver.  Firmware encoders
	 * explicitly enable the shared line around each HW operation; do the
	 * same for direct MMIO and clear any stale completion before triggering.
	 */
	ctx->int_cond[0] = 0;
	ctx->int_type[0] = 0;
	ctx->irq_status = 0;
	enable_irq(dev->enc_irq);

	/* H264_EncodeFrameEnc_TriggerHW() ~90681-90684. */
	venc_direct_write(dev, VENC_SYS_ENABLE, 1);
	venc_direct_write(dev, VENC_SYS_TRIGGER_UNIT,
			  VENC_SYS_TRIGGER_UNIT_FRAME);

	ret = venc_direct_wait_done(ctx, &irq_status, &bs_size);
	disable_irq(dev->enc_irq);
	if (ret)
		return ret;

	if (bs_size > bs_buf->size ||
	    bs_size > inst->work[VENC_H264_BUF_BITSTREAM].size)
		return -ENOSPC;

	/* The IOMMU faults are in the low range that the capture buffers occupy.
	 * Isolate the hardware bitstream writes in a VENC-owned coherent mapping
	 * and copy the completed stream to the capture plane after DMA stops.
	 */
	memcpy(bs_buf->va, inst->work[VENC_H264_BUF_BITSTREAM].va, bs_size);

	if (inst->prepend_hdr &&
	    bs_size + inst->header_len <= bs_buf->size) {
		memmove(bs_buf->va + inst->header_len, bs_buf->va, bs_size);
		memcpy(bs_buf->va, inst->header.va, inst->header_len);
		bs_size += inst->header_len;
	}

	result->bs_size = bs_size;
	result->is_key_frm = true;
	inst->frm_cnt++;

	mtk_venc_debug(ctx, "IDR frame %u: bs_size=%u irq=%#x",
		       inst->frm_cnt - 1, bs_size, irq_status);
	return 0;
}

static int venc_h264_direct_deinit(void *handle)
{
	struct venc_h264_direct_inst *inst = handle;

	venc_h264_direct_free_work(inst);
	kfree(inst);
	return 0;
}

const struct venc_common_if venc_h264_direct_if = {
	.init = venc_h264_direct_init,
	.encode = venc_h264_direct_encode,
	.set_param = venc_h264_direct_set_param,
	.deinit = venc_h264_direct_deinit,
};
