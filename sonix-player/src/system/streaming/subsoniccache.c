#include "subsoniccache.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <pthread.h>

#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/core/md5.h"
#include "src/system/core/utils.h"
#include "src/system/decode/growfile.h"
#include "src/system/net/http.h"
#include "src/system/streaming/streamturn.h"

#define FETCH_TIMEOUT_SECS 20
#define FETCH_CHUNK 32768

// How long the caller of subsoniccache_start() may be kept waiting for the
// prebuffer before playback starts anyway.
//
// subsoniccache_start() runs on the GUI thread when a track is tapped, and the
// prebuffer it builds is measured in bytes, not time: 512 KB, or more when the
// network turns out to be slower than the track consumes. On a 1 Mbit/s link
// that is tens of seconds of a frozen interface -- the reported symptom -- and
// the very thing the reserve was for is already handled downstream: the decoder
// reads a file that is still growing (growfile) and audio.c refills it. So the
// wait is capped, and a slow network trades a possible small stall later for an
// interface that answers now.
//
// Only once the first bytes have arrived: a path to a zero-length file is not a
// track, so the connection itself is still allowed to take as long as its own
// timeout allows.
#define STREAM_START_BUDGET_MS 1200

static char cache_dir[512];

// The reason a download failed, for the page to show. Every error exit records
// what happened, so that no card, no network and a full card do not all reach
// the page as one "download failed".
static __thread char last_error[192];

const char *subsoniccache_last_error(void) { return last_error; }

static void set_error(const char *what) { snprintf(last_error, sizeof(last_error), "%s", what ? what : ""); }

void subsoniccache_abandon_all(void);

// Registers with the shared turn once, at the first opportunity: from then on a
// track requested from Tidal knows how to make a Subsonic download let go, and
// vice versa.
static void register_abandon_once(void) {
	static bool done;
	if (!done) {
		done = true;
		streamturn_register_abandon(subsoniccache_abandon_all);
	}
}

void subsoniccache_set_root(const char *sd_root) {
	register_abandon_once();
	if (!sd_root || !*sd_root) {
		cache_dir[0] = '\0';
		return;
	}
	snprintf(cache_dir, sizeof(cache_dir), "%s/%s", sd_root, SUBSONICCACHE_DIR);
}

bool subsoniccache_ready(void) { return cache_dir[0] != '\0'; }

// ---------------------------------------------------------------------------
// keeping what was streamed
// ---------------------------------------------------------------------------

// Kept in the configuration rather than in a variable, so that the page, this
// module and the shutdown cleanup cannot disagree. Default false: streaming is
// what a music server is for, and a card that quietly fills with files the user
// never put there is a surprise, not a feature.
bool subsoniccache_keep_downloads(void) { return config_get_bool("subsonic", "keep", false); }

void subsoniccache_set_keep_downloads(bool keep) {
	config_set_bool("subsonic", "keep", keep);
	config_save();
}

// Deletes one cached track and everything written beside it. The decoder may
// still hold the file open for an instant after the queue moved on; unlinking
// an open file is safe on this kernel -- the inode lives until the last close.
static void marker_path(const char *path, char *out, size_t size);

static void drop_track(const char *path) {
	if (!path || !*path || !subsoniccache_owns(path)) {
		return;
	}

	// Its sidecars first: a .tags without a track beside it is litter that the
	// next download would not overwrite.
	char tags[544];
	snprintf(tags, sizeof(tags), "%.500s.tags", path);
	remove(tags);

	char cover[544];
	const char *dot = strrchr(path, '.');
	snprintf(cover, sizeof(cover), "%.*s.jpg", dot ? (int)(dot - path) : (int)strlen(path), path);
	remove(cover);

	char marker[544];
	marker_path(path, marker, sizeof(marker));
	remove(marker);

	if (remove(path) == 0) {
		printf("subsoniccache: streamed and dropped %s\n", strrchr(path, '/') ? strrchr(path, '/') + 1 : path);
	}
}

// The path playback was on at the last tick, so the one it has just left is
// known. Guarded because the tick runs on the GUI thread and a card change can
// clear it from another.
static pthread_mutex_t stream_lock = PTHREAD_MUTEX_INITIALIZER;
static char stream_last[512];

void subsoniccache_stream_tick(const char *current_path) {
	if (!subsoniccache_ready()) {
		return;
	}

	// "Nothing is playing" is not a reason to delete: the track is still the
	// current one and a pause must not cost a download. Only a different track
	// taking over retires the old file.
	char now[512];
	if (current_path && subsoniccache_owns(current_path)) {
		snprintf(now, sizeof(now), "%s", current_path);
	} else {
		now[0] = '\0';
	}

	pthread_mutex_lock(&stream_lock);
	char previous[512];
	snprintf(previous, sizeof(previous), "%s", stream_last);
	if (now[0]) {
		snprintf(stream_last, sizeof(stream_last), "%s", now);
	}
	pthread_mutex_unlock(&stream_lock);

	if (!now[0] || !previous[0] || strcmp(previous, now) == 0) {
		return;
	}
	if (subsoniccache_keep_downloads()) {
		return; // the cache keeps what it played
	}

	// Not the one playing, and the setting says nothing is kept: let it go.
	drop_track(previous);
}

