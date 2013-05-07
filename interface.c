/*
 * Copyright (C) 2012 Mark Hills <mark@xwax.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License version 2 for more details.
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; if not, write to the Free
 * Software Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301, USA.
 *
 */

#include <assert.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <SDL.h>
#include <SDL_ttf.h>

#include "interface.h"
#include "layout.h"
#include "player.h"
#include "rig.h"
#include "status.h"
#include "timecoder.h"
#include "xwax.h"

/* Screen refresh time in milliseconds */

#define REFRESH 10

/* Font definitions */

#define FONT "DejaVuSans.ttf"
#define FONT_SIZE 10
#define FONT_SPACE 15

#define EM_FONT "DejaVuSans-Oblique.ttf"

#define BIG_FONT "DejaVuSans-Bold.ttf"
#define BIG_FONT_SIZE 14
#define BIG_FONT_SPACE 19

#define CLOCK_FONT FONT
#define CLOCK_FONT_SIZE 32

#define DECI_FONT FONT
#define DECI_FONT_SIZE 20

#define DETAIL_FONT "DejaVuSansMono.ttf"
#define DETAIL_FONT_SIZE 9
#define DETAIL_FONT_SPACE 12

/* Screen size (pixels) */

#define DEFAULT_WIDTH 960
#define DEFAULT_HEIGHT 720

/* Relationship between pixels and screen units */

#define ZOOM 1.0

/* Dimensions in our own screen units */

#define BORDER 1
#define SPACER 8
#define HALF_SPACER 4

#define CURSOR_WIDTH 4

#define PLAYER_HEIGHT 213
#define OVERVIEW_HEIGHT 16

#define LIBRARY_MIN_WIDTH 64
#define LIBRARY_MIN_HEIGHT 64

#define DEFAULT_METER_SCALE 8

#define MAX_METER_SCALE 11

#define SEARCH_HEIGHT (FONT_SPACE)
#define STATUS_HEIGHT (DETAIL_FONT_SPACE)

#define BPM_WIDTH 32
#define SORT_WIDTH 21
#define RESULTS_ARTIST_WIDTH 200

#define TOKEN_SPACE 2

#define CLOCKS_WIDTH 160

#define SPINNER_SIZE (CLOCK_FONT_SIZE * 2 - 6)
#define SCOPE_SIZE (CLOCK_FONT_SIZE * 2 - 6)

#define SCROLLBAR_SIZE 10

#define METER_WARNING_TIME 20 /* time in seconds for "red waveform" warning */

/* Function key (F1-F12) definitions */

#define FUNC_LOAD 0
#define FUNC_RECUE 1
#define FUNC_TIMECODE 2
#define FUNC_TEMPO 3

/* Types of SDL_USEREVENT */

#define EVENT_TICKER 0
#define EVENT_QUIT 1
#define EVENT_STATUS 2

/* Macro functions */

#define MIN(x,y) ((x)<(y)?(x):(y))
#define SQ(x) ((x)*(x))

#define LOCK(sf) if (SDL_MUSTLOCK(sf)) SDL_LockSurface(sf)
#define UNLOCK(sf) if (SDL_MUSTLOCK(sf)) SDL_UnlockSurface(sf)
#define UPDATE(sf, rect) SDL_UpdateRect(sf, (rect)->x, (rect)->y, \
                                        (rect)->w, (rect)->h)

/* List of directories to use as search path for fonts. */

static const char *font_dirs[] = {
    "/usr/X11R6/lib/X11/fonts/TTF",
    "/usr/share/fonts/truetype/ttf-dejavu/",
    "/usr/share/fonts/ttf-dejavu",
    "/usr/share/fonts/dejavu",
    "/usr/share/fonts/TTF",
    NULL
};

static TTF_Font *clock_font, *deci_font, *detail_font,
    *font, *em_font, *big_font;

static SDL_Color background_col = {0, 0, 0, 255},
    text_col = {224, 224, 224, 255},
    warn_col = {192, 64, 0, 255},
    elapsed_col = {0, 32, 255, 255},
    detail_col = {128, 128, 128, 255},
    needle_col = {255, 255, 255, 255};

static unsigned short *spinner_angle, spinner_size;

static int width = DEFAULT_WIDTH, height = DEFAULT_HEIGHT,
    meter_scale = DEFAULT_METER_SCALE;
static pthread_t ph;
static bool vsplit = false;

/*
 * Set beat
 */

