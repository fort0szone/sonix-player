#ifndef SUBSONIC_H
#define SUBSONIC_H

#include <stdbool.h>
#include <stddef.h>

// Subsonic, and everything that speaks its protocol: Navidrome, Airsonic,
// Gonic, Ampache, LMS, Funkwhale, Supysonic, Astiga...
//
// The API is a single HTTP endpoint per call under /rest/, answering either XML
// or JSON. This asks for JSON (`f=json`) and reads it with json.h, exactly as
// the Qobuz client does.
//
// Authentication
// --------------
// The protocol's own scheme, not a session token: every request carries
// `u` (user), `s` (a salt) and `t` = md5(password + salt). There is no token to
// keep and no login call -- which is why the password would be the natural
// thing to store, and why it is NOT stored here.
//
// Instead the salt is generated once, at sign-in, and kept with the digest it
// produced. The pair signs every later request and is all an attacker reading
// device_config.ini would find; the password itself is never written anywhere.
// Changing the password on the server invalidates the pair, and the player asks
// for the password once more -- the same behaviour as Qobuz's expired token.
//
// OpenSubsonic adds an `apiKey` parameter as an alternative. It is not used
// here: Navidrome, and every server that implements the extension, still
// accepts `u`/`t`/`s`, so one path covers all of them.
//
// Everything that talks to the network blocks. Call from a worker thread, never
// from the GUI thread -- the same rule as http.h, qobuz.h and radio.c.

// ---------------------------------------------------------------------------
// Sizes. Deliberately the same numbers as qobuz.h: the lists hold the same kind
// of thing, and a page that holds two services' entries in the same static
// array must not need the larger of two sets of limits.
// ---------------------------------------------------------------------------

#define SUBSONIC_TITLE_MAX 200
#define SUBSONIC_NAME_MAX 128
#define SUBSONIC_URL_MAX 512

// Subsonic item ids are opaque strings: Navidrome sends UUIDs, Airsonic sends
// "al-123", others send numbers. Nothing may assume they fit a long, which is
// why they are strings here and why the cache names files by their digest.
#define SUBSONIC_ID_MAX 64

// The container of a song, as the server reports it in `suffix` ("flac", "mp3",
// "m4a"...). It is what decides the extension of the file the cache writes, and
// therefore what the decoder is asked to open.
#define SUBSONIC_SUFFIX_MAX 12

// ---------------------------------------------------------------------------
// What the lists show
// ---------------------------------------------------------------------------

typedef struct {
	char id[SUBSONIC_ID_MAX];
	char title[SUBSONIC_TITLE_MAX];
	char artist[SUBSONIC_NAME_MAX];
	char album[SUBSONIC_TITLE_MAX];
	char album_id[SUBSONIC_ID_MAX]; // to reopen the album the song comes from
	char cover_id[SUBSONIC_ID_MAX]; // "" when the server has no picture
	char suffix[SUBSONIC_SUFFIX_MAX];
	char content_type[48]; // "audio/flac" and the like, as the server says
	int duration;		   // seconds
	int track;			   // track number, 0 when unknown
	int bit_rate;		   // kbps, 0 when unknown
	long size;			   // bytes, 0 when unknown
	bool starred;
} subsonic_song_t;

typedef struct {
	char id[SUBSONIC_ID_MAX];
	char name[SUBSONIC_TITLE_MAX];
	char artist[SUBSONIC_NAME_MAX];
	char cover_id[SUBSONIC_ID_MAX];
	char year[8]; // text: servers send 1997, 1997-04-01 or nothing at all
	int song_count;
	int duration;
	bool starred;
} subsonic_album_t;

typedef struct {
	char id[SUBSONIC_ID_MAX];
	char name[SUBSONIC_NAME_MAX];
	char cover_id[SUBSONIC_ID_MAX];
	int album_count;
} subsonic_artist_t;

typedef struct {
	char id[SUBSONIC_ID_MAX];
	char name[SUBSONIC_TITLE_MAX];
	char owner[SUBSONIC_NAME_MAX];
	char cover_id[SUBSONIC_ID_MAX];
	int song_count;
	int duration;
} subsonic_playlist_t;

// ---------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------

// Reads the saved server, user and signed salt from the configuration. Once at
// startup, after config_init().
void subsonic_init(void);

// False until a server has been saved: the service is not broken, it is
// unconfigured, and the menu entry must say so rather than try and fail.
bool subsonic_configured(void);

