/*
 * Copyright (c) 2007-2013 Intel Corporation. All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL PRECISION INSIGHT AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <getopt.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <assert.h>
#include <pthread.h>
#include <errno.h>
#include <math.h>

#include "va_h264.h"
#include "va_display.h"

#define CHECK_VASTATUS(va_status,func)                                  \
    if (va_status != VA_STATUS_SUCCESS) {                               \
        fprintf(stderr,"%s:%s (%d) failed,exit\n", __func__, func, __LINE__); \
        return  va_status;                                                      \
    }

#define CHECK_VASTATUS_VOID(va_status,func)                                  \
    if (va_status != VA_STATUS_SUCCESS) {                               \
        fprintf(stderr,"%s:%s (%d) failed,exit\n", __func__, func, __LINE__); \
        return;                                                      \
    }

#define CHECK_VASTATUS_RETNULL(va_status,func)                                  \
    if (va_status != VA_STATUS_SUCCESS) {                               \
        fprintf(stderr,"%s:%s (%d) failed,exit\n", __func__, func, __LINE__); \
        return NULL;                                                      \
    }

#include "loadsurface.h"

#define NAL_REF_IDC_NONE        0
#define NAL_REF_IDC_LOW         1
#define NAL_REF_IDC_MEDIUM      2
#define NAL_REF_IDC_HIGH        3

#define NAL_NON_IDR             1
#define NAL_IDR                 5
#define NAL_SPS                 7
#define NAL_PPS                 8
#define NAL_SEI			        6

#define SLICE_TYPE_P            0
#define SLICE_TYPE_B            1
#define SLICE_TYPE_I            2
#define IS_P_SLICE(type) (SLICE_TYPE_P == (type))
#define IS_B_SLICE(type) (SLICE_TYPE_B == (type))
#define IS_I_SLICE(type) (SLICE_TYPE_I == (type))

#define ENTROPY_MODE_CAVLC      0
#define ENTROPY_MODE_CABAC      1

#define PROFILE_IDC_BASELINE    66
#define PROFILE_IDC_MAIN        77
#define PROFILE_IDC_HIGH        100

#define BITSTREAM_ALLOCATE_STEPPING     4096

static const unsigned int MaxFrameNum = (2<<16);
static const unsigned int MaxPicOrderCntLsb = (2<<8);
static const unsigned int Log2MaxFrameNum = 16;
static const unsigned int Log2MaxPicOrderCntLsb = 8;
static const unsigned int num_ref_frames = 2;
static const int srcyuv_fourcc = VA_FOURCC_NV12;
static const unsigned int frame_slices = 1;

static  int rc_default_modes[] = {
    VA_RC_VBR,
    VA_RC_CQP,
    VA_RC_VBR_CONSTRAINED,
    VA_RC_CBR,
    VA_RC_VCM,
    VA_RC_NONE,
};

#define MIN(a, b) ((a)>(b)?(b):(a))
#define MAX(a, b) ((a)>(b)?(a):(b))

// Default entrypoint for Encode
static VAEntrypoint requested_entrypoint = -1;
static VAEntrypoint selected_entrypoint = -1;

struct __bitstream {
    unsigned int *buffer;
    int bit_offset;
    int max_size_in_dword;
};
typedef struct __bitstream bitstream;


static unsigned int
va_swap32(unsigned int val)
{
    unsigned char *pval = (unsigned char *)&val;

    return ((pval[0] << 24)     |
            (pval[1] << 16)     |
            (pval[2] << 8)      |
            (pval[3] << 0));
}

static void
bitstream_start(bitstream *bs)
{
    bs->max_size_in_dword = BITSTREAM_ALLOCATE_STEPPING;
    bs->buffer = calloc(bs->max_size_in_dword * sizeof(int), 1);
    assert(bs->buffer);
    bs->bit_offset = 0;
}

static void
bitstream_end(bitstream *bs)
{
    int pos = (bs->bit_offset >> 5);
    int bit_offset = (bs->bit_offset & 0x1f);
    int bit_left = 32 - bit_offset;

    if (bit_offset) {
        bs->buffer[pos] = va_swap32((bs->buffer[pos] << bit_left));
    }
}

static void
bitstream_put_ui(bitstream *bs, unsigned int val, int size_in_bits)
{
    int pos = (bs->bit_offset >> 5);
    int bit_offset = (bs->bit_offset & 0x1f);
    int bit_left = 32 - bit_offset;

    if (!size_in_bits)
        return;

    bs->bit_offset += size_in_bits;

    if (bit_left > size_in_bits) {
        bs->buffer[pos] = (bs->buffer[pos] << size_in_bits | val);
    } else {
        size_in_bits -= bit_left;
        bs->buffer[pos] = (bs->buffer[pos] << bit_left) | (val >> size_in_bits);
        bs->buffer[pos] = va_swap32(bs->buffer[pos]);

        if (pos + 1 == bs->max_size_in_dword) {
            bs->max_size_in_dword += BITSTREAM_ALLOCATE_STEPPING;
            bs->buffer = realloc(bs->buffer, bs->max_size_in_dword * sizeof(unsigned int));
            assert(bs->buffer);
        }

        bs->buffer[pos + 1] = val;
    }
}

static void
bitstream_put_ue(bitstream *bs, unsigned int val)
{
    int size_in_bits = 0;
    int tmp_val = ++val;

    while (tmp_val) {
        tmp_val >>= 1;
        size_in_bits++;
    }

    bitstream_put_ui(bs, 0, size_in_bits - 1); // leading zero
    bitstream_put_ui(bs, val, size_in_bits);
}

static void
bitstream_put_se(bitstream *bs, int val)
{
    unsigned int new_val;

    if (val <= 0)
        new_val = -2 * val;
    else
        new_val = 2 * val - 1;

    bitstream_put_ue(bs, new_val);
}

static void
bitstream_byte_aligning(bitstream *bs, int bit)
{
    int bit_offset = (bs->bit_offset & 0x7);
    int bit_left = 8 - bit_offset;
    int new_val;

    if (!bit_offset)
        return;

    assert(bit == 0 || bit == 1);

    if (bit)
        new_val = (1 << bit_left) - 1;
    else
        new_val = 0;

    bitstream_put_ui(bs, new_val, bit_left);
}

static void
rbsp_trailing_bits(bitstream *bs)
{
    bitstream_put_ui(bs, 1, 1);
    bitstream_byte_aligning(bs, 0);
}

static void nal_start_code_prefix(bitstream *bs)
{
    bitstream_put_ui(bs, 0x00000001, 32);
}

static void nal_header(bitstream *bs, int nal_ref_idc, int nal_unit_type)
{
    bitstream_put_ui(bs, 0, 1);                /* forbidden_zero_bit: 0 */
    bitstream_put_ui(bs, nal_ref_idc, 2);
    bitstream_put_ui(bs, nal_unit_type, 5);
}