const char *subsoniccache_dir(void) { return cache_dir[0] ? cache_dir : NULL; }

// The extension a song's file will have. The decoder picks its path from the
// extension -- nothing opens a FLAC named .bin -- so this has to be right
// before the file exists, which is why it comes from the server's own `suffix`
// and not from a guess about the MIME type.
//
// With transcoding on it is MP3 whatever the server holds: `suffix` describes
// the file on the server, and what arrives is what the transcoder produced.
// Reading `suffix` in that case wrote an MP3 to "<digest>.flac", and the FLAC
// decoder was handed it -- every quality but Original failed to play.
static const char *extension_for(const subsonic_song_t *song) {
	if (subsonic_get_bitrate() > 0) {
		return "mp3";
	}
	if (!song) {
		return "flac";
	}
	if (song->suffix[0]) {
		return song->suffix;
	}

	// No suffix: fall back to the content type, which every server that omits
	// `suffix` still sends.
	const char *mime = song->content_type;
	if (!mime || !*mime) {
		return "flac";
	}
	if (strstr(mime, "mpeg") || strstr(mime, "mp3")) {
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
	return "flac";
}

// The file's stem: the MD5 of the id. Stable, filesystem-safe whatever the
// server's ids look like, and unique without a table to keep.
static void stem_for(const char *song_id, char *out, size_t size) {
	if (!song_id || !*song_id) {
		if (size) {
			out[0] = '\0';
		}
		return;
	}
	char digest[MD5_HEX_LEN];
	md5_hex(song_id, strlen(song_id), digest);
	snprintf(out, size, "%s", digest);
}

static void path_for(const char *song_id, const char *ext, char *out, size_t size) {
	if (!out || size == 0) {
		return;
	}
	if (!cache_dir[0] || !song_id || !*song_id) {
		out[0] = '\0';
		return;
	}
	char stem[MD5_HEX_LEN];
	stem_for(song_id, stem, sizeof(stem));
	// The directory part is bounded so the file name always fits: a path
	// truncated halfway would silently open (or delete) something elsewhere.
	snprintf(out, size, "%.400s/%s.%s", cache_dir, stem, ext && *ext ? ext : "flac");
}

void subsoniccache_path(const subsonic_song_t *song, char *out, size_t size) {
	if (!song) {
		if (out && size) {
			out[0] = '\0';
		}
		return;
	}
	path_for(song->id, extension_for(song), out, size);
}

// Path of the marker that says a download is incomplete. While a track is
// coming down an empty marker sits next to it, removed only when the download
// finishes. The track is written straight to its final name (the decoder opens
// it while it grows, and a rename partway would swap the file under it), so
// completeness has to be recorded somewhere else.
static void marker_path(const char *path, char *out, size_t size) {
	snprintf(out, size, "%.500s.incompleto", path);
}

bool subsoniccache_has_id(const char *song_id, const char *suffix, char *out, size_t size) {
	char path[512];
	path_for(song_id, suffix && *suffix ? suffix : "flac", path, sizeof(path));
	if (!path[0]) {
		return false;
	}

	char marker[544];
	marker_path(path, marker, sizeof(marker));
	struct stat unused;
	if (stat(marker, &unused) == 0) {
		return false; // interrupted: download it again
	}

	struct stat st;
	if (stat(path, &st) != 0 || st.st_size == 0) {
		return false;
	}
	if (out && size) {
		snprintf(out, size, "%s", path);
	}
	return true;
}

bool subsoniccache_has(const subsonic_song_t *song, char *out, size_t size) {
	if (!song) {
		return false;
	}
	return subsoniccache_has_id(song->id, extension_for(song), out, size);
}

bool subsoniccache_find(const char *song_id, char *out, size_t size) {
	// Every container this cache may have written. A server that transcode-s
	// and one that serves originals can both be behind the same id.
	static const char *const EXTENSIONS[] = {"flac", "mp3", "m4a", "opus", "ogg", "wav", "wv", "dsf", "ape"};
	for (size_t i = 0; i < sizeof(EXTENSIONS) / sizeof(EXTENSIONS[0]); i++) {
		if (subsoniccache_has_id(song_id, EXTENSIONS[i], out, size)) {
			return true;
		}
	}
	if (out && size) {
		out[0] = '\0';
	}
	return false;
}

// mkdir -p over the two levels needed (.local, then the cache directory).
static bool ensure_dir(void) {
	if (!subsoniccache_ready()) {
		return false;
	}

	char path[512];
	snprintf(path, sizeof(path), "%s", cache_dir);
	for (char *p = path + 1; *p; p++) {
		if (*p != '/') {
			continue;
		}
		*p = '\0';
		mkdir(path, 0777);
		*p = '/';
	}
	if (mkdir(path, 0777) != 0 && errno != EEXIST) {
		fprintf(stderr, "subsoniccache: cannot create %s: %s\n", path, strerror(errno));
		return false;
	}
	return true;
}

// ---------------------------------------------------------------------------
// measuring and pruning
// ---------------------------------------------------------------------------

typedef struct {
	char name[96];
	long size;
	long atime; // last access: what tells which tracks are actually listened to
} entry_t;

static int scan(entry_t *out, int max, long *total_out) {
	long total = 0;
	int count = 0;

	DIR *d = opendir(cache_dir);
	if (!d) {
		if (total_out) {
			*total_out = 0;
		}
		return 0;
	}

	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		if (e->d_name[0] == '.') {
			continue;
		}
		char path[640];
		if ((size_t)snprintf(path, sizeof(path), "%s/%s", cache_dir, e->d_name) >= sizeof(path)) {
			continue;
		}
		struct stat st;
		if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
			continue;
		}
		total += st.st_size;

		// What is coming down right now is not prunable, nor are its sidecar
		// files: the comparison is on the name without extension, so
		// "<digest>.flac", "<digest>.flac.tags" and "<digest>.jpg" are all
		// protected while "<digest>.flac" grows.
		char stem[640];
		snprintf(stem, sizeof(stem), "%.500s/%.95s", cache_dir, e->d_name);
		char *dot = strrchr(stem + strlen(cache_dir) + 1, '.');
		if (dot) {
			*dot = '\0';
		}
		if (growfile_prefix_is_growing(stem)) {
			continue;
		}
		if (out && count < max && strlen(e->d_name) < sizeof(out[0].name)) {
			snprintf(out[count].name, sizeof(out[count].name), "%.95s", e->d_name);
			out[count].size = (long)st.st_size;
			out[count].atime = (long)st.st_atime;
			count++;
		}
	}
	closedir(d);

	if (total_out) {
		*total_out = total;
	}
	return count;
}

