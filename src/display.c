/* SPDX-License-Identifier: MIT */

#include "display.h"
#include "adt.h"
#include "assert.h"
#include "dcp.h"
#include "dcp_iboot.h"
#include "fb.h"
#include "string.h"
#include "utils.h"
#include "xnuboot.h"

#define DISPLAY_STATUS_DELAY   100
#define DISPLAY_STATUS_RETRIES 20

#define COMPARE(a, b)                                                                              \
    if ((a) > (b)) {                                                                               \
        *best = modes[i];                                                                          \
        continue;                                                                                  \
    } else if ((a) < (b)) {                                                                        \
        continue;                                                                                  \
    }

dcp_dev_t *dcp;
dcp_iboot_if_t *iboot;
u64 fb_dva;

#define abs(x) ((x) >= 0 ? (x) : -(x))

static void display_choose_timing_mode(dcp_timing_mode_t *modes, int cnt, dcp_timing_mode_t *best,
                                       dcp_timing_mode_t *want)
{
    *best = modes[0];

    for (int i = 1; i < cnt; i++) {
        COMPARE(modes[i].valid, best->valid);
        if (want && want->valid) {
            COMPARE(modes[i].width == want->width && modes[i].height == want->height,
                    best->width == want->width && best->height == want->height);
            COMPARE(-abs((long)modes[i].fps - (long)want->fps),
                    -abs((long)best->fps - (long)want->fps));
        }
        COMPARE(modes[i].width <= 1920, best->width <= 1920);
        COMPARE(modes[i].height <= 1200, best->height <= 1200);
        COMPARE(modes[i].fps <= 60 << 16, best->fps <= 60 << 16);
        COMPARE(modes[i].width, best->width);
        COMPARE(modes[i].height, best->height);
        COMPARE(modes[i].fps, best->fps);
    }

    printf("display: timing mode: valid=%d %dx%d %d.%02d Hz\n", best->valid, best->width,
           best->height, best->fps >> 16, ((best->fps & 0xffff) * 100 + 0x7fff) >> 16);
}

static void display_choose_color_mode(dcp_color_mode_t *modes, int cnt, dcp_color_mode_t *best)
{
    *best = modes[0];

    for (int i = 1; i < cnt; i++) {
        COMPARE(modes[i].valid, best->valid);
        COMPARE(modes[i].bpp <= 32, best->bpp <= 32);
        COMPARE(modes[i].bpp, best->bpp);
        COMPARE(-modes[i].colorimetry, -best->colorimetry);
        COMPARE(-modes[i].encoding, -best->encoding);
        COMPARE(-modes[i].eotf, -best->eotf);
    }

    printf("display: color mode: valid=%d colorimetry=%d eotf=%d encoding=%d bpp=%d\n", best->valid,
           best->colorimetry, best->eotf, best->encoding, best->bpp);
}

static int display_map_fb(uintptr_t iova, void *paddr, size_t size)
{
    int ret = dart_map(dcp->dart_disp, iova, paddr, size);
    if (ret < 0) {
        printf("display: failed to map fb to dart-disp0\n");
        return -1;
    }

    ret = dart_map(dcp->dart_dcp, iova, paddr, size);
    if (ret < 0) {
        printf("display: failed to map fb to dart-dcp\n");
        dart_unmap(dcp->dart_disp, iova, size);
        return -1;
    }

    return 0;
}

