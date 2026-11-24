#include "podcastdl.h"

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <utime.h>
#include <unistd.h>

#include "src/system/device/system.h"
#include "src/system/net/http.h"
#include "src/system/playback/device_state.h"
#include "src/system/playback/playlist.h"
#include "src/system/core/utils.h"
#include "src/system/streaming/podcastcache.h"
#include "src/system/streaming/podcastoptions.h"
#include "src/system/streaming/podcaststate.h"

#define DL_TIMEOUT_SECS 20
#define DL_CHUNK 32768
#define DL_NAME_MAX 150 // bytes of a file or folder name, before the extension
#define KEPT_MAX 256	// far past the largest episode limit a podcast can have

typedef struct {
	podcast_episode_t episode;
	char feed_title[PODCAST_TITLE_MAX];
	char feed_author[PODCAST_AUTHOR_MAX];
	char folder[512];
	char path[768];
	char part[800];
	char image[PODCAST_URL_MAX];
} job_t;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static podcastdl_state_t state = PODCASTDL_IDLE;
static long long bytes_done;
static long long bytes_total;
static volatile bool cancel_asked;
static http_stream_t *active_stream; // guarded by `lock`; NULL outside the body loop

static char root[512];

const char *podcastdl_root(void) {
	const char *sd = storage_sd_root();
	if (!sd || !sd[0]) {
		root[0] = '\0';
	} else {
		snprintf(root, sizeof(root), "%.480s/%s", sd, PODCASTDL_FOLDER);
	}
	return root;
}

// The same mapping as podcastcache.c: the decoder picks its path from the
// extension, and a feed that declares nothing is almost always MP3.
static const char *extension_for(const char *mime) {
	if (!mime || !*mime) {
		return "mp3";
	}
	if (strstr(mime, "mp4") || strstr(mime, "m4a") || strstr(mime, "aac")) {
		return "m4a";
	}
	if (strstr(mime, "ogg") || strstr(mime, "opus")) {
		return "opus";
	}
	if (strstr(mime, "wav")) {
		return "wav";
	}
	if (strstr(mime, "flac")) {
		return "flac";
	}
	return "mp3";
}

void podcastdl_safe_name(const char *title, const char *fallback, char *out, size_t size) {
	if (!out || size == 0) {
		return;
	}
	size_t cap = size - 1 < DL_NAME_MAX ? size - 1 : DL_NAME_MAX;
	size_t n = 0;
	for (const unsigned char *p = (const unsigned char *)(title ? title : ""); *p && n < cap; p++) {
		unsigned char c = *p;
		if (c < 0x20 || strchr("/\\*?\"<>|", c)) {
			continue;
		}
		// "Part 1: The start" becomes "Part 1 - The start", "12:30" becomes "12-30".
		if (c == ':') {
			if (p[1] == ' ' && n + 2 <= cap) {
				out[n++] = ' ';
			}
			c = '-';
		}
		out[n++] = (char)c;
	}
	// A cut inside a multi-byte character leaves the lead bytes of it behind.
	if (n == cap) {
		while (n > 0 && ((unsigned char)out[n - 1] & 0xC0) == 0x80) {
			n--;
		}
		if (n > 0 && ((unsigned char)out[n - 1] & 0x80)) {
			n--;
		}
	}
	out[n] = '\0';

	// FAT drops trailing dots and spaces on its own, which would make the name
	// on the card differ from the one asked for.
	while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '.')) {
		out[--n] = '\0';
	}
	size_t lead = 0;
	while (out[lead] == ' ' || out[lead] == '.') {
		lead++;
	}
	if (lead) {
		memmove(out, out + lead, n - lead + 1);
	}
	if (!out[0]) {
		snprintf(out, size, "%s", fallback ? fallback : "Podcast");
	}
}

static bool folder_of(const char *feed_title, char *out, size_t size) {
	const char *base = podcastdl_root();
	if (!base[0]) {
		return false;
	}
	char name[DL_NAME_MAX + 1];
	podcastdl_safe_name(feed_title, "Podcast", name, sizeof(name));
	int n = snprintf(out, size, "%s/%s", base, name);
	return n > 0 && (size_t)n < size;
}

static bool build_path(const podcast_episode_t *episode, const char *feed_title, bool automatic, char *out,
					   size_t size) {
	if (!episode || !out || size == 0) {
		return false;
	}
	char folder[512];
	if (!folder_of(feed_title && feed_title[0] ? feed_title : episode->feed_title, folder, sizeof(folder))) {
		return false;
	}
	char fallback[32];
	snprintf(fallback, sizeof(fallback), "%lld", episode->id);
	char name[DL_NAME_MAX + 1];
	podcastdl_safe_name(episode->title, fallback, name, sizeof(name));
	snprintf(out, size, "%s/%s%s.%s", folder, name, automatic ? "-auto" : "", extension_for(episode->mime));
	return true;
}

