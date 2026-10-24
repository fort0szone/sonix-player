#include "subsonic.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "src/system/core/config.h"
#include "src/system/core/json.h"
#include "src/system/core/lang.h"
#include "src/system/core/md5.h"
#include "src/system/net/http.h"

// ---------------------------------------------------------------------------
// Subsonic, and everything that speaks its protocol. See subsonic.h for why
// nothing here keeps a password.
//
// Two things about the protocol shape this file:
//
//   * every call is one GET, and the reply is always
//     {"subsonic-response":{"status":"ok"|"failed", ...}}. The wrapper is
//     identical for every endpoint, so api_call() unwraps it once and each
//     function below starts from the payload;
//   * ids are opaque strings, never numbers. Nothing may be parsed with
//     strtol, and the cache names its files by the id's digest rather than the
//     id itself.
// ---------------------------------------------------------------------------

// What the protocol says this client is. `c` is required by every server and
// shows up in their logs and their "now playing" page.
#define SUBSONIC_CLIENT "SonixPlayer"

// The API version asked for. 1.16.1 is what every server implements, and what
// OpenSubsonic builds on.
#define SUBSONIC_API_VERSION "1.16.1"

// How long a reply is waited for, and how much of one is held in memory. A
// five-hundred-album page stays around 200 KB; the cap keeps a runaway reply
// from becoming a memory problem on a device with 64 MB in total.
#define SUBSONIC_TIMEOUT_SECS 20
#define SUBSONIC_BODY_LIMIT (4 * 1024 * 1024)

// The most any single call asks for. Navidrome caps getAlbumList2 at 500 and
// silently shortens anything longer, so asking for more than this buys nothing
// and costs memory in the static result arrays the pages hold.
#define SUBSONIC_PAGE_MAX 500

static char server[SUBSONIC_URL_MAX];
static char user[SUBSONIC_NAME_MAX];
static char salt[16];
static char token[MD5_HEX_LEN];
static int bitrate; // 0 = the file as it is on the server

// Per-thread like http_last_error(), and for the same reason: more than one
// worker can have a request in flight, and they must not overwrite each
// other's result.
static __thread char last_error[256];

const char *subsonic_last_error(void) { return last_error; }

static void set_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void set_error(const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	vsnprintf(last_error, sizeof(last_error), fmt, args);
	va_end(args);
}

static void clear_error(void) { last_error[0] = '\0'; }

// ---------------------------------------------------------------------------
// session
// ---------------------------------------------------------------------------

// Strips trailing slashes and adds a scheme when there is none. "music.local"
// is what people type; "http://music.local" is what http_get() needs.
static void normalise_server(const char *in, char *out, size_t size) {
	if (!in || !*in || size == 0) {
		if (size) {
			out[0] = '\0';
		}
		return;
	}

	const char *scheme = strstr(in, "://");
	if (scheme) {
		snprintf(out, size, "%s", in);
	} else {
		// No scheme: plain http, because a server reached by a bare host name on
		// a home network is almost never carrying a certificate. A user who has
		// one types https:// themselves.
		snprintf(out, size, "http://%s", in);
	}

	// One trailing slash at most, and never past the scheme: "http://" alone is
	// not a server, but truncating it to "http:" would be worse than leaving it.
	size_t len = strlen(out);
	while (len > 8 && out[len - 1] == '/') {
		out[--len] = '\0';
	}
}

void subsonic_init(void) {
	normalise_server(config_get("subsonic", "server", ""), server, sizeof(server));
	snprintf(user, sizeof(user), "%s", config_get("subsonic", "user", ""));
	snprintf(salt, sizeof(salt), "%s", config_get("subsonic", "salt", ""));
	snprintf(token, sizeof(token), "%s", config_get("subsonic", "token", ""));

	bitrate = (int)config_get_int("subsonic", "bitrate", 0);
	if (bitrate < 0 || bitrate > 512) {
		bitrate = 0;
	}

	printf("subsonic: %s, %s\n", subsonic_configured() ? server : "no server",
		   subsonic_logged_in() ? "signed in" : "no session");
}

bool subsonic_configured(void) { return server[0] && user[0]; }

bool subsonic_logged_in(void) { return subsonic_configured() && salt[0] && token[0]; }