long subsoniccache_bytes(void) {
	if (!subsoniccache_ready()) {
		return 0;
	}
	long total = 0;
	scan(NULL, 0, &total);
	return total;
}

#define PRUNE_MAX_ENTRIES 512

// Drops the least listened to until the cache fits again. Ordered by last
// access, not creation: an album downloaded a month ago but replayed yesterday
// should stay, one downloaded yesterday and never replayed can go.
static void prune(long keep_room_for) {
	// Borrowed for the length of the pass and given straight back: this runs
	// only when a download is about to overflow the cache, so the memory is not
	// worth holding for the life of the player.
	entry_t *entries = malloc(sizeof(*entries) * PRUNE_MAX_ENTRIES);
	if (!entries) {
		return; // nothing is deleted, and the ceiling is checked again next time
	}

	long total = 0;
	int count = scan(entries, PRUNE_MAX_ENTRIES, &total);
	if (total + keep_room_for <= SUBSONICCACHE_MAX_BYTES) {
		free(entries);
		return;
	}

	// Selection sort: at most 512 entries, and only occasionally.
	for (int i = 0; i < count; i++) {
		int oldest = i;
		for (int k = i + 1; k < count; k++) {
			if (entries[k].atime < entries[oldest].atime) {
				oldest = k;
			}
		}
		entry_t tmp = entries[i];
		entries[i] = entries[oldest];
		entries[oldest] = tmp;
	}

	for (int i = 0; i < count && total + keep_room_for > SUBSONICCACHE_MAX_BYTES; i++) {
		char path[640];
		snprintf(path, sizeof(path), "%.500s/%.95s", cache_dir, entries[i].name);
		if (remove(path) == 0) {
			total -= entries[i].size;
			printf("subsoniccache: removed %s (%ld KB)\n", entries[i].name, entries[i].size / 1024);
		}
	}

	free(entries);
}

static bool is_recent_stem(const char *stem);

// The tracks the queue still has ahead, which the periodic clear must not
// touch.
static pthread_mutex_t protected_lock = PTHREAD_MUTEX_INITIALIZER;
static char protected_paths[SUBSONICCACHE_PROTECTED_MAX][512];
static int protected_count;

void subsoniccache_set_protected(const char *const *paths, int count) {
	pthread_mutex_lock(&protected_lock);
	protected_count = 0;
	for (int i = 0; i < count && protected_count < SUBSONICCACHE_PROTECTED_MAX; i++) {
		if (paths[i] && paths[i][0]) {
			snprintf(protected_paths[protected_count++], sizeof(protected_paths[0]), "%s", paths[i]);
		}
	}
	pthread_mutex_unlock(&protected_lock);
}

