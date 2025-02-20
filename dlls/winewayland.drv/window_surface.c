/*
 * Wayland window surface implementation
 *
 * Copyright 2020 Alexandros Frantzis for Collabora Ltd
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <limits.h>
#include <stdlib.h>

#include "waylanddrv.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(waylanddrv);

struct wayland_window_surface
{
    struct window_surface header;
    HWND hwnd;
    struct wayland_surface *wayland_surface;
    RECT bounds;
    void *bits;
    pthread_mutex_t mutex;
    BITMAPINFO info;
};

static struct wayland_window_surface *wayland_window_surface_cast(
    struct window_surface *window_surface)
{
    return (struct wayland_window_surface *)window_surface;
}

static inline void reset_bounds(RECT *bounds)
{
    bounds->left = bounds->top = INT_MAX;
    bounds->right = bounds->bottom = INT_MIN;
}

static void buffer_release(void *data, struct wl_buffer *buffer)
{
    struct wayland_shm_buffer *shm_buffer = data;
    TRACE("shm_buffer=%p\n", shm_buffer);
    wayland_shm_buffer_destroy(shm_buffer);
}

static const struct wl_buffer_listener buffer_listener = { buffer_release };

/***********************************************************************
 *           wayland_window_surface_lock
 */
static void wayland_window_surface_lock(struct window_surface *window_surface)
{
    struct wayland_window_surface *wws = wayland_window_surface_cast(window_surface);
    pthread_mutex_lock(&wws->mutex);
}

/***********************************************************************
 *           wayland_window_surface_unlock
 */
static void wayland_window_surface_unlock(struct window_surface *window_surface)
{
    struct wayland_window_surface *wws = wayland_window_surface_cast(window_surface);
    pthread_mutex_unlock(&wws->mutex);
}

/***********************************************************************
 *           wayland_window_surface_get_bitmap_info
 */
static void *wayland_window_surface_get_bitmap_info(struct window_surface *window_surface,
                                                    BITMAPINFO *info)
{
    struct wayland_window_surface *surface = wayland_window_surface_cast(window_surface);
    /* We don't store any additional information at the end of our BITMAPINFO, so
     * just copy the structure itself. */
    memcpy(info, &surface->info, sizeof(*info));
    return surface->bits;
}

/***********************************************************************
 *           wayland_window_surface_get_bounds
 */
static RECT *wayland_window_surface_get_bounds(struct window_surface *window_surface)
{
    struct wayland_window_surface *wws = wayland_window_surface_cast(window_surface);
    return &wws->bounds;
}

/***********************************************************************
 *           wayland_window_surface_set_region
 */
static void wayland_window_surface_set_region(struct window_surface *window_surface,
                                              HRGN region)
{
    /* TODO */
}

/***********************************************************************
 *           wayland_window_surface_flush
 */
static void wayland_window_surface_flush(struct window_surface *window_surface)
{
    struct wayland_window_surface *wws = wayland_window_surface_cast(window_surface);
    struct wayland_shm_buffer *shm_buffer;
    BOOL flushed = FALSE;

    wayland_window_surface_lock(window_surface);

    if (IsRectEmpty(&wws->bounds)) goto done;

    if (!wws->wayland_surface)
    {
        ERR("missing wayland surface, returning\n");
        goto done;
    }

    TRACE("surface=%p hwnd=%p surface_rect=%s bounds=%s\n", wws, wws->hwnd,
          wine_dbgstr_rect(&wws->header.rect), wine_dbgstr_rect(&wws->bounds));

    shm_buffer = wayland_shm_buffer_create(wws->info.bmiHeader.biWidth,
                                           abs(wws->info.bmiHeader.biHeight),
                                           WL_SHM_FORMAT_XRGB8888);
    if (!shm_buffer)
    {
        ERR("failed to create Wayland SHM buffer, returning\n");
        goto done;
    }

    wl_buffer_add_listener(shm_buffer->wl_buffer, &buffer_listener, shm_buffer);

    memcpy(shm_buffer->map_data, wws->bits, shm_buffer->map_size);

    pthread_mutex_lock(&wws->wayland_surface->mutex);
    if (wws->wayland_surface->current_serial)
    {
        wayland_surface_attach_shm(wws->wayland_surface, shm_buffer);
        wl_surface_commit(wws->wayland_surface->wl_surface);
        flushed = TRUE;
    }
    else
    {
        TRACE("Wayland surface not configured yet, not flushing\n");
        wayland_shm_buffer_destroy(shm_buffer);
    }
    pthread_mutex_unlock(&wws->wayland_surface->mutex);
    wl_display_flush(process_wayland.wl_display);

done:
    if (flushed) reset_bounds(&wws->bounds);
    wayland_window_surface_unlock(window_surface);
}