bool podcastdl_path(const podcast_episode_t *episode, const char *feed_title, char *out, size_t size) {
	return build_path(episode, feed_title, false, out, size);
}

bool podcastdl_auto_path(const podcast_episode_t *episode, const char *feed_title, char *out, size_t size) {
	return build_path(episode, feed_title, true, out, size);
}

bool podcastdl_is_auto(const char *path) {
	if (!path || !path[0]) {
		return false;
	}
	const char *dot = strrchr(path, '.');
	if (!dot || dot - path < 5) {
		return false;
	}
	return strncasecmp(dot - 5, "-auto", 5) == 0;
}

static bool file_there(const char *path) {
	struct stat st;
	return path && path[0] && stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}

bool podcastdl_exists(const podcast_episode_t *episode, const char *feed_title) {
	char path[768];
	return (podcastdl_path(episode, feed_title, path, sizeof(path)) && file_there(path)) ||
		   (podcastdl_auto_path(episode, feed_title, path, sizeof(path)) && file_there(path));
}

static void set_state(podcastdl_state_t s) {
	pthread_mutex_lock(&lock);
	state = s;
	pthread_mutex_unlock(&lock);
}

// The podcast's artwork, once per folder: cover.jpg or cover.png by what the
// bytes are, which is what the player and the thumbnails look for.
static void fetch_cover(const char *folder, const char *url) {
	if (!url || !url[0]) {
		return;
	}
	char jpg[560], png[560];
	snprintf(jpg, sizeof(jpg), "%s/cover.jpg", folder);
	snprintf(png, sizeof(png), "%s/cover.png", folder);
	struct stat st;
	if (stat(jpg, &st) == 0 || stat(png, &st) == 0) {
		return;
	}
	char *body = NULL;
	size_t len = 0;
	if (!http_get(url, &body, &len, 8 * 1024 * 1024, DL_TIMEOUT_SECS) || !body || len < 8) {
		free(body);
		return;
	}
	const unsigned char *b = (const unsigned char *)body;
	const char *dest = NULL;
	if (b[0] == 0xFF && b[1] == 0xD8) {
		dest = jpg;
	} else if (b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G') {
		dest = png;
	}
	if (dest) {
		FILE *f = fopen(dest, "wb");
		if (f) {
			bool ok = fwrite(body, 1, len, f) == len;
			if (fclose(f) != 0 || !ok) {
				remove(dest);
			}
		}
	}
	free(body);
}

static bool download_body(job_t *job) {
	http_stream_t *st = calloc(1, sizeof(*st));
	if (!st) {
		return false;
	}
	if (!http_stream_open(st, job->episode.enclosure, DL_TIMEOUT_SECS)) {
		fprintf(stderr, "podcastdl: %s does not open\n", job->episode.enclosure);
		free(st);
		return false;
	}

	pthread_mutex_lock(&lock);
	bytes_total = st->content_length > 0 ? st->content_length : 0;
	active_stream = st;
	pthread_mutex_unlock(&lock);

	bool ok = false;
	FILE *f = cancel_asked ? NULL : fopen(job->part, "wb");
	if (f) {
		char *buf = malloc(DL_CHUNK);
		ok = buf != NULL;
		long long done = 0;
		while (ok && !cancel_asked) {
			int n = http_stream_read(st, buf, DL_CHUNK);
			if (n < 0) {
				ok = false;
				break;
			}
			if (n == 0) {
				break;
			}
			if (fwrite(buf, 1, (size_t)n, f) != (size_t)n) {
				fprintf(stderr, "podcastdl: write failed: %s\n", strerror(errno));
				ok = false;
				break;
			}
			done += n;
			pthread_mutex_lock(&lock);
			bytes_done = done;
			pthread_mutex_unlock(&lock);
		}
		free(buf);
		if (fclose(f) != 0) {
			ok = false;
		}
		if (cancel_asked || done == 0 || (bytes_total > 0 && done != bytes_total)) {
			ok = false;
		}
	}

	pthread_mutex_lock(&lock);
	active_stream = NULL;
	pthread_mutex_unlock(&lock);
	http_stream_close(st);
	free(st);
	return ok;
}

