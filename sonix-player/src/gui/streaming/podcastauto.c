#include "podcastauto.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "src/gui/shell/toast.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/core/utils.h"
#include "src/system/net/wifi.h"
#include "src/system/streaming/podcast.h"
#include "src/system/streaming/podcastdl.h"
#include "src/system/streaming/podcastoptions.h"
#include "src/system/streaming/podcaststate.h"
#include "src/system/streaming/podcastsubs.h"

// Check on boot, then every fifteen minutes, and also at the exact moment a
// Wi-Fi address appears. A check that finds nothing is simply the one at that
// quarter hour: the next one comes on its own.
#define AUTO_CHECK_SECS (15 * 60)
#define AUTO_EPISODES 20 // one catalogue page per podcast per check
#define AUTO_WAIT_SECS 120

typedef struct {
	bool all; // false for a single podcast asked for by its settings page
	long long feed_id;
	char title[PODCAST_TITLE_MAX];
} auto_job_t;

static pthread_mutex_t sync_lock = PTHREAD_MUTEX_INITIALIZER;
static bool sync_running;
static time_t last_check;
static bool boot_checked;
static lv_timer_t *auto_timer;

static bool network_up(void) {
#ifdef HOST_BUILD
	return true; // the simulator has a network and no wlan0
#else
	wifi_status_t status;
	wifi_get_status(&status);
	return status.state == WIFI_STATE_CONNECTED && status.ip[0] != '\0';
#endif
}

static void done_cb(void *user) {
	(void)user;
	toast_success("podcast_auto_done");
}

// Runs on a thread of its own: every call to podcast_episodes(), podcast_feed()
// and the downloader blocks. Only the finished toast goes back to the UI.
static void *auto_main(void *arg) {
	auto_job_t *job = arg;
	thread_be_background("podcast autodl");
	podcast_episode_t *episodes = calloc(AUTO_EPISODES, sizeof(*episodes));
	bool notified = false;

	if (episodes && podcast_configured() && podcastdl_root()[0]) {
		podcast_feed_t feeds[PODCASTSUBS_MAX];
		int count = 0;
		if (job->all) {
			count = podcastsubs_list(feeds, PODCASTSUBS_MAX);
		} else if (podcastsubs_is_followed(job->feed_id)) {
			count = podcastsubs_list(feeds, PODCASTSUBS_MAX);
			for (int i = 0; i < count; i++) {
				if (feeds[i].id == job->feed_id) {
					feeds[0] = feeds[i];
					count = 1;
					break;
				}
			}
		} else if (podcast_feed(job->feed_id, &feeds[0])) {
			snprintf(feeds[0].title, sizeof(feeds[0].title), "%s", job->title);
			count = 1;
		}

		for (int i = 0; i < count && network_up(); i++) {
			const podcast_feed_t *feed = &feeds[i];
			if (!job->all && feed->id != job->feed_id) {
				continue;
			}
			if (feed->id <= 0) {
				continue;
			}
			const char *title = feed->title[0] ? feed->title : job->title;

			// Retention is its own setting: it applies even when automatic
			// downloads are off, and on every check rather than only after a
			// new file lands. That way lowering a limit takes effect at once.
			podcastdl_trim(title, PODCASTDL_KIND_AUTO, podcast_options_keep_auto(feed->id));
			podcastdl_trim(title, PODCASTDL_KIND_MANUAL, podcast_options_keep_manual(feed->id));

			if (!podcast_options_auto(feed->id)) {
				continue;
			}
			int keep = podcast_options_keep_auto(feed->id);
			int stored = podcastdl_count(title, PODCASTDL_KIND_AUTO);
			int got = podcast_episodes(feed->id, episodes, AUTO_EPISODES);
			if (got <= 0) {
				continue;
			}
			int downloaded = 0;
			for (int k = 0; k < got && network_up(); k++) {
				podcast_episode_t *episode = &episodes[k];
				if (stored >= keep) {
					break;
				}
				if (podcastdl_exists(episode, title)) {
					continue;
				}
				podcaststate_t state;
				if (podcaststate_get_episode(episode->id, &state) && state.played) {
					continue;
				}
				// The player's own download queue has the downloader while it
				// runs; wait for it rather than failing the check.
				time_t waited = time(NULL);
				while (podcastdl_busy() && time(NULL) - waited < AUTO_WAIT_SECS) {
					usleep(200000);
					if (!network_up()) {
						break;
					}
				}
				if (podcastdl_busy() || !network_up()) {
					break;
				}
				if (!podcastdl_start_auto(episode, title, feed->author, feed->image)) {
					continue;
				}
				waited = time(NULL);
				while (podcastdl_busy() && time(NULL) - waited < AUTO_WAIT_SECS) {
					usleep(200000);
				}
				if (podcastdl_busy()) {
					break; // still running: leave it to finish on its own
				}
				podcastdl_state_t result = podcastdl_state(NULL, NULL);
				podcastdl_acknowledge();
				if (result != PODCASTDL_DONE) {
					continue;
				}
				stored++;
				downloaded++;
				// Keep the automatic copies at their own limit as each lands.
				podcastdl_trim(title, PODCASTDL_KIND_AUTO, keep);
			}
			if (downloaded > 0) {
				notified = true;
			}
		}
	}
	free(episodes);
	free(job);
	if (notified) {
		gui_post(done_cb, NULL);
	}
	pthread_mutex_lock(&sync_lock);
	sync_running = false;
	pthread_mutex_unlock(&sync_lock);
	return NULL;
}