static void sps_rbsp(VA264Context * context, bitstream *bs)
{
    int profile_idc = PROFILE_IDC_BASELINE;

    if (context->config.h264_profile  == VAProfileH264High)
        profile_idc = PROFILE_IDC_HIGH;
    else if (context->config.h264_profile  == VAProfileH264Main)
        profile_idc = PROFILE_IDC_MAIN;

    bitstream_put_ui(bs, profile_idc, 8);               /* profile_idc */
    bitstream_put_ui(bs, !!(context->constraint_set_flag & 1), 1);                         /* constraint_set0_flag */
    bitstream_put_ui(bs, !!(context->constraint_set_flag & 2), 1);                         /* constraint_set1_flag */
    bitstream_put_ui(bs, !!(context->constraint_set_flag & 4), 1);                         /* constraint_set2_flag */
    bitstream_put_ui(bs, !!(context->constraint_set_flag & 8), 1);                         /* constraint_set3_flag */
    bitstream_put_ui(bs, 0, 4);                         /* reserved_zero_4bits */
    bitstream_put_ui(bs, context->seq_param.level_idc, 8);      /* level_idc */
    bitstream_put_ue(bs, context->seq_param.seq_parameter_set_id);      /* seq_parameter_set_id */

    if ( profile_idc == PROFILE_IDC_HIGH) {
        bitstream_put_ue(bs, 1);        /* chroma_format_idc = 1, 4:2:0 */
        bitstream_put_ue(bs, 0);        /* bit_depth_luma_minus8 */
        bitstream_put_ue(bs, 0);        /* bit_depth_chroma_minus8 */
        bitstream_put_ui(bs, 0, 1);     /* qpprime_y_zero_transform_bypass_flag */
        bitstream_put_ui(bs, 0, 1);     /* seq_scaling_matrix_present_flag */
    }

    bitstream_put_ue(bs, context->seq_param.seq_fields.bits.log2_max_frame_num_minus4); /* log2_max_frame_num_minus4 */
    bitstream_put_ue(bs, context->seq_param.seq_fields.bits.pic_order_cnt_type);        /* pic_order_cnt_type */

    if (context->seq_param.seq_fields.bits.pic_order_cnt_type == 0)
        bitstream_put_ue(bs, context->seq_param.seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4);     /* log2_max_pic_order_cnt_lsb_minus4 */
    else {
        assert(0);
    }

    bitstream_put_ue(bs, context->seq_param.max_num_ref_frames);        /* num_ref_frames */
    bitstream_put_ui(bs, 0, 1);                                 /* gaps_in_frame_num_value_allowed_flag */

    bitstream_put_ue(bs, context->seq_param.picture_width_in_mbs - 1);  /* pic_width_in_mbs_minus1 */
    bitstream_put_ue(bs, context->seq_param.picture_height_in_mbs - 1); /* pic_height_in_map_units_minus1 */
    bitstream_put_ui(bs, context->seq_param.seq_fields.bits.frame_mbs_only_flag, 1);    /* frame_mbs_only_flag */

    if (!context->seq_param.seq_fields.bits.frame_mbs_only_flag) {
        assert(0);
    }

    bitstream_put_ui(bs, context->seq_param.seq_fields.bits.direct_8x8_inference_flag, 1);      /* direct_8x8_inference_flag */
    bitstream_put_ui(bs, context->seq_param.frame_cropping_flag, 1);            /* frame_cropping_flag */

    if (context->seq_param.frame_cropping_flag) {
        bitstream_put_ue(bs, context->seq_param.frame_crop_left_offset);        /* frame_crop_left_offset */
        bitstream_put_ue(bs, context->seq_param.frame_crop_right_offset);       /* frame_crop_right_offset */
        bitstream_put_ue(bs, context->seq_param.frame_crop_top_offset);         /* frame_crop_top_offset */
        bitstream_put_ue(bs, context->seq_param.frame_crop_bottom_offset);      /* frame_crop_bottom_offset */
    }

    //if ( frame_bit_rate < 0 ) { //TODO EW: the vui header isn't correct
    if ( 1 ) {
        bitstream_put_ui(bs, 0, 1); /* vui_parameters_present_flag */
    } else {
        bitstream_put_ui(bs, 1, 1); /* vui_parameters_present_flag */
        bitstream_put_ui(bs, 0, 1); /* aspect_ratio_info_present_flag */
        bitstream_put_ui(bs, 0, 1); /* overscan_info_present_flag */
        bitstream_put_ui(bs, 0, 1); /* video_signal_type_present_flag */
        bitstream_put_ui(bs, 0, 1); /* chroma_loc_info_present_flag */
        bitstream_put_ui(bs, 1, 1); /* timing_info_present_flag */
        {
            bitstream_put_ui(bs, 15, 32);
            bitstream_put_ui(bs, 900, 32);
            bitstream_put_ui(bs, 1, 1);
        }
        bitstream_put_ui(bs, 1, 1); /* nal_hrd_parameters_present_flag */
        {
            // hrd_parameters
            bitstream_put_ue(bs, 0);    /* cpb_cnt_minus1 */
            bitstream_put_ui(bs, 4, 4); /* bit_rate_scale */
            bitstream_put_ui(bs, 6, 4); /* cpb_size_scale */

            bitstream_put_ue(bs, context->config.frame_bitrate - 1); /* bit_rate_value_minus1[0] */
            bitstream_put_ue(bs, context->config.frame_bitrate*8 - 1); /* cpb_size_value_minus1[0] */
            bitstream_put_ui(bs, 1, 1);  /* cbr_flag[0] */

            bitstream_put_ui(bs, 23, 5);   /* initial_cpb_removal_delay_length_minus1 */
            bitstream_put_ui(bs, 23, 5);   /* cpb_removal_delay_length_minus1 */
            bitstream_put_ui(bs, 23, 5);   /* dpb_output_delay_length_minus1 */
            bitstream_put_ui(bs, 23, 5);   /* time_offset_length  */
        }
        bitstream_put_ui(bs, 0, 1);   /* vcl_hrd_parameters_present_flag */
        bitstream_put_ui(bs, 0, 1);   /* low_delay_hrd_flag */

        bitstream_put_ui(bs, 0, 1); /* pic_struct_present_flag */
        bitstream_put_ui(bs, 0, 1); /* bitstream_restriction_flag */
    }

    rbsp_trailing_bits(bs);     /* rbsp_trailing_bits */
}


static void pps_rbsp(VA264Context * context, bitstream *bs)
{
    bitstream_put_ue(bs, context->pic_param.pic_parameter_set_id);      /* pic_parameter_set_id */
    bitstream_put_ue(bs, context->pic_param.seq_parameter_set_id);      /* seq_parameter_set_id */

    bitstream_put_ui(bs, context->pic_param.pic_fields.bits.entropy_coding_mode_flag, 1);  /* entropy_coding_mode_flag */

    bitstream_put_ui(bs, 0, 1);                         /* pic_order_present_flag: 0 */

    bitstream_put_ue(bs, 0);                            /* num_slice_groups_minus1 */

    bitstream_put_ue(bs, context->pic_param.num_ref_idx_l0_active_minus1);      /* num_ref_idx_l0_active_minus1 */
    bitstream_put_ue(bs, context->pic_param.num_ref_idx_l1_active_minus1);      /* num_ref_idx_l1_active_minus1 1 */

    bitstream_put_ui(bs, context->pic_param.pic_fields.bits.weighted_pred_flag, 1);     /* weighted_pred_flag: 0 */
    bitstream_put_ui(bs, context->pic_param.pic_fields.bits.weighted_bipred_idc, 2);	/* weighted_bipred_idc: 0 */

    bitstream_put_se(bs, context->pic_param.pic_init_qp - 26);  /* pic_init_qp_minus26 */
    bitstream_put_se(bs, 0);                            /* pic_init_qs_minus26 */
    bitstream_put_se(bs, 0);                            /* chroma_qp_index_offset */

    bitstream_put_ui(bs, context->pic_param.pic_fields.bits.deblocking_filter_control_present_flag, 1); /* deblocking_filter_control_present_flag */
    bitstream_put_ui(bs, 0, 1);                         /* constrained_intra_pred_flag */
    bitstream_put_ui(bs, 0, 1);                         /* redundant_pic_cnt_present_flag */

    /* more_rbsp_data */
    bitstream_put_ui(bs, context->pic_param.pic_fields.bits.transform_8x8_mode_flag, 1);    /*transform_8x8_mode_flag */
    bitstream_put_ui(bs, 0, 1);                         /* pic_scaling_matrix_present_flag */
    bitstream_put_se(bs, context->pic_param.second_chroma_qp_index_offset );    /*second_chroma_qp_index_offset */

    rbsp_trailing_bits(bs);
}

