#include "podcastoptions.h"

#include <stdio.h>

#include "src/system/core/config.h"

// The config's keys are 31 bytes plus a terminator at most. A feed id is at
// most nineteen digits, so the shortest suffixes below are needed to fit.
static void feed_key(long long feed_id, const char *what, char *out, size_t size) {
	snprintf(out, size, "feed_%lld_%s", feed_id, what);
}

static int keep_get(long long feed_id, const char *what) {
	char key[32];
	feed_key(feed_id, what, key, sizeof(key));
	int keep = (int)config_get_int("podcast", key, PODCAST_KEEP_DEFAULT);
	if (keep < PODCAST_KEEP_MIN || keep > PODCAST_KEEP_MAX) {
		return PODCAST_KEEP_DEFAULT;
	}
	return keep;
}

static void keep_set(long long feed_id, const char *what, int episodes) {
	if (feed_id <= 0 || episodes < PODCAST_KEEP_MIN || episodes > PODCAST_KEEP_MAX) {
		return;
	}
	char key[32];
	feed_key(feed_id, what, key, sizeof(key));
	config_set_int("podcast", key, episodes);
	config_save();
}

bool podcast_options_auto(long long feed_id) {
	if (feed_id <= 0) {
		return false;
	}
	char key[32];
	feed_key(feed_id, "auto", key, sizeof(key));
	return config_get_bool("podcast", key, false);
}

bool podcast_options_delete_heard(long long feed_id) {
	if (feed_id <= 0) {
		return false;
	}
	char key[32];
	feed_key(feed_id, "heard", key, sizeof(key));
	return config_get_bool("podcast", key, false);
}

int podcast_options_keep_auto(long long feed_id) {
	if (feed_id <= 0) {
		return PODCAST_KEEP_DEFAULT;
	}
	return keep_get(feed_id, "akeep");
}

int podcast_options_keep_manual(long long feed_id) {
	if (feed_id <= 0) {
		return PODCAST_KEEP_DEFAULT;
	}
	return keep_get(feed_id, "mkeep");
}

bool podcast_options_delete_all(long long feed_id) {
	if (feed_id <= 0) {
		return false;
	}
	char key[32];
	feed_key(feed_id, "scope", key, sizeof(key));
	return config_get_bool("podcast", key, false);
}

void podcast_options_set_auto(long long feed_id, bool on) {
	if (feed_id <= 0) {
		return;
	}
	char key[32];
	feed_key(feed_id, "auto", key, sizeof(key));
	config_set_bool("podcast", key, on);
	config_save();
}

void podcast_options_set_delete_heard(long long feed_id, bool on) {
	if (feed_id <= 0) {
		return;
	}
	char key[32];
	feed_key(feed_id, "heard", key, sizeof(key));
	config_set_bool("podcast", key, on);
	config_save();
}

void podcast_options_set_keep_auto(long long feed_id, int episodes) { keep_set(feed_id, "akeep", episodes); }

void podcast_options_set_keep_manual(long long feed_id, int episodes) { keep_set(feed_id, "mkeep", episodes); }

void podcast_options_set_delete_all(long long feed_id, bool all) {
	if (feed_id <= 0) {
		return;
	}
	char key[32];
	feed_key(feed_id, "scope", key, sizeof(key));
	config_set_bool("podcast", key, all);
	config_save();
}