// The sidecar podcastcache.c writes for an episode it streams, with the id of
// the episode added: here the file name is its title, not its id.
static void write_tags(const job_t *job) {
	char tags[800];
	char tmp[816];
	snprintf(tags, sizeof(tags), "%s.tags", job->path);
	snprintf(tmp, sizeof(tmp), "%s.tmp", tags);
	FILE *f = fopen(tmp, "w");
	if (!f) {
		return;
	}
	const podcast_episode_t *e = &job->episode;
	fprintf(f, "title=%s\n", e->title);
	fprintf(f, "artist=%s\n", job->feed_title);
	fprintf(f, "album=%s\n", job->feed_title);
	fprintf(f, "feed_id=%lld\n", e->feed_id);
	fprintf(f, "feed_author=%s\n", job->feed_author);
	fprintf(f, "feed_image=%s\n", job->image);
	fprintf(f, "cover_url=%s\n", e->image);
	fprintf(f, "episode_id=%lld\n", e->id);
	bool ok = fclose(f) == 0;
	if (!ok || rename(tmp, tags) != 0) {
		remove(tmp);
	}
}

static void *download_main(void *arg) {
	job_t *job = arg;
	thread_be_low_priority("podcast save"); // the card on screen is waiting for it

	bool ok = !cancel_asked && download_body(job);
	if (ok && rename(job->part, job->path) != 0) {
		fprintf(stderr, "podcastdl: rename to %s failed: %s\n", job->path, strerror(errno));
		ok = false;
	}
	if (!ok) {
		remove(job->part);
		rmdir(job->folder); // only when this download was all it would have held
	} else {
		if (job->episode.published > 0) {
			struct utimbuf times = {(time_t)job->episode.published, (time_t)job->episode.published};
			utime(job->path, &times);
		}
		write_tags(job);
		podcaststate_downloaded(job->episode.id, job->path);
		fetch_cover(job->folder, job->image);
	}
	sync();

	printf("podcastdl: %s %s\n", job->path, ok ? "complete" : cancel_asked ? "cancelled" : "failed");
	set_state(ok ? PODCASTDL_DONE : cancel_asked ? PODCASTDL_CANCELLED : PODCASTDL_FAILED);
	free(job);
	return NULL;
}

bool podcastdl_owns(const char *path) {
	const char *base = podcastdl_root();
	size_t n = strlen(base);
	return path && n > 0 && strncasecmp(path, base, n) == 0 && path[n] == '/';
}

static bool start_job(const podcast_episode_t *episode, const char *feed_title, const char *feed_author,
					  const char *feed_image, bool automatic) {
	if (!episode || !episode->enclosure[0]) {
		return false;
	}
	pthread_mutex_lock(&lock);
	bool busy = state == PODCASTDL_RUNNING;
	pthread_mutex_unlock(&lock);
	if (busy) {
		return false;
	}

	job_t *job = calloc(1, sizeof(*job));
	if (!job) {
		return false;
	}
	job->episode = *episode;
	const char *title = feed_title && feed_title[0] ? feed_title : episode->feed_title;
	snprintf(job->feed_title, sizeof(job->feed_title), "%s", title);
	snprintf(job->feed_author, sizeof(job->feed_author), "%s", feed_author ? feed_author : "");
	if (!folder_of(title, job->folder, sizeof(job->folder)) ||
		!(automatic ? podcastdl_auto_path(episode, title, job->path, sizeof(job->path))
					: podcastdl_path(episode, title, job->path, sizeof(job->path)))) {
		free(job);
		return false;
	}
	mkdir(podcastdl_root(), 0755);
	mkdir(job->folder, 0755);
	struct stat st;
	if (stat(job->folder, &st) != 0 || !S_ISDIR(st.st_mode)) {
		fprintf(stderr, "podcastdl: cannot make %s\n", job->folder);
		free(job);
		return false;
	}
	const char *slash = strrchr(job->path, '/');
	int n = snprintf(job->part, sizeof(job->part), "%s/.%s.part", job->folder, slash ? slash + 1 : job->path);
	if (n < 0 || (size_t)n >= sizeof(job->part)) {
		fprintf(stderr, "podcastdl: path too long for %s\n", job->path);
		free(job);
		return false;
	}
	snprintf(job->image, sizeof(job->image), "%s", feed_image && feed_image[0] ? feed_image : episode->image);

	pthread_mutex_lock(&lock);
	state = PODCASTDL_RUNNING;
	bytes_done = 0;
	bytes_total = 0;
	cancel_asked = false;
	pthread_mutex_unlock(&lock);

	pthread_t thread;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	pthread_attr_setstacksize(&attr, 256 * 1024);
	bool started = pthread_create(&thread, &attr, download_main, job) == 0;
	pthread_attr_destroy(&attr);
	if (!started) {
		free(job);
		set_state(PODCASTDL_FAILED);
		return false;
	}
	return true;
}