static void slice_header(VA264Context * context, bitstream *bs)
{
    int first_mb_in_slice = context->slice_param.macroblock_address;

    bitstream_put_ue(bs, first_mb_in_slice);        /* first_mb_in_slice: 0 */
    bitstream_put_ue(bs, context->slice_param.slice_type);   /* slice_type */
    bitstream_put_ue(bs, context->slice_param.pic_parameter_set_id);        /* pic_parameter_set_id: 0 */
    bitstream_put_ui(bs, context->pic_param.frame_num, context->seq_param.seq_fields.bits.log2_max_frame_num_minus4 + 4); /* frame_num */

    /* frame_mbs_only_flag == 1 */
    if (!context->seq_param.seq_fields.bits.frame_mbs_only_flag) {
        /* FIXME: */
        assert(0);
    }

    if (context->pic_param.pic_fields.bits.idr_pic_flag)
        bitstream_put_ue(bs, context->slice_param.idr_pic_id);		/* idr_pic_id: 0 */

    if (context->seq_param.seq_fields.bits.pic_order_cnt_type == 0) {
        bitstream_put_ui(bs, context->pic_param.CurrPic.TopFieldOrderCnt, context->seq_param.seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 + 4);
        /* pic_order_present_flag == 0 */
    } else {
        /* FIXME: */
        assert(0);
    }

    /* redundant_pic_cnt_present_flag == 0 */
    /* slice type */
    if (IS_P_SLICE(context->slice_param.slice_type)) {
        bitstream_put_ui(bs, context->slice_param.num_ref_idx_active_override_flag, 1);            /* num_ref_idx_active_override_flag: */

        if (context->slice_param.num_ref_idx_active_override_flag)
            bitstream_put_ue(bs, context->slice_param.num_ref_idx_l0_active_minus1);

        /* ref_pic_list_reordering */
        bitstream_put_ui(bs, 0, 1);            /* ref_pic_list_reordering_flag_l0: 0 */
    } else if (IS_B_SLICE(context->slice_param.slice_type)) {
        bitstream_put_ui(bs, context->slice_param.direct_spatial_mv_pred_flag, 1);            /* direct_spatial_mv_pred: 1 */

        bitstream_put_ui(bs, context->slice_param.num_ref_idx_active_override_flag, 1);       /* num_ref_idx_active_override_flag: */

        if (context->slice_param.num_ref_idx_active_override_flag) {
            bitstream_put_ue(bs, context->slice_param.num_ref_idx_l0_active_minus1);
            bitstream_put_ue(bs, context->slice_param.num_ref_idx_l1_active_minus1);
        }

        /* ref_pic_list_reordering */
        bitstream_put_ui(bs, 0, 1);            /* ref_pic_list_reordering_flag_l0: 0 */
        bitstream_put_ui(bs, 0, 1);            /* ref_pic_list_reordering_flag_l1: 0 */
    }

    if ((context->pic_param.pic_fields.bits.weighted_pred_flag &&
         IS_P_SLICE(context->slice_param.slice_type)) ||
        ((context->pic_param.pic_fields.bits.weighted_bipred_idc == 1) &&
         IS_B_SLICE(context->slice_param.slice_type))) {
        /* FIXME: fill weight/offset table */
        assert(0);
    }

    /* dec_ref_pic_marking */
    if (context->pic_param.pic_fields.bits.reference_pic_flag) {     /* nal_ref_idc != 0 */
        unsigned char no_output_of_prior_pics_flag = 0;
        unsigned char long_term_reference_flag = 0;
        unsigned char adaptive_ref_pic_marking_mode_flag = 0;

        if (context->pic_param.pic_fields.bits.idr_pic_flag) {
            bitstream_put_ui(bs, no_output_of_prior_pics_flag, 1);            /* no_output_of_prior_pics_flag: 0 */
            bitstream_put_ui(bs, long_term_reference_flag, 1);            /* long_term_reference_flag: 0 */
        } else {
            bitstream_put_ui(bs, adaptive_ref_pic_marking_mode_flag, 1);            /* adaptive_ref_pic_marking_mode_flag: 0 */
        }
    }

    if (context->pic_param.pic_fields.bits.entropy_coding_mode_flag &&
        !IS_I_SLICE(context->slice_param.slice_type))
        bitstream_put_ue(bs, context->slice_param.cabac_init_idc);               /* cabac_init_idc: 0 */

    bitstream_put_se(bs, context->slice_param.slice_qp_delta);                   /* slice_qp_delta: 0 */

    /* ignore for SP/SI */

    if (context->pic_param.pic_fields.bits.deblocking_filter_control_present_flag) {
        bitstream_put_ue(bs, context->slice_param.disable_deblocking_filter_idc);           /* disable_deblocking_filter_idc: 0 */

        if (context->slice_param.disable_deblocking_filter_idc != 1) {
            bitstream_put_se(bs, context->slice_param.slice_alpha_c0_offset_div2);          /* slice_alpha_c0_offset_div2: 2 */
            bitstream_put_se(bs, context->slice_param.slice_beta_offset_div2);              /* slice_beta_offset_div2: 2 */
        }
    }

    if (context->pic_param.pic_fields.bits.entropy_coding_mode_flag) {
        bitstream_byte_aligning(bs, 1);
    }
}

static int
build_packed_pic_buffer(VA264Context * context, unsigned char **header_buffer)
{
    bitstream bs;

    bitstream_start(&bs);
    nal_start_code_prefix(&bs);
    nal_header(&bs, NAL_REF_IDC_HIGH, NAL_PPS);
    pps_rbsp(context, &bs);
    bitstream_end(&bs);

    *header_buffer = (unsigned char *)bs.buffer;
    return bs.bit_offset;
}

static int
build_packed_seq_buffer(VA264Context * context, unsigned char **header_buffer)
{
    bitstream bs;

    bitstream_start(&bs);
    nal_start_code_prefix(&bs);
    nal_header(&bs, NAL_REF_IDC_HIGH, NAL_SPS);
    sps_rbsp(context, &bs);
    bitstream_end(&bs);

    *header_buffer = (unsigned char *)bs.buffer;
    return bs.bit_offset;
}

static int build_packed_slice_buffer(VA264Context * context, unsigned char **header_buffer)
{
    bitstream bs;
    int is_idr = !!context->pic_param.pic_fields.bits.idr_pic_flag;
    int is_ref = !!context->pic_param.pic_fields.bits.reference_pic_flag;

    bitstream_start(&bs);
    nal_start_code_prefix(&bs);

    if (IS_I_SLICE(context->slice_param.slice_type)) {
        nal_header(&bs, NAL_REF_IDC_HIGH, is_idr ? NAL_IDR : NAL_NON_IDR);
    } else if (IS_P_SLICE(context->slice_param.slice_type)) {
        nal_header(&bs, NAL_REF_IDC_MEDIUM, NAL_NON_IDR);
    } else {
        assert(IS_B_SLICE(context->slice_param.slice_type));
        nal_header(&bs, is_ref ? NAL_REF_IDC_LOW : NAL_REF_IDC_NONE, NAL_NON_IDR);
    }

    slice_header(context, &bs);
    bitstream_end(&bs);

    *header_buffer = (unsigned char *)bs.buffer;
    return bs.bit_offset;
}


/*
 * Helper function for profiling purposes
 */
static unsigned int GetTickCount()
{
    struct timeval tv;
    if (gettimeofday(&tv, NULL))
        return 0;
    return tv.tv_usec/1000+tv.tv_sec*1000;
}

/*
  Assume frame sequence is: Frame#0,#1,#2,...,#M,...,#X,... (encoding order)
  1) period between Frame #X and Frame #N = #X - #N
  2) 0 means infinite for intra_period/intra_idr_period, and 0 is invalid for ip_period
  3) intra_idr_period % intra_period (intra_period > 0) and intra_period % ip_period must be 0
  4) intra_period and intra_idr_period take precedence over ip_period
  5) if ip_period > 1, intra_period and intra_idr_period are not  the strict periods
     of I/IDR frames, see bellow examples
  -------------------------------------------------------------------
  intra_period intra_idr_period ip_period frame sequence (intra_period/intra_idr_period/ip_period)
  0            ignored          1          IDRPPPPPPP ...     (No IDR/I any more)
  0            ignored        >=2          IDR(PBB)(PBB)...   (No IDR/I any more)
  1            0                ignored    IDRIIIIIII...      (No IDR any more)
  1            1                ignored    IDR IDR IDR IDR...
  1            >=2              ignored    IDRII IDRII IDR... (1/3/ignore)
  >=2          0                1          IDRPPP IPPP I...   (3/0/1)
  >=2          0              >=2          IDR(PBB)(PBB)(IBB) (6/0/3)
                                              (PBB)(IBB)(PBB)(IBB)...
  >=2          >=2              1          IDRPPPPP IPPPPP IPPPPP (6/18/1)
                                           IDRPPPPP IPPPPP IPPPPP...
  >=2          >=2              >=2        {IDR(PBB)(PBB)(IBB)(PBB)(IBB)(PBB)} (6/18/3)
                                           {IDR(PBB)(PBB)(IBB)(PBB)(IBB)(PBB)}...
                                           {IDR(PBB)(PBB)(IBB)(PBB)}           (6/12/3)
                                           {IDR(PBB)(PBB)(IBB)(PBB)}...
                                           {IDR(PBB)(PBB)}                     (6/6/3)
                                           {IDR(PBB)(PBB)}.
*/

/*
 * Return displaying order with specified periods and encoding order
 * displaying_order: displaying order
 * frame_type: frame type
 */
#define FRAME_P 0
#define FRAME_B 1
#define FRAME_I 2
#define FRAME_IDR 7
void encoding2display_order(
    unsigned long long encoding_order,int intra_period,
    int intra_idr_period,int ip_period,
    unsigned long long *displaying_order,
    int *frame_type)
{
    int encoding_order_gop = 0;

    if (intra_period == 1) { /* all are I/IDR frames */
        *displaying_order = encoding_order;
        if (intra_idr_period == 0)
            *frame_type = (encoding_order == 0)?FRAME_IDR:FRAME_I;
        else
            *frame_type = (encoding_order % intra_idr_period == 0)?FRAME_IDR:FRAME_I;
        return;
    }

    if (intra_period == 0)
        intra_idr_period = 0;

    /* new sequence like
     * IDR PPPPP IPPPPP
     * IDR (PBB)(PBB)(IBB)(PBB)
     */
    encoding_order_gop = (intra_idr_period == 0) ? encoding_order :
        (encoding_order % (intra_idr_period + ((ip_period == 1) ? 0 : 1)));

    if (encoding_order_gop == 0) { /* the first frame */
        *frame_type = FRAME_IDR;
        *displaying_order = encoding_order;
    } else if (((encoding_order_gop - 1) % ip_period) != 0) { /* B frames */
        *frame_type = FRAME_B;
        *displaying_order = encoding_order - 1;
    } else if ((intra_period != 0) && /* have I frames */
               (encoding_order_gop >= 2) &&
               ((ip_period == 1 && encoding_order_gop % intra_period == 0) || /* for IDR PPPPP IPPPP */
                /* for IDR (PBB)(PBB)(IBB) */
                (ip_period >= 2 && ((encoding_order_gop - 1) / ip_period % (intra_period / ip_period)) == 0))) {
    *frame_type = FRAME_I;
    *displaying_order = encoding_order + ip_period - 1;
    } else {
    *frame_type = FRAME_P;
    *displaying_order = encoding_order + ip_period - 1;
    }
}