// True when `stem` (the name without extension) belongs to one of the protected
// tracks or to one of its sidecar files.
static bool is_protected_stem(const char *stem) {
	size_t len = strlen(stem);
	pthread_mutex_lock(&protected_lock);
	bool match = false;
	for (int i = 0; i < protected_count && !match; i++) {
		match = strncmp(protected_paths[i], stem, len) == 0;
	}
	pthread_mutex_unlock(&protected_lock);
	return match;
}

// `keep_playing` spares what is coming down right now (and its sidecar files).
// The periodic clear needs it because it happens while something is playing; a
// clear the user asked for takes everything.
static void clear_files(bool keep_playing) {
	if (!subsoniccache_ready()) {
		return;
	}
	DIR *d = opendir(cache_dir);
	if (!d) {
		return;
	}
	int removed = 0;
	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		if (e->d_name[0] == '.') {
			continue;
		}
		char path[640];
		if ((size_t)snprintf(path, sizeof(path), "%s/%s", cache_dir, e->d_name) >= sizeof(path)) {
			continue;
		}
		if (keep_playing) {
			char stem[640];
			snprintf(stem, sizeof(stem), "%.500s/%.95s", cache_dir, e->d_name);
			char *dot = strrchr(stem + strlen(cache_dir) + 1, '.');
			if (dot) {
				*dot = '\0';
			}
			if (growfile_prefix_is_growing(stem) || is_protected_stem(stem) || is_recent_stem(stem)) {
				continue;
			}
		}
		if (remove(path) == 0) {
			removed++;
		}
	}
	closedir(d);
	printf("subsoniccache: cleared (%d files)\n", removed);
}

void subsoniccache_clear(void) { clear_files(false); }

// ---------------------------------------------------------------------------
// the periodic clear
//
// The cache is a waypoint, not a library. Keeping everything ever listened to
// fills the user's card with files they did not put there and will never see
// (it lives in a hidden directory), and the one-gigabyte ceiling alone is not
// enough: it is reached and then stays reached.
// ---------------------------------------------------------------------------

static int played_since_clear;

// Ring of the most recently downloaded tracks. Deliberately small: it is a
// safety net for the window between "the file exists" and "the queue knows
// about it", not a second cache.
#define RECENT_MAX 8
static pthread_mutex_t recent_lock = PTHREAD_MUTEX_INITIALIZER;
static char recent_paths[RECENT_MAX][512];
static int recent_next;

static bool is_recent_stem(const char *stem) {
	size_t len = strlen(stem);
	pthread_mutex_lock(&recent_lock);
	bool match = false;
	for (int i = 0; i < RECENT_MAX && !match; i++) {
		match = recent_paths[i][0] && strncmp(recent_paths[i], stem, len) == 0;
	}
	pthread_mutex_unlock(&recent_lock);
	return match;
}

void subsoniccache_note_played(const char *path) {
	if (!subsoniccache_ready()) {
		return;
	}

	if (path && path[0]) {
		pthread_mutex_lock(&recent_lock);
		snprintf(recent_paths[recent_next], sizeof(recent_paths[0]), "%s", path);
		recent_next = (recent_next + 1) % RECENT_MAX;
		pthread_mutex_unlock(&recent_lock);
	}

	if (++played_since_clear >= SUBSONICCACHE_CLEAR_EVERY) {
		played_since_clear = 0;
		clear_files(true);
	}
}

void subsoniccache_clear_on_exit(void) {
	if (!subsoniccache_ready()) {
		return;
	}
	printf("subsoniccache: shutting down, clearing the cache\n");
	clear_files(false);
}

// ---------------------------------------------------------------------------
// sidecar files
// ---------------------------------------------------------------------------

// Reads one key back out of the tags sidecar. Returns false when it is absent.
static bool read_tag(const char *path, const char *key, char *out, size_t size) {
	if (!out || size == 0) {
		return false;
	}
	out[0] = '\0';

	char tags[544];
	snprintf(tags, sizeof(tags), "%.500s.tags", path);
	FILE *f = fopen(tags, "r");
	if (!f) {
		return false;
	}

	size_t key_len = strlen(key);
	char line[512];
	bool found = false;
	while (!found && fgets(line, sizeof(line), f)) {
		if (strncmp(line, key, key_len) != 0 || line[key_len] != '=') {
			continue;
		}
		char *value = line + key_len + 1;
		char *end = value + strlen(value);
		while (end > value && (end[-1] == '\n' || end[-1] == '\r')) {
			*--end = '\0';
		}
		snprintf(out, size, "%s", value);
		found = out[0] != '\0';
	}
	fclose(f);
	return found;
}

bool subsoniccache_album_id(const char *path, char *out, size_t size) {
	if (!subsoniccache_owns(path)) {
		if (out && size) {
			out[0] = '\0';
		}
		return false;
	}
	return read_tag(path, "album_id", out, size);
}