static void set_beat_marker(struct player *pl, size_t n)
{
    static long marker[] = { -1, -1, -1, -1 };

    if (!pl || !pl->track || n < 0 || n >= ndeck || pl->position < 0)
        return;

    struct track *tr = pl->track;
    unsigned long pos = player_get_elapsed(pl) * tr->rate;

    if (marker[n] < 0) {
        marker[n] = pos;
        return;
    }
    if (pos == marker[n])
        return;

    unsigned long int dt = pos > marker[n] ? pos - marker[n] : marker[n] - pos;
    tr->beat_interval = dt;
    tr->beat_offset = pos % dt;

    marker[n] = -1;

    fprintf(stderr, "deck %d: interval = %ld, offset = %ld, bpm = %.5f\n",
            (int) n, dt, tr->beat_offset, (double) tr->rate * 60 / dt);
}

/*
 * Scale a dimension according to the current zoom level
 *
 * FIXME: This function is used where a rendering does not
 * acknowledge the scale given in the local rectangle.
 * These cases should probably be removed.
 */

static int zoom(int d)
{
    return d * ZOOM;
}

/*
 * Calculate a lookup which maps a position on screen to an angle,
 * relative to the centre of the spinner
 */

static void calculate_angle_lut(unsigned short *lut, int size)
{
    int r, c, nr, nc;
    float theta, rat;

    for (r = 0; r < size; r++) {
        nr = r - size / 2;

        for (c = 0; c < size; c++) {
            nc = c - size / 2;

            if (nr == 0)
                theta = M_PI_2;

            else if (nc == 0) {
                theta = 0;

                if (nr < 0)
                    theta = M_PI;

            } else {
                rat = (float)(nc) / -nr;
                theta = atanf(rat);

                if (rat < 0)
                    theta += M_PI;
            }

            if (nc <= 0)
                theta += M_PI;

            /* The angles stored in the lookup table range from 0 to
             * 1023 (where 1024 is 360 degrees) */

            lut[r * size + c]
                = ((int)(theta * 1024 / (M_PI * 2)) + 1024) % 1024;
        }
    }
}

static int init_spinner(int size)
{
    spinner_angle = malloc(size * size * (sizeof *spinner_angle));
    if (spinner_angle == NULL) {
        perror("malloc");
        return -1;
    }

    calculate_angle_lut(spinner_angle, size);
    spinner_size = size;
    return 0;
}

static void clear_spinner(void)
{
    free(spinner_angle);
}

/*
 * Open a font, given the leafname
 *
 * This scans the available font directories for the file, to account
 * for different software distributions.
 *
 * As this is an SDL (it is not an X11 app) we prefer to avoid the use
 * of fontconfig to select fonts.
 */

static TTF_Font* open_font(const char *name, int size) {
    int r, pt;
    char buf[256];
    const char **dir;
    struct stat st;
    TTF_Font *font;

    pt = zoom(size);

    dir = &font_dirs[0];

    while (*dir) {

        sprintf(buf, "%s/%s", *dir, name);

        r = stat(buf, &st);

        if (r != -1) { /* something exists at this path */
            fprintf(stderr, "Loading font '%s', %dpt...\n", buf, pt);

            font = TTF_OpenFont(buf, pt);
            if (!font)
                fprintf(stderr, "Font error: %s\n", TTF_GetError());
            return font; /* or NULL */
        }

        if (errno != ENOENT) {
            perror("stat");
            return NULL;
        }

        dir++;
        continue;
    }

    fprintf(stderr, "Font '%s' cannot be found in", name);

    dir = &font_dirs[0];
    while (*dir) {
        fputc(' ', stderr);
        fputs(*dir, stderr);
        dir++;
    }
    fputc('.', stderr);
    fputc('\n', stderr);

    return NULL;
}

/*
 * Load all fonts
 */

static int load_fonts(void)
{
    clock_font = open_font(CLOCK_FONT, CLOCK_FONT_SIZE);
    if (!clock_font)
        return -1;

    deci_font = open_font(DECI_FONT, DECI_FONT_SIZE);
    if (!deci_font)
        return -1;

    font = open_font(FONT, FONT_SIZE);
    if (!font)
        return -1;

    em_font = open_font(EM_FONT, FONT_SIZE);
    if (!em_font)
        return -1;

    big_font = open_font(BIG_FONT, BIG_FONT_SIZE);
    if (!big_font)
        return -1;

    detail_font = open_font(DETAIL_FONT, DETAIL_FONT_SIZE);
    if (!detail_font)
        return -1;

    return 0;
}