static char *fourcc_to_string(int fourcc)
{
    switch (fourcc) {
    case VA_FOURCC_NV12:
        return "NV12";
    case VA_FOURCC_I420:
        return "I420";
    case VA_FOURCC_YV12:
        return "YV12";
    case VA_FOURCC_UYVY:
        return "UYVY";
    default:
        return "Unknown";
    }
}

static int string_to_fourcc(char *str)
{
    int fourcc;

    if (!strncmp(str, "NV12", 4))
        fourcc = VA_FOURCC_NV12;
    else if (!strncmp(str, "I420", 4))
        fourcc = VA_FOURCC_I420;
    else if (!strncmp(str, "YV12", 4))
        fourcc = VA_FOURCC_YV12;
    else if (!strncmp(str, "UYVY", 4))
        fourcc = VA_FOURCC_UYVY;
    else {
        printf("Unknow FOURCC\n");
        fourcc = -1;
    }
    return fourcc;
}


static char *rc_to_string(int rcmode)
{
    switch (rcmode) {
    case VA_RC_NONE:
        return "NONE";
    case VA_RC_CBR:
        return "CBR";
    case VA_RC_VBR:
        return "VBR";
    case VA_RC_VCM:
        return "VCM";
    case VA_RC_CQP:
        return "CQP";
    case VA_RC_VBR_CONSTRAINED:
        return "VBR_CONSTRAINED";
    default:
        return "Unknown";
    }
}

static int init_va(VA264Context * context)
{
    VAProfile profile_list[]={VAProfileH264High,VAProfileH264Main,VAProfileH264ConstrainedBaseline};
    VAEntrypoint *entrypoints;
    int num_entrypoints, slice_entrypoint;
    int support_encode = 0;
    int major_ver, minor_ver;
    VAStatus va_status;
    unsigned int i;

    context->va_dpy = va_open_display();
    if(!context->va_dpy) {
        return VA_STATUS_ERROR_INVALID_DISPLAY;
    }

    va_status = vaInitialize(context->va_dpy, &major_ver, &minor_ver);
    CHECK_VASTATUS(va_status, "vaInitialize");

    num_entrypoints = vaMaxNumEntrypoints(context->va_dpy);
    entrypoints = malloc(num_entrypoints * sizeof(*entrypoints));
    if (!entrypoints) {
        fprintf(stderr, "error: failed to initialize VA entrypoints array\n");
        return VA_STATUS_ERROR_INVALID_DISPLAY;
    }

    /* use the highest profile */
    for (i = 0; i < sizeof(profile_list)/sizeof(profile_list[0]); i++) {
        if ((context->config.h264_profile != ~0) && context->config.h264_profile != profile_list[i])
            continue;

        context->config.h264_profile = profile_list[i];
        vaQueryConfigEntrypoints(context->va_dpy, context->config.h264_profile, entrypoints, &num_entrypoints);
        for (slice_entrypoint = 0; slice_entrypoint < num_entrypoints; slice_entrypoint++) {
            if (requested_entrypoint == -1 ) {
                // Select the entry point based on what is avaiable
                if ( (entrypoints[slice_entrypoint] == VAEntrypointEncSlice) ||
                     (entrypoints[slice_entrypoint] == VAEntrypointEncSliceLP) ) {
                    support_encode = 1;
                    selected_entrypoint = entrypoints[slice_entrypoint];
                    break;
                }
            } else if ((entrypoints[slice_entrypoint] == requested_entrypoint)) {
                // Select the entry point based on what was requested in cmd line option
                support_encode = 1;
                selected_entrypoint = entrypoints[slice_entrypoint];
                break;
            }
        }
        if (support_encode == 1) {
            printf("Using EntryPoint - %d \n",selected_entrypoint);
            break;
        }
    }

    if (support_encode == 0) {
        printf("Can't find VAEntrypointEncSlice or VAEntrypointEncSliceLP for H264 profiles\n");
        return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
    } else {
        switch (context->config.h264_profile) {
            case VAProfileH264ConstrainedBaseline:
                printf("Use profile VAProfileH264ConstrainedBaseline\n");
                context->constraint_set_flag |= (1 << 0 | 1 << 1); /* Annex A.2.2 */
                context->config.ip_period = 1;
                break;

            case VAProfileH264Main:
                printf("Use profile VAProfileH264Main\n");
                context->constraint_set_flag |= (1 << 1); /* Annex A.2.2 */
                break;

            case VAProfileH264High:
                context->constraint_set_flag |= (1 << 3); /* Annex A.2.4 */
                printf("Use profile VAProfileH264High\n");
                break;
            default:
                printf("unknow profile. Set to Constrained Baseline");
                context->config.h264_profile = VAProfileH264ConstrainedBaseline;
                context->constraint_set_flag |= (1 << 0 | 1 << 1); /* Annex A.2.1 & A.2.2 */
                context->config.ip_period = 1;
                break;
        }
    }

    /* find out the format for the render target, and rate control mode */
    for (i = 0; i < VAConfigAttribTypeMax; i++)
        context->attrib[i].type = i;

    va_status = vaGetConfigAttributes(context->va_dpy, context->config.h264_profile, selected_entrypoint,
                                      &context->attrib[0], VAConfigAttribTypeMax);
    CHECK_VASTATUS(va_status, "vaGetConfigAttributes");
    /* check the interested configattrib */
    if ((context->attrib[VAConfigAttribRTFormat].value & VA_RT_FORMAT_YUV420) == 0) {
        printf("Not find desired YUV420 RT format\n");
        return VA_STATUS_ERROR_INVALID_CONFIG;
    } else {
        context->config_attrib[context->config_attrib_num].type = VAConfigAttribRTFormat;
        context->config_attrib[context->config_attrib_num].value = VA_RT_FORMAT_YUV420;
        context->config_attrib_num++;
    }

    if (context->attrib[VAConfigAttribRateControl].value != VA_ATTRIB_NOT_SUPPORTED) {
        context->attrib[VAConfigAttribRateControl].value = VA_RC_CBR;
        int tmp = context->attrib[VAConfigAttribRateControl].value;

        printf("Support rate control mode (0x%x):", tmp);

        if (tmp & VA_RC_NONE)
            printf("NONE ");
        if (tmp & VA_RC_CBR)
            printf("CBR ");
        if (tmp & VA_RC_VBR)
            printf("VBR ");
        if (tmp & VA_RC_VCM)
            printf("VCM ");
        if (tmp & VA_RC_CQP)
            printf("CQP ");
        if (tmp & VA_RC_VBR_CONSTRAINED)
            printf("VBR_CONSTRAINED ");

        printf("\n");

        if (context->config.rc_mode == -1 || !(context->config.rc_mode & tmp))  {
            if (context->config.rc_mode != -1) {
                printf("Warning: Don't support the specified RateControl mode: %s!!!, switch to ", rc_to_string(context->config.rc_mode));
            }

            for (i = 0; i < sizeof(rc_default_modes) / sizeof(rc_default_modes[0]); i++) {
                if (rc_default_modes[i] & tmp) {
                    context->config.rc_mode = rc_default_modes[i];
                    break;
                }
            }

            printf("RateControl mode: %s\n", rc_to_string(context->config.rc_mode));
        }

        context->config_attrib[context->config_attrib_num].type = VAConfigAttribRateControl;
        context->config_attrib[context->config_attrib_num].value = context->config.rc_mode;
        context->config_attrib_num++;
    }


    if (context->attrib[VAConfigAttribEncPackedHeaders].value != VA_ATTRIB_NOT_SUPPORTED) {
        int tmp = context->attrib[VAConfigAttribEncPackedHeaders].value;

        printf("Support VAConfigAttribEncPackedHeaders\n");

        context->h264_packedheader = 1;
        context->config_attrib[context->config_attrib_num].type = VAConfigAttribEncPackedHeaders;
        context->config_attrib[context->config_attrib_num].value = VA_ENC_PACKED_HEADER_NONE;

        if (tmp & VA_ENC_PACKED_HEADER_SEQUENCE) {
            printf("Support packed sequence headers\n");
            context->config_attrib[context->config_attrib_num].value |= VA_ENC_PACKED_HEADER_SEQUENCE;
        }

        if (tmp & VA_ENC_PACKED_HEADER_PICTURE) {
            printf("Support packed picture headers\n");
            context->config_attrib[context->config_attrib_num].value |= VA_ENC_PACKED_HEADER_PICTURE;
        }

        if (tmp & VA_ENC_PACKED_HEADER_SLICE) {
            printf("Support packed slice headers\n");
            context->config_attrib[context->config_attrib_num].value |= VA_ENC_PACKED_HEADER_SLICE;
        }

        if (tmp & VA_ENC_PACKED_HEADER_MISC) {
            printf("Support packed misc headers\n");
            context->config_attrib[context->config_attrib_num].value |= VA_ENC_PACKED_HEADER_MISC;
        }

        context->enc_packed_header_idx = context->config_attrib_num;
        context->config_attrib_num++;
    }

    if (context->attrib[VAConfigAttribEncInterlaced].value != VA_ATTRIB_NOT_SUPPORTED) {
        int tmp = context->attrib[VAConfigAttribEncInterlaced].value;

        printf("Support VAConfigAttribEncInterlaced\n");

        if (tmp & VA_ENC_INTERLACED_FRAME)
            printf("support VA_ENC_INTERLACED_FRAME\n");
        if (tmp & VA_ENC_INTERLACED_FIELD)
            printf("Support VA_ENC_INTERLACED_FIELD\n");
        if (tmp & VA_ENC_INTERLACED_MBAFF)
            printf("Support VA_ENC_INTERLACED_MBAFF\n");
        if (tmp & VA_ENC_INTERLACED_PAFF)
            printf("Support VA_ENC_INTERLACED_PAFF\n");

        context->config_attrib[context->config_attrib_num].type = VAConfigAttribEncInterlaced;
        context->config_attrib[context->config_attrib_num].value = VA_ENC_PACKED_HEADER_NONE;
        context->config_attrib_num++;
    }

    if (context->attrib[VAConfigAttribEncMaxRefFrames].value != VA_ATTRIB_NOT_SUPPORTED) {
            context->h264_maxref = context->attrib[VAConfigAttribEncMaxRefFrames].value;

        printf("Support %d RefPicList0 and %d RefPicList1\n",
               context->h264_maxref & 0xffff, (context->h264_maxref >> 16) & 0xffff );
    }

    if (context->attrib[VAConfigAttribEncMaxSlices].value != VA_ATTRIB_NOT_SUPPORTED)
        printf("Support %d slices\n", context->attrib[VAConfigAttribEncMaxSlices].value);

    if (context->attrib[VAConfigAttribEncSliceStructure].value != VA_ATTRIB_NOT_SUPPORTED) {
        int tmp = context->attrib[VAConfigAttribEncSliceStructure].value;

        printf("Support VAConfigAttribEncSliceStructure\n");

        if (tmp & VA_ENC_SLICE_STRUCTURE_ARBITRARY_ROWS)
            printf("Support VA_ENC_SLICE_STRUCTURE_ARBITRARY_ROWS\n");
        if (tmp & VA_ENC_SLICE_STRUCTURE_POWER_OF_TWO_ROWS)
            printf("Support VA_ENC_SLICE_STRUCTURE_POWER_OF_TWO_ROWS\n");
        if (tmp & VA_ENC_SLICE_STRUCTURE_ARBITRARY_MACROBLOCKS)
            printf("Support VA_ENC_SLICE_STRUCTURE_ARBITRARY_MACROBLOCKS\n");
    }
    if (context->attrib[VAConfigAttribEncMacroblockInfo].value != VA_ATTRIB_NOT_SUPPORTED) {
        printf("Support VAConfigAttribEncMacroblockInfo\n");
    }

    free(entrypoints);
    return 0;
}