bool subsonic_logged_in(void);

// The account name, for the settings page. Empty when signed out.
const char *subsonic_display_name(void);

// The server address as typed, without a trailing slash. Empty when unset.
const char *subsonic_server(void);

// Blocks. Signs in with `password` against `server`, and only stores anything
// once the server has answered a ping with it: a wrong address or a wrong
// password leaves the previous session exactly as it was.
//
// `server` may be given with or without a scheme and with or without a trailing
// slash: "music.example.com", "http://192.168.1.10:4533" and
// "https://x/navidrome/" all work.
bool subsonic_login(const char *server, const char *user, const char *password);

// Forgets the server, the user and the signed salt.
void subsonic_logout(void);

// Why the last call failed, already in a form fit to display. Empty on success.
// Per thread, like http_last_error().
const char *subsonic_last_error(void);

// Whether the server answers. Blocks.
bool subsonic_ping(void);

// ---------------------------------------------------------------------------
// What is streamed
//
// 0 keeps the file as it is on the server. Anything else asks the server to
// transcode to MP3 at that bit rate, which is what makes a large FLAC library
// usable over a weak Wi-Fi link.
// ---------------------------------------------------------------------------

int subsonic_get_bitrate(void);
void subsonic_set_bitrate(int kbps);

// The MIME type a song will arrive as at the selected quality, which is what
// lets the queue name a file before it has been requested. It is the server's
// own `contentType` at 0, and "audio/mpeg" otherwise.
void subsonic_expected_mime(const subsonic_song_t *song, char *out, size_t size);

// ---------------------------------------------------------------------------
// Requests. All return how many items they wrote, or -1 on error
// (`subsonic_last_error()` says what happened). All block.
//
// The servers differ in how much they will send at once -- Navidrome caps
// getAlbumList2 at 500 entries, some builds cap it lower -- so `offset`/`size`
// are always given, and a short page means the end of the list.
// ---------------------------------------------------------------------------

// `type` is the protocol's own: "newest", "recent", "frequent", "random",
// "alphabeticalByName", "alphabeticalByArtist", "starred", "highest".
int subsonic_album_list(const char *type, int offset, int size, subsonic_album_t *out, int max);

// The whole artist list. The protocol has no paging for it: getArtists answers
// every artist at once, indexed by initial letter, and Navidrome sends all of
// them however many there are.
int subsonic_artists(subsonic_artist_t *out, int max);

int subsonic_artist_albums(const char *artist_id, subsonic_album_t *out, int max);

// A whole album's songs. getAlbum has no paging either -- one call, every
// track, which for an album is exactly right.
int subsonic_album_songs(const char *album_id, subsonic_song_t *out, int max);

int subsonic_playlists(subsonic_playlist_t *out, int max);
int subsonic_playlist_songs(const char *playlist_id, int offset, subsonic_song_t *out, int max);

int subsonic_starred_albums(int offset, int size, subsonic_album_t *out, int max);
int subsonic_starred_songs(int offset, int size, subsonic_song_t *out, int max);

// search3: the protocol's own, which searches artists, albums and songs in one
// call. Three entry points so that a caller asks for what it is going to show.
int subsonic_search_songs(const char *query, int offset, int size, subsonic_song_t *out, int max);
int subsonic_search_albums(const char *query, int offset, int size, subsonic_album_t *out, int max);
int subsonic_search_artists(const char *query, int offset, int size, subsonic_artist_t *out, int max);

// The URL the audio comes from, signed like every other request. `format=raw`
// unless a bit rate was chosen.
bool subsonic_stream_url(const subsonic_song_t *song, char *url, size_t size);

// The URL of a cover picture, square, `pixels` across. False when `cover_id` is
// empty: not every song has one.
bool subsonic_cover_url(const char *cover_id, int pixels, char *url, size_t size);

// ---------------------------------------------------------------------------
// Writing back to the account
//
// `submission` false is "started playing", true is "played through". Servers
// count a play from the second one, so both are sent: the first is what puts a
// track into "recently played" as it starts, the second is what the server's
// own statistics are built from.
// ---------------------------------------------------------------------------
bool subsonic_scrobble(const char *song_id, bool submission);

// Stars or unstars a song, an album or a playlist: the protocol takes the id
// alone and works out the kind.
bool subsonic_star(const char *item_id, bool star);

#endif /* SUBSONIC_H */
