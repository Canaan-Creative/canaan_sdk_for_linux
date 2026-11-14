// SPDX-License-Identifier: MIT
/*
 * Copyright (c) 2026, Canaan Creative Co., Ltd.
 *
 * For the licensing terms, see the LICENSE file (MIT License).
 *
 * NO WARRANTY. The Software is provided "AS IS", without warranty of any kind, express or
 * implied, including but not limited to the warranties of merchantability, fitness for a
 * particular purpose and non-infringement. In no event shall the authors or copyright
 * holders be liable for any claim, damages or other liability arising from, out of or in
 * connection with the Software or the use or other dealings in the Software.
 */

#define _DEFAULT_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include "mc_paths.h"
#undef MM_CFGWR_WP_DIR
#define MM_CFGWR_WP_DIR "wallpaper" /* Wallpaper directory, relative to the temporary test root */
#include "../ui/ui_screen.c"

int main(void)
{
    char root[] = "/tmp/ui-wallpaper-XXXXXX";
    assert(mkdtemp(root) && chdir(root) == 0);
    assert(mkdir(MM_CFGWR_WP_DIR, 0700) == 0);
    int is_gif = -1;
    assert(wallpaper_src(&is_gif) == NULL);
    static const unsigned char header[] = { 'G', 'I', 'F', '8', '9', 'a', 1, 0, 1, 0, 0x80, 0, 0, 0,
        0, 0, 255, 255, 255 };
    static const unsigned char frame[] = { 0x21, 0xf9, 4, 0, 10, 0, 0, 0, 0x2c, 0, 0, 0, 0, 1, 0, 1,
        0, 0, 2, 2, 0x44, 1, 0 };
    FILE                      *f       = fopen("wallpaper/test.GIF", "wb");
    assert(f);
    fwrite(header, sizeof header, 1, f);
    fwrite(frame, sizeof frame, 1, f);
    unsigned char second[sizeof frame];
    memcpy(second, frame, sizeof frame);
    second[20] = 0x4c;
    fwrite(second, sizeof second, 1, f);
    fputc(0x3b, f);
    fclose(f);
    const char *src = wallpaper_src(&is_gif);
    assert(src && strstr(src, "test.GIF") && is_gif == 1);
    lv_init();
    lv_display_t *display = lv_display_create(240, 240);
    assert(display);
    lv_obj_t *screen = screen_wallpaper();
    lv_obj_t *gif    = lv_obj_get_child(screen, 0);
    assert(lv_obj_check_type(gif, &lv_gif_class));
    assert(lv_gif_is_loaded(gif));
    const lv_image_dsc_t *image = lv_image_get_src(gif);
    uint32_t              first_pixel;
    memcpy(&first_pixel, image->data, sizeof first_pixel);
    lv_tick_inc(150);
    lv_timer_handler();
    uint32_t second_pixel;
    memcpy(&second_pixel, image->data, sizeof second_pixel);
    assert(first_pixel != second_pixel);
    lv_obj_delete(screen);
    assert(unlink("wallpaper/test.GIF") == 0);
    assert(wallpaper_src(&is_gif) == NULL);
    f = fopen("wallpaper/bad.gif", "wb");
    assert(f);
    fputs("not a gif", f);
    fclose(f);
    screen = screen_wallpaper();
    assert(lv_obj_check_type(lv_obj_get_child(screen, 0), &lv_image_class));
    lv_obj_delete(screen);
    assert(unlink("wallpaper/bad.gif") == 0);
    f = fopen("wallpaper/test.png", "wb");
    assert(f);
    fclose(f);
    src = wallpaper_src(&is_gif);
    assert(src && strstr(src, "test.png") && is_gif == 0);
    screen = screen_wallpaper();
    assert(lv_obj_check_type(lv_obj_get_child(screen, 0), &lv_image_class));
    lv_obj_delete(screen);
    lv_display_delete(display);
    lv_deinit();
    unlink("wallpaper/test.png");
    rmdir("wallpaper");
    assert(chdir("/") == 0);
    rmdir(root);
    puts("GIF decode, tick, cleanup and PNG selection OK");
    return 0;
}
