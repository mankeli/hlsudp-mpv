/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>
#include <sys/stat.h>

#include <libswscale/swscale.h>

#include "config.h"
#include "misc/bstr.h"
#include "osdep/io.h"
#include "options/m_config.h"
#include "options/path.h"
#include "mpv_talloc.h"
#include "common/common.h"
#include "common/msg.h"
#include "video/out/vo.h"
#include "video/csputils.h"
#include "video/mp_image.h"
#include "video/fmt-conversion.h"
#include "video/image_writer.h"
#include "video/sws_utils.h"
#include "sub/osd.h"
#include "options/m_option.h"
#include "udpcomm_c.h"

#define IMGFMT IMGFMT_RGB24
#define FIXW 192
#define FIXH 96

struct vo_hlsudp_opts {
    int brightness;
    int delay;
}; 

#define OPT_BASE_STRUCT struct vo_hlsudp_opts

static const struct m_sub_options vo_hlsudp_conf = {
    .opts = (const struct m_option[]) {
        OPT_INT("vo-hlsudp-brightness", brightness, 1),
        OPT_INT("vo-hlsudp-delay", delay, 1),
        {0},
    },
    .size = sizeof(struct vo_hlsudp_opts),
};

struct priv {
    struct vo_hlsudp_opts *opts;

    struct mp_image *current;
    uint16_t* maptab;
    int frame;
    hlsudpcomm_t* ctx;

    int swidth, sheight;

    struct mp_sws_context *sws;

};

static int reconfig(struct vo *vo, struct mp_image_params *params)
{
    struct priv *p = vo->priv;
    //mp_image_unrefp(&p->current);

    p->swidth = FIXW;
    p->sheight = FIXH;

    mp_sws_set_from_cmdline(p->sws, vo->global);
    p->sws->src = *params;
    p->sws->dst = (struct mp_image_params) {
        .imgfmt = IMGFMT,
        .w = p->swidth,
        .h = p->sheight,
        .p_w = 1,
        .p_h = 1,
    };

    p->current = mp_image_alloc(IMGFMT, p->swidth, p->sheight);
    if (!p->current)
        return -1;

    if (mp_sws_reinit(p->sws) < 0)
        return -1;


    return 0;
}

static uint16_t mapgamma(uint8_t c, uint16_t brightness) {
    float x = c/255.f;
    float a = 0.055f;
    float lin = (x <= 0.04045 ? x * (1.0 / 12.92) : powf((x + a) * (1.0 / (1.f + a)), 2.4));
    return lin*65536.f*((float)brightness/65535.f);
}

static uint16_t* gengammatab(uint16_t br)
{
    uint16_t* maptab = (uint16_t*)malloc(256*2);

    for (int i = 0; i < 256; i++)
    {
        maptab[i] = mapgamma(i, br);
    }

    return maptab;
}

static void draw_image(struct vo *vo, mp_image_t *mpi)
{
    struct priv *p = vo->priv;

//    p->current = mpi;
    struct mp_image src = *mpi;

    struct mp_osd_res dim = osd_res_from_image_params(vo->params);
    osd_draw_on_image(vo->osd, dim, mpi->pts, OSD_DRAW_SUB_ONLY, mpi);

    mp_sws_scale(p->sws, p->current, &src);

  //  struct mp_osd_res dim = osd_res_from_image_params(vo->params);
//    osd_draw_on_image(vo->osd, dim, mpi->pts, OSD_DRAW_SUB_ONLY, p->current);

    talloc_free(mpi);
}

static void flip_page(struct vo *vo)
{
    struct priv *p = vo->priv;
    if (!p->current)
        return;

    (p->frame)++;


#if 0
    static int hf = 0;
    int pop = p->frame & 7;

    if (pop == 6)
            hlsudp_sendswap(p->ctx, hf);
    if (pop)
    {
        mp_image_unrefp(&p->current);
        return;
    }
    hf++;
#endif

    MP_VERBOSE(vo, "sending tiles for %i, swap %i\n", p->frame, p->frame-2);


    hlsudp_sendswap(p->ctx, p->frame-2);

    static const int tilesize_x = 16;
    static const int tilesize_y = 16;

    uint16_t data[tilesize_x*tilesize_y*3];

//  diod.sendswap();

    for (int xb = 0; xb < p->current->w; xb += tilesize_x)
    {
        for (int yb = 0; yb < p->current->h; yb += tilesize_y)
        {
            for (int xx = 0; xx < tilesize_x; xx++)
            {
                for (int yy = 0; yy < tilesize_x; yy++)
                {
                    int idx = (yy*tilesize_y+xx)*3;
                    int x = xb+xx;
                    int y = yb+yy;

                    data[idx+0] = p->maptab[p->current->planes[0][y*p->current->stride[0] + x*3 + 0]];
                    data[idx+1] = p->maptab[p->current->planes[0][y*p->current->stride[0] + x*3 + 1]];
                    data[idx+2] = p->maptab[p->current->planes[0][y*p->current->stride[0] + x*3 + 2]];

                }
            }

            hlsudp_sendtile(p->ctx, (uint8_t*)data, 6, p->frame, xb, yb);
//            hlsudp_sendtile(p->ctx, (uint8_t*)data, 6, hf, xb, yb);
        }
        usleep(p->opts->delay);
    }
}

static int query_format(struct vo *vo, int fmt)
{
//    return fmt == IMGFMT;

    if (sws_isSupportedInput(imgfmt2pixfmt(fmt)))
        return 1;
return 0;
}

static void uninit(struct vo *vo)
{
    struct priv *p = vo->priv;

//    mp_image_unrefp(&p->current);
    if (p->sws)
        talloc_free(p->sws);

    hlsudp_shutdown(p->ctx);
}

static int preinit(struct vo *vo)
{
    struct priv *p = vo->priv;
    p->opts = mp_get_config_group(vo, vo->global, &vo_hlsudp_conf);
    p->sws = mp_sws_alloc(vo);

    p->maptab = gengammatab(p->opts->brightness);

    p->ctx = hlsudp_open();

    return 0;
}

static int control(struct vo *vo, uint32_t request, void *data)
{
    return VO_NOTIMPL;
}

const struct vo_driver video_out_hlsudp =
{
    .description = "Send images with HLS",
    .name = "hlsudp",
    .untimed = true,
    .priv_size = sizeof(struct priv),
    .preinit = preinit,
    .query_format = query_format,
    .reconfig = reconfig,
    .control = control,
    .draw_image = draw_image,
    .flip_page = flip_page,
    .uninit = uninit,
    .global_opts = &vo_hlsudp_conf,
};
