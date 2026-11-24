#ifndef PODCAST_OPTIONS_H
#define PODCAST_OPTIONS_H

#include <stdbool.h>

// The choices made on one podcast's Downloads page, reached from the three dots
// beside it in the list of podcasts. They live in the main config, keyed by the
// podcast's id so a rename cannot lose them:
//
//     [podcast]
//     feed_123_auto = 1
//     feed_123_akeep = 10
//     feed_123_mkeep = 10
//     feed_123_scope = 0
//     feed_123_heard = 1
//
// Keys are at most 31 characters, hence the short "akeep" / "mkeep". A podcast
// copied by hand onto the card has no id and no automatic downloads: there is
// no feed to check for new episodes. Its downloaded files can still be managed.

#define PODCAST_KEEP_MIN 5
#define PODCAST_KEEP_MAX 25
#define PODCAST_KEEP_DEFAULT 10

bool podcast_options_auto(long long feed_id);
bool podcast_options_delete_heard(long long feed_id);

// How many downloads of each kind to leave on the card. The oldest beyond the
// limit go when a new one lands (and at the next automatic check).
int podcast_options_keep_auto(long long feed_id);
int podcast_options_keep_manual(long long feed_id);

// The scope of deletions that do not name one file: true for every download,
// false for automatic downloads only. The default is the safer "auto only".
bool podcast_options_delete_all(long long feed_id);

void podcast_options_set_auto(long long feed_id, bool on);
void podcast_options_set_delete_heard(long long feed_id, bool on);
void podcast_options_set_keep_auto(long long feed_id, int episodes);
void podcast_options_set_keep_manual(long long feed_id, int episodes);
void podcast_options_set_delete_all(long long feed_id, bool all);

#endif /* PODCAST_OPTIONS_H */
