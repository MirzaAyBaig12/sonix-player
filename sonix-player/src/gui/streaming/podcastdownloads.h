#ifndef PODCAST_DOWNLOADS_H
#define PODCAST_DOWNLOADS_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

extern lv_obj_t *podcastdownloads_screen;

// Built once with the rest of the streaming pages.
void podcastdownloads_init(gui_config_t *cfg);

// Opens the Downloads page of one podcast in the list. `feed_id` is 0 for a
// folder copied onto the card: it has no feed to check for new episodes, but
// the files already there can still be managed.
void podcastdownloads_open(long long feed_id, const char *title);

#endif /* PODCAST_DOWNLOADS_H */