static int setup_encode(VA264Context * context)
{
    VAStatus va_status;
    VASurfaceID *tmp_surfaceid;
    int codedbuf_size, i;

    va_status = vaCreateConfig(context->va_dpy, context->config.h264_profile, selected_entrypoint,
            &context->config_attrib[0], context->config_attrib_num, &context->config_id);
    CHECK_VASTATUS(va_status, "vaCreateConfig");

    /* create source surfaces */
    va_status = vaCreateSurfaces(context->va_dpy,
                                 VA_RT_FORMAT_YUV420, context->frame_width_mbaligned, context->frame_height_mbaligned,
                                 &context->src_surface[0], SURFACE_NUM,
                                 NULL, 0);
    CHECK_VASTATUS(va_status, "vaCreateSurfaces");

    /* create reference surfaces */
    va_status = vaCreateSurfaces(
        context->va_dpy,
        VA_RT_FORMAT_YUV420, context->frame_width_mbaligned, context->frame_height_mbaligned,
        &context->ref_surface[0], SURFACE_NUM,
        NULL, 0
        );
    CHECK_VASTATUS(va_status, "vaCreateSurfaces");

    tmp_surfaceid = calloc(2 * SURFACE_NUM, sizeof(VASurfaceID));
    assert(tmp_surfaceid);
    memcpy(tmp_surfaceid, context->src_surface, SURFACE_NUM * sizeof(VASurfaceID));
    memcpy(tmp_surfaceid + SURFACE_NUM, context->ref_surface, SURFACE_NUM * sizeof(VASurfaceID));

    /* Create a context for this encode pipe */
    va_status = vaCreateContext(context->va_dpy, 
                                context->config_id,
                                context->frame_width_mbaligned, context->frame_height_mbaligned,
                                VA_PROGRESSIVE,
                                tmp_surfaceid, 2 * SURFACE_NUM,
                                &context->context_id);
    CHECK_VASTATUS(va_status, "vaCreateContext");
    free(tmp_surfaceid);

    codedbuf_size = (context->frame_width_mbaligned * context->frame_height_mbaligned * 400) / (16 * 16);

    for (i = 0; i < SURFACE_NUM; i++) {
        /* create coded buffer once for all
         * other VA buffers which won't be used again after vaRenderPicture.
         * so APP can always vaCreateBuffer for every frame
         * but coded buffer need to be mapped and accessed after vaRenderPicture/vaEndPicture
         * so VA won't maintain the coded buffer
         */
        va_status = vaCreateBuffer(context->va_dpy, context->context_id, VAEncCodedBufferType,
                codedbuf_size, 1, NULL, &context->coded_buf[i]);
        CHECK_VASTATUS(va_status,"vaCreateBuffer");
    }

    return 0;
}



#define partition(ref, field, key, ascending)   \
    while (i <= j) {                            \
        if (ascending) {                        \
            while (ref[i].field < key)          \
                i++;                            \
            while (ref[j].field > key)          \
                j--;                            \
        } else {                                \
            while (ref[i].field > key)          \
                i++;                            \
            while (ref[j].field < key)          \
                j--;                            \
        }                                       \
        if (i <= j) {                           \
            tmp = ref[i];                       \
            ref[i] = ref[j];                    \
            ref[j] = tmp;                       \
            i++;                                \
            j--;                                \
        }                                       \
    }                                           \

static void sort_one(VAPictureH264 ref[], int left, int right,
                     int ascending, int frame_idx)
{
    int i = left, j = right;
    unsigned int key;
    VAPictureH264 tmp;

    if (frame_idx) {
        key = ref[(left + right) / 2].frame_idx;
        partition(ref, frame_idx, key, ascending);
    } else {
        key = ref[(left + right) / 2].TopFieldOrderCnt;
        partition(ref, TopFieldOrderCnt, (signed int)key, ascending);
    }

    /* recursion */
    if (left < j)
        sort_one(ref, left, j, ascending, frame_idx);

    if (i < right)
        sort_one(ref, i, right, ascending, frame_idx);
}

static void sort_two(VAPictureH264 ref[], int left, int right, unsigned int key, unsigned int frame_idx,
                     int partition_ascending, int list0_ascending, int list1_ascending)
{
    int i = left, j = right;
    VAPictureH264 tmp;

    if (frame_idx) {
        partition(ref, frame_idx, key, partition_ascending);
    } else {
        partition(ref, TopFieldOrderCnt, (signed int)key, partition_ascending);
    }


    sort_one(ref, left, i-1, list0_ascending, frame_idx);
    sort_one(ref, j+1, right, list1_ascending, frame_idx);
}

static int update_ReferenceFrames(VA264Context * context)
{
    int i;

    if (context->current_frame_type == FRAME_B)
        return 0;

    context->CurrentCurrPic.flags = VA_PICTURE_H264_SHORT_TERM_REFERENCE;
    context->numShortTerm++;
    if (context->numShortTerm > num_ref_frames)
        context->numShortTerm = num_ref_frames;
    for (i = context->numShortTerm - 1; i > 0; i--)
        context->ReferenceFrames[i] = context->ReferenceFrames[i-1];
    context->ReferenceFrames[0] = context->CurrentCurrPic;

    if (context->current_frame_type != FRAME_B)
        context->current_frame_num++;
    if (context->current_frame_num > MaxFrameNum)
        context->current_frame_num = 0;

    return 0;
}


