#ifndef PODCAST_AUTO_H
#define PODCAST_AUTO_H

#include "src/gui/shell/gui.h"

#include <time.h>

// The automatic side of podcasts: a check at boot, another every fifteen
// minutes, and one the moment Wi-Fi comes up. Only podcasts whose own settings
// ask for automatic downloads are checked.
void podcastauto_init(gui_config_t *cfg);

// Checks one podcast now, for the switch on its settings page and when its
// episodes page first opens. Does nothing when that podcast has not asked for
// automatic downloads.
void podcastauto_sync_feed(long long feed_id, const char *title);

// When the last automatic check started, or 0 before the first one. Used by
// the Downloads page to say when the feeds were last looked at.
time_t podcastauto_last_sync(void);

#endif /* PODCAST_AUTO_H */