const char *subsonic_display_name(void) { return user; }
const char *subsonic_server(void) { return server; }

int subsonic_get_bitrate(void) { return bitrate; }

void subsonic_set_bitrate(int kbps) {
	if (kbps < 0 || kbps > 512) {
		return;
	}
	bitrate = kbps;
	config_set_int("subsonic", "bitrate", bitrate);
	config_save();
}

void subsonic_logout(void) {
	server[0] = '\0';
	user[0] = '\0';
	salt[0] = '\0';
	token[0] = '\0';
	config_set("subsonic", "server", "");
	config_set("subsonic", "user", "");
	config_set("subsonic", "salt", "");
	config_set("subsonic", "token", "");
	config_save();
	printf("subsonic: signed out\n");
}

// Eight hex characters out of /dev/urandom. Not because the protocol needs a
// fresh salt per request -- it accepts any, and a captured request can be
// replayed whatever the salt -- but because a salt that is generated once and
// never repeats across devices keeps one device's digest from being another's.
static void make_salt(char *out, size_t size) {
	unsigned char raw[4] = {0};
	FILE *f = fopen("/dev/urandom", "rb");
	if (f) {
		if (fread(raw, 1, sizeof(raw), f) != sizeof(raw)) {
			raw[0] = raw[1] = raw[2] = raw[3] = 0;
		}
		fclose(f);
	}
	if (raw[0] == 0 && raw[1] == 0 && raw[2] == 0 && raw[3] == 0) {
		// No /dev/urandom (or a bad read): the clock still gives a salt that is
		// different from every other device's, which is all this is for.
		unsigned long mix = (unsigned long)time(NULL) ^ (unsigned long)(size_t)out;
		raw[0] = (unsigned char)(mix >> 24);
		raw[1] = (unsigned char)(mix >> 16);
		raw[2] = (unsigned char)(mix >> 8);
		raw[3] = (unsigned char)mix;
	}
	snprintf(out, size, "%02x%02x%02x%02x", raw[0], raw[1], raw[2], raw[3]);
}

// ---------------------------------------------------------------------------
// building a request
// ---------------------------------------------------------------------------