/*
 * Free resources associated with fonts
 */

static void clear_fonts(void)
{
    TTF_CloseFont(clock_font);
    TTF_CloseFont(deci_font);
    TTF_CloseFont(font);
    TTF_CloseFont(em_font);
    TTF_CloseFont(big_font);
    TTF_CloseFont(detail_font);
}

static Uint32 palette(SDL_Surface *sf, SDL_Color *col)
{
    return SDL_MapRGB(sf->format, col->r, col->g, col->b);
}

/*
 * Draw text at the given coordinates
 *
 * Return: width of text drawn
 */

static int draw_text(SDL_Surface *sf, const struct rect *rect,
                     const char *buf, TTF_Font *font,
                     SDL_Color fg, SDL_Color bg)
{
    SDL_Surface *rendered;
    SDL_Rect dst, src, fill;

    if (buf == NULL) {
        src.w = 0;
        src.h = 0;

    } else if (buf[0] == '\0') { /* SDL_ttf fails for empty string */
        src.w = 0;
        src.h = 0;

    } else {
        rendered = TTF_RenderText_Shaded(font, buf, fg, bg);

        src.x = 0;
        src.y = 0;
        src.w = MIN(rect->w, rendered->w);
        src.h = MIN(rect->h, rendered->h);

        dst.x = rect->x;
        dst.y = rect->y;

        SDL_BlitSurface(rendered, &src, sf, &dst);
        SDL_FreeSurface(rendered);
    }

    /* Complete the remaining space with a blank rectangle */

    if (src.w < rect->w) {
        fill.x = rect->x + src.w;
        fill.y = rect->y;
        fill.w = rect->w - src.w;
        fill.h = rect->h;
        SDL_FillRect(sf, &fill, palette(sf, &bg));
    }

    if (src.h < rect->h) {
        fill.x = rect->x;
        fill.y = rect->y + src.h;
        fill.w = src.w; /* the x-fill rectangle does the corner */
        fill.h = rect->h - src.h;
        SDL_FillRect(sf, &fill, palette(sf, &bg));
    }

    return src.w;
}

/*
 * Dim a colour for display
 */

static SDL_Color dim(const SDL_Color x, int n)
{
    SDL_Color c;

    c.r = x.r >> n;
    c.g = x.g >> n;
    c.b = x.b >> n;

    return c;
}

/*
 * Get a colour from RGB values
 */

static SDL_Color rgb(double r, double g, double b)
{
    SDL_Color c;

    c.r = r * 255;
    c.g = g * 255;
    c.b = b * 255;

    return c;
}

/*
 * Get a colour from HSV values
 *
 * Pre: h is in degrees, in the range 0.0 to 360.0
 */

static SDL_Color hsv(double h, double s, double v)
{
    int i;
    double f, p, q, t;

    if (s == 0.0)
        return rgb(v, v, v);

    h /= 60;
    i = floor(h);
    f = h - i;
    p = v * (1 - s);
    q = v * (1 - s * f);
    t = v * (1 - s * (1 - f));

    switch (i) {
    case 0:
        return rgb(v, t, p);
    case 1:
        return rgb(q, v, p);
    case 2:
        return rgb(p, v, t);
    case 3:
        return rgb(p, q, v);
    case 4:
        return rgb(t, p, v);
    case 5:
    case 6:
        return rgb(v, p, q);
    default:
        abort();
    }
}

/*
 * Draw the high-level overview meter which shows the whole length
 * of the track
 */

static void draw_overview(SDL_Surface *surface, const struct rect *rect,
                          struct track *tr, int position)
{
    int x, y, w, h, r, c, sp, fade, bytes_per_pixel, pitch, height,
        current_position;
    Uint8 *pixels, *p;
    SDL_Color col;

    x = rect->x;
    y = rect->y;
    w = rect->w;
    h = rect->h;

    pixels = surface->pixels;
    bytes_per_pixel = surface->format->BytesPerPixel;
    pitch = surface->pitch;

    if (tr->length)
        current_position = (long long)position * w / tr->length;
    else
        current_position = 0;