bool podcastdl_start(const podcast_episode_t *episode, const char *feed_title, const char *feed_author,
					 const char *feed_image) {
	return start_job(episode, feed_title, feed_author, feed_image, false);
}

bool podcastdl_start_auto(const podcast_episode_t *episode, const char *feed_title, const char *feed_author,
						  const char *feed_image) {
	return start_job(episode, feed_title, feed_author, feed_image, true);
}

void podcastdl_cancel(void) {
	cancel_asked = true;
	// A read blocked on a stalled socket returns only at the receive timeout
	// unless the socket is shut under it.
	pthread_mutex_lock(&lock);
	if (active_stream) {
		http_stream_wake(active_stream);
	}
	pthread_mutex_unlock(&lock);
}

podcastdl_state_t podcastdl_state(long long *done, long long *total) {
	pthread_mutex_lock(&lock);
	podcastdl_state_t s = state;
	if (done) {
		*done = bytes_done;
	}
	if (total) {
		*total = bytes_total;
	}
	pthread_mutex_unlock(&lock);
	return s;
}

void podcastdl_acknowledge(void) {
	pthread_mutex_lock(&lock);
	if (state != PODCASTDL_RUNNING) {
		state = PODCASTDL_IDLE;
	}
	pthread_mutex_unlock(&lock);
}

bool podcastdl_busy(void) { return podcastdl_state(NULL, NULL) == PODCASTDL_RUNNING; }

// ---------------------------------------------------------------------------
// the downloads of one podcast: listing, keeping a limit, deleting
// ---------------------------------------------------------------------------

// Newest first, which is also the order in which episodes are kept when a
// limit is applied.
static int file_cmp(const void *a, const void *b) {
	const podcastdl_file_t *x = a, *y = b;
	if (x->when != y->when) {
		return x->when < y->when ? 1 : -1;
	}
	return strcasecmp(y->path, x->path);
}

// Reads the podcast's folder. Only files with a sidecar Sonix wrote are
// included: a file copied in by hand has no business in a manage list or under
// a delete button. `feed_id <= 0` accepts any sidecar, for a show on the card.
static int list_files(long long feed_id, const char *feed_title, podcastdl_kind_t kind, podcastdl_file_t *out,
					  int max) {
	char folder[600];
	if (!feed_title || !feed_title[0] || !folder_of(feed_title, folder, sizeof(folder))) {
		return 0;
	}
	DIR *dir = opendir(folder);
	if (!dir) {
		return 0;
	}
	int n = 0;
	struct dirent *de;
	int limit = out ? max : KEPT_MAX;
	while ((de = readdir(dir)) != NULL && n < limit) {
		if (de->d_name[0] == '.' || !playlist_is_playable_file(de->d_name)) {
			continue;
		}
		char path[800];
		snprintf(path, sizeof(path), "%.590s/%.200s", folder, de->d_name);
		struct stat st;
		if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
			continue;
		}
		long long id = 0;
		if (!podcastcache_feed_id(path, &id) || (feed_id > 0 && id != feed_id)) {
			continue;
		}
		bool automatic = podcastdl_is_auto(path);
		if (kind == PODCASTDL_KIND_AUTO && !automatic) {
			continue;
		}
		if (kind == PODCASTDL_KIND_MANUAL && automatic) {
			continue;
		}

		if (out) {
			podcastdl_file_t *f = &out[n];
			memset(f, 0, sizeof(*f));
			snprintf(f->path, sizeof(f->path), "%s", path);
			f->when = st.st_mtime;
			f->automatic = automatic;
			const char *dot = strrchr(de->d_name, '.');
			int len = dot ? (int)(dot - de->d_name) : (int)strlen(de->d_name);
			if (automatic && len >= 5) {
				len -= 5; // the "-auto" the downloader added
			}
			snprintf(f->title, sizeof(f->title), "%.*s", len > 0 ? len : 0, de->d_name);
			podcaststate_t state;
			if (podcaststate_get(path, &state)) {
				f->played = state.played;
				f->position = state.position;
				// Playing now, or left part way through: never delete it under the
				// listener, even when it is past the limit.
				f->active = state.touched == PODCASTSTATE_TOUCHED_NOW || (!state.played && state.position > 5);
			}
		}
		n++;
	}
	closedir(dir);
	if (out) {
		qsort(out, (size_t)n, sizeof(*out), file_cmp);
	}
	return n;
}