static uintptr_t display_map_vram(void)
{
    int ret = 0;
    u64 paddr, size;
    int adt_path[4];
    int node = adt_path_offset_trace(adt, "/vram", adt_path);

    if (node < 0) {
        printf("display: '/vram' not found\n");
        return 0;
    }

    int pp = 0;
    while (adt_path[pp])
        pp++;
    adt_path[pp + 1] = 0;

    ret = adt_get_reg(adt, adt_path, "reg", 0, &paddr, &size);
    if (ret < 0) {
        printf("display: failed to read /vram/reg\n");
        return 0;
    }

    if (paddr != cur_boot_args.video.base) {
        printf("display: vram does not match boot_args.video.base\n");
        return 0;
    }

    s64 iova_disp0 = 0;
    s64 iova_dcp = 0;

    iova_dcp = dart_find_iova(dcp->dart_dcp, iova_dcp, size);
    if (iova_dcp < 0) {
        printf("display: failed to find IOVA for fb of %06zx bytes (dcp)\n", size);
        return 0;
    }

    // try to map the fb to the same IOVA on disp0
    iova_disp0 = dart_find_iova(dcp->dart_dcp, iova_dcp, size);
    if (iova_disp0 < 0) {
        printf("display: failed to find IOVA for fb of %06zx bytes (disp0)\n", size);
        return 0;
    }

    // assume this results in the same IOVA, not sure if this is required but matches what iboot
    // does on other models.
    if (iova_disp0 != iova_dcp) {
        printf("display: IOVA mismatch for fb between dcp (%08lx) and disp0 (%08lx)\n",
               (u64)iova_dcp, (u64)iova_disp0);
        return 0;
    }

    uintptr_t iova = iova_dcp;
    ret = display_map_fb(iova, (void *)paddr, size);
    if (ret < 0)
        return 0;

    return iova;
}

static int display_start_dcp(void)
{
    if (iboot)
        return 0;

    dcp = dcp_init("/arm-io/dcp", "/arm-io/dart-dcp", "/arm-io/dart-disp0");
    if (!dcp) {
        printf("display: failed to initialize DCP\n");
        return -1;
    }

    // Find the framebuffer DVA
    fb_dva = dart_search(dcp->dart_disp, (void *)cur_boot_args.video.base);
    // framebuffer is not mapped on the M1 Ultra Mac Studio
    if (!fb_dva)
        fb_dva = display_map_vram();
    if (!fb_dva) {
        printf("display: failed to find display DVA\n");
        dcp_shutdown(dcp);
        return -1;
    }

    iboot = dcp_ib_init(dcp);
    if (!iboot) {
        printf("display: failed to initialize DCP iBoot interface\n");
        dcp_shutdown(dcp);
        return -1;
    }

    return 0;
}

int display_parse_mode(const char *config, dcp_timing_mode_t *mode)
{
    memset(mode, 0, sizeof(*mode));

    if (!config || !strcmp(config, "auto"))
        return 0;

    const char *s_w = config;
    const char *s_h = strchr(config, 'x');
    const char *s_fps = strchr(config, '@');

    if (s_w && s_h) {
        mode->width = atol(s_w);
        mode->height = atol(s_h + 1);
        mode->valid = mode->width && mode->height;
    }

    if (s_fps) {
        mode->fps = atol(s_fps + 1) << 16;

        const char *s_fps_frac = strchr(s_fps + 1, '.');
        if (s_fps_frac) {
            // Assumes two decimals...
            mode->fps += (atol(s_fps_frac + 1) << 16) / 100;
        }
    }

    printf("display: want mode: valid=%d %dx%d %d.%02d Hz\n", mode->valid, mode->width,
           mode->height, mode->fps >> 16, ((mode->fps & 0xffff) * 100 + 0x7fff) >> 16);

    return mode->valid;
}