    for (c = 0; c < w; c++) {

        /* Collect the correct meter value for this column */

        sp = (long long)tr->length * c / w;

        if (sp < tr->length) /* account for rounding */
            height = track_get_overview(tr, sp) * h / 256;
        else
            height = 0;

        /* Choose a base colour to display in */

        if (!tr->length) {
            col = background_col;
            fade = 0;
        } else if (c == current_position || c == w / 2) {
            col = needle_col;
            fade = 1;
        } else {
            double f = 0;
            if (position > 0 && position < tr->length)
                f = 360.0 * position / tr->length;

            col = hsv(f, 1, 0.8);
            fade = 3;
        }

        if (track_is_importing(tr))
            col = dim(col, 1);

        if (c < current_position)
            fade = 1;

        /* Store a pointer to this column of the framebuffer */

        p = pixels + y * pitch + (x + c) * bytes_per_pixel;

        r = h;
        while (r > (height + h) / 2) {
            p[0] = col.b;
            p[1] = col.g;
            p[2] = col.r;
            p += pitch;
            r--;
        }
        while (r > (h - height) / 2) {
            p[0] = col.b >> fade;
            p[1] = col.g >> fade;
            p[2] = col.r >> fade;
            p += pitch;
            r--;
        }
        while (r) {
            p[0] = col.b;
            p[1] = col.g;
            p[2] = col.r;
            p += pitch;
            r--;
        }
    }
}

/*
 * Draw the close-up meter, which can be zoomed to a level set by
 * 'scale'
 */

static void draw_closeup(SDL_Surface *surface, const struct rect *rect,
                         struct track *tr, int position, int scale, bool rev)
{
    int x, y, w, h, c;
    size_t bytes_per_pixel, pitch;
    Uint8 *pixels;

    x = rect->x;
    y = rect->y;
    w = rect->w;
    h = rect->h;

    pixels = surface->pixels;
    bytes_per_pixel = surface->format->BytesPerPixel;
    pitch = surface->pitch;

    /* Draw in columns. This may seem like a performance hit,
     * but oprofile shows it makes no difference */

    for (c = 0; c < w; c++) {
        int r, sp, height, fade;
        Uint8 *p;
        SDL_Color col;

        /* Work out the meter height in pixels for this column */

        sp = position - (position % (1 << scale))
            + ((c - w / 2) << scale);

        if (sp < tr->length && sp > 0)
            height = track_get_ppm(tr, sp) * h / 256;
        else
            height = 0;

        /* Select the appropriate colour */

        col = elapsed_col;
        fade = 3;


        if (c == w / 2) {
            col = needle_col;
            fade = 1;
        } else if (tr->length && sp > 0 && sp < tr->length) {

            /* Beat grid */
            if (tr->beat_interval &&
                fmod(sp - tr->beat_offset, tr->beat_interval) < (1 << scale)) {
                col = needle_col;
                fade = 2;

            /* Waveform hue depends on position */
            } else {
                col = hsv(360.0 * sp / tr->length, 1.0, 0.8);
                fade = 3;
            }

        } else {
            col = hsv(180.0, 1, 0.8);
            col = dim(col, 3);
            fade = 0;
        }

        /* Get a pointer to the top of the column, and increment
         * it for each row */

        p = pixels + y * pitch + (x + c) * bytes_per_pixel;

        r = h;
        while (r > (rev ? h - height : height)) {
            p[0] = col.b >> (fade * !rev);
            p[1] = col.g >> (fade * !rev);
            p[2] = col.r >> (fade * !rev);
            p += pitch;
            r--;
        }
        while (r) {
            p[0] = col.b >> (fade * rev);
            p[1] = col.g >> (fade * rev);
            p[2] = col.r >> (fade * rev);
            p += pitch;
            r--;
        }
    }
}

/*
 * Draw the audio meters for a deck
 */

static void draw_meters(SDL_Surface *surface, const struct rect *rect,
                        struct track *tr, int position, int scale)
{
    int h = rect->h;
    struct rect overview, closeup_upper, closeup_bottom;

    split(*rect, from_top(2 * h / 5, 0), &closeup_upper, &overview);
    split(overview, from_top(h / 5, 0), &overview, &closeup_bottom);

    draw_closeup(surface, &closeup_upper, tr, position, scale, false);
    draw_overview(surface, &overview, tr, position);
    draw_closeup(surface, &closeup_bottom, tr, position, scale, true);
}

/*
 * Draw a single deck
 */

static void draw_deck(SDL_Surface *surface, const struct rect *rect,
                      struct deck *deck, int meter_scale)
{
    int position;
    struct player *pl;
    struct track *t;

    pl = &deck->player;
    t = pl->track;

    position = player_get_elapsed(pl) * t->rate;

    draw_meters(surface, rect, t, position, meter_scale);
}

/*
 * Draw all the decks in the system
 */