static void start_check(bool all, long long feed_id, const char *title) {
	if (!network_up() || !podcast_configured() || !podcastdl_root()[0] || podcastsubs_count() <= 0) {
		return;
	}
	pthread_mutex_lock(&sync_lock);
	if (sync_running) {
		pthread_mutex_unlock(&sync_lock);
		return;
	}
	auto_job_t *job = calloc(1, sizeof(*job));
	if (!job) {
		pthread_mutex_unlock(&sync_lock);
		return;
	}
	job->all = all;
	job->feed_id = feed_id;
	snprintf(job->title, sizeof(job->title), "%s", title ? title : "");
	sync_running = true;
	pthread_mutex_unlock(&sync_lock);

	pthread_t thread;
	if (pthread_create(&thread, NULL, auto_main, job) != 0) {
		free(job);
		pthread_mutex_lock(&sync_lock);
		sync_running = false;
		pthread_mutex_unlock(&sync_lock);
		return;
	}
	pthread_detach(thread);
}

void podcastauto_sync_feed(long long feed_id, const char *title) {
	if (feed_id <= 0 || !podcast_options_auto(feed_id)) {
		return;
	}
	start_check(false, feed_id, title);
}

// From the wifi worker, on the edge that also starts the clock sync.
static void connected_cb(void) {
	boot_checked = true; // the boot check has happened, one way or another
	last_check = time(NULL);
	start_check(true, 0, NULL);
}

static void timer_cb(lv_timer_t *timer) {
	(void)timer;
	// Heard episodes are deleted here, once they are no longer under the
	// player: marking heard and finishing an episode both only note the file.
	podcastdl_apply_pending();

	if (!boot_checked) {
		boot_checked = true;
		last_check = time(NULL);
		start_check(true, 0, NULL);
		return;
	}
	time_t now = time(NULL);
	if (now - last_check >= AUTO_CHECK_SECS) {
		last_check = now;
		start_check(true, 0, NULL);
	}
}

time_t podcastauto_last_sync(void) {
	return last_check;
}

void podcastauto_init(gui_config_t *cfg) {
	(void)cfg;
	wifi_set_connected_hook(connected_cb);
	auto_timer = lv_timer_create(timer_cb, 1000, NULL);
}