static int update_RefPicList(VA264Context * context)
{
    unsigned int current_poc = context->CurrentCurrPic.TopFieldOrderCnt;

    if (context->current_frame_type == FRAME_P) {
        memcpy(context->RefPicList0_P, context->ReferenceFrames, context->numShortTerm * sizeof(VAPictureH264));
        sort_one(context->RefPicList0_P, 0, context->numShortTerm - 1, 0, 1);
    }

    if (context->current_frame_type == FRAME_B) {
        memcpy(context->RefPicList0_B, context->ReferenceFrames, context->numShortTerm * sizeof(VAPictureH264));
        sort_two(context->RefPicList0_B, 0, context->numShortTerm - 1, current_poc, 0,
                 1, 0, 1);

        memcpy(context->RefPicList1_B, context->ReferenceFrames, context->numShortTerm * sizeof(VAPictureH264));
        sort_two(context->RefPicList1_B, 0, context->numShortTerm - 1, current_poc, 0,
                 0, 1, 0);
    }

    return 0;
}


static int render_sequence(VA264Context * context)
{
    VABufferID seq_param_buf, rc_param_buf, misc_param_tmpbuf, render_id[2];
    VAStatus va_status;
    VAEncMiscParameterBuffer *misc_param, *misc_param_tmp;
    VAEncMiscParameterRateControl *misc_rate_ctrl;

    context->seq_param.level_idc = 41 /*SH_LEVEL_3*/;
    context->seq_param.picture_width_in_mbs = context->frame_width_mbaligned / 16;
    context->seq_param.picture_height_in_mbs = context->frame_height_mbaligned / 16;
    context->seq_param.bits_per_second = context->config.frame_bitrate;

    context->seq_param.intra_period = context->config.intra_period;
    context->seq_param.intra_idr_period = context->config.intra_idr_period;
    context->seq_param.ip_period = context->config.ip_period;

    context->seq_param.max_num_ref_frames = num_ref_frames;
    context->seq_param.seq_fields.bits.frame_mbs_only_flag = 1;
    context->seq_param.time_scale = 900;
    context->seq_param.num_units_in_tick = 15; /* Tc = num_units_in_tick / time_sacle */
    context->seq_param.seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 = Log2MaxPicOrderCntLsb - 4;
    context->seq_param.seq_fields.bits.log2_max_frame_num_minus4 = Log2MaxFrameNum - 4;
    context->seq_param.seq_fields.bits.frame_mbs_only_flag = 1;
    context->seq_param.seq_fields.bits.chroma_format_idc = 1;
    context->seq_param.seq_fields.bits.direct_8x8_inference_flag = 1;

    if (context->config.frame_width != context->frame_width_mbaligned ||
        context->config.frame_height != context->frame_height_mbaligned) {
        context->seq_param.frame_cropping_flag = 1;
        context->seq_param.frame_crop_left_offset = 0;
        context->seq_param.frame_crop_right_offset = (context->frame_width_mbaligned - context->config.frame_width)/2;
        context->seq_param.frame_crop_top_offset = 0;
        context->seq_param.frame_crop_bottom_offset = (context->frame_height_mbaligned - context->config.frame_height)/2;
    }

    va_status = vaCreateBuffer(context->va_dpy, context->context_id,
                               VAEncSequenceParameterBufferType,
                               sizeof(context->seq_param), 1, &context->seq_param, &seq_param_buf);
    CHECK_VASTATUS(va_status,"vaCreateBuffer");

    va_status = vaCreateBuffer(context->va_dpy, context->context_id,
                               VAEncMiscParameterBufferType,
                               sizeof(VAEncMiscParameterBuffer) + sizeof(VAEncMiscParameterRateControl),
                               1,NULL,&rc_param_buf);
    CHECK_VASTATUS(va_status,"vaCreateBuffer");

    vaMapBuffer(context->va_dpy, rc_param_buf,(void **)&misc_param);
    misc_param->type = VAEncMiscParameterTypeRateControl;
    misc_rate_ctrl = (VAEncMiscParameterRateControl *)misc_param->data;
    memset(misc_rate_ctrl, 0, sizeof(*misc_rate_ctrl));
    misc_rate_ctrl->bits_per_second = context->config.frame_bitrate;
    misc_rate_ctrl->target_percentage = 66;
    misc_rate_ctrl->window_size = 1000;
    misc_rate_ctrl->initial_qp = context->config.initial_qp;
    misc_rate_ctrl->min_qp = context->config.minimal_qp;
    misc_rate_ctrl->basic_unit_size = 0;
    vaUnmapBuffer(context->va_dpy, rc_param_buf);

    render_id[0] = seq_param_buf;
    render_id[1] = rc_param_buf;

    va_status = vaRenderPicture(context->va_dpy, context->context_id, &render_id[0], 2);
    CHECK_VASTATUS(va_status,"vaRenderPicture");;

    return 0;
}

static char *frametype_to_string(int ftype)
{
    switch (ftype) {
    case FRAME_P:
        return "P";
    case FRAME_B:
        return "B";
    case FRAME_I:
        return "I";
    case FRAME_IDR:
        return "IDR";
    default:
        return "Unknown";
    }
}

static int calc_poc(VA264Context * context, int pic_order_cnt_lsb)
{
    static int PicOrderCntMsb_ref = 0, pic_order_cnt_lsb_ref = 0;
    int prevPicOrderCntMsb, prevPicOrderCntLsb;
    int PicOrderCntMsb, TopFieldOrderCnt;

    if (context->current_frame_type == FRAME_IDR)
        prevPicOrderCntMsb = prevPicOrderCntLsb = 0;
    else {
        prevPicOrderCntMsb = PicOrderCntMsb_ref;
        prevPicOrderCntLsb = pic_order_cnt_lsb_ref;
    }

    if ((pic_order_cnt_lsb < prevPicOrderCntLsb) &&
        ((prevPicOrderCntLsb - pic_order_cnt_lsb) >= (int)(MaxPicOrderCntLsb / 2)))
        PicOrderCntMsb = prevPicOrderCntMsb + MaxPicOrderCntLsb;
    else if ((pic_order_cnt_lsb > prevPicOrderCntLsb) &&
             ((pic_order_cnt_lsb - prevPicOrderCntLsb) > (int)(MaxPicOrderCntLsb / 2)))
        PicOrderCntMsb = prevPicOrderCntMsb - MaxPicOrderCntLsb;
    else
        PicOrderCntMsb = prevPicOrderCntMsb;

    TopFieldOrderCnt = PicOrderCntMsb + pic_order_cnt_lsb;

    if (context->current_frame_type != FRAME_B) {
        PicOrderCntMsb_ref = PicOrderCntMsb;
        pic_order_cnt_lsb_ref = pic_order_cnt_lsb;
    }

    return TopFieldOrderCnt;
}

static int render_picture(VA264Context * context)
{
    VABufferID pic_param_buf;
    VAStatus va_status;
    int i = 0;

    context->pic_param.CurrPic.picture_id = context->ref_surface[(context->current_frame_display % SURFACE_NUM)];
    context->pic_param.CurrPic.frame_idx = context->current_frame_num;
    context->pic_param.CurrPic.flags = 0;
    context->pic_param.CurrPic.TopFieldOrderCnt = calc_poc(context, (context->current_frame_display - context->current_IDR_display) % MaxPicOrderCntLsb);
    context->pic_param.CurrPic.BottomFieldOrderCnt = context->pic_param.CurrPic.TopFieldOrderCnt;
    context->CurrentCurrPic = context->pic_param.CurrPic;

    if (getenv("TO_DEL")) { /* set RefPicList into ReferenceFrames */
        update_RefPicList(context); /* calc RefPicList */
        memset(context->pic_param.ReferenceFrames, 0xff, 16 * sizeof(VAPictureH264)); /* invalid all */
        if (context->current_frame_type == FRAME_P) {
            context->pic_param.ReferenceFrames[0] = context->RefPicList0_P[0];
        } else if (context->current_frame_type == FRAME_B) {
            context->pic_param.ReferenceFrames[0] = context->RefPicList0_B[0];
            context->pic_param.ReferenceFrames[1] = context->RefPicList1_B[0];
        }
    } else {
        memcpy(context->pic_param.ReferenceFrames, context->ReferenceFrames, context->numShortTerm * sizeof(VAPictureH264));
        for (i = context->numShortTerm; i < SURFACE_NUM; i++) {
            context->pic_param.ReferenceFrames[i].picture_id = VA_INVALID_SURFACE;
            context->pic_param.ReferenceFrames[i].flags = VA_PICTURE_H264_INVALID;
        }
    }

    context->pic_param.pic_fields.bits.idr_pic_flag = (context->current_frame_type == FRAME_IDR);
    context->pic_param.pic_fields.bits.reference_pic_flag = (context->current_frame_type != FRAME_B);
    context->pic_param.pic_fields.bits.entropy_coding_mode_flag = context->config.h264_entropy_mode;
    context->pic_param.pic_fields.bits.deblocking_filter_control_present_flag = 1;
    context->pic_param.frame_num = context->current_frame_num;
    context->pic_param.coded_buf = context->coded_buf[(context->current_frame_display % SURFACE_NUM)];
    context->pic_param.last_picture = 0; // (context->current_frame_encoding == frame_count);
    context->pic_param.pic_init_qp = context->config.initial_qp;

    va_status = vaCreateBuffer(context->va_dpy, context->context_id, VAEncPictureParameterBufferType,
                               sizeof(context->pic_param), 1, &context->pic_param, &pic_param_buf);
    CHECK_VASTATUS(va_status,"vaCreateBuffer");;

    va_status = vaRenderPicture(context->va_dpy, context->context_id, &pic_param_buf, 1);
    CHECK_VASTATUS(va_status,"vaRenderPicture");

    return 0;
}

