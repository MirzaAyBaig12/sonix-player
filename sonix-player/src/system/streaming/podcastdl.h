#ifndef PODCASTDL_H
#define PODCASTDL_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#include "src/system/streaming/podcast.h"

// Podcast episodes kept on the card for good, as ordinary files:
//
//   <card>/Podcast/<podcast title>/<episode title>.<ext>
//   <card>/Podcast/<podcast title>/<episode title>-auto.<ext>
//
// The second name is what the automatic downloader writes. The "-auto" is
// before the extension, so the player still sees an ordinary audio file, and
// lets the two kinds of download be listed and deleted separately. Everything
// else is shared: the podcast's artwork beside them as cover.jpg (or
// cover.png), each file's modification time set to the episode's publication
// date, and a "<file>.tags" sidecar in podcastcache.h's format. Downloads stay
// until the listener deletes them or a podcast's own settings ask for them to
// be trimmed.
//
// One download at a time, on a thread of its own. The file is written under a
// hidden ".<name>.part" and renamed when complete, so a cut download never
// shows up as an episode.

#define PODCASTDL_FOLDER "Podcast"

typedef enum {
	PODCASTDL_IDLE,
	PODCASTDL_RUNNING,
	PODCASTDL_DONE,
	PODCASTDL_FAILED,
	PODCASTDL_CANCELLED,
} podcastdl_state_t;

// <card>/Podcast, or "" when there is no card.
const char *podcastdl_root(void);

// Where `episode` of the podcast titled `feed_title` is saved. False when there
// is no card. Says nothing about whether the file exists.
bool podcastdl_path(const podcast_episode_t *episode, const char *feed_title, char *out, size_t size);

// Where an automatically downloaded copy would go: the same name with "-auto"
// before the extension. A manual and an automatic copy of one episode can
// therefore live side by side.
bool podcastdl_auto_path(const podcast_episode_t *episode, const char *feed_title, char *out, size_t size);

// True when `path` is a file the automatic downloader wrote.
bool podcastdl_is_auto(const char *path);

// Whether either copy is already on the card.
bool podcastdl_exists(const podcast_episode_t *episode, const char *feed_title);

// Starts downloading `episode` into the folder of `feed_title`. `feed_image` is
// the podcast's artwork, fetched once per folder; `feed_author` goes into the
// sidecar for the player's follow star. Either NULL or empty for none. False,
// starting nothing, when a download is already running, there is no card, or
// the folder cannot be made.
bool podcastdl_start(const podcast_episode_t *episode, const char *feed_title, const char *feed_author,
					 const char *feed_image);

// The same, but names the finished file with "-auto" before its extension.
bool podcastdl_start_auto(const podcast_episode_t *episode, const char *feed_title, const char *feed_author,
						  const char *feed_image);

// True when `path` is inside the Podcast folder.
bool podcastdl_owns(const char *path);

// Stops the running download and deletes what it wrote. Returns at once; the
// state becomes PODCASTDL_CANCELLED when the thread has wound up.
void podcastdl_cancel(void);

// The state, with the bytes written and the size the server announced (0 when
// it did not say). Either pointer may be NULL.
podcastdl_state_t podcastdl_state(long long *done, long long *total);

// Back to PODCASTDL_IDLE after a finished, failed or cancelled download has
// been reported.
void podcastdl_acknowledge(void);

// True while a download holds the network.
bool podcastdl_busy(void);

// One file in a podcast's download folder, for the two "manage downloads"
// views and for the automatic retention rules.
typedef struct {
	char path[800];
	char title[PODCAST_TITLE_MAX]; // file name without extension or "-auto"
	time_t when;				   // publication date, or the file's mtime
	bool automatic;
	bool played;
	bool active; // playing now, or left part way through: never delete it
	double position;
} podcastdl_file_t;

typedef enum {
	PODCASTDL_KIND_ALL = 0,
	PODCASTDL_KIND_AUTO,
	PODCASTDL_KIND_MANUAL,
} podcastdl_kind_t;

// Lists the downloaded episodes of `feed_title`, newest first, optionally
// restricted to one of the two kinds. Returns how many were written.
int podcastdl_list(const char *feed_title, podcastdl_kind_t kind, podcastdl_file_t *out, int max);

// How many episodes of one kind are on the card. A light answer for the
// automatic downloader, which does not need the list itself.
int podcastdl_count(const char *feed_title, podcastdl_kind_t kind);

// Keeps only the newest `keep` episodes of one kind (0 disables trimming).
// An episode being played, or left part way through, is never deleted.
// Returns how many files were removed.
int podcastdl_trim(const char *feed_title, podcastdl_kind_t kind, int keep);

// Deletes every episode of one kind. Returns how many files were removed.
int podcastdl_delete_all(const char *feed_title, podcastdl_kind_t kind);

// Deletes one downloaded episode and its sidecars. Files that Sonix did not
// download are left alone.
bool podcastdl_delete_download(const char *path);

// An episode that has just been marked heard is not deleted under the player's
// feet: it is noted here instead, and podcastdl_apply_pending() (once a second,
// from the interface thread) removes it once it is no longer the track being
// played. The per-podcast option is read again at that point, so turning the
// switch off in between is enough to keep the file.
void podcastdl_delete_when_idle(const char *path, long long feed_id);
void podcastdl_apply_pending(void);

// Turns a title into a file or folder name the card accepts: the characters
// FAT forbids go, leading and trailing dots and spaces go, the length is capped
// on a UTF-8 boundary. `fallback` when nothing is left.
void podcastdl_safe_name(const char *title, const char *fallback, char *out, size_t size);

#endif /* PODCASTDL_H */
