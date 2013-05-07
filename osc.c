#include <stdio.h>
#include <string.h>
#include <lo/lo.h>
#include <sys/poll.h>
#include <sys/types.h>

#include "controller.h"
#include "deck.h"
#include "osc.h"
#include "xwax.h"

#define PATH_MAX_LEN 20

/* Handlers */
typedef int (osc_handler)(const char *path, const char *types,
    lo_arg **argv, int argc, lo_message msg, void *user_data);

struct osc {
    lo_server lo;
    int ndeck;
};


static int handler_clone(const char *path, const char *types,
    lo_arg **argv, int argc, lo_message msg, void *user_data)
{
    struct deck *de = user_data;
    int i = argv[0]->i;

    if (i > 0 && i <= ndeck)
        deck_clone(de, &deck[i - 1]);

    return 0;
}

static int handler_connect(const char *path, const char *types,
    lo_arg **argv, int argc, lo_message msg, void *user_data)
{
    struct deck *de = user_data;
    struct player *pl = &de->player;

    player_toggle_timecode_control(pl);

    return 0;
}

static int handler_cue(const char *path, const char *types,
    lo_arg **argv, int argc, lo_message msg, void *user_data)
{
    struct deck *de = user_data;
    int label = argv[0]->i - 1;

    if (label < 0 || label > MAX_CUES)
        return 0;

    deck_cue(de, label);

    return 0;
}

static int handler_cue_go(const char *path, const char *types,
    lo_arg **argv, int argc, lo_message msg, void *user_data)
{
    struct deck *de = user_data;
    int label = argv[0]->i - 1;
    double p;

    if (label < 0 || label > MAX_CUES)
        return 0;

    p = cues_get(&de->cues, label);
    if (p != CUE_UNSET)
        player_seek_to(&de->player, p);

    return 0;
}

static int handler_cue_set(const char *path, const char *types,
    lo_arg **argv, int argc, lo_message msg, void *user_data)
{
    struct deck *de = user_data;
    int label = argv[0]->i - 1;
    double p;
    lo_arg d;

    if (label < 0 || label > MAX_CUES)
        return 0;

    if (argc > 0 && lo_coerce(LO_DOUBLE, &d, types[1], argv[1]))
        p = d.d;
    else
        p = player_get_position(&de->player);

    cues_set(&de->cues, label, p);

    return 0;
}

static int handler_cue_set_many(const char *path, const char *types,
    lo_arg **argv, int argc, lo_message msg, void *user_data)
{
    struct deck *de = user_data;
    int i, label = 0;
    lo_arg p;

    for (i = 0; types[i] && label < MAX_CUES; ++i) {
        if (!lo_coerce(LO_DOUBLE, &p, types[i], argv[i]))
            continue;
        cues_set(&de->cues, label++, p.d);
    }

    return 0;
}

static int handler_cue_unset(const char *path, const char *types,
    lo_arg **argv, int argc, lo_message msg, void *user_data)
{
    struct deck *de = user_data;
    int label = argv[0]->i - 1;

    if (label < 0 || label > MAX_CUES)
        return 0;

    deck_unset_cue(de, label);

    return 0;
}

static int handler_pitch(const char *path, const char *types,
    lo_arg **argv, int argc, lo_message msg, void *user_data)
{
    struct deck *de = user_data;
    struct player *pl = &de->player;
    double p = argv[0]->d;

    if (!pl)
        return 0;

    pl->pitch = p;

    return 0;
}

static int handler_play(const char *path, const char *types,
    lo_arg **argv, int argc, lo_message msg, void *user_data)
{
    struct deck *de = user_data;
    struct player *pl;

    pl = &de->player;
    if (!pl)
        return 0;

    if (pl->timecode_control) {
        player_set_timecode_control(pl, 0);
        pl->pitch = 1;
    } else
        pl->pitch = !pl->pitch;

    return 0;
}

static int handler_position_get(const char *path, const char *types,
    lo_arg **argv, int argc, lo_message msg, void *user_data)
{
    struct deck *de = user_data;
    struct player *pl = &de->player;
    lo_address dest;

    pl = &de->player;
    if (!pl)
        return 0;

    dest = lo_address_new_from_url(&argv[0]->s);
    if (!dest)
        return -1;

    lo_send(dest, &argv[1]->s, "fi", (float)(pl->position - pl->offset), argv[2]->i);

    return 0;
}

static int handler_position_set(const char *path, const char *types,
    lo_arg **argv, int argc, lo_message msg, void *user_data)
{
    struct deck *de = user_data;
    struct player *pl = &de->player;
    double p = argv[0]->d;

    if (!pl)
        return 0;

    player_seek_to(pl, p);

    return 0;
}