static int render_packedsequence(VA264Context * context)
{
    VAEncPackedHeaderParameterBuffer packedheader_param_buffer;
    VABufferID packedseq_para_bufid, packedseq_data_bufid, render_id[2];
    unsigned int length_in_bits;
    unsigned char *packedseq_buffer = NULL;
    VAStatus va_status;

    length_in_bits = build_packed_seq_buffer(context, &packedseq_buffer);

    packedheader_param_buffer.type = VAEncPackedHeaderSequence;

    packedheader_param_buffer.bit_length = length_in_bits; /*length_in_bits*/
    packedheader_param_buffer.has_emulation_bytes = 0;
    va_status = vaCreateBuffer(context->va_dpy,
                               context->context_id,
                               VAEncPackedHeaderParameterBufferType,
                               sizeof(packedheader_param_buffer), 1, &packedheader_param_buffer,
                               &packedseq_para_bufid);
    CHECK_VASTATUS(va_status,"vaCreateBuffer");

    va_status = vaCreateBuffer(context->va_dpy,
                               context->context_id,
                               VAEncPackedHeaderDataBufferType,
                               (length_in_bits + 7) / 8, 1, packedseq_buffer,
                               &packedseq_data_bufid);
    CHECK_VASTATUS(va_status,"vaCreateBuffer");

    render_id[0] = packedseq_para_bufid;
    render_id[1] = packedseq_data_bufid;
    va_status = vaRenderPicture(context->va_dpy, context->context_id, render_id, 2);
    CHECK_VASTATUS(va_status,"vaRenderPicture");

    free(packedseq_buffer);

    return 0;
}

static int render_packedpicture(VA264Context * context)
{
    VAEncPackedHeaderParameterBuffer packedheader_param_buffer;
    VABufferID packedpic_para_bufid, packedpic_data_bufid, render_id[2];
    unsigned int length_in_bits;
    unsigned char *packedpic_buffer = NULL;
    VAStatus va_status;

    length_in_bits = build_packed_pic_buffer(context, &packedpic_buffer);
    packedheader_param_buffer.type = VAEncPackedHeaderPicture;
    packedheader_param_buffer.bit_length = length_in_bits;
    packedheader_param_buffer.has_emulation_bytes = 0;

    va_status = vaCreateBuffer(context->va_dpy,
                               context->context_id,
                               VAEncPackedHeaderParameterBufferType,
                               sizeof(packedheader_param_buffer), 1, &packedheader_param_buffer,
                               &packedpic_para_bufid);
    CHECK_VASTATUS(va_status,"vaCreateBuffer");

    va_status = vaCreateBuffer(context->va_dpy,
                               context->context_id,
                               VAEncPackedHeaderDataBufferType,
                               (length_in_bits + 7) / 8, 1, packedpic_buffer,
                               &packedpic_data_bufid);
    CHECK_VASTATUS(va_status,"vaCreateBuffer");

    render_id[0] = packedpic_para_bufid;
    render_id[1] = packedpic_data_bufid;
    va_status = vaRenderPicture(context->va_dpy, context->context_id, render_id, 2);
    CHECK_VASTATUS(va_status,"vaRenderPicture");

    free(packedpic_buffer);

    return 0;
}

static void render_packedslice(VA264Context * context)
{
    VAEncPackedHeaderParameterBuffer packedheader_param_buffer;
    VABufferID packedslice_para_bufid, packedslice_data_bufid, render_id[2];
    unsigned int length_in_bits;
    unsigned char *packedslice_buffer = NULL;
    VAStatus va_status;

    length_in_bits = build_packed_slice_buffer(context, &packedslice_buffer);
    packedheader_param_buffer.type = VAEncPackedHeaderSlice;
    packedheader_param_buffer.bit_length = length_in_bits;
    packedheader_param_buffer.has_emulation_bytes = 0;

    va_status = vaCreateBuffer(context->va_dpy,
                               context->context_id,
                               VAEncPackedHeaderParameterBufferType,
                               sizeof(packedheader_param_buffer), 1, &packedheader_param_buffer,
                               &packedslice_para_bufid);
    CHECK_VASTATUS_VOID(va_status,"vaCreateBuffer");

    va_status = vaCreateBuffer(context->va_dpy,
                               context->context_id,
                               VAEncPackedHeaderDataBufferType,
                               (length_in_bits + 7) / 8, 1, packedslice_buffer,
                               &packedslice_data_bufid);
    CHECK_VASTATUS_VOID(va_status,"vaCreateBuffer");

    render_id[0] = packedslice_para_bufid;
    render_id[1] = packedslice_data_bufid;
    va_status = vaRenderPicture(context->va_dpy, context->context_id, render_id, 2);
    CHECK_VASTATUS_VOID(va_status,"vaRenderPicture");

    free(packedslice_buffer);
}

static int render_slice(VA264Context * context)
{
    VABufferID slice_param_buf;
    VAStatus va_status;
    int i;

    update_RefPicList(context);

    /* one frame, one slice */
    context->slice_param.macroblock_address = 0;
    context->slice_param.num_macroblocks = context->frame_width_mbaligned * context->frame_height_mbaligned / (16 * 16); /* Measured by MB */
    context->slice_param.slice_type = (context->current_frame_type == FRAME_IDR) ? 2 : context->current_frame_type;
    if (context->current_frame_type == FRAME_IDR) {
        if (context->current_frame_encoding != 0)
            ++context->slice_param.idr_pic_id;
    } else if (context->current_frame_type == FRAME_P) {
        int refpiclist0_max = context->h264_maxref & 0xffff;
        memcpy(context->slice_param.RefPicList0, context->RefPicList0_P, ((refpiclist0_max > 32) ? 32 : refpiclist0_max)*sizeof(VAPictureH264));

        for (i = refpiclist0_max; i < 32; i++) {
            context->slice_param.RefPicList0[i].picture_id = VA_INVALID_SURFACE;
            context->slice_param.RefPicList0[i].flags = VA_PICTURE_H264_INVALID;
        }
    } else if (context->current_frame_type == FRAME_B) {
        int refpiclist0_max = context->h264_maxref & 0xffff;
        int refpiclist1_max = (context->h264_maxref >> 16) & 0xffff;

        memcpy(context->slice_param.RefPicList0, context->RefPicList0_B, ((refpiclist0_max > 32) ? 32 : refpiclist0_max)*sizeof(VAPictureH264));
        for (i = refpiclist0_max; i < 32; i++) {
            context->slice_param.RefPicList0[i].picture_id = VA_INVALID_SURFACE;
            context->slice_param.RefPicList0[i].flags = VA_PICTURE_H264_INVALID;
        }

        memcpy(context->slice_param.RefPicList1, context->RefPicList1_B, ((refpiclist1_max > 32) ? 32 : refpiclist1_max)*sizeof(VAPictureH264));
        for (i = refpiclist1_max; i < 32; i++) {
            context->slice_param.RefPicList1[i].picture_id = VA_INVALID_SURFACE;
            context->slice_param.RefPicList1[i].flags = VA_PICTURE_H264_INVALID;
        }
    }

    context->slice_param.slice_alpha_c0_offset_div2 = 0;
    context->slice_param.slice_beta_offset_div2 = 0;
    context->slice_param.direct_spatial_mv_pred_flag = 1;
    context->slice_param.pic_order_cnt_lsb = (context->current_frame_display - context->current_IDR_display) % MaxPicOrderCntLsb;


    if (context->h264_packedheader &&
        context->config_attrib[context->enc_packed_header_idx].value & VA_ENC_PACKED_HEADER_SLICE)
        render_packedslice(context);

    va_status = vaCreateBuffer(context->va_dpy, context->context_id, VAEncSliceParameterBufferType,
                               sizeof(context->slice_param), 1, &context->slice_param, &slice_param_buf);
    CHECK_VASTATUS(va_status,"vaCreateBuffer");;

    va_status = vaRenderPicture(context->va_dpy, context->context_id, &slice_param_buf, 1);
    CHECK_VASTATUS(va_status,"vaRenderPicture");

    return 0;
}