// Percent-encodes into `out`. Only the unreserved characters survive: a query
// value may hold anything -- a password with an '&' in it, a search for
// "AC/DC", an album called "100%" -- and a raw '&' would end the value early
// and turn the rest into parameters the server did not expect.
static void url_encode(const char *in, char *out, size_t size) {
	static const char HEX[] = "0123456789ABCDEF";
	size_t o = 0;
	if (size == 0) {
		return;
	}
	for (const unsigned char *p = (const unsigned char *)in; p && *p; p++) {
		bool plain = (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-' ||
					 *p == '_' || *p == '.' || *p == '~';
		if (plain) {
			if (o + 1 >= size) {
				break;
			}
			out[o++] = (char)*p;
		} else {
			if (o + 3 >= size) {
				break;
			}
			out[o++] = '%';
			out[o++] = HEX[*p >> 4];
			out[o++] = HEX[*p & 0x0F];
		}
	}
	out[o] = '\0';
}

// Appends to a growing-by-bounds string. Everything below builds a URL of at
// most a few hundred bytes, so a fixed buffer with a checked append is simpler
// than an allocator and cannot leak.
typedef struct {
	char *buf;
	size_t size;
	size_t used;
	bool ok;
} urlbuf_t;

static void ub_init(urlbuf_t *ub, char *buf, size_t size) {
	ub->buf = buf;
	ub->size = size;
	ub->used = 0;
	ub->ok = size > 0;
	if (size) {
		buf[0] = '\0';
	}
}

static void ub_add(urlbuf_t *ub, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void ub_add(urlbuf_t *ub, const char *fmt, ...) {
	if (!ub->ok) {
		return;
	}
	va_list args;
	va_start(args, fmt);
	int n = vsnprintf(ub->buf + ub->used, ub->size - ub->used, fmt, args);
	va_end(args);
	if (n < 0 || (size_t)n >= ub->size - ub->used) {
		ub->ok = false; // truncated: the URL would be wrong, so it is not used
		return;
	}
	ub->used += (size_t)n;
}

// The part every call carries: who is asking, as what, in which format, and the
// signature. The salt and digest are already hex, so they go in as they are.
static void ub_auth(urlbuf_t *ub) {
	char enc_user[SUBSONIC_NAME_MAX * 3 + 1];
	url_encode(user, enc_user, sizeof(enc_user));
	ub_add(ub, "&u=%s&t=%s&s=%s&v=%s&c=%s&f=json", enc_user, token, salt, SUBSONIC_API_VERSION, SUBSONIC_CLIENT);
}

// Opens a URL for `method` and parses the reply, with the protocol's own
// status/error wrapper checked. On success the caller owns *body and must free
// it after it is done with *doc: the document points into the text instead of
// copying it.
static bool api_call(const char *method, const char *params, char **body, json_doc_t *doc) {
	clear_error();
	*body = NULL;

	if (!subsonic_configured()) {
		set_error("%s", tr("subsonic_not_configured"));
		return false;
	}
	if (!salt[0] || !token[0]) {
		set_error("%s", tr("subsonic_sign_in_first"));
		return false;
	}

	char url[SUBSONIC_URL_MAX * 2];
	urlbuf_t ub;
	ub_init(&ub, url, sizeof(url));
	ub_add(&ub, "%s/rest/%s.view?", server, method);
	if (params && *params) {
		ub_add(&ub, "%s", params);
	}
	ub_auth(&ub);
	if (!ub.ok) {
		set_error("%s", tr("subsonic_bad_address"));
		return false;
	}

	int status = 0;
	size_t len = 0;
	if (!http_get_ex(url, NULL, true, body, &len, SUBSONIC_BODY_LIMIT, SUBSONIC_TIMEOUT_SECS, &status)) {
		const char *why = http_last_error();
		set_error("%s", why && why[0] ? why : tr("subsonic_no_reply"));
		return false;
	}

	if (!json_parse(*body, doc)) {
		free(*body);
		*body = NULL;
		set_error("%s", tr("subsonic_unreadable_reply"));
		return false;
	}

	// The wrapper is the same for every endpoint, including the failures: a
	// 401 with a JSON body says which of "wrong password", "no such user" or
	// "the server is down" happened, and that sentence is worth far more than
	// the status code.
	int resp = json_path(doc, json_root(doc), "subsonic-response");
	if (resp < 0) {
		json_free(doc);
		free(*body);
		*body = NULL;
		// Something answered, but not the protocol: a proxy's login page, or a
		// server under another path.
		set_error("%s", tr("subsonic_not_a_subsonic_server"));
		return false;
	}

	char state[16] = "";
	json_obj_str(doc, resp, "status", state, sizeof(state));
	if (strcmp(state, "ok") != 0) {
		char message[200] = "";
		int err = json_get(doc, resp, "error");
		json_obj_str(doc, err, "message", message, sizeof(message));
		if (message[0]) {
			set_error("%s", message);
		} else {
			set_error(tr("subsonic_answered_with"), status);
		}
		json_free(doc);
		free(*body);
		*body = NULL;
		return false;
	}

	return true;
}

static void api_done(char *body, json_doc_t *doc) {
	json_free(doc);
	free(body);
}

// The payload of a reply: the object inside "subsonic-response". -1 when it is
// missing, which api_call() has already rejected.
static int payload(const json_doc_t *doc) { return json_path(doc, json_root(doc), "subsonic-response"); }

// ---------------------------------------------------------------------------
// reading the shapes
//
// Each of these takes one element of a list and copies the fields out. Every
// field is optional in the protocol -- a server may leave out coverArt, year,
// bitRate or track, and several do -- so nothing is required except an id.
// ---------------------------------------------------------------------------

static void read_song(const json_doc_t *doc, int tok, subsonic_song_t *out) {
	memset(out, 0, sizeof(*out));
	json_obj_str(doc, tok, "id", out->id, sizeof(out->id));
	json_obj_str(doc, tok, "title", out->title, sizeof(out->title));
	json_obj_str(doc, tok, "artist", out->artist, sizeof(out->artist));
	json_obj_str(doc, tok, "album", out->album, sizeof(out->album));
	json_obj_str(doc, tok, "albumId", out->album_id, sizeof(out->album_id));
	json_obj_str(doc, tok, "coverArt", out->cover_id, sizeof(out->cover_id));
	json_obj_str(doc, tok, "suffix", out->suffix, sizeof(out->suffix));
	json_obj_str(doc, tok, "contentType", out->content_type, sizeof(out->content_type));
	out->duration = (int)json_obj_long(doc, tok, "duration", 0);
	out->track = (int)json_obj_long(doc, tok, "track", 0);
	out->bit_rate = (int)json_obj_long(doc, tok, "bitRate", 0);
	out->size = (long)json_obj_llong(doc, tok, "size", 0);
	out->starred = json_get(doc, tok, "starred") >= 0;

	// Some servers leave the artist out of a song and only name the album's.
	// An empty artist is a row with a blank second line, which reads as a bug;
	// the album name is a better lie than nothing.
	if (!out->artist[0]) {
		snprintf(out->artist, sizeof(out->artist), "%s", out->album);
	}
	if (!out->title[0]) {
		snprintf(out->title, sizeof(out->title), "%s", out->id);
	}
}

static void read_album(const json_doc_t *doc, int tok, subsonic_album_t *out) {
	memset(out, 0, sizeof(*out));
	json_obj_str(doc, tok, "id", out->id, sizeof(out->id));
	json_obj_str(doc, tok, "name", out->name, sizeof(out->name));
	json_obj_str(doc, tok, "artist", out->artist, sizeof(out->artist));
	json_obj_str(doc, tok, "coverArt", out->cover_id, sizeof(out->cover_id));
	json_obj_str(doc, tok, "year", out->year, sizeof(out->year));
	out->song_count = (int)json_obj_long(doc, tok, "songCount", 0);
	out->duration = (int)json_obj_long(doc, tok, "duration", 0);
	out->starred = json_get(doc, tok, "starred") >= 0;
	if (!out->name[0]) {
		snprintf(out->name, sizeof(out->name), "%s", out->id);
	}
}

static void read_artist(const json_doc_t *doc, int tok, subsonic_artist_t *out) {
	memset(out, 0, sizeof(*out));
	json_obj_str(doc, tok, "id", out->id, sizeof(out->id));
	json_obj_str(doc, tok, "name", out->name, sizeof(out->name));
	json_obj_str(doc, tok, "coverArt", out->cover_id, sizeof(out->cover_id));
	out->album_count = (int)json_obj_long(doc, tok, "albumCount", 0);
	if (!out->name[0]) {
		snprintf(out->name, sizeof(out->name), "%s", out->id);
	}
}

static void read_playlist(const json_doc_t *doc, int tok, subsonic_playlist_t *out) {
	memset(out, 0, sizeof(*out));
	json_obj_str(doc, tok, "id", out->id, sizeof(out->id));
	json_obj_str(doc, tok, "name", out->name, sizeof(out->name));
	json_obj_str(doc, tok, "owner", out->owner, sizeof(out->owner));
	json_obj_str(doc, tok, "coverArt", out->cover_id, sizeof(out->cover_id));
	out->song_count = (int)json_obj_long(doc, tok, "songCount", 0);
	out->duration = (int)json_obj_long(doc, tok, "duration", 0);
	if (!out->name[0]) {
		snprintf(out->name, sizeof(out->name), "%s", out->id);
	}
}

// Copies up to `max` elements of the array at `arr` (which may be -1: the
// protocol omits an empty list rather than sending []). Returns the count.
//
// The reader is written with a void* so that one loop serves all four kinds;
// the wrappers below are what give it back its type, which is cheaper to read
// than a cast at every call site and cannot go wrong silently.
typedef void (*element_reader_t)(const json_doc_t *, int, void *);

static void read_album_v(const json_doc_t *doc, int tok, void *out) { read_album(doc, tok, (subsonic_album_t *)out); }
static void read_artist_v(const json_doc_t *doc, int tok, void *out) {
	read_artist(doc, tok, (subsonic_artist_t *)out);
}
static void read_playlist_v(const json_doc_t *doc, int tok, void *out) {
	read_playlist(doc, tok, (subsonic_playlist_t *)out);
}

static int copy_array(const json_doc_t *doc, int arr, int max, element_reader_t read, size_t elem_size, void *out) {
	if (arr < 0 || max <= 0) {
		return 0;
	}
	int n = json_len(doc, arr);
	if (n > max) {
		n = max;
	}
	for (int i = 0; i < n; i++) {
		int tok = json_at(doc, arr, i);
		if (tok >= 0) {
			read(doc, tok, (char *)out + (size_t)i * elem_size);
		}
	}
	return n;
}

// The same for a list of songs, which has no paging of its own (an album, a
// playlist): every entry the server sent, up to `max`.
static int read_songs(const json_doc_t *doc, int arr, subsonic_song_t *out, int max) {
	if (arr < 0 || max <= 0) {
		return 0;
	}
	int n = json_len(doc, arr);
	if (n > max) {
		n = max;
	}
	for (int i = 0; i < n; i++) {
		int tok = json_at(doc, arr, i);
		if (tok >= 0) {
			read_song(doc, tok, &out[i]);
		}
	}
	return n;
}

// ---------------------------------------------------------------------------
// album lists
// ---------------------------------------------------------------------------

int subsonic_album_list(const char *type, int offset, int size, subsonic_album_t *out, int max) {
	if (!type || !*type) {
		type = "newest";
	}
	if (size <= 0 || size > SUBSONIC_PAGE_MAX) {
		size = 100;
	}
	if (offset < 0) {
		offset = 0;
	}

	char params[160];
	snprintf(params, sizeof(params), "type=%s&size=%d&offset=%d", type, size, offset);

	char *body = NULL;
	json_doc_t doc;
	if (!api_call("getAlbumList2", params, &body, &doc)) {
		return -1;
	}
	int n = copy_array(&doc, json_path(&doc, payload(&doc), "albumList2.album"), max,
					   read_album_v, sizeof(subsonic_album_t), out);
	api_done(body, &doc);
	return n;
}

int subsonic_starred_albums(int offset, int size, subsonic_album_t *out, int max) {
	(void)offset;
	(void)size;
	char *body = NULL;
	json_doc_t doc;
	if (!api_call("getStarred2", NULL, &body, &doc)) {
		return -1;
	}
	int n = copy_array(&doc, json_path(&doc, payload(&doc), "starred2.album"), max,
					   read_album_v, sizeof(subsonic_album_t), out);
	api_done(body, &doc);
	return n;
}

int subsonic_starred_songs(int offset, int size, subsonic_song_t *out, int max) {
	(void)offset;
	(void)size;
	char *body = NULL;
	json_doc_t doc;
	if (!api_call("getStarred2", NULL, &body, &doc)) {
		return -1;
	}
	int n = read_songs(&doc, json_path(&doc, payload(&doc), "starred2.song"), out, max);
	api_done(body, &doc);
	return n;
}

// ---------------------------------------------------------------------------
// artists and albums
// ---------------------------------------------------------------------------

int subsonic_artists(subsonic_artist_t *out, int max) {
	char *body = NULL;
	json_doc_t doc;
	if (!api_call("getArtists", NULL, &body, &doc)) {
		return -1;
	}

	// The reply is indexed by initial letter:
	//   {"artists":{"index":[{"name":"A","artist":[...]}, {"name":"B",...}]}}
	// so the artists are one level deeper than a plain list and have to be
	// gathered index by index. An index with no artist array is a letter with
	// nothing under it and contributes nothing.
	int root = payload(&doc);
	int indexes = json_path(&doc, root, "artists.index");
	int count = 0;
	int n = indexes >= 0 ? json_len(&doc, indexes) : 0;
	for (int i = 0; i < n && count < max; i++) {
		int index = json_at(&doc, indexes, i);
		int arr = json_get(&doc, index, "artist");
		count += copy_array(&doc, arr, max - count, read_artist_v, sizeof(subsonic_artist_t), &out[count]);
	}

	api_done(body, &doc);
	return count;
}

int subsonic_artist_albums(const char *artist_id, subsonic_album_t *out, int max) {
	if (!artist_id || !*artist_id) {
		return 0;
	}
	char enc[SUBSONIC_ID_MAX * 3 + 1];
	url_encode(artist_id, enc, sizeof(enc));
	char params[256];
	snprintf(params, sizeof(params), "id=%s", enc);

	char *body = NULL;
	json_doc_t doc;
	if (!api_call("getArtist", params, &body, &doc)) {
		return -1;
	}
	int n = copy_array(&doc, json_path(&doc, payload(&doc), "artist.album"), max,
					   read_album_v, sizeof(subsonic_album_t), out);
	api_done(body, &doc);
	return n;
}

int subsonic_album_songs(const char *album_id, subsonic_song_t *out, int max) {
	if (!album_id || !*album_id) {
		return 0;
	}
	char enc[SUBSONIC_ID_MAX * 3 + 1];
	url_encode(album_id, enc, sizeof(enc));
	char params[256];
	snprintf(params, sizeof(params), "id=%s", enc);

	char *body = NULL;
	json_doc_t doc;
	if (!api_call("getAlbum", params, &body, &doc)) {
		return -1;
	}
	int n = read_songs(&doc, json_path(&doc, payload(&doc), "album.song"), out, max);
	api_done(body, &doc);
	return n;
}

// ---------------------------------------------------------------------------
// playlists
// ---------------------------------------------------------------------------

int subsonic_playlists(subsonic_playlist_t *out, int max) {
	char *body = NULL;
	json_doc_t doc;
	if (!api_call("getPlaylists", NULL, &body, &doc)) {
		return -1;
	}
	int n = copy_array(&doc, json_path(&doc, payload(&doc), "playlists.playlist"), max,
					   read_playlist_v, sizeof(subsonic_playlist_t), out);
	api_done(body, &doc);
	return n;
}

int subsonic_playlist_songs(const char *playlist_id, int offset, subsonic_song_t *out, int max) {
	if (!playlist_id || !*playlist_id) {
		return 0;
	}
	char enc[SUBSONIC_ID_MAX * 3 + 1];
	url_encode(playlist_id, enc, sizeof(enc));
	char params[256];
	snprintf(params, sizeof(params), "id=%s", enc);

	char *body = NULL;
	json_doc_t doc;
	if (!api_call("getPlaylist", params, &body, &doc)) {
		return -1;
	}

	// getPlaylist answers every entry at once. `offset` is applied here rather
	// than asked for: the protocol has no paging on this endpoint, and a
	// playlist is bounded by what a person put in it.
	int arr = json_path(&doc, payload(&doc), "playlist.entry");
	int total = arr >= 0 ? json_len(&doc, arr) : 0;
	if (offset < 0) {
		offset = 0;
	}
	int n = 0;
	for (int i = offset; i < total && n < max; i++) {
		int tok = json_at(&doc, arr, i);
		if (tok >= 0) {
			read_song(&doc, tok, &out[n++]);
		}
	}

	api_done(body, &doc);
	return n;
}

// ---------------------------------------------------------------------------
// search
// ---------------------------------------------------------------------------

// search3 searches all three kinds in one reply. Each of the three entry points
// below therefore asks for nothing but its own kind, by asking for zero of the
// others: the reply is then a third of the size, which on a slow link is the
// difference between a search that feels instant and one that does not.
static bool search(const char *query, int offset, int size, int songs, int albums, int artists, json_doc_t *doc,
				   char **body) {
	if (!query || !*query) {
		return false;
	}
	if (size <= 0 || size > SUBSONIC_PAGE_MAX) {
		size = 50;
	}
	if (offset < 0) {
		offset = 0;
	}

	char enc[512];
	url_encode(query, enc, sizeof(enc));

	char params[768];
	snprintf(params, sizeof(params),
			 "query=%s&songCount=%d&songOffset=%d&albumCount=%d&albumOffset=%d&artistCount=%d&artistOffset=%d", enc,
			 songs, songs ? offset : 0, albums, albums ? offset : 0, artists, artists ? offset : 0);

	return api_call("search3", params, body, doc);
}

int subsonic_search_songs(const char *query, int offset, int size, subsonic_song_t *out, int max) {
	char *body = NULL;
	json_doc_t doc;
	if (!search(query, offset, size, size, 0, 0, &doc, &body)) {
		return -1;
	}
	int n = read_songs(&doc, json_path(&doc, payload(&doc), "searchResult3.song"), out, max);
	api_done(body, &doc);
	return n;
}

int subsonic_search_albums(const char *query, int offset, int size, subsonic_album_t *out, int max) {
	char *body = NULL;
	json_doc_t doc;
	if (!search(query, offset, size, 0, size, 0, &doc, &body)) {
		return -1;
	}
	int n = copy_array(&doc, json_path(&doc, payload(&doc), "searchResult3.album"), max,
					   read_album_v, sizeof(subsonic_album_t), out);
	api_done(body, &doc);
	return n;
}

int subsonic_search_artists(const char *query, int offset, int size, subsonic_artist_t *out, int max) {
	char *body = NULL;
	json_doc_t doc;
	if (!search(query, offset, size, 0, 0, size, &doc, &body)) {
		return -1;
	}
	int n = copy_array(&doc, json_path(&doc, payload(&doc), "searchResult3.artist"), max,
					   read_artist_v, sizeof(subsonic_artist_t), out);
	api_done(body, &doc);
	return n;
}

// ---------------------------------------------------------------------------
// URLs for the audio and the pictures
//
// Both are plain GETs like everything else, and both are signed the same way.
// They are built here rather than in the cache so that the signature has one
// implementation: a URL assembled anywhere else would eventually be assembled
// without it, and a 401 from a stream endpoint is a hard failure to read.
// ---------------------------------------------------------------------------

static const char *subsonic_mime_for_suffix(const char *suffix) {
	if (!suffix || !*suffix) {
		return "audio/mpeg";
	}
	if (!strcasecmp(suffix, "flac")) {
		return "audio/flac";
	}
	if (!strcasecmp(suffix, "mp3")) {
		return "audio/mpeg";
	}
	if (!strcasecmp(suffix, "m4a") || !strcasecmp(suffix, "mp4") || !strcasecmp(suffix, "aac") ||
		!strcasecmp(suffix, "alac")) {
		return "audio/mp4";
	}
	if (!strcasecmp(suffix, "ogg") || !strcasecmp(suffix, "oga") || !strcasecmp(suffix, "opus")) {
		return "audio/ogg";
	}
	if (!strcasecmp(suffix, "wav")) {
		return "audio/wav";
	}
	if (!strcasecmp(suffix, "wv")) {
		return "audio/x-wavpack";
	}
	if (!strcasecmp(suffix, "dsf") || !strcasecmp(suffix, "dff")) {
		return "audio/dsd";
	}
	if (!strcasecmp(suffix, "ape")) {
		return "audio/x-ape";
	}
	return "audio/mpeg";
}

void subsonic_expected_mime(const subsonic_song_t *song, char *out, size_t size) {
	if (!out || size == 0) {
		return;
	}
	if (bitrate > 0) {
		// Transcoded: whatever the source was, what arrives is MP3.
		snprintf(out, size, "audio/mpeg");
		return;
	}
	if (song && song->content_type[0]) {
		snprintf(out, size, "%s", song->content_type);
		return;
	}
	snprintf(out, size, "%s", subsonic_mime_for_suffix(song ? song->suffix : NULL));
}

bool subsonic_stream_url(const subsonic_song_t *song, char *url, size_t size) {
	if (!song || !song->id[0]) {
		return false;
	}

	char enc[SUBSONIC_ID_MAX * 3 + 1];
	url_encode(song->id, enc, sizeof(enc));

	urlbuf_t ub;
	ub_init(&ub, url, size);
	ub_add(&ub, "%s/rest/stream.view?id=%s", server, enc);
	if (bitrate > 0) {
		// `format=mp3` is the one transcode every server implements; the bit
		// rate caps it, and the server picks the closest it can do.
		ub_add(&ub, "&format=mp3&maxBitRate=%d", bitrate);
	} else {
		ub_add(&ub, "&format=raw");
	}
	ub_auth(&ub);
	return ub.ok;
}

bool subsonic_cover_url(const char *cover_id, int pixels, char *url, size_t size) {
	if (!cover_id || !*cover_id) {
		return false;
	}
	if (pixels <= 0) {
		pixels = 300;
	}

	char enc[SUBSONIC_ID_MAX * 3 + 1];
	url_encode(cover_id, enc, sizeof(enc));

	urlbuf_t ub;
	ub_init(&ub, url, size);
	ub_add(&ub, "%s/rest/getCoverArt.view?id=%s&size=%d", server, enc, pixels);
	ub_auth(&ub);
	return ub.ok;
}

// ---------------------------------------------------------------------------
// writing back
// ---------------------------------------------------------------------------

bool subsonic_scrobble(const char *song_id, bool submission) {
	if (!song_id || !*song_id) {
		return false;
	}
	char enc[SUBSONIC_ID_MAX * 3 + 1];
	url_encode(song_id, enc, sizeof(enc));
	char params[256];
	// Milliseconds since the epoch, which is 1.7e12 and does not fit a 32-bit
	// long -- the target's long is 32 bits and would wrap, scrobbling a time in
	// the distant past. 64-bit here, whatever the ABI.
	snprintf(params, sizeof(params), "id=%s&submission=%s&time=%lld", enc, submission ? "true" : "false",
			 (long long)time(NULL) * 1000LL);

	char *body = NULL;
	json_doc_t doc;
	bool ok = api_call("scrobble", params, &body, &doc);
	if (ok) {
		api_done(body, &doc);
	}
	return ok;
}

bool subsonic_star(const char *item_id, bool star) {
	if (!item_id || !*item_id) {
		return false;
	}
	char enc[SUBSONIC_ID_MAX * 3 + 1];
	url_encode(item_id, enc, sizeof(enc));
	char params[256];
	snprintf(params, sizeof(params), "id=%s", enc);

	char *body = NULL;
	json_doc_t doc;
	bool ok = api_call(star ? "star" : "unstar", params, &body, &doc);
	if (ok) {
		api_done(body, &doc);
	}
	return ok;
}

// ---------------------------------------------------------------------------
// sign-in
// ---------------------------------------------------------------------------

bool subsonic_ping(void) {
	char *body = NULL;
	json_doc_t doc;
	bool ok = api_call("ping", NULL, &body, &doc);
	if (ok) {
		api_done(body, &doc);
	}
	return ok;
}

bool subsonic_login(const char *new_server, const char *new_user, const char *password) {
	clear_error();

	if (!new_server || !*new_server || !new_user || !*new_user || !password || !*password) {
		set_error("%s", tr("credentials_required"));
		return false;
	}

	// Built aside and only installed once the server has answered: a wrong
	// address or a wrong password must leave a working session exactly as it
	// was, not replace it with a broken one.
	char candidate_server[SUBSONIC_URL_MAX];
	char candidate_salt[16];
	char candidate_token[MD5_HEX_LEN];

	normalise_server(new_server, candidate_server, sizeof(candidate_server));
	if (!candidate_server[0]) {
		set_error("%s", tr("subsonic_bad_address"));
		return false;
	}
	make_salt(candidate_salt, sizeof(candidate_salt));

	char signed_text[512];
	snprintf(signed_text, sizeof(signed_text), "%s%s", password, candidate_salt);
	md5_hex(signed_text, strlen(signed_text), candidate_token);

	char saved_server[SUBSONIC_URL_MAX];
	char saved_user[SUBSONIC_NAME_MAX];
	char saved_salt[16];
	char saved_token[MD5_HEX_LEN];
	memcpy(saved_server, server, sizeof(saved_server));
	memcpy(saved_user, user, sizeof(saved_user));
	memcpy(saved_salt, salt, sizeof(saved_salt));
	memcpy(saved_token, token, sizeof(saved_token));

	snprintf(server, sizeof(server), "%s", candidate_server);
	snprintf(user, sizeof(user), "%s", new_user);
	snprintf(salt, sizeof(salt), "%s", candidate_salt);
	snprintf(token, sizeof(token), "%s", candidate_token);

	if (!subsonic_ping()) {
		// Put back what was there. last_error already says why the ping failed,
		// and it is the sentence the login page shows.
		memcpy(server, saved_server, sizeof(server));
		memcpy(user, saved_user, sizeof(user));
		memcpy(salt, saved_salt, sizeof(salt));
		memcpy(token, saved_token, sizeof(token));
		return false;
	}

	config_set("subsonic", "server", server);
	config_set("subsonic", "user", user);
	config_set("subsonic", "salt", salt);
	config_set("subsonic", "token", token);
	config_save();

	// The digest, never the password. This line is the whole point of the
	// salt: what is on the card is not a credential anyone can log in with
	// elsewhere.
	printf("subsonic: signed in to %s as %s\n", server, user);
	return true;
}