static int handler_position_rel(const char *path, const char *types,
    lo_arg **argv, int argc, lo_message msg, void *user_data)
{
    struct deck *de = user_data;
    struct player *pl = &de->player;
    double l, p = argv[0]->d;

    if (!pl)
        return 0;

    if (p < 0)
        p = 0;
    else if (p > 1)
        p = 1;

    l = (double)pl->track->length / pl->track->rate;
    player_seek_to(pl, p * l);

    return 0;
}

static int handler_recue(const char *path, const char *types,
    lo_arg **argv, int argc, lo_message msg, void *user_data)
{
    struct deck *de = user_data;
    deck_recue(de);
    return 0;
}

static int set_handler(struct osc *osc, const char *path, const char *typespec,
    osc_handler *handler, void *user_data)
{
    if (!osc)
        return -1;
    return !lo_server_thread_add_method(osc->lo, path, typespec,
                                        handler, user_data);
}


static int add_deck(struct controller *c, struct deck *deck)
{
    struct osc *osc = c->local;
    char path[PATH_MAX_LEN], *pathtail;
    int len;

    len = snprintf(path, PATH_MAX_LEN, "/deck%d/", osc->ndeck + 1);
    pathtail = path + len;
    len = PATH_MAX_LEN - len;

    strncpy(pathtail, "clone", len);
    if (set_handler(osc, path, "i", handler_clone, deck))
        return -1;

    strncpy(pathtail, "connect", len);
    if (set_handler(osc, path, NULL, handler_connect, deck))
        return -1;

    strncpy(pathtail, "cue", len);
    if (set_handler(osc, path, "i", handler_cue, deck))
        return -1;

    strncpy(pathtail, "cue/go", len);
    if (set_handler(osc, path, "i", handler_cue_go, deck))
        return -1;

    strncpy(pathtail, "cue/set", len);
    if (set_handler(osc, path, "i", handler_cue_set, deck))
        return -1;

    strncpy(pathtail, "cue/set", len);
    if (set_handler(osc, path, "id", handler_cue_set, deck))
        return -1;

    strncpy(pathtail, "cue/set", len);
    if (set_handler(osc, path, NULL, handler_cue_set_many,  deck))
        return -1;

    strncpy(pathtail, "cue/unset", len);
    if (set_handler(osc, path, "i", handler_cue_unset, deck))
        return -1;

    strncpy(pathtail, "pitch", len);
    if (set_handler(osc, path, "d", handler_pitch, deck))
        return -1;

    strncpy(pathtail, "play", len);
    if (set_handler(osc, path, NULL, handler_play, deck))
        return -1;

    strncpy(pathtail, "position", len);
    if (set_handler(osc, path, "d", handler_position_set, deck))
        return -1;

    strncpy(pathtail, "position/get", len);
    if (set_handler(osc, path, "ssi", handler_position_get, deck))
        return -1;

    strncpy(pathtail, "position/set", len);
    if (set_handler(osc, path, "d", handler_position_set, deck))
        return -1;

    strncpy(pathtail, "recue", len);
    if (set_handler(osc, path, NULL, handler_recue, deck))
        return -1;

    strncpy(pathtail, "seek", len);
    if (set_handler(osc, path, "d", handler_position_rel, deck))
        return -1;

    osc->ndeck += 1;
    return 0;
}

static ssize_t pollfds(struct controller *c, struct pollfd *pe, size_t z)
{
    return 0;
}

static int realtime(struct controller *c)
{
    return 0;
}

static void clear(struct controller *c)
{
    struct osc *osc = c->local;
    lo_server_thread_stop(osc->lo);
    lo_server_thread_free(osc->lo);
    free(osc);
}

static struct controller_ops osc_ops = {
    .add_deck = add_deck,
    .pollfds = pollfds,
    .realtime = realtime,
    .clear = clear,
};

int osc_init(struct controller *c, struct rt *rt, const char *port)
{
    struct osc *osc;
    osc = malloc(sizeof *osc);
    if (!osc) {
        perror("malloc()");
        goto fail1;
    }

    if (!strcmp("0", port))
        port = NULL;

    osc->ndeck = 0;
    osc->lo = lo_server_thread_new(port, NULL);
    if (!osc->lo) {
        perror("lo_server_thread_new()");
        goto fail2;
    }

    if (lo_server_thread_start(osc->lo) < 0) {
        perror("lo_server_thread_start()");
        goto fail3;
     }

    controller_init(c, &osc_ops);
    c->local = osc;

    fprintf(stderr, "OSC server started on port %d\n", lo_server_thread_get_port(osc->lo));
    return 0;

fail3:
    lo_server_thread_free(osc->lo);
fail2:
    free(osc);
fail1:
    return -1;
}
