#ifndef SUBSONICCACHE_H
#define SUBSONICCACHE_H

#include <stdbool.h>
#include <stddef.h>

#include "src/system/streaming/subsonic.h"

// Subsonic tracks downloaded to the card and then played like any other file.
//
// The reasoning is qobuzcache's, unchanged, and worth repeating only in one
// point: decoder_open() wants a path, seeking needs to go backwards, and the
// equaliser, ReplayGain and the progress bar all work for free once the bytes
// are on the card. So a track is fetched, written, and played from the file --
// not decoded as it arrives, which is what radio does because a radio stream
// has no end and is never seeked.
//
// What differs from Qobuz is only the naming. A Subsonic id is an opaque
// string -- Navidrome sends UUIDs, Airsonic "al-123", others numbers -- so a
// file cannot be named after it directly. The name is the MD5 of the id, which
// is stable, filesystem-safe and unique, and the id itself is written into the
// tags sidecar so a playing file maps back to its song.
//
// The extension comes from the server's own `suffix` field, not from the MIME
// type, so the name is known before the file is requested -- which is what lets
// the page queue a whole album and let the tracks arrive underneath.

#define SUBSONICCACHE_DIR ".local/subsonic-cache"

// How large the cache may grow before the least recently used tracks are
// dropped. One GB is about thirty 16/44 albums: enough not to re-download what
// is being listened to these days, small enough not to silently fill a card.
#define SUBSONICCACHE_MAX_BYTES (1024L * 1024L * 1024L)

// ---------------------------------------------------------------------------
// Streaming, and keeping what was streamed
//
// The device cannot decode from a socket: decoder_open() wants a path and
// seeking has to go backwards. So even "streaming" is a download -- the
// difference is only what happens to the file afterwards.
//
//   keep == false  the file is deleted as soon as playback moves off it. The
//                  card holds at most the current track and whatever is being
//                  prefetched ahead, which is what "stream, do not download"
//                  means to the person asking for it: the card does not fill
//                  up with music they did not put there.
//   keep == true   the file stays in the hidden cache and is replayed from
//                  there next time, which is what a slow link or a small
//                  library wants.
//
// The setting lives in device_config.ini as [subsonic] keep, so the page, the
// cache and the cleanup at shutdown all read the same answer.
// ---------------------------------------------------------------------------
bool subsoniccache_keep_downloads(void);
void subsoniccache_set_keep_downloads(bool keep);

// Where the card is. Called once at startup and on every card change; NULL or
// empty disables the cache (and with it streaming, which cannot play without
// it).
void subsoniccache_set_root(const char *sd_root);
bool subsoniccache_ready(void);

// The path this song would have, from its id and suffix alone. Says nothing
// about whether it exists.
void subsoniccache_path(const subsonic_song_t *song, char *out, size_t size);

// True if the song has been downloaded in full.
bool subsoniccache_has(const subsonic_song_t *song, char *out, size_t size);

// The same, for callers that hold only the id and the suffix the queue
// predicted.
bool subsoniccache_has_id(const char *song_id, const char *suffix, char *out, size_t size);

// Like subsoniccache_has(), but without knowing the format: tries every
// extension this cache may have used. For a track queued before it was asked
// for, where the predicted suffix and the one the server sent may differ.
bool subsoniccache_find(const char *song_id, char *out, size_t size);

// Starts downloading `song` into the cache and puts the local path in `out`.
//
// Returns as soon as there is enough material for the decoder to open the file
// (or all of it, if the track is short); the rest keeps downloading on its own
// thread. The caller can hand the path to playback immediately.
//
// Blocks for the duration of the prebuffer -- a few tenths of a second -- so it
// must be called from a worker thread, never from the GUI thread.
//
// A track already in the cache returns immediately without touching the
// network.
bool subsoniccache_start(const subsonic_song_t *song, char *out, size_t size);

// Waits until no download is in progress. Only one runs at a time, across all
// services: see streamturn.h.
void subsoniccache_wait_idle(void);

// Called when the prebuffer will take more than a moment, with the estimate in
// seconds. From the download thread: the receiver must bounce to the GUI thread
// itself.
void subsoniccache_set_slow_start_cb(void (*cb)(int seconds));

// Abandons downloads in progress. Call when switching to another track.
void subsoniccache_abandon_all(void);

// The id of the song downloading right now, empty if none.
bool subsoniccache_downloading_id(char *out, size_t size);

// Called once a second from the queue watchdog, with the path playback is on
// (empty when it is on nothing, or on a file that is not ours).
//
// In streaming mode this is what deletes the track that has just been left:
// the file is read by the decoder until the queue moves on, and once it has
// moved on nothing will read it again. Deleting only when a DIFFERENT track
// takes over -- never on "playback stopped" -- is deliberate: stopping and
// pressing play again is one gesture, and re-downloading the track in between
// would make a pause feel like a failure.
void subsoniccache_stream_tick(const char *current_path);

// "Subsonic needs the network", even when nothing is downloading at this
// instant. Needed by the Wi-Fi parking logic, for the same reason as
// qobuzcache_network_wanted().
void subsoniccache_set_network_wanted(bool wanted);
bool subsoniccache_network_wanted(void);

// True if `path` is inside the Subsonic cache.
bool subsoniccache_owns(const char *path);

// The cache directory, or NULL with no card.
const char *subsoniccache_dir(void);

// Writes beside the track what the API knows and the file does not: title,
// artist, album, album id and the song id (in `<file>.tags`) and the cover (in
// `<stem>.jpg`). Without it the player would show the file's digest as its
// name.
void subsoniccache_write_sidecars(const subsonic_song_t *song, const char *cover_url);

// The album id the song belongs to, read back from the sidecar. False if the
// path is not from this cache or the sidecar does not say.
bool subsoniccache_album_id(const char *path, char *out, size_t size);

// The Subsonic song id from the local path, read back from the sidecar. False
// when the path is not from this cache.
bool subsoniccache_song_id(const char *path, char *out, size_t size);

// Why the last subsoniccache_start() failed, as a sentence fit to display.
// Empty on success. Per thread, like http_last_error().
const char *subsoniccache_last_error(void);

// How many tracks between automatic cache sweeps.
#define SUBSONICCACHE_CLEAR_EVERY 10

// Call for every track that enters the cache, with its path. Sweeps every
// SUBSONICCACHE_CLEAR_EVERY calls, and keeps a ring of recently downloaded
// files that the sweep never touches.
void subsoniccache_note_played(const char *path);

// How many paths can be protected from the automatic sweep.
#define SUBSONICCACHE_PROTECTED_MAX 64

// The tracks the queue expects to play: the current one and those after it.
// Call from the GUI thread, the only owner of the queue.
void subsoniccache_set_protected(const char *const *paths, int count);

// Call on the way out (shutdown, reboot): the cache does not survive a power
// cycle.
void subsoniccache_clear_on_exit(void);

// Current size, and how to empty it.
long subsoniccache_bytes(void);
void subsoniccache_clear(void);

#endif /* SUBSONICCACHE_H */