int display_configure(const char *config)
{
    dcp_timing_mode_t want;

    display_parse_mode(config, &want);

    int ret = display_start_dcp();
    if (ret < 0)
        return ret;

    // Power on
    if ((ret = dcp_ib_set_power(iboot, true)) < 0) {
        printf("display: failed to set power\n");
        return ret;
    }

    // Detect if display is connected
    int timing_cnt, color_cnt;
    int hpd = 0, retries = 0;

    /* After boot DCP does not immediately report a connected display. Retry getting display
     * information for 2 seconds.
     */
    while (retries++ < DISPLAY_STATUS_RETRIES) {
        hpd = dcp_ib_get_hpd(iboot, &timing_cnt, &color_cnt);
        if (hpd < 0)
            ret = hpd;
        else if (hpd && timing_cnt && color_cnt)
            break;
        if (retries < DISPLAY_STATUS_RETRIES)
            mdelay(DISPLAY_STATUS_DELAY);
    }
    printf("display: waited %d ms for display status\n", (retries - 1) * DISPLAY_STATUS_DELAY);
    if (ret < 0) {
        printf("display: failed to get display status\n");
        return 0;
    }

    printf("display: connected:%d timing_cnt:%d color_cnt:%d\n", hpd, timing_cnt, color_cnt);

    if (!hpd || !timing_cnt || !color_cnt)
        return 0;

    // Find best modes
    dcp_timing_mode_t *tmodes, tbest;
    if ((ret = dcp_ib_get_timing_modes(iboot, &tmodes)) < 0) {
        printf("display: failed to get timing modes\n");
        return -1;
    }
    assert(ret == timing_cnt);
    display_choose_timing_mode(tmodes, timing_cnt, &tbest, &want);

    dcp_color_mode_t *cmodes, cbest;
    if ((ret = dcp_ib_get_color_modes(iboot, &cmodes)) < 0) {
        printf("display: failed to get color modes\n");
        return -1;
    }
    assert(ret == color_cnt);
    display_choose_color_mode(cmodes, color_cnt, &cbest);

    // Set mode
    if ((ret = dcp_ib_set_mode(iboot, &tbest, &cbest)) < 0) {
        printf("display: failed to set mode\n");
        return -1;
    }

    // Swap!
    int swap_id = ret = dcp_ib_swap_begin(iboot);
    if (swap_id < 0) {
        printf("display: failed to start swap\n");
        return -1;
    }

    dcp_layer_t layer = {
        .planes = {{
            .addr = fb_dva,
            .stride = tbest.width * 4,
            .addr_format = ADDR_PLANAR,
        }},
        .plane_cnt = 1,
        .width = tbest.width,
        .height = tbest.height,
        .surface_fmt = FMT_w30r,
        .colorspace = 2,
        .eotf = EOTF_GAMMA_SDR,
        .transform = XFRM_NONE,
    };

    dcp_rect_t rect = {tbest.width, tbest.height, 0, 0};

    if ((ret = dcp_ib_swap_set_layer(iboot, 0, &layer, &rect, &rect)) < 0) {
        printf("display: failed to set layer\n");
        return -1;
    }

    if ((ret = dcp_ib_swap_end(iboot)) < 0) {
        printf("display: failed to complete swap\n");
        return -1;
    }

    printf("display: swapped! (swap_id=%d)\n", swap_id);

    if (cur_boot_args.video.stride != layer.planes[0].stride ||
        cur_boot_args.video.width != layer.width || cur_boot_args.video.height != layer.height ||
        cur_boot_args.video.depth != 30) {
        cur_boot_args.video.stride = layer.planes[0].stride;
        cur_boot_args.video.width = layer.width;
        cur_boot_args.video.height = layer.height;
        cur_boot_args.video.depth = 30;
        fb_reinit();
    }

    /* Update for python / subsequent stages */
    memcpy((void *)boot_args_addr, &cur_boot_args, sizeof(cur_boot_args));

    return 1;
}

int display_init(void)
{
    if (cur_boot_args.video.width == 640 && cur_boot_args.video.height == 1136) {
        printf("display: Dummy framebuffer found, initializing display\n");

        return display_configure(NULL);
    } else {
        printf("display: Display is already initialized (%ldx%ld)\n", cur_boot_args.video.width,
               cur_boot_args.video.height);
        return 0;
    }
}

void display_shutdown(void)
{
    if (iboot) {
        dcp_ib_shutdown(iboot);
        dcp_shutdown(dcp);
    }
}