static void draw_decks(SDL_Surface *surface, const struct rect *rect,
                       struct deck deck[], size_t ndecks, int meter_scale)
{
    int d;
    struct rect left, right;

    right = *rect;

    for (d = 0; d < ndecks; d++) {
        if (vsplit)
            split(right, rows(d, ndecks, BORDER), &left, &right);
        else
            split(right, columns(d, ndecks, BORDER), &left, &right);

        draw_deck(surface, &left, &deck[d], meter_scale);
    }
}

/*
 * Draw the status bar
 */

static void draw_status(SDL_Surface *sf, const struct rect *rect)
{
    if (status_level() >= STATUS_ERROR)
        draw_text(sf, rect, status(), detail_font, text_col, dim(warn_col, 2));
    else
        draw_text(sf, rect, status(), detail_font, detail_col, background_col);
}

/*
 * Handle a single key event
 */

static void handle_key(SDLKey key, SDLMod mod)
{
    if ((key == SDLK_EQUALS) || (key == SDLK_PLUS)) {
        meter_scale--;

        if (meter_scale < 0)
            meter_scale = 0;

        fprintf(stderr, "Meter scale decreased to %d\n", meter_scale);

    } else if (key == SDLK_MINUS) {
        meter_scale++;

        if (meter_scale > MAX_METER_SCALE)
            meter_scale = MAX_METER_SCALE;

        fprintf(stderr, "Meter scale increased to %d\n", meter_scale);

    } else if (key >= SDLK_F1 && key <= SDLK_F12) {
        size_t d;

        /* Handle the function key press in groups of four --
         * F1-F4 (deck 0), F5-F8 (deck 1) etc. */

        d = (key - SDLK_F1) / 4;

        if (d < ndeck) {
            int func;
            struct deck *de;
            struct player *pl;
            struct timecoder *tc;

            func = (key - SDLK_F1) % 4;

            de = &deck[d];
            pl = &de->player;
            tc = &de->timecoder;

            if (mod & KMOD_SHIFT) {
                if (func < ndeck)
                    deck_clone(de, &deck[func]);

            } else switch(func) {
            case FUNC_RECUE:
                deck_recue(de);
                break;

            case FUNC_TIMECODE:
                if (mod & KMOD_CTRL) {
                    timecoder_cycle_definition(tc);
                } else {
                    (void)player_toggle_timecode_control(pl);
                }
                break;

            case FUNC_TEMPO:
                set_beat_marker(pl, d);
                break;
            }
        }
    }
}

/*
 * Action on size change event on the main window
 */

static SDL_Surface* set_size(int w, int h, struct rect *r)
{
    SDL_Surface *surface;

    surface = SDL_SetVideoMode(w, h, 32, SDL_RESIZABLE);
    if (surface == NULL) {
        fprintf(stderr, "%s\n", SDL_GetError());
        return NULL;
    }

    *r = shrink(rect(0, 0, w, h, ZOOM), BORDER);

    fprintf(stderr, "New interface size is %dx%d.\n", w, h);

    return surface;
}

/*
 * Timer which posts a screen redraw event
 */

static Uint32 ticker(Uint32 interval, void *p)
{
    SDL_Event event;

    if (!SDL_PeepEvents(&event, 1, SDL_PEEKEVENT, SDL_EVENTMASK(SDL_USEREVENT)))
    {
        event.type = SDL_USEREVENT;
        event.user.code = EVENT_TICKER;
        SDL_PushEvent(&event);
    }

    return interval;
}

/*
 * Callback to tell the interface that status has changed
 */

static void status_change(void)
{
    SDL_Event e;

    e.type = SDL_USEREVENT;
    e.user.code = EVENT_STATUS;
    SDL_PushEvent(&e);
}

/*
 * The SDL interface thread
 */