bool subsoniccache_song_id(const char *path, char *out, size_t size) {
	if (!subsoniccache_owns(path)) {
		if (out && size) {
			out[0] = '\0';
		}
		return false;
	}
	return read_tag(path, "song_id", out, size);
}

int subsoniccache_bitrate(const char *path) {
	if (!subsoniccache_owns(path)) {
		return 0;
	}
	char value[16];
	if (!read_tag(path, "bitrate", value, sizeof(value))) {
		return 0;
	}
	return atoi(value);
}

int subsoniccache_duration(const char *path) {
	if (!subsoniccache_owns(path)) {
		return 0;
	}
	char value[16];
	if (!read_tag(path, "duration", value, sizeof(value))) {
		return 0;
	}
	return atoi(value);
}

bool subsoniccache_owns(const char *path) {
	return path && cache_dir[0] && strncmp(path, cache_dir, strlen(cache_dir)) == 0;
}

void subsoniccache_write_sidecars(const subsonic_song_t *song, const char *cover_url) {
	if (!subsoniccache_ready() || !song) {
		return;
	}

	char path[512];
	subsoniccache_path(song, path, sizeof(path));
	if (!path[0]) {
		return;
	}

	char tags[544];
	snprintf(tags, sizeof(tags), "%.500s.tags", path);

	// Atomic write: a temporary file first, then rename(). fopen(.., "w")
	// straight onto the .tags truncates it to zero before writing, and anyone
	// reading the metadata at that instant -- device_state at track start,
	// from another thread -- sees an empty file and falls back to the file
	// name. A rename within the same filesystem is indivisible.
	char tags_tmp[560];
	snprintf(tags_tmp, sizeof(tags_tmp), "%s.tmp", tags);
	FILE *f = fopen(tags_tmp, "w");
	if (f) {
		// Deliberately dumb format: one line per field, key and value separated
		// by an equals sign. metadata.c reads it back in thirty lines and a
		// human can read it without tools.
		fprintf(f, "title=%s\n", song->title);
		fprintf(f, "artist=%s\n", song->artist);
		fprintf(f, "album=%s\n", song->album);
		// Not a tag and never displayed: what "show album" in the player needs
		// to know which album to open. The name is not enough, since different
		// albums share a title.
		fprintf(f, "album_id=%s\n", song->album_id);
		// The id the file is named after only as a digest. Without this a
		// playing file could not be traced back to its song, which is what
		// scrobbling and starring need.
		fprintf(f, "song_id=%s\n", song->id);
		fprintf(f, "track=%d\n", song->track);
		// What the server says the track is. Not something the file can be
		// asked for: it describes the copy on the server, and for a transcoded
		// stream it describes the stream. The details page shows it.
		fprintf(f, "bitrate=%d\n", song->bit_rate);
		// The length as the server knows it. A transcoded stream is a CBR MP3
		// whose Xing header states no frame count, so dr_mp3 can only estimate
		// its length from the bytes that have arrived -- and while the file is
		// still growing that estimate is short, which left the player's bar
		// sitting at its end with the position stuck. The server's figure is
		// the truth and costs one line here.
		fprintf(f, "duration=%d\n", song->duration);
		fprintf(f, "suffix=%s\n", song->suffix);
		fclose(f);
		if (rename(tags_tmp, tags) != 0) {
			remove(tags_tmp);
		}
	}

	if (!cover_url || !*cover_url) {
		return;
	}

	// The cover takes the track's name: the cover loader already looks for
	// "<track name>.jpg" next to the file, so it needs no extra rule.
	char cover[544];
	const char *dot = strrchr(path, '.');
	snprintf(cover, sizeof(cover), "%.*s.jpg", dot ? (int)(dot - path) : (int)strlen(path), path);

	struct stat st;
	if (stat(cover, &st) == 0 && st.st_size > 0) {
		return; // already downloaded
	}

	char *body = NULL;
	size_t len = 0;
	if (!http_get(cover_url, &body, &len, 4 * 1024 * 1024, FETCH_TIMEOUT_SECS) || !body) {
		return;
	}

	FILE *img = fopen(cover, "wb");
	if (img) {
		fwrite(body, 1, len, img);
		fclose(img);
	}
	free(body);
}

// ---------------------------------------------------------------------------
// downloading
//
// A track is written straight to its final name, not to a temporary renamed at
// the end: the decoder opens it while it grows, and a rename partway would swap
// the file under it. Completeness is recorded by an empty marker alongside,
// removed only once everything has arrived.
// ---------------------------------------------------------------------------

typedef struct {
	char song_id[SUBSONIC_ID_MAX];
	char path[512];
	char marker[544];
	http_stream_t stream;
	FILE *f;
	long done;
	long total;
	unsigned generation;
} download_t;

// Changing track bumps this counter; threads downloading something else notice
// at the next chunk and stop, so the bandwidth goes to what is playing now.
static unsigned generation = 1;
static pthread_mutex_t gen_lock = PTHREAD_MUTEX_INITIALIZER;