int podcastdl_list(const char *feed_title, podcastdl_kind_t kind, podcastdl_file_t *out, int max) {
	if (!out || max <= 0) {
		return 0;
	}
	return list_files(0, feed_title, kind, out, max);
}

int podcastdl_count(const char *feed_title, podcastdl_kind_t kind) {
	return list_files(0, feed_title, kind, NULL, 0);
}

static bool file_delete(const char *path) {
	if (remove(path) != 0) {
		return false;
	}
	char side[820];
	snprintf(side, sizeof(side), "%s.tags", path);
	remove(side);
	snprintf(side, sizeof(side), "%s.jpg", path);
	remove(side);
	snprintf(side, sizeof(side), "%s.png", path);
	remove(side);
	podcaststate_forget(path);
	return true;
}

int podcastdl_trim(const char *feed_title, podcastdl_kind_t kind, int keep) {
	if (keep <= 0) {
		return 0;
	}
	podcastdl_file_t *items = calloc(KEPT_MAX, sizeof(*items));
	if (!items) {
		return 0;
	}
	int n = list_files(0, feed_title, kind, items, KEPT_MAX);
	int kept = 0;
	int removed = 0;
	for (int i = 0; i < n; i++) {
		if (items[i].active) {
			continue;
		}
		kept++;
		if (kept > keep && file_delete(items[i].path)) {
			removed++;
		}
	}
	free(items);
	if (removed > 0) {
		sync();
	}
	return removed;
}

int podcastdl_delete_all(const char *feed_title, podcastdl_kind_t kind) {
	podcastdl_file_t *items = calloc(KEPT_MAX, sizeof(*items));
	if (!items) {
		return 0;
	}
	int n = list_files(0, feed_title, kind, items, KEPT_MAX);
	int removed = 0;
	for (int i = 0; i < n; i++) {
		if (!items[i].active && file_delete(items[i].path)) {
			removed++;
		}
	}
	free(items);
	if (removed > 0) {
		sync();
	}
	return removed;
}

bool podcastdl_delete_download(const char *path) {
	if (!path || !path[0] || !podcastdl_owns(path) || !file_delete(path)) {
		return false;
	}
	sync();
	return true;
}

#define PENDING_MAX 32
static struct {
	char path[800];
	long long feed_id;
} pending[PENDING_MAX];
static int pending_count;
static pthread_mutex_t pending_lock = PTHREAD_MUTEX_INITIALIZER;

void podcastdl_delete_when_idle(const char *path, long long feed_id) {
	if (!path || !path[0] || feed_id <= 0 || !podcastdl_owns(path)) {
		return;
	}
	pthread_mutex_lock(&pending_lock);
	for (int i = 0; i < pending_count; i++) {
		if (strcmp(pending[i].path, path) == 0) {
			pthread_mutex_unlock(&pending_lock);
			return;
		}
	}
	if (pending_count < PENDING_MAX) {
		snprintf(pending[pending_count].path, sizeof(pending[0].path), "%s", path);
		pending[pending_count].feed_id = feed_id;
		pending_count++;
	}
	pthread_mutex_unlock(&pending_lock);
}

void podcastdl_apply_pending(void) {
	pthread_mutex_lock(&pending_lock);
	if (pending_count > 0) {
		device_state_t device;
		device_state_get(&device);
		for (int i = 0; i < pending_count;) {
			podcaststate_t state;
			bool known = podcaststate_get(pending[i].path, &state);
			// The state row still says "playing" when the player is left sitting
			// at the end of a finished episode with repeat off, so ask the player
			// itself rather than trusting `touched` here.
			bool playing = !device.live && strcmp(device.current_file, pending[i].path) == 0;
			bool drop = true;
			if (known && state.played) {
				if (playing) {
					drop = false; // try again once the player has moved on
				} else if (podcast_options_delete_heard(pending[i].feed_id) &&
						   (podcast_options_delete_all(pending[i].feed_id) ||
							podcastdl_is_auto(pending[i].path))) {
					// "Only delete automatic downloads" applies here too: a
					// manual copy stays unless the listener asks for all of them.
					podcastdl_delete_download(pending[i].path);
				}
			}
			if (drop) {
				pending_count--;
				if (i < pending_count) {
					pending[i] = pending[pending_count];
				}
			} else {
				i++;
			}
		}
	}
	pthread_mutex_unlock(&pending_lock);
}