static int interface_main(void)
{
    bool decks_update, status_update;

    SDL_Event event;
    SDL_TimerID timer;
    SDL_Surface *surface;

    struct rect rworkspace, rplayers, rstatus;

    surface = set_size(width, height, &rworkspace);
    if (!surface)
        return -1;

    decks_update = true;
    status_update = true;

    /* The final action is to add the timer which triggers refresh */

    timer = SDL_AddTimer(REFRESH, ticker, NULL);

    rig_lock();

    for (;;) {

        rig_unlock();

        if (SDL_WaitEvent(&event) < 0)
            break;

        rig_lock();

        switch(event.type) {
        case SDL_QUIT: /* user request to quit application; eg. window close */
            if (rig_quit() == -1)
                return -1;
            break;

        case SDL_VIDEORESIZE:
            surface = set_size(event.resize.w, event.resize.h, &rworkspace);
            if (!surface)
                return -1;

            decks_update = true;
            status_update = true;

            break;

        case SDL_USEREVENT:
            switch (event.user.code) {
            case EVENT_TICKER: /* request to poll the clocks */
                decks_update = true;
                break;

            case EVENT_QUIT: /* internal request to finish this thread */
                goto finish;

            case EVENT_STATUS:
                status_update = true;
                break;

            default:
                abort();
            }
            break;

        case SDL_KEYDOWN:
            handle_key(event.key.keysym.sym, event.key.keysym.mod);
            break;

        } /* switch(event.type) */

        /* Split the display into the various areas. */
        split(rworkspace, from_bottom(STATUS_HEIGHT, SPACER), &rplayers, &rstatus);

        if (rplayers.h < 0 || rplayers.w < 0)
            decks_update = false;

        LOCK(surface);

        if (status_update)
            draw_status(surface, &rstatus);

        if (decks_update)
            draw_decks(surface, &rplayers, deck, ndeck, meter_scale);

        UNLOCK(surface);

        if (status_update) {
            UPDATE(surface, &rstatus);
            status_update = false;
        }

        if (decks_update) {
            UPDATE(surface, &rplayers);
            decks_update = false;
        }

    } /* main loop */

 finish:
    rig_unlock();

    SDL_RemoveTimer(timer);

    return 0;
}

static void* launch(void *p)
{
    interface_main();
    return NULL;
}

/*
 * Parse and action the given geometry string. Format is "960x720" or
 * "970x720+20+20"
 *
 * Return: -1 if string could not be actioned, otherwise 0
 */

static int parse_geometry(const char *s)
{
    int n, x, y;
    char buf[128];

    n = sscanf(s, "%dx%d+%d+%d", &width, &height, &x, &y);

    switch (n) {
    case EOF: /* empty string */
        break;

    case 2:
        break;

    case 4:
        /* FIXME: Not a desirable way to get geometry information to
         * SDL, but it seems to be the only way */

        sprintf(buf, "SDL_VIDEO_WINDOW_POS=%d,%d", x, y);
        if (putenv(buf) != 0)
            return -1;

        break;

    default:
        return -1;
    }

    return 0;
}

/*
 * Start the SDL interface
 *
 * FIXME: There are multiple points where resources are leaked on
 * error
 */

int interface_start(const char *geo, bool vs)
{
    size_t n;

    vsplit = vs;

    if (parse_geometry(geo) == -1) {
        fprintf(stderr, "Window geometry ('%s') is not valid.\n", geo);
        return -1;
    }

    for (n = 0; n < ndeck; n++) {
        if (timecoder_monitor_init(&deck[n].timecoder, zoom(SCOPE_SIZE)) == -1)
            return -1;
    }

    if (init_spinner(zoom(SPINNER_SIZE)) == -1)
        return -1;

    status_notify(status_change);
    status_set(STATUS_VERBOSE, banner);

    fprintf(stderr, "Initialising SDL...\n");

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) == -1) {
        fprintf(stderr, "%s\n", SDL_GetError());
        return -1;
    }
    SDL_WM_SetCaption(banner, NULL);
    SDL_EnableKeyRepeat(SDL_DEFAULT_REPEAT_DELAY, SDL_DEFAULT_REPEAT_INTERVAL);

    /* Initialise the fonts */

    if (TTF_Init() == -1) {
        fprintf(stderr, "%s\n", TTF_GetError());
        return -1;
    }

    if (load_fonts() == -1)
        return -1;

    fprintf(stderr, "Launching interface thread...\n");

    if (pthread_create(&ph, NULL, launch, NULL)) {
        perror("pthread_create");
        return -1;
    }

    return 0;
}

/*
 * Synchronise with the SDL interface and exit
 */

void interface_stop(void)
{
    size_t n;
    SDL_Event quit;

    quit.type = SDL_USEREVENT;
    quit.user.code = EVENT_QUIT;
    if (SDL_PushEvent(&quit) == -1)
        abort();

    if (pthread_join(ph, NULL) != 0)
        abort();

    for (n = 0; n < ndeck; n++)
        timecoder_monitor_clear(&deck[n].timecoder);

    clear_spinner();
    clear_fonts();

    TTF_Quit();
    SDL_Quit();
}