// ---------------------------------------------------------------------------
// one download at a time
//
// The turn is not Subsonic's, it belongs to the device, which has one network
// and one card to share with Tidal, Qobuz and podcasts as well, so it lives in
// streamturn.c. The full reasoning is there.
// ---------------------------------------------------------------------------

static void busy_acquire(void) { streamturn_acquire(); }
static void busy_release(void) { streamturn_release(); }

static void (*slow_start_cb)(int seconds);

void subsoniccache_set_slow_start_cb(void (*cb)(int seconds)) { slow_start_cb = cb; }

void subsoniccache_wait_idle(void) { streamturn_wait_idle(); }

// The in-progress download's stream, so another thread can jolt it awake. Only
// one runs at a time, so a single pointer suffices. Guarded by its own lock and
// cleared before download_end frees the memory, so the wake never lands on a
// dead object.
static pthread_mutex_t active_lock = PTHREAD_MUTEX_INITIALIZER;
static http_stream_t *active_stream;

// The id of the song coming down right now (empty = none). "Is this track
// already downloading?" is answered from here, by id, not from growfile by
// path.
static char active_id[SUBSONIC_ID_MAX];

bool subsoniccache_downloading_id(char *out, size_t size) {
	pthread_mutex_lock(&active_lock);
	bool any = active_id[0] != '\0';
	if (any && out && size) {
		snprintf(out, size, "%s", active_id);
	}
	pthread_mutex_unlock(&active_lock);
	return any;
}

// See subsoniccache.h. A single bool, written by the UI thread and read by the
// power loop: neither can observe half of it.
static volatile bool network_wanted;

void subsoniccache_set_network_wanted(bool wanted) { network_wanted = wanted; }
bool subsoniccache_network_wanted(void) { return network_wanted; }

void subsoniccache_abandon_all(void) {
	pthread_mutex_lock(&gen_lock);
	generation++;
	pthread_mutex_unlock(&gen_lock);

	// On a real network the counter bump alone is not enough: the downloader
	// only notices at the next chunk, and if the socket is idle the next chunk
	// arrives after the receive timeout. For all that time the abandoned
	// download holds the turn and the track the user just asked for cannot
	// start. Shutting the socket down ends the blocked recv() at once.
	pthread_mutex_lock(&active_lock);
	if (active_stream) {
		http_stream_wake(active_stream);
	}
	pthread_mutex_unlock(&active_lock);
}

static unsigned current_generation(void) {
	pthread_mutex_lock(&gen_lock);
	unsigned value = generation;
	pthread_mutex_unlock(&gen_lock);
	return value;
}

// Stops advertising this track as downloading. Call on every path that exits
// without having downloaded anything: the id is published as soon as the turn
// is taken, so a failed start would leave it lying, and whoever believes it
// waits for a file that is not coming.
static void clear_active(const char *song_id) {
	pthread_mutex_lock(&active_lock);
	if (strcmp(active_id, song_id) == 0) {
		active_id[0] = '\0';
	}
	pthread_mutex_unlock(&active_lock);
}

// Closes everything and records how it ended for readers of the file.
static void download_end(download_t *d, bool ok) {
	// The stream is about to die: take it out of view of whoever might jolt it
	// first, close it second. The other order is a wake on freed memory.
	pthread_mutex_lock(&active_lock);
	if (active_stream == &d->stream) {
		active_stream = NULL;
	}
	if (strcmp(active_id, d->song_id) == 0) {
		active_id[0] = '\0';
	}
	pthread_mutex_unlock(&active_lock);

	if (d->f) {
		if (fclose(d->f) != 0) {
			ok = false;
		}
		d->f = NULL;
	}
	http_stream_close(&d->stream);

	// Less arrived than the server promised: this is not a usable file.
	if (ok && d->total > 0 && d->done != d->total) {
		fprintf(stderr, "subsoniccache: %s cut short at %ld of %ld bytes\n", d->song_id, d->done, d->total);
		ok = false;
	}
	if (ok && d->done == 0) {
		ok = false;
	}

	growfile_finish(d->path, ok);

	if (ok) {
		remove(d->marker); // complete now
		printf("subsoniccache: %s complete (%ld KB)\n", d->song_id, d->done / 1024);
	} else {
		// The marker stays: next time subsoniccache_has() says no and the
		// track is downloaded again instead of playing half of it.
		printf("subsoniccache: %s not completed\n", d->song_id);
	}

	// Release the turn last, not as soon as the stream is closed: while the
	// marker is still there the track counts as incomplete, so anyone waiting
	// for the turn would start re-downloading what has just finished. This call
	// must stay below the remove().
	busy_release();

	free(d);
}