static int release_encode(VA264Context * context)
{
    int i;

    vaDestroySurfaces(context->va_dpy, &context->src_surface[0], SURFACE_NUM);
    vaDestroySurfaces(context->va_dpy, &context->ref_surface[0], SURFACE_NUM);

    for (i = 0; i < SURFACE_NUM; i++)
        vaDestroyBuffer(context->va_dpy, context->coded_buf[i]);

    vaDestroyContext(context->va_dpy, context->context_id);
    vaDestroyConfig(context->va_dpy, context->config_id);

    return 0;
}

static int deinit_va(VA264Context * context)
{
    vaTerminate(context->va_dpy);

    va_close_display(context->va_dpy);

    return 0;
}

void destroyContext(void * context)
{
    VA264Context * ctx = (VA264Context *)context;
    if(ctx->encoded_buffer)
    {
        free(ctx->encoded_buffer);
        ctx->encoded_buffer = 0;
    }
    release_encode(ctx);
    deinit_va(ctx);
    free(ctx);
}

void * createContext(int width, int height, int bitrate, int intra_period, int idr_period, int ip_period, int frame_rate)
{
    VA264Context * context = (VA264Context*)malloc(sizeof(VA264Context));
    memset((void*)context, 0, sizeof(VA264Context));
    context->config.h264_entropy_mode = 1; // cabac
    context->config.frame_width = width;
    context->config.frame_height = height;
    context->config.frame_rate = frame_rate;
    context->config.frame_bitrate = bitrate;
    context->config.initial_qp = 26;
    context->config.minimal_qp = 0;
    context->config.intra_period = intra_period;
    context->config.intra_idr_period = idr_period;
    context->config.ip_period = ip_period;
    context->config.rc_mode = VA_RC_VBR;//-1;
    context->h264_maxref = (1<<16|1);

    if (context->config.ip_period < 1) {
        printf(" ip_period must be greater than 0\n");
        free(context);
        return NULL;
    }
    if (context->config.intra_period != 1 && context->config.intra_period % context->config.ip_period != 0) {
        printf(" intra_period must be a multiplier of ip_period\n");
        free(context);
        return NULL;
    }
    if (context->config.intra_period != 0 && context->config.intra_idr_period % context->config.intra_period != 0) {
        printf(" idr_period must be a multiplier of intra_period\n");
        free(context);
        return NULL;
    }

    if (context->config.frame_bitrate == 0)
    {
        context->config.frame_bitrate = context->config.frame_width * context->config.frame_height * 12 * context->config.frame_rate / 50;
    }

    // one of: VAProfileH264ConstrainedBaseline, VAProfileH264Main, VAProfileH264High
    context->config.h264_profile = VAProfileH264Main;

    context->frame_width_mbaligned = (context->config.frame_width + 15) & (~15);
    context->frame_height_mbaligned = (context->config.frame_height + 15) & (~15);
    if (context->config.frame_width != context->frame_width_mbaligned ||
        context->config.frame_height != context->frame_height_mbaligned) {
        printf("Source frame is %dx%d and will code clip to %dx%d with crop\n",
               context->config.frame_width, context->config.frame_height,
               context->frame_width_mbaligned, context->frame_height_mbaligned
               );
    }

    // the buffer to receive the encoded frames from encodeImage
    context->encoded_buffer = (uint8_t*)malloc(context->frame_width_mbaligned * context->frame_height_mbaligned * 3);
    
    if(init_va(context) != VA_STATUS_SUCCESS) {
        free(context->encoded_buffer);
        return NULL;
    }
    
    if(setup_encode(context) != VA_STATUS_SUCCESS) {
        free(context->encoded_buffer);
        return NULL;
    }

    // reset sps/pps/slice params
    memset(&context->seq_param, 0, sizeof(context->seq_param));
    memset(&context->pic_param, 0, sizeof(context->pic_param));
    memset(&context->slice_param, 0, sizeof(context->slice_param));

    return (void*)context;
}

uint8_t * encodeImage(void * ctx, int fourcc, uint8_t * y, uint8_t * u, uint8_t * v, int * encodedsize, bool forceIDR)
{
    VA264Context * context = (VA264Context *)ctx;

    if(forceIDR) {
        // reset the sequence to start with a new IDR regardless of layout
        context->current_frame_num = context->current_frame_display = context->current_frame_encoding = 0;
    }

    uint8_t * output = context->encoded_buffer;
    VASurfaceID surface = context->src_surface[context->current_frame_encoding % SURFACE_NUM];
    int retv = upload_surface_yuv(context->va_dpy, surface, fourcc, context->config.frame_width, context->config.frame_height, y, u, v);
    CHECK_VASTATUS_RETNULL(retv,"encodeImage");

    encoding2display_order(context->current_frame_encoding, context->config.intra_period, context->config.intra_idr_period, context->config.ip_period,
                               &context->current_frame_display, &context->current_frame_type);

    if (context->current_frame_type == FRAME_IDR) 
    {
        context->numShortTerm = 0;
        context->current_frame_num = 0;
        context->current_IDR_display = context->current_frame_display;
    }

    VAStatus va_status = vaBeginPicture(context->va_dpy, context->context_id, context->src_surface[(context->current_frame_display % SURFACE_NUM)]);
    CHECK_VASTATUS_RETNULL(va_status,"vaBeginPicture");

    if (context->current_frame_type == FRAME_IDR) {
        render_sequence(context);
        render_picture(context);
        if (context->h264_packedheader) {
            render_packedsequence(context);
            render_packedpicture(context);
        }
    } else {
        render_picture(context);
    }
    render_slice(context);

    va_status = vaEndPicture(context->va_dpy, context->context_id);
    CHECK_VASTATUS_RETNULL(va_status,"vaEndPicture");

    va_status = vaSyncSurface(context->va_dpy, context->src_surface[context->current_frame_display % SURFACE_NUM]);
    CHECK_VASTATUS_RETNULL(va_status,"vaSyncSurface");

    VACodedBufferSegment *buf_list = NULL;
    unsigned int coded_size = 0;

    va_status = vaMapBuffer(context->va_dpy, context->coded_buf[context->current_frame_display % SURFACE_NUM], (void **)(&buf_list));
    CHECK_VASTATUS_RETNULL(va_status,"vaMapBuffer");
    while (buf_list != NULL) {
        memcpy(&output[coded_size], buf_list->buf, buf_list->size);
        coded_size += buf_list->size;
        buf_list = (VACodedBufferSegment *) buf_list->next;
    }
    *encodedsize = coded_size;

    vaUnmapBuffer(context->va_dpy, context->coded_buf[context->current_frame_display % SURFACE_NUM]);

    update_ReferenceFrames(context);

    context->current_frame_encoding++;
    return output;
}

#ifdef MAKE_MAIN
int main(int argc,char **argv)
{
    // We'll create a test.264 file with 1000 frames where every 100th frame is an IDR frame and all others will be P frames.
    // To define a stream that starts with an IDR and has perpetual P frames with no I or B frames:
    //   |intra_period |intra_idr_period |ip_period |frame sequence (intra_period/intra_idr_period/ip_period)
    //   |0            |ignored          |1         | IDRPPPPPPP ...     (No IDR/I any more)
    // We will then specify forceIDR=true for every 100th frame.  This in effect resets the internal frame type tracking so it starts 
    // at frame '0' again.  This will in effect force an IDR frame regardless of which ever GOP structure you are using.
    int intra_period = 60;//0;
    int intra_idr_period = 0;//-1;
    int ip_period = 1;
    VA264Context * context = (VA264Context *)createContext(640, 480, 500000, intra_period, intra_idr_period, ip_period, 30);
    if(!context) {
        fprintf(stderr, "Failed to create vaapi context\n");
        exit(1);
    }

    int idr_every = 100;
    FILE* fout = fopen("/tmp/test.264","w+");
    unsigned int encsize = 0;

    uint8_t * y = (uint8_t *)malloc(640 * 480);
    uint8_t * u = (uint8_t *)malloc(640 * 240);

    // for nv12 format, u and v are on the same plane, so we simply offset the U start position by one to obtain V
    uint8_t * v = (uint8_t*)&((uint8_t*)u)[1];

    for(int i = 0; i < 1000; i++)
    {
        // use the yuvgen_planar function to generate a new frame for each iteration
        yuvgen_planar(640, 480, y, 640, u, 640, v, 640, VA_FOURCC_NV12, 8, i, 0);
        
        // encode the image (yay!)
        bool forceIDR = !(i % idr_every);
        uint8_t * output = encodeImage(context, VA_FOURCC_NV12, y, u, v, &encsize, forceIDR);

        // output the frame number, it's frame type and encoded size
        printf("encoding frame %d %s %d\n", i, frametype_to_string(context->current_frame_type), encsize);

        if(encsize != 0 && output)
        {
            fwrite(output, encsize, 1, fout);
        }
        else
        {
            break;
        }
    }

    fclose(fout);

    release_encode(context);
    deinit_va(context);
}
#endif