/***********************************************************************
 *           wayland_window_surface_destroy
 */
static void wayland_window_surface_destroy(struct window_surface *window_surface)
{
    struct wayland_window_surface *wws = wayland_window_surface_cast(window_surface);

    TRACE("surface=%p\n", wws);

    pthread_mutex_destroy(&wws->mutex);
    free(wws->bits);
    free(wws);
}

static const struct window_surface_funcs wayland_window_surface_funcs =
{
    wayland_window_surface_lock,
    wayland_window_surface_unlock,
    wayland_window_surface_get_bitmap_info,
    wayland_window_surface_get_bounds,
    wayland_window_surface_set_region,
    wayland_window_surface_flush,
    wayland_window_surface_destroy
};

/***********************************************************************
 *           wayland_window_surface_create
 */
struct window_surface *wayland_window_surface_create(HWND hwnd, const RECT *rect)
{
    struct wayland_window_surface *wws;
    int width = rect->right - rect->left;
    int height = rect->bottom - rect->top;
    pthread_mutexattr_t mutexattr;

    TRACE("hwnd %p rect %s\n", hwnd, wine_dbgstr_rect(rect));

    wws = calloc(1, sizeof(*wws));
    if (!wws) return NULL;
    wws->info.bmiHeader.biSize = sizeof(wws->info.bmiHeader);
    wws->info.bmiHeader.biClrUsed = 0;
    wws->info.bmiHeader.biBitCount = 32;
    wws->info.bmiHeader.biCompression = BI_RGB;
    wws->info.bmiHeader.biWidth = width;
    wws->info.bmiHeader.biHeight = -height; /* top-down */
    wws->info.bmiHeader.biPlanes = 1;
    wws->info.bmiHeader.biSizeImage = width * height * 4;

    pthread_mutexattr_init(&mutexattr);
    pthread_mutexattr_settype(&mutexattr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&wws->mutex, &mutexattr);
    pthread_mutexattr_destroy(&mutexattr);

    wws->header.funcs = &wayland_window_surface_funcs;
    wws->header.rect = *rect;
    wws->header.ref = 1;
    wws->hwnd = hwnd;
    reset_bounds(&wws->bounds);

    if (!(wws->bits = malloc(wws->info.bmiHeader.biSizeImage)))
        goto failed;

    TRACE("created %p hwnd %p %s bits [%p,%p)\n", wws, hwnd, wine_dbgstr_rect(rect),
          wws->bits, (char *)wws->bits + wws->info.bmiHeader.biSizeImage);

    return &wws->header;

failed:
    wayland_window_surface_destroy(&wws->header);
    return NULL;
}

/***********************************************************************
 *           wayland_window_surface_update_wayland_surface
 */
void wayland_window_surface_update_wayland_surface(struct window_surface *window_surface,
                                                   struct wayland_surface *wayland_surface)
{
    struct wayland_window_surface *wws = wayland_window_surface_cast(window_surface);

    wayland_window_surface_lock(window_surface);

    TRACE("surface=%p hwnd=%p wayland_surface=%p\n", wws, wws->hwnd, wayland_surface);

    wws->wayland_surface = wayland_surface;

    wayland_window_surface_unlock(window_surface);
}