// Writes until `until` bytes are on disk (or, with until == 0, to the end of
// the stream). Returns false when it went wrong.
static long monotonic_ms(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (long)t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

// `deadline_ms` is an absolute monotonic_ms() value, or 0 for no limit. On
// expiry the pump stops without failing: *finished stays false, which is the
// same thing it says for a file that has not all arrived yet, so the caller
// starts on what is there and the rest follows on the download thread.
static bool download_pump(download_t *d, long until, bool *finished, long deadline_ms) {
	char buf[FETCH_CHUNK];
	*finished = false;

	while (d->done < until || until == 0) {
		if (current_generation() != d->generation) {
			return false; // something else is playing now
		}
		if (deadline_ms && d->done > 0 && monotonic_ms() >= deadline_ms) {
			return true; // out of patience, not out of luck: play what is here
		}

		int n = http_stream_read(&d->stream, buf, (int)sizeof(buf));
		if (n < 0) {
			return false;
		}
		if (n == 0) {
			*finished = true;
			return true;
		}
		if (fwrite(buf, 1, (size_t)n, d->f) != (size_t)n) {
			fprintf(stderr, "subsoniccache: write failed (card full?)\n");
			return false;
		}

		// Flush before announcing the progress: without it a reader would go
		// looking on disk for bytes still sitting in the stdio buffer.
		fflush(d->f);
		d->done += n;
		growfile_progress(d->path, d->done);

		if (until == 0 && *finished) {
			break;
		}
	}
	return true;
}

// The rest of the download, after playback has started.
static void *download_thread(void *arg) {
	// Background priority, like every other worker: there is one core, and a
	// thread writing to the card at full priority takes it away from the UI.
	thread_be_background("subsonic download");

	download_t *d = arg;
	bool finished = false;
	bool ok = download_pump(d, 0, &finished, 0);
	download_end(d, ok && finished);
	return NULL;
}

bool subsoniccache_start(const subsonic_song_t *song, char *out, size_t size) {
	if (!out || size == 0) {
		return false;
	}
	out[0] = '\0';
	set_error("");

	if (!song || !song->id[0]) {
		set_error(tr("subsonic_no_song_address"));
		return false;
	}
	if (!subsoniccache_ready()) {
		fprintf(stderr, "subsoniccache: no card to write to\n");
		set_error(tr("download_no_card"));
		return false;
	}

	// Already fully downloaded: no network, no wait.
	if (subsoniccache_has(song, out, size)) {
		return true;
	}

	char url[SUBSONIC_URL_MAX * 2];
	if (!subsonic_stream_url(song, url, sizeof(url))) {
		set_error(tr("subsonic_no_song_address"));
		return false;
	}

	if (!ensure_dir()) {
		set_error(tr("cache_folder_failed"));
		return false;
	}

	download_t *d = calloc(1, sizeof(*d));
	if (!d) {
		set_error(tr("out_of_memory"));
		return false;
	}
	snprintf(d->song_id, sizeof(d->song_id), "%s", song->id);
	subsoniccache_path(song, d->path, sizeof(d->path));
	marker_path(d->path, d->marker, sizeof(d->marker));

	// This track is already coming down, and it is the one playing. Starting
	// again would reopen the file for writing -- that is, truncate it -- under
	// the decoder reading it. Tapping the same track twice means wanting to
	// hear it, not to restart it, so return the path already in use.
	if (growfile_is_growing(d->path)) {
		snprintf(out, size, "%.500s", d->path);
		free(d);
		return true;
	}

	// One at a time: if another download is running, wait for the turn. Not
	// abandoning it from here, tempting as it is: prefetch also calls this
	// function, for the track after the one playing, and abandoning here would
	// mean reading ahead interrupts the download of the track being played.
	// Abandoning is done by whoever received a user command: the page.
	busy_acquire();

	// Take the generation AFTER the wait: a value read before it would be stale
	// by the time the turn arrives, and a track change that happened during the
	// wait would cancel this download the moment it starts.
	d->generation = current_generation();

	// Publish the song id immediately, not once the connection is up: the other
	// publication point sits after http_stream_open(), that is after the full
	// round of DNS, TCP, TLS and request, and until the id is out callers that
	// ask "is this downloading?" get no for a track that very much is.
	pthread_mutex_lock(&active_lock);
	snprintf(active_id, sizeof(active_id), "%s", d->song_id);
	pthread_mutex_unlock(&active_lock);

	if (!http_stream_open(&d->stream, url, FETCH_TIMEOUT_SECS)) {
		const char *why = http_last_error();
		fprintf(stderr, "subsoniccache: %s does not open%s%s\n", d->song_id, why && why[0] ? ": " : "",
				why && why[0] ? why : "");
		set_error(why && why[0] ? why : tr("subsonic_cannot_reach_server"));
		clear_active(d->song_id);
		busy_release();
		free(d);
		return false;
	}

	d->total = d->stream.content_length;
	prune(d->total > 0 ? d->total : 0);

	// The marker before the file: if power is lost between the two lines, an
	// orphan marker (which does nothing) beats an incomplete file that looks
	// good.
	FILE *marker = fopen(d->marker, "wb");
	if (marker) {
		fclose(marker);
	}

	d->f = fopen(d->path, "wb");
	if (!d->f) {
		fprintf(stderr, "subsoniccache: %s does not open for writing: %s\n", d->path, strerror(errno));
		set_error(errno == ENOSPC ? tr("card_full") : tr("card_write_failed"));
		http_stream_close(&d->stream);
		clear_active(d->song_id);
		busy_release();
		free(d);
		return false;
	}

	// From here on this is the download in progress, and an abandon must be
	// able to shut its socket down.
	pthread_mutex_lock(&active_lock);
	active_stream = &d->stream;
	pthread_mutex_unlock(&active_lock);

	growfile_announce(d->path, d->total);

	// --- prebuffer, measured rather than guessed -----------------------------
	//
	// The arithmetic is qobuzcache's, and the reasoning is in streamturn.h: a
	// first chunk is downloaded and timed, which gives the network rate, and
	// with R the rate the track consumes and M the measured one, starting with
	// H bytes in hand keeps the reserve from running out exactly when
	// H >= total * (1 - M/R). Capped in seconds, because the refill in audio.c
	// makes "never stall" unnecessary and a slow network should not mean
	// minutes of silence before the first note.
	long want = STREAM_PREBUFFER;
	if (d->total > 0 && d->total < want) {
		want = d->total;
	}

	long probe = want < STREAM_PROBE ? want : STREAM_PROBE;
	struct timespec t0;
	clock_gettime(CLOCK_MONOTONIC, &t0);

	// One deadline for the whole prebuffer, probe and top-up together: the wait
	// the caller sees is STREAM_START_BUDGET_MS, not a budget per phase.
	long deadline = monotonic_ms() + STREAM_START_BUDGET_MS;

	bool finished = false;
	if (!download_pump(d, probe, &finished, deadline)) {
		set_error(current_generation() != d->generation ? tr("download_cancelled") : tr("download_interrupted"));
		download_end(d, false);
		return false;
	}

	if (!finished && d->total > 0 && song->duration > 0) {
		struct timespec t1;
		clock_gettime(CLOCK_MONOTONIC, &t1);
		double elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;

		// Under a third of a second the measurement is not a measurement: the
		// first chunk can arrive all at once out of the socket buffer and make
		// the network look ten times faster than it is.
		double measured = elapsed > 0.3 ? d->done / elapsed : 0.0;
		double required = (double)d->total / song->duration;

		// The floor: a few seconds of music in hand regardless, even when the
		// network looks very fast.
		if (required > 0) {
			long floor_bytes = (long)(required * STREAM_HEAD_SECS);
			if (floor_bytes > want) {
				want = floor_bytes;
			}
			if (want > d->total) {
				want = d->total;
			}
		}

		if (measured > 0 && required > 0) {
			// A fifth of margin: a measurement over one chunk is noisy, and a
			// wobbling network must not start a track that will stall at once.
			double head = (double)d->total * (1.0 - measured / (required * 1.2));

			// Capped: without a cap, on a network slower than the track this
			// asks for nearly the whole file.
			double cap = required * STREAM_HEAD_MAX_SECS;
			if (head > cap) {
				head = cap;
			}
			if (head > want) {
				want = (long)head;
			}
			if (want > d->total) {
				want = d->total;
			}
			printf("subsoniccache: %s needs %.0f KB/s, the network gives %.0f -> starting with %ld KB of %ld\n",
				   d->song_id, required / 1024, measured / 1024, want / 1024, d->total / 1024);

			// If the wait is measured in seconds, tell the user.
			int wait_secs = (int)((want - d->done) / measured);
			if (wait_secs >= 2 && slow_start_cb) {
				slow_start_cb(wait_secs);
			}
		}
	}

	if (!finished && d->done < want && !download_pump(d, want, &finished, deadline)) {
		set_error(current_generation() != d->generation ? tr("download_cancelled") : tr("download_interrupted"));
		download_end(d, false);
		return false;
	}

	if (finished) {
		// Short track: all of it is already here.
		snprintf(out, size, "%.500s", d->path);
		download_end(d, true);
		return true;
	}

	snprintf(out, size, "%.500s", d->path);

	// Both numbers are taken before starting the thread: from that moment `d`
	// belongs to it, and a short file can finish downloading and free it before
	// any line below could read them.
	long started_done = d->done;
	long started_total = d->total;

	pthread_t thread;
	if (pthread_create(&thread, NULL, download_thread, d) != 0) {
		set_error(tr("cannot_start_the_download"));
		download_end(d, false); // also releases the turn
		out[0] = '\0';
		return false;
	}
	pthread_detach(thread);

	printf("subsoniccache: %s starts at %ld KB of %ld KB\n", d->song_id, started_done / 1024,
		   started_total > 0 ? started_total / 1024 : 0);
	return true;
}
