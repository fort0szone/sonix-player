#include "subsonicpage.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/cover.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/confirm.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/spinner.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/gui/streaming/qobuzart.h"
#include "src/gui/wireless/wifisettings.h"
#include "src/system/core/lang.h"
#include "src/system/core/utils.h"
#include "src/system/decode/growfile.h"
#include "src/system/device/system.h"
#include "src/system/net/wifi.h"
#include "src/system/playback/device_state.h"
#include "src/system/playback/playlist.h"
#include "src/system/streaming/streamturn.h"
#include "src/system/streaming/subsonic.h"
#include "src/system/streaming/subsoniccache.h"

// ---------------------------------------------------------------------------
// Subsonic front end.
//
// Everything that talks to the network BLOCKS (see subsonic.h), so none of it
// can run on the GUI thread: there is a single worker with a single slot for
// the job to do, and results come back through gui_post().
//
// One slot rather than a queue: touching a second entry while the first is
// still loading means the second is what matters. The first is dropped, not
// queued.
// ---------------------------------------------------------------------------

#define PAGE_LIMIT 50
// Maximum entries held: four pages. Reaching the bottom asks for another
// fifty; past this cap the list really stops, because every entry is static
// memory on a device with 64 MB in total.
#define MAX_HELD (4 * PAGE_LIMIT)

#define ROW_THUMB 64

static lv_obj_t *login_screen;
static lv_obj_t *list_screen;
static lv_obj_t *search_screen;
static lv_obj_t *settings_screen;

// Global rather than hidden behind a function: the Streaming grid wants the
// address of the variable (grid_entry_t.target), as for every other page.
lv_obj_t *subsonic_screen;

// ---------------------------------------------------------------------------
// the worker
// ---------------------------------------------------------------------------

typedef enum {
	JOB_NONE = 0,
	JOB_LOGIN,
	JOB_ALBUMS, // an album list: type in job.text
	JOB_ARTISTS,
	JOB_PLAYLISTS,
	JOB_ARTIST_ALBUMS,
	JOB_ALBUM_SONGS,
	JOB_PLAYLIST_SONGS,
	JOB_STARRED_ALBUMS,
	JOB_STARRED_SONGS,
	JOB_SEARCH_SONGS,
	JOB_SEARCH_ALBUMS,
	JOB_SEARCH_ARTISTS,
	JOB_PLAY,
} job_kind_t;

typedef struct {
	job_kind_t kind;
	char text[256];				 // search terms, or the album-list type
	char text2[128];			 // password
	char server[SUBSONIC_URL_MAX]; // sign-in address
	char id[SUBSONIC_ID_MAX];
	int index;	// which song of the list playback starts from
	int offset; // first entry of this page: 0 = new list
} job_t;

static pthread_mutex_t job_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t job_ready = PTHREAD_COND_INITIALIZER;
static job_t pending;
static bool have_pending;
static bool worker_started;

// Job serial number: when it changes, a late result belongs to a request that
// no longer matters and is discarded.
static unsigned job_serial;

// What came back. Only the worker writes here, and only while the GUI is not
// reading (it reads inside the async, which runs after the worker is done).
static subsonic_song_t result_songs[MAX_HELD];
static subsonic_album_t result_albums[MAX_HELD];
static subsonic_artist_t result_artists[MAX_HELD];
static subsonic_playlist_t result_playlists[MAX_HELD];
static int result_count;
static int result_from; // where the last page received starts (0 = new list)
static job_kind_t result_kind;
static char result_error[256];
static char result_title[160];
// The request that produced the list on screen, so "load more" asks the same
// question again with a new offset.
static job_t shown_job;

// What the list is showing right now, so a row tap knows what to do.
static job_kind_t list_kind;

// Pagination: the last page came back full, so there may be another -- and one
// request at a time.
static bool page_more;
static bool page_loading;

static void submit(const job_t *job) {
	pthread_mutex_lock(&job_lock);
	pending = *job;
	have_pending = true;
	job_serial++;
	pthread_cond_signal(&job_ready);
	pthread_mutex_unlock(&job_lock);
}

// ---------------------------------------------------------------------------
// the busy veil
// ---------------------------------------------------------------------------

static lv_obj_t *busy_layer;

static void busy_hide(void) {
	if (busy_layer) {
		lv_obj_set_hidden(busy_layer, true);
	}
}

static void busy_show(const char *text) {
	(void)text;
	if (!busy_layer) {
		return;
	}
	lv_obj_set_hidden(busy_layer, false);
	lv_obj_move_foreground(busy_layer);
}

static void build_busy_layer(void) {
	busy_layer = lv_obj_create(lv_layer_sys());
	lv_obj_set_size(busy_layer, lv_pct(100), lv_pct(100));
	lv_obj_set_pos(busy_layer, 0, 0);
	lv_obj_set_style_bg_color(busy_layer, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(busy_layer, LV_OPA_50, 0);
	lv_obj_set_style_border_width(busy_layer, 0, 0);
	lv_obj_set_style_radius(busy_layer, 0, 0);
	lv_obj_set_scrollable(busy_layer, false);
	lv_obj_set_hidden(busy_layer, true);
	lv_obj_set_flex_flow(busy_layer, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(busy_layer, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	// White rather than the theme colour: the veil underneath is 50% black over
	// any page, so the background here is dark in both themes.
	lv_obj_t *spin = spinner_create(busy_layer, &icon_loader_big);
	lv_obj_set_style_image_recolor(spin, lv_color_white(), 0);
	lv_obj_set_style_image_recolor_opa(spin, LV_OPA_COVER, 0);
}

// ---------------------------------------------------------------------------
// the result list
// ---------------------------------------------------------------------------

static lv_obj_t *list_container;
static lv_obj_t *list_title;
static lv_obj_t *list_empty;

static void rebuild_home(void);
static void row_clicked_cb(lv_event_t *e);
static bool kind_pages(job_kind_t kind);

// "3:42"
static void duration_text(int seconds, char *out, size_t size) {
	if (seconds <= 0) {
		out[0] = '\0';
		return;
	}
	snprintf(out, size, "%d:%02d", seconds / 60, seconds % 60);
}

// ---------------------------------------------------------------------------
// cover art
//
// qobuzart is service-agnostic (it takes a URL, returns an image, and stores it
// under the URL's MD5), so the same twelve slots serve this page. The owner
// token is what keeps the two pages from cancelling each other's requests.
// ---------------------------------------------------------------------------

static lv_obj_t *row_thumbs[MAX_HELD];
static cover_image_t row_images[MAX_HELD];
static bool row_has_image[MAX_HELD];
static bool row_art_failed[MAX_HELD];
static char row_cover_url[MAX_HELD][SUBSONIC_URL_MAX];

static const char subsonic_art_owner;
static int slot_owner[QOBUZART_SLOTS];
static lv_timer_t *art_timer;

static bool art_any_pending(void) {
	for (int i = 0; i < QOBUZART_SLOTS; i++) {
		if (slot_owner[i] >= 0) {
			return true;
		}
	}
	return false;
}

static void art_wake(void) {
	if (art_timer) {
		lv_timer_resume(art_timer);
	}
}

static void art_release_slot(int slot) {
	if (slot < 0 || slot >= QOBUZART_SLOTS) {
		return;
	}
	qobuzart_release(&subsonic_art_owner, slot);
	slot_owner[slot] = -1;
}

static void art_request_visible(void);

static void art_poll_cb(lv_timer_t *timer) {
	bool freed = false;
	if (!art_any_pending()) {
		lv_timer_pause(timer); // nothing in flight: art_wake() restarts it
		return;
	}

	for (int slot = 0; slot < QOBUZART_SLOTS; slot++) {
		int row = slot_owner[slot];
		if (row < 0 || row >= MAX_HELD || !row_thumbs[row]) {
			continue;
		}
		cover_image_t image;
		bool finished = false;
		if (!qobuzart_take(&subsonic_art_owner, slot, &image, &finished)) {
			if (finished) {
				row_art_failed[row] = true; // failed: never requested again
				freed = true;
				slot_owner[slot] = -1;
			}
			continue;
		}

		cover_free(&row_images[row]);
		row_images[row] = image;
		row_has_image[row] = true;
		// The grey note placeholder was recoloured by the theme: a real cover
		// must keep its own colours, otherwise it turns into a flat square.
		lv_obj_remove_style(row_thumbs[row], &theme_style_icon, 0);
		lv_obj_set_style_image_recolor_opa(row_thumbs[row], LV_OPA_TRANSP, 0);
		lv_obj_set_style_radius(row_thumbs[row], 6, 0);
		lv_obj_set_style_clip_corner(row_thumbs[row], true, 0);
		lv_image_set_src(row_thumbs[row], &row_images[row].dsc);

		slot_owner[slot] = -1;
		freed = true;
	}

	if (freed) {
		art_request_visible();
	}
}

// Requests covers for the visible rows, plus some margin above and below so
// slow scrolling finds them already there.
static void art_request_visible(void) {
	if (!list_container) {
		return;
	}

	int32_t view_top = lv_obj_get_scroll_y(list_container);
	int32_t view_height = lv_obj_get_height(list_container);
	int32_t margin = view_height / 2;

	uint32_t children = lv_obj_get_child_count(list_container);
	for (uint32_t c = 0; c < children && c < MAX_HELD; c++) {
		int row = (int)c;
		if (!row_thumbs[row] || row_has_image[row] || row_art_failed[row] || !row_cover_url[row][0]) {
			continue;
		}

		lv_obj_t *widget = lv_obj_get_child(list_container, (int32_t)c);
		int32_t y = lv_obj_get_y(widget);
		int32_t h = lv_obj_get_height(widget);
		if (y + h < view_top - margin || y > view_top + view_height + margin) {
			continue; // too far off screen to be worth a request
		}

		bool queued = false;
		for (int slot = 0; slot < QOBUZART_SLOTS && !queued; slot++) {
			queued = slot_owner[slot] == row;
		}
		if (queued) {
			continue;
		}

		int free_slot = -1;
		for (int slot = 0; slot < QOBUZART_SLOTS && free_slot < 0; slot++) {
			if (slot_owner[slot] < 0) {
				free_slot = slot;
			}
		}
		if (free_slot < 0) {
			return; // all busy: the rest on the next pass
		}

		slot_owner[free_slot] = row;
		art_wake();
		qobuzart_request(&subsonic_art_owner, free_slot, row_cover_url[row], ROW_THUMB);
	}
}

// The rows all go away at once (lv_obj_clean): the pointers must be forgotten
// BEFORE that, or the thumbnail pass writes to dead objects.
static void art_forget_rows(void) {
	for (int slot = 0; slot < QOBUZART_SLOTS; slot++) {
		art_release_slot(slot);
	}
	for (int i = 0; i < MAX_HELD; i++) {
		cover_free(&row_images[i]);
		row_has_image[i] = false;
		row_art_failed[i] = false;
		row_thumbs[i] = NULL;
		row_cover_url[i][0] = '\0';
	}
}

static void maybe_load_more(void);

static void list_scrolled_cb(lv_event_t *e) {
	(void)e;
	art_request_visible();
	maybe_load_more();
}

static lv_obj_t *make_row(const char *name, const char *detail, const char *cover_url, int index) {
	lv_obj_t *row = lv_btn_create(list_container);
	lv_obj_set_size(row, lv_pct(100), 88);
	lv_obj_add_style(row, &theme_style_card, 0);
	lv_obj_add_style(row, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(row, 12, 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_shadow_width(row, 0, 0);
	lv_obj_set_style_pad_hor(row, 16, 0);
	lv_obj_set_style_pad_ver(row, 0, 0);
	lv_obj_set_style_pad_column(row, 8, 0);
	lv_obj_set_scrollable(row, false);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	// Gestures (back, and pulling in the player sheet) often start on a row,
	// and a button keeps the press to itself: without this neither drag works.
	lv_obj_set_event_bubble(row, true);
	lv_obj_add_event_cb(row, row_clicked_cb, LV_EVENT_CLICKED, (void *)(intptr_t)index);

	// The cover, where there is one. It starts as the grey note and becomes the
	// real image once the thread has downloaded it: the list is readable
	// immediately, without waiting for the network.
	if (cover_url && cover_url[0] && index < MAX_HELD) {
		lv_obj_t *thumb = lv_image_create(row);
		lv_obj_set_size(thumb, ROW_THUMB, ROW_THUMB);
		lv_image_set_inner_align(thumb, LV_IMAGE_ALIGN_CENTER);
		lv_image_set_src(thumb, &icon_music2);
		lv_obj_add_style(thumb, &theme_style_icon, 0);
		lv_obj_set_style_image_recolor_opa(thumb, LV_OPA_COVER, 0);
		lv_obj_set_event_bubble(thumb, true);
		row_thumbs[index] = thumb;
		snprintf(row_cover_url[index], sizeof(row_cover_url[0]), "%s", cover_url);
	}

	lv_obj_t *texts = lv_obj_create(row);
	lv_obj_set_flex_grow(texts, 1);
	lv_obj_set_height(texts, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(texts, 0, 0);
	lv_obj_set_style_border_width(texts, 0, 0);
	lv_obj_set_style_pad_all(texts, 0, 0);
	lv_obj_set_style_pad_row(texts, 6, 0);
	lv_obj_set_scrollable(texts, false);
	lv_obj_set_event_bubble(texts, true);
	lv_obj_set_flex_flow(texts, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(texts, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

	lv_obj_t *label = lv_label_create(texts);
	lv_label_set_text(label, name);
	lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
	lv_obj_set_width(label, lv_pct(100));
	lv_obj_set_height(label, lv_font_get_line_height(&font_ui_22));
	lv_obj_add_style(label, &theme_style_text, 0);
	lv_obj_set_style_text_font(label, &font_ui_22, 0);

	if (detail && detail[0]) {
		lv_obj_t *sub = lv_label_create(texts);
		lv_label_set_text(sub, detail);
		lv_label_set_long_mode(sub, LV_LABEL_LONG_DOT);
		lv_obj_set_width(sub, lv_pct(100));
		lv_obj_set_height(sub, lv_font_get_line_height(&font_ui_18));
		lv_obj_add_style(sub, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(sub, &font_ui_18, 0);
	}

	return row;
}

// `from` is the first row to create: 0 rebuilds everything, a higher value
// APPENDS the new rows at the bottom without touching the others -- which is
// what keeps the scroll position steady when the next page arrives.
static void fill_list(int from) {
	if (from == 0) {
		art_forget_rows();
		lv_obj_clean(list_container);
	}
	list_kind = result_kind;

	for (int i = from; i < result_count; i++) {
		char detail[400];
		char cover[SUBSONIC_URL_MAX];

		switch (result_kind) {
		case JOB_ALBUM_SONGS:
		case JOB_PLAYLIST_SONGS:
		case JOB_STARRED_SONGS:
		case JOB_SEARCH_SONGS: {
			const subsonic_song_t *s = &result_songs[i];
			char length[16];
			duration_text(s->duration, length, sizeof(length));
			snprintf(detail, sizeof(detail), "%s \xC2\xB7 %s", s->artist, length);
			cover[0] = '\0';
			subsonic_cover_url(s->cover_id, ROW_THUMB, cover, sizeof(cover));
			make_row(s->title, detail, cover, i);
			break;
		}
		case JOB_ALBUMS:
		case JOB_ARTIST_ALBUMS:
		case JOB_STARRED_ALBUMS:
		case JOB_SEARCH_ALBUMS: {
			const subsonic_album_t *a = &result_albums[i];
			// "Artist · 12 tracks", with whichever parts the server sent.
			if (a->song_count > 0) {
				snprintf(detail, sizeof(detail), "%s \xC2\xB7 %d", a->artist, a->song_count);
			} else {
				snprintf(detail, sizeof(detail), "%s", a->artist);
			}
			cover[0] = '\0';
			subsonic_cover_url(a->cover_id, ROW_THUMB, cover, sizeof(cover));
			make_row(a->name, detail, cover, i);
			break;
		}
		case JOB_ARTISTS:
		case JOB_SEARCH_ARTISTS: {
			const subsonic_artist_t *a = &result_artists[i];
			if (a->album_count > 0) {
				snprintf(detail, sizeof(detail), "%d %s", a->album_count, tr("albums"));
			} else {
				detail[0] = '\0';
			}
			cover[0] = '\0';
			subsonic_cover_url(a->cover_id, ROW_THUMB, cover, sizeof(cover));
			make_row(a->name, detail, cover, i);
			break;
		}
		case JOB_PLAYLISTS: {
			const subsonic_playlist_t *p = &result_playlists[i];
			if (p->owner[0]) {
				snprintf(detail, sizeof(detail), "%s \xC2\xB7 %d", p->owner, p->song_count);
			} else {
				snprintf(detail, sizeof(detail), "%d", p->song_count);
			}
			cover[0] = '\0';
			subsonic_cover_url(p->cover_id, ROW_THUMB, cover, sizeof(cover));
			make_row(p->name, detail, cover, i);
			break;
		}
		default:
			break;
		}
	}

	// Covers for the visible rows. This runs after the rows exist and after
	// LVGL has computed the layout, because it reads their coordinates.
	lv_obj_update_layout(list_container);
	art_request_visible();

	if (result_count == 0) {
		lv_obj_set_hidden(list_empty, false);
	} else {
		lv_obj_set_hidden(list_empty, true);
	}

	lv_label_set_text(list_title, result_title);
}

// ---------------------------------------------------------------------------
// what comes back from the worker, on the GUI thread
// ---------------------------------------------------------------------------

static void job_done_cb(void *user) {
	(void)user;
	busy_hide();
	page_loading = false;

	if (result_error[0]) {
		toast_error(result_error);
		result_error[0] = '\0';
		return;
	}

	// Signing in is not a list: it changes what the home page offers.
	if (result_kind == JOB_LOGIN) {
		rebuild_home();
		toast_success("signed_in");
		switch_screen(subsonic_screen);
		return;
	}

	fill_list(result_from);
	switch_screen(list_screen);
}

static void run(const job_t *job, const char *waiting_text) {
	busy_show(waiting_text);
	submit(job);
}

// ---------------------------------------------------------------------------
// playback
//
// The queue is built from the WHOLE list at once, not from what has downloaded:
// every track's path is known before its bytes are (the file name is the MD5 of
// the id plus the server's own suffix), so the files can land underneath a queue
// that already knows about them. That is what makes next/previous work, and what
// lets the queue page show titles instead of digests.
// ---------------------------------------------------------------------------

static subsonic_song_t queued_songs[MAX_HELD];
static int queued_count;

// True while playback is parked waiting for a file that is still downloading.
static bool waiting_for_track;

// ---------------------------------------------------------------------------
// prefetching, on a thread of its own
//
// Not on the worker that serves taps: prefetching sits in the download for the
// whole track, which on a real network is minutes, and for all that time
// tapping another entry would do nothing. The worker always answers, and this
// thread checks the wanted id before each step, so as soon as another track is
// started it notices it is no longer needed and gives up.
// ---------------------------------------------------------------------------

static pthread_mutex_t prefetch_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t prefetch_ready = PTHREAD_COND_INITIALIZER;

// Which queued track must be downloaded now, by id. Empty = nothing to do.
// Written by the queue watchdog on the GUI thread and by do_play, read by the
// prefetch thread.
static char prefetch_want[SUBSONIC_ID_MAX];
static bool prefetch_started;

// The song with this id in the current queue, if it is still there.
static bool find_queued(const char *id, subsonic_song_t *out) {
	if (!id || !*id) {
		return false;
	}
	bool found = false;
	pthread_mutex_lock(&prefetch_lock);
	for (int i = 0; i < queued_count && !found; i++) {
		if (strcmp(queued_songs[i].id, id) == 0) {
			*out = queued_songs[i];
			found = true;
		}
	}
	pthread_mutex_unlock(&prefetch_lock);
	return found;
}

// Asks for `id` to be downloaded next. Replaces whatever was wanted: only one
// track can matter at a time.
static void prefetch_request(const char *id) {
	pthread_mutex_lock(&prefetch_lock);
	snprintf(prefetch_want, sizeof(prefetch_want), "%s", id ? id : "");
	pthread_cond_signal(&prefetch_ready);
	pthread_mutex_unlock(&prefetch_lock);
}

static void cover_ready_async(void *user) {
	char *path = user;
	device_state_t state;
	device_state_get(&state);
	if (path && strcmp(path, state.current_file) == 0) {
		player_refresh_now_playing();
	}
	free(path);
}

static void *prefetch_main(void *arg) {
	(void)arg;
	thread_be_background("subsonic prefetch");

	for (;;) {
		pthread_mutex_lock(&prefetch_lock);
		while (!prefetch_want[0]) {
			pthread_cond_wait(&prefetch_ready, &prefetch_lock);
		}
		char want[SUBSONIC_ID_MAX];
		snprintf(want, sizeof(want), "%s", prefetch_want);
		prefetch_want[0] = '\0';
		pthread_mutex_unlock(&prefetch_lock);

		subsonic_song_t song;
		if (!find_queued(want, &song)) {
			continue; // the queue moved on: nothing to do
		}

		char path[512];
		if (!subsoniccache_start(&song, path, sizeof(path))) {
			fprintf(stderr, "subsonic: prefetch of '%s' failed: %s\n", song.title, subsoniccache_last_error());
			continue;
		}

		char cover[SUBSONIC_URL_MAX];
		cover[0] = '\0';
		subsonic_cover_url(song.cover_id, 300, cover, sizeof(cover));
		subsoniccache_write_sidecars(&song, cover);
		subsoniccache_note_played(path);

		// The cover may land an instant after the player has already looked for
		// one. If it belongs to the track being played, the player must be told.
		char *owned = strdup(path);
		if (owned) {
			gui_post(cover_ready_async, owned);
		}
	}
}

static void start_prefetch_thread(void) {
	pthread_mutex_lock(&prefetch_lock);
	if (!prefetch_started) {
		prefetch_started = true;
		pthread_t thread;
		if (pthread_create(&thread, NULL, prefetch_main, NULL) == 0) {
			pthread_detach(thread);
		} else {
			prefetch_started = false;
		}
	}
	pthread_mutex_unlock(&prefetch_lock);
}

// ---------------------------------------------------------------------------
// the prepare callback
//
// device_state calls this before it opens a file. The queue holds the whole
// list, so playback can reach a track before its download has -- and then this
// is what stops the player rather than handing the decoder a path to nothing.
// The queue watchdog below restarts it once the file is there.
// ---------------------------------------------------------------------------

// True when the file can already be played: fully downloaded, or growing right
// now (the decoder reads a growing file, which is how the first note plays
// without waiting for the end).
static bool track_is_playable(const char *path) {
	// Growing right now: the decoder reads a growing file, which is how the
	// first note plays without waiting for the end of the download.
	if (growfile_is_growing(path)) {
		return true;
	}

	char song_id[SUBSONIC_ID_MAX];
	if (!subsoniccache_song_id(path, song_id, sizeof(song_id))) {
		// No sidecar yet -- the queue was built a moment ago. The file exists
		// under the name the queue predicted, or it does not.
		struct stat st;
		return stat(path, &st) == 0 && st.st_size > 0;
	}

	// Fully downloaded, under this name or under another container.
	char found[512];
	if (subsoniccache_find(song_id, found, sizeof(found))) {
		return true;
	}

	char downloading[SUBSONIC_ID_MAX];
	if (subsoniccache_downloading_id(downloading, sizeof(downloading))) {
		return strcmp(downloading, song_id) == 0;
	}
	return false;
}

static void downloading_note_async(void *user) {
	(void)user;
	gui_notify_popup("downloading");
}

static bool subsonic_prepare_track(const char *path) {
	if (!path || !subsoniccache_owns(path)) {
		waiting_for_track = false;
		return true; // not one of ours
	}
	if (track_is_playable(path)) {
		waiting_for_track = false;
		return true;
	}

	// Not there. Stop first, and not out of politeness: the previous track is
	// reading a file that is still downloading, and that file is one the
	// prefetch is about to abandon. Pulling it out from under the audio thread
	// makes it report "track finished", the queue advances onto another missing
	// track, and the loop starts over.
	//
	// The network is needed, and needed NOW: the Wi-Fi idle parking is told to
	// hold before playback is even stopped, or the radio goes down in the gap.
	subsoniccache_set_network_wanted(true);
	waiting_for_track = true;
	device_state_stop();

	char song_id[SUBSONIC_ID_MAX];
	if (subsoniccache_song_id(path, song_id, sizeof(song_id)) && song_id[0]) {
		prefetch_request(song_id);
	} else {
		// No sidecar to name it by: find the queue entry whose predicted path is
		// this one.
		pthread_mutex_lock(&prefetch_lock);
		for (int i = 0; i < queued_count; i++) {
			char predicted[512];
			subsoniccache_path(&queued_songs[i], predicted, sizeof(predicted));
			if (strcmp(predicted, path) == 0) {
				snprintf(song_id, sizeof(song_id), "%s", queued_songs[i].id);
				break;
			}
		}
		pthread_mutex_unlock(&prefetch_lock);
		if (song_id[0]) {
			prefetch_request(song_id);
		}
	}

	gui_post(downloading_note_async, NULL);
	return false;
}

static void play_started_async(void *user) {
	(void)user;
	switch_screen(list_screen);
	switcher_set_player_return(list_screen);
}

// Scrobbles on its own thread: the call blocks on the server, so it must not
// sit on the GUI thread or on the API worker (which must stay free for taps).
static void *scrobble_main(void *arg) {
	subsonic_scrobble((const char *)arg, false);
	free(arg);
	return NULL;
}

// Tells the server "now playing", which is what puts a track into the server's
// own "recently played" as it starts. Failure is deliberately silent: the
// music must never stop because a housekeeping call could not reach the server.
static void scrobble_started(const char *song_id) {
	if (!song_id || !*song_id) {
		return;
	}
	char *owned = strdup(song_id);
	if (!owned) {
		return;
	}
	pthread_t thread;
	if (pthread_create(&thread, NULL, scrobble_main, owned) == 0) {
		pthread_detach(thread);
	} else {
		free(owned);
	}
}

static void do_play(int index) {
	if (index < 0 || index >= result_count) {
		return;
	}

	const subsonic_song_t *song = &result_songs[index];

	// Whatever was downloading for another track is no longer needed: the
	// bandwidth belongs to this one.
	streamturn_abandon_all();

	char path[512];
	if (!subsoniccache_start(song, path, sizeof(path))) {
		const char *why = subsoniccache_last_error();
		toast_error(why && why[0] ? why : tr("download_failed"));
		return;
	}
	subsoniccache_note_played(path);

	// Tags BEFORE starting playback: that is when device_state reads the
	// metadata, and a moment later would be too late.
	char cover[SUBSONIC_URL_MAX];
	cover[0] = '\0';
	subsonic_cover_url(song->cover_id, 300, cover, sizeof(cover));
	subsoniccache_write_sidecars(song, cover);

	// --- the queue: the WHOLE list, not only what has downloaded ------------
	static char paths[MAX_HELD][512];
	static const char *path_ptr[MAX_HELD];

	pthread_mutex_lock(&prefetch_lock);
	queued_count = 0;
	int start = 0;
	for (int i = 0; i < result_count && queued_count < MAX_HELD; i++) {
		if (i == index) {
			start = queued_count;
			// The tapped one has already downloaded: it is queued under its REAL
			// name, which with an unexpected container may differ from the
			// predicted one.
			snprintf(paths[queued_count], sizeof(paths[0]), "%s", path);
		} else {
			subsoniccache_path(&result_songs[i], paths[queued_count], sizeof(paths[0]));
		}
		path_ptr[queued_count] = paths[queued_count];
		queued_songs[queued_count] = result_songs[i];
		queued_count++;
	}
	pthread_mutex_unlock(&prefetch_lock);

	if (queued_count == 0) {
		return;
	}

	// Tags for the WHOLE queue, immediately. They are text files of a few dozen
	// bytes and cost nothing. Without them the Queue page would show the file
	// name, which is a digest, instead of the title.
	for (int i = 0; i < queued_count; i++) {
		if (i == start) {
			continue; // already written, and under the real name
		}
		subsoniccache_write_sidecars(&queued_songs[i], NULL);
	}

	waiting_for_track = false;
	device_state_play_list(path_ptr, queued_count, start);

	scrobble_started(song->id);
	gui_post(play_started_async, NULL);
}

// ---------------------------------------------------------------------------
// the queue watchdog, once a second on the GUI thread
//
// Two things, both of which need to know where playback is -- which cannot be
// asked from another thread, because the queue belongs to the GUI:
//
//   1. tells the cache which files it must not evict (those still to be
//      played);
//   2. tells the prefetch thread WHICH track to download now: the first missing
//      one between the current track and the next two. It is the same question
//      whether prefetching ahead or playback has reached a track that is not
//      there yet -- in that case the first missing one is exactly the one being
//      waited for.
// ---------------------------------------------------------------------------

static void queue_watch_cb(lv_timer_t *timer) {
	(void)timer;

	int count = playlist_count();
	int current = playlist_current_index();
	if (count <= 0 || current < 0) {
		subsoniccache_set_protected(NULL, 0);
		waiting_for_track = false;
		return;
	}

	// The paths the queue still has ahead, which the periodic sweep must not
	// delete.
	static char protected_paths[16][512];
	static const char *protected_ptr[16];
	int protected_count = 0;

	int last = current + 2 < count ? current + 2 : count - 1;
	char wanted[SUBSONIC_ID_MAX] = "";
	char current_path[512] = "";

	for (int i = current; i <= last; i++) {
		char path[512];
		if (!playlist_path_at(i, path, sizeof(path)) || !subsoniccache_owns(path)) {
			continue;
		}
		if (protected_count < 16) {
			snprintf(protected_paths[protected_count], sizeof(protected_paths[0]), "%s", path);
			protected_ptr[protected_count] = protected_paths[protected_count];
			protected_count++;
		}
		if (i == current) {
			snprintf(current_path, sizeof(current_path), "%s", path);
		}
		if (!wanted[0] && !track_is_playable(path)) {
			char song_id[SUBSONIC_ID_MAX];
			if (subsoniccache_song_id(path, song_id, sizeof(song_id)) && song_id[0]) {
				snprintf(wanted, sizeof(wanted), "%s", song_id);
			} else {
				// The sidecar is not written yet: match the queue table instead.
				pthread_mutex_lock(&prefetch_lock);
				for (int k = 0; k < queued_count; k++) {
					char predicted[512];
					subsoniccache_path(&queued_songs[k], predicted, sizeof(predicted));
					if (strcmp(predicted, path) == 0) {
						snprintf(wanted, sizeof(wanted), "%s", queued_songs[k].id);
						break;
					}
				}
				pthread_mutex_unlock(&prefetch_lock);
			}
		}
	}

	subsoniccache_set_protected(protected_ptr, protected_count);

	// Streaming mode: the track playback has just left goes off the card.
	subsoniccache_stream_tick(current_path);

	if (wanted[0]) {
		prefetch_request(wanted);
	}

	// Playback parked on a missing file: restart it now that the file is here.
	if (waiting_for_track && current_path[0] && track_is_playable(current_path)) {
		waiting_for_track = false;
		fprintf(stderr, "subsonic: '%s' is ready, restarting the queue\n", current_path);
		device_state_play_queue_index(current);
		player_refresh_now_playing();
	}

	// Nothing is being waited for and nothing is downloading: the radio may
	// park again.
	if (!waiting_for_track) {
		subsoniccache_set_network_wanted(false);
	}
}

// ---------------------------------------------------------------------------
// what a row tap does
// ---------------------------------------------------------------------------

static void row_clicked_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return;
	}
	int index = (int)(intptr_t)lv_event_get_user_data(e);

	job_t job = {0};
	switch (list_kind) {
	case JOB_ALBUM_SONGS:
	case JOB_PLAYLIST_SONGS:
	case JOB_STARRED_SONGS:
	case JOB_SEARCH_SONGS:
		do_play(index);
		return;

	case JOB_ALBUMS:
	case JOB_ARTIST_ALBUMS:
	case JOB_STARRED_ALBUMS:
	case JOB_SEARCH_ALBUMS:
		if (index < 0 || index >= result_count) {
			return;
		}
		job.kind = JOB_ALBUM_SONGS;
		snprintf(job.id, sizeof(job.id), "%s", result_albums[index].id);
		snprintf(job.text, sizeof(job.text), "%s", result_albums[index].name);
		break;

	case JOB_ARTISTS:
	case JOB_SEARCH_ARTISTS:
		if (index < 0 || index >= result_count) {
			return;
		}
		job.kind = JOB_ARTIST_ALBUMS;
		snprintf(job.id, sizeof(job.id), "%s", result_artists[index].id);
		snprintf(job.text, sizeof(job.text), "%s", result_artists[index].name);
		break;

	case JOB_PLAYLISTS:
		if (index < 0 || index >= result_count) {
			return;
		}
		job.kind = JOB_PLAYLIST_SONGS;
		snprintf(job.id, sizeof(job.id), "%s", result_playlists[index].id);
		snprintf(job.text, sizeof(job.text), "%s", result_playlists[index].name);
		break;

	default:
		return;
	}

	run(&job, "loading");
}

// ---------------------------------------------------------------------------
// the worker thread
// ---------------------------------------------------------------------------

// Runs one job and returns how many entries this PAGE wrote, or -1. Entries go
// into the result arrays at the job's offset, so a second page appends instead
// of overwriting the first.
static int execute(const job_t *job) {
	const int base = job->offset > 0 ? job->offset : 0;
	const int room = MAX_HELD - base;
	if (room <= 0) {
		return 0;
	}

	switch (job->kind) {
	case JOB_LOGIN:
		if (!subsonic_login(job->server, job->text, job->text2)) {
			return -1;
		}
		return 0;

	case JOB_ALBUMS:
		return subsonic_album_list(job->text, base, PAGE_LIMIT, result_albums + base, room);

	case JOB_ARTISTS:
		return subsonic_artists(result_artists, MAX_HELD);

	case JOB_PLAYLISTS:
		return subsonic_playlists(result_playlists, MAX_HELD);

	case JOB_ARTIST_ALBUMS:
		return subsonic_artist_albums(job->id, result_albums, MAX_HELD);

	case JOB_ALBUM_SONGS:
		return subsonic_album_songs(job->id, result_songs, MAX_HELD);

	case JOB_PLAYLIST_SONGS:
		return subsonic_playlist_songs(job->id, base, result_songs + base, room);

	case JOB_STARRED_ALBUMS:
		return subsonic_starred_albums(base, PAGE_LIMIT, result_albums + base, room);

	case JOB_STARRED_SONGS:
		return subsonic_starred_songs(base, PAGE_LIMIT, result_songs + base, room);

	case JOB_SEARCH_SONGS:
		return subsonic_search_songs(job->text, base, PAGE_LIMIT, result_songs + base, room);

	case JOB_SEARCH_ALBUMS:
		return subsonic_search_albums(job->text, base, PAGE_LIMIT, result_albums + base, room);

	case JOB_SEARCH_ARTISTS:
		return subsonic_search_artists(job->text, base, PAGE_LIMIT, result_artists + base, room);

	default:
		return 0;
	}
}

static void *worker_main(void *arg) {
	(void)arg;
	thread_be_background("subsonic api");

	for (;;) {
		pthread_mutex_lock(&job_lock);
		while (!have_pending) {
			pthread_cond_wait(&job_ready, &job_lock);
		}
		job_t job = pending;
		have_pending = false;
		unsigned serial = job_serial;
		pthread_mutex_unlock(&job_lock);

		int count = execute(&job);

		// A later request has arrived while this one was in flight: its result
		// is what the page is waiting for, and this one is dropped.
		pthread_mutex_lock(&job_lock);
		bool stale = serial != job_serial;
		pthread_mutex_unlock(&job_lock);
		if (stale) {
			continue;
		}

		result_kind = job.kind;
		result_error[0] = '\0';

		if (count < 0) {
			snprintf(result_error, sizeof(result_error), "%s", subsonic_last_error());
			result_count = 0;
			result_from = 0;
			page_more = false;
		} else {
			result_from = job.offset > 0 ? job.offset : 0;
			result_count = result_from + count;
			// A page that came back full may have another behind it.
			page_more = kind_pages(job.kind) && count >= PAGE_LIMIT && result_count < MAX_HELD;
		}
		shown_job = job;

		// The title: what was asked for, so the page says where it is.
		if (job.text[0] && (job.kind == JOB_ALBUM_SONGS || job.kind == JOB_ARTIST_ALBUMS ||
							job.kind == JOB_PLAYLIST_SONGS || job.kind == JOB_SEARCH_SONGS ||
							job.kind == JOB_SEARCH_ALBUMS || job.kind == JOB_SEARCH_ARTISTS)) {
			snprintf(result_title, sizeof(result_title), "%s", job.text);
		} else {
			switch (job.kind) {
			case JOB_ALBUMS:
				if (!strcmp(job.text, "recent")) {
					snprintf(result_title, sizeof(result_title), "%s", tr("subsonic_recently_played"));
				} else if (!strcmp(job.text, "frequent")) {
					snprintf(result_title, sizeof(result_title), "%s", tr("subsonic_most_played"));
				} else if (!strcmp(job.text, "random")) {
					snprintf(result_title, sizeof(result_title), "%s", tr("subsonic_random"));
				} else if (!strcmp(job.text, "alphabeticalByName")) {
					snprintf(result_title, sizeof(result_title), "%s", tr("subsonic_alphabetical"));
				} else {
					snprintf(result_title, sizeof(result_title), "%s", tr("subsonic_recently_added"));
				}
				break;
			case JOB_ARTISTS:
				snprintf(result_title, sizeof(result_title), "%s", tr("artists"));
				break;
			case JOB_PLAYLISTS:
				snprintf(result_title, sizeof(result_title), "%s", tr("playlists"));
				break;
			case JOB_STARRED_ALBUMS:
				snprintf(result_title, sizeof(result_title), "%s", tr("subsonic_starred_albums"));
				break;
			case JOB_STARRED_SONGS:
				snprintf(result_title, sizeof(result_title), "%s", tr("favourites"));
				break;
			default:
				snprintf(result_title, sizeof(result_title), "%s", tr("subsonic"));
				break;
			}
		}

		gui_post(job_done_cb, NULL);
	}
}

static void start_worker(void) {
	pthread_mutex_lock(&job_lock);
	if (!worker_started) {
		worker_started = true;
		pthread_t thread;
		if (pthread_create(&thread, NULL, worker_main, NULL) == 0) {
			pthread_detach(thread);
		} else {
			worker_started = false;
		}
	}
	pthread_mutex_unlock(&job_lock);
}

// Whether a kind can ask for another page.
static bool kind_pages(job_kind_t kind) {
	return kind == JOB_ALBUMS || kind == JOB_STARRED_ALBUMS || kind == JOB_STARRED_SONGS ||
		   kind == JOB_SEARCH_SONGS || kind == JOB_SEARCH_ALBUMS || kind == JOB_SEARCH_ARTISTS ||
		   kind == JOB_PLAYLIST_SONGS;
}

static void maybe_load_more(void) {
	if (!page_more || page_loading || result_count >= MAX_HELD || !kind_pages(list_kind)) {
		return;
	}
	if (lv_obj_get_scroll_bottom(list_container) > 400) {
		return; // not near the end yet
	}

	// The request that produced the list, asked again for its next page: same
	// kind, same query, new offset. Anything else would show a different list.
	job_t job = shown_job;
	job.offset = result_count;

	page_loading = true;
	submit(&job);
}

// ---------------------------------------------------------------------------
// the pages
// ---------------------------------------------------------------------------

static lv_obj_t *user_field;
static lv_obj_t *password_field;
static lv_obj_t *server_field;
static lv_obj_t *password_eye_icon;
static keyboard_t *login_keyboard;

static void password_eye_refresh(void) {
	bool hidden = lv_textarea_get_password_mode(password_field);
	lv_image_set_src(password_eye_icon, hidden ? &icon_eye : &icon_eye_off);
}

static void password_eye_cb(lv_event_t *e) {
	(void)e;
	bool hidden = lv_textarea_get_password_mode(password_field);
	lv_textarea_set_password_mode(password_field, !hidden);
	password_eye_refresh();
}

// The keyboard writes into whichever of the three fields was touched last, and
// the touched one takes the focused state and the caret while the other two
// give both up. Without keyboard_set_field() the keys would keep typing into
// the server field whatever was tapped -- which is exactly how this page
// behaved before: the fields looked dead.
static void focus_field(lv_obj_t *field) {
	lv_obj_t *const all[3] = {server_field, user_field, password_field};
	keyboard_set_field(login_keyboard, field);
	for (int i = 0; i < 3; i++) {
		if (!all[i]) {
			continue;
		}
		if (all[i] == field) {
			lv_obj_add_state(all[i], LV_STATE_FOCUSED);
			keyboard_show_caret(all[i], true);
		} else {
			lv_obj_remove_state(all[i], LV_STATE_FOCUSED);
			keyboard_show_caret(all[i], false);
		}
	}
}

static void field_focus_cb(lv_event_t *e) { focus_field(lv_event_get_target(e)); }

static void login_accept_cb(lv_event_t *e) {
	(void)e;
	const char *server = lv_textarea_get_text(server_field);
	const char *user = lv_textarea_get_text(user_field);
	const char *password = lv_textarea_get_text(password_field);
	if (!server || !*server || !user || !*user || !password || !*password) {
		gui_notify_popup("credentials_required");
		return;
	}

	job_t job = {0};
	job.kind = JOB_LOGIN;
	snprintf(job.server, sizeof(job.server), "%s", server);
	snprintf(job.text, sizeof(job.text), "%s", user);
	snprintf(job.text2, sizeof(job.text2), "%s", password);
	run(&job, "subsonic_signing_in");

	// The password does not stay in the field a second longer than needed.
	lv_textarea_set_text(password_field, "");
}

static lv_obj_t *make_field(lv_obj_t *parent, gui_config_t *cfg, const char *placeholder, int y) {
	lv_obj_t *field = lv_textarea_create(parent);
	lv_textarea_set_one_line(field, true);
	lv_textarea_set_placeholder_text(field, tr(placeholder));
	lv_obj_set_size(field, cfg->screen_width - 2 * cfg->padding, 62);
	lv_obj_set_scrollbar_mode(field, LV_SCROLLBAR_MODE_OFF);
	lv_obj_align(field, LV_ALIGN_TOP_LEFT, cfg->padding, y);
	lv_obj_add_style(field, &theme_style_card, 0);
	lv_obj_set_style_radius(field, 12, 0);
	lv_obj_set_style_border_width(field, 0, 0);
	lv_obj_set_style_shadow_width(field, 0, 0);
	lv_obj_set_style_pad_all(field, 14, 0);
	lv_obj_set_style_text_font(field, &font_ui_24, 0);
	keyboard_style_caret(field);
	lv_obj_add_event_cb(field, field_focus_cb, LV_EVENT_CLICKED, NULL);
	return field;
}

static void build_login_page(gui_config_t *cfg) {
	login_screen = lv_obj_create(NULL);
	lv_obj_add_style(login_screen, &theme_style_screen, 0);

	settingsrow_title(login_screen, cfg, "subsonic_sign_in");
	int top = settingsrow_content_top(cfg);

	server_field = make_field(login_screen, cfg, "subsonic_server_address", top);
	user_field = make_field(login_screen, cfg, "subsonic_username", top + 78);
	password_field = make_field(login_screen, cfg, "password", top + 156);

	// The password as on any modern system: dots from the first keystroke, and
	// the eye to reveal the whole of it.
	keyboard_style_password(password_field, 0);
	lv_obj_set_style_pad_right(password_field, 60, 0);

	lv_obj_t *eye = lv_btn_create(login_screen);
	lv_obj_set_size(eye, 56, 56);
	lv_obj_align(eye, LV_ALIGN_TOP_RIGHT, -cfg->padding - 4, top + 156 + 3);
	lv_obj_set_style_bg_opa(eye, LV_OPA_TRANSP, 0);
	lv_obj_set_style_shadow_width(eye, 0, 0);
	lv_obj_set_style_border_width(eye, 0, 0);
	lv_obj_add_event_cb(eye, password_eye_cb, LV_EVENT_CLICKED, NULL);

	password_eye_icon = lv_image_create(eye);
	lv_obj_add_style(password_eye_icon, &theme_style_icon, 0);
	lv_obj_center(password_eye_icon);
	password_eye_refresh();

	lv_obj_t *note = lv_label_create(login_screen);
	lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(note, cfg->screen_width - 2 * cfg->padding);
	lv_obj_align(note, LV_ALIGN_TOP_LEFT, cfg->padding, top + 234);
	lv_obj_add_style(note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(note, &font_ui_18, 0);
	lv_label_set_text(note, tr("subsonic_login_note"));

	login_keyboard = keyboard_create(login_screen, cfg->screen_width, 316, server_field, NULL, "ok", login_accept_cb,
									 NULL);
	switcher_attach_back_gesture(login_screen);
}

static void open_login_cb(lv_event_t *e) {
	(void)e;
	lv_textarea_set_text(server_field, subsonic_server());
	lv_textarea_set_text(user_field, subsonic_display_name());
	lv_textarea_set_text(password_field, "");
	lv_textarea_set_password_mode(password_field, true);
	password_eye_refresh();
	keyboard_reset(login_keyboard);
	focus_field(server_field);
	switch_screen(login_screen);
}

// ---------------------------------------------------------------------------
// the main page
// ---------------------------------------------------------------------------

static lv_obj_t *home_container;
static lv_obj_t *settings_btn;
static lv_obj_t *search_btn;

static void update_corner_buttons(void) {
	bool visible = subsonic_logged_in();
	lv_obj_t *const buttons[] = {settings_btn, search_btn};
	for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++) {
		if (!buttons[i]) {
			continue;
		}
		lv_obj_set_hidden(buttons[i], !visible);
	}
}

static lv_obj_t *corner_button(gui_config_t *cfg, int slot, const lv_image_dsc_t *glyph, lv_event_cb_t cb) {
	lv_obj_t *button = lv_btn_create(subsonic_screen);
	lv_obj_set_size(button, 56, 56);
	lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(button, 0, 0);
	lv_obj_set_style_shadow_width(button, 0, 0);
	lv_obj_set_style_pad_all(button, 0, 0);
	lv_obj_align(button, LV_ALIGN_TOP_RIGHT, -cfg->padding - slot * (56 + 6), cfg->padding + cfg->top_bar_height);

	lv_obj_t *icon = lv_image_create(button);
	lv_image_set_src(icon, glyph);
	lv_obj_add_style(icon, &theme_style_icon, 0);
	lv_obj_center(icon);

	lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, NULL);
	return button;
}

// ---------------------------------------------------------------------------
// settings
// ---------------------------------------------------------------------------

static lv_obj_t *quality_pills[5];

// 0 keeps the file as the server has it. The rest ask for MP3 at that rate,
// which is what makes a large FLAC library usable over a weak link.
static const int BITRATE_VALUES[5] = {0, 320, 256, 192, 128};
static const char *const BITRATE_NAMES[5] = {"subsonic_original", "320 kbps", "256 kbps", "192 kbps", "128 kbps"};

static void quality_refresh(void) {
	int current = subsonic_get_bitrate();
	for (int i = 0; i < 5; i++) {
		if (!quality_pills[i]) {
			continue;
		}
		bool on = BITRATE_VALUES[i] == current;
		lv_obj_set_style_bg_color(quality_pills[i], on ? theme()->accent : theme()->surface_pressed, 0);
		lv_obj_set_style_text_color(lv_obj_get_child(quality_pills[i], 0), on ? lv_color_white() : theme()->text_primary,
									0);
	}
}

// ---------------------------------------------------------------------------
// what happens to a track once it has been played
//
// Two positions, and the same pill shape as the quality row below: "Stream"
// keeps nothing on the card, "Keep" stores what was played. The card in the
// middle of the page explains the difference, because the names alone do not
// say that one of them leaves files behind.
// ---------------------------------------------------------------------------

static lv_obj_t *keep_pills[2];

static void keep_refresh(void) {
	bool keep = subsoniccache_keep_downloads();
	for (int i = 0; i < 2; i++) {
		if (!keep_pills[i]) {
			continue;
		}
		// Pill 0 is Stream, pill 1 is Keep: "on" is whichever matches.
		bool on = (i == 1) == keep;
		lv_obj_set_style_bg_color(keep_pills[i], on ? theme()->accent : theme()->surface_pressed, 0);
		lv_obj_set_style_text_color(lv_obj_get_child(keep_pills[i], 0), on ? lv_color_white() : theme()->text_primary,
									0);
	}
}

static void keep_pick_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return;
	}
	subsoniccache_set_keep_downloads((int)(intptr_t)lv_event_get_user_data(e) != 0);
	keep_refresh();
}

static void add_keep_card(lv_obj_t *container) {
	lv_obj_t *card = lv_obj_create(container);
	lv_obj_set_width(card, lv_pct(100));
	lv_obj_set_height(card, LV_SIZE_CONTENT);
	lv_obj_add_style(card, &theme_style_card, 0);
	lv_obj_set_style_radius(card, 12, 0);
	lv_obj_set_style_border_width(card, 0, 0);
	lv_obj_set_style_shadow_width(card, 0, 0);
	lv_obj_set_style_pad_all(card, 20, 0);
	lv_obj_set_style_pad_row(card, 18, 0);
	lv_obj_set_scrollable(card, false);
	lv_obj_set_event_bubble(card, true);
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

	lv_obj_t *name = lv_label_create(card);
	lv_label_set_text(name, tr("subsonic_playback"));
	lv_obj_add_style(name, &theme_style_text, 0);
	lv_obj_set_style_text_font(name, &font_ui_24, 0);

	lv_obj_t *pills = lv_obj_create(card);
	lv_obj_set_size(pills, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(pills, 0, 0);
	lv_obj_set_style_border_width(pills, 0, 0);
	lv_obj_set_style_pad_all(pills, 0, 0);
	lv_obj_set_style_pad_gap(pills, 12, 0);
	lv_obj_set_scrollable(pills, false);
	lv_obj_set_event_bubble(pills, true);
	lv_obj_set_flex_flow(pills, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(pills, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);

	static const char *const KEEP_NAMES[2] = {"subsonic_stream", "subsonic_keep"};
	for (int i = 0; i < 2; i++) {
		lv_obj_t *btn = lv_btn_create(pills);
		lv_obj_set_size(btn, LV_SIZE_CONTENT, 56);
		lv_obj_set_style_pad_hor(btn, 22, 0);
		lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_shadow_width(btn, 0, 0);
		lv_obj_set_style_border_width(btn, 0, 0);
		lv_obj_add_event_cb(btn, keep_pick_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

		lv_obj_t *label = lv_label_create(btn);
		lv_label_set_text(label, tr(KEEP_NAMES[i]));
		lv_obj_set_style_text_font(label, &font_ui_22, 0);
		lv_obj_center(label);

		keep_pills[i] = btn;
	}

	lv_obj_t *note = lv_label_create(card);
	lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(note, lv_pct(100));
	lv_obj_add_style(note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(note, &font_ui_18, 0);
	lv_label_set_text(note, tr("subsonic_playback_note"));
}

static void quality_pick_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return;
	}
	subsonic_set_bitrate((int)(intptr_t)lv_event_get_user_data(e));
	quality_refresh();
}

static void do_logout(void *user) {
	(void)user;
	subsonic_logout();
	// The tracks downloaded for that account are of no use to anyone.
	subsoniccache_clear();
	qobuzart_clear();
	rebuild_home();
	toast_success("signed_out");
}

static void logout_cb(lv_event_t *e) {
	(void)e;
	char message[160];
	snprintf(message, sizeof(message), tr("sign_out_confirm_note"), subsonic_display_name());
	confirm_show("subsonic_sign_out", message, "leave", do_logout, NULL);
}

static void open_settings_cb(lv_event_t *e) {
	(void)e;
	quality_refresh();
	switch_screen(settings_screen);
}

static void build_settings_page(gui_config_t *cfg) {
	settings_screen = lv_obj_create(NULL);
	lv_obj_t *container = settingsrow_page(settings_screen, cfg, "subsonic_settings");

	// Streaming versus keeping, first: it is the setting that decides whether
	// the card fills up, so it belongs above the one about how good it sounds.
	add_keep_card(container);

	lv_obj_t *card = lv_obj_create(container);
	lv_obj_set_width(card, lv_pct(100));
	lv_obj_set_height(card, LV_SIZE_CONTENT);
	lv_obj_add_style(card, &theme_style_card, 0);
	lv_obj_set_style_radius(card, 12, 0);
	lv_obj_set_style_border_width(card, 0, 0);
	lv_obj_set_style_shadow_width(card, 0, 0);
	lv_obj_set_style_pad_all(card, 20, 0);
	lv_obj_set_style_pad_row(card, 18, 0);
	lv_obj_set_scrollable(card, false);
	lv_obj_set_event_bubble(card, true);
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

	lv_obj_t *name = lv_label_create(card);
	lv_label_set_text(name, tr("subsonic_streaming_quality"));
	lv_obj_add_style(name, &theme_style_text, 0);
	lv_obj_set_style_text_font(name, &font_ui_24, 0);

	lv_obj_t *pills = lv_obj_create(card);
	lv_obj_set_size(pills, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(pills, 0, 0);
	lv_obj_set_style_border_width(pills, 0, 0);
	lv_obj_set_style_pad_all(pills, 0, 0);
	lv_obj_set_style_pad_gap(pills, 12, 0);
	lv_obj_set_scrollable(pills, false);
	lv_obj_set_event_bubble(pills, true);
	lv_obj_set_flex_flow(pills, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_flex_align(pills, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);

	for (int i = 0; i < 5; i++) {
		lv_obj_t *btn = lv_btn_create(pills);
		lv_obj_set_size(btn, LV_SIZE_CONTENT, 56);
		lv_obj_set_style_pad_hor(btn, 22, 0);
		lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_shadow_width(btn, 0, 0);
		lv_obj_set_style_border_width(btn, 0, 0);
		lv_obj_add_event_cb(btn, quality_pick_cb, LV_EVENT_CLICKED, (void *)(intptr_t)BITRATE_VALUES[i]);

		lv_obj_t *label = lv_label_create(btn);
		lv_label_set_text(label, tr(BITRATE_NAMES[i]));
		lv_obj_set_style_text_font(label, &font_ui_22, 0);
		lv_obj_center(label);

		quality_pills[i] = btn;
	}

	settingsrow_action(container, "leave", logout_cb, NULL);

	quality_refresh();
	keep_refresh();
	theme_register_refresh(quality_refresh);
	theme_register_refresh(keep_refresh);
	switcher_attach_back_gesture(settings_screen);
}

// ---------------------------------------------------------------------------
// search
// ---------------------------------------------------------------------------

static lv_obj_t *search_field;
static lv_obj_t *search_clear_btn;
static lv_obj_t *search_pills[3];
static keyboard_t *search_keyboard;
static int search_kind_index;

static void paint_search_pills(void) {
	for (int i = 0; i < 3; i++) {
		if (!search_pills[i]) {
			continue;
		}
		bool on = i == search_kind_index;
		lv_obj_set_style_bg_color(search_pills[i], on ? theme()->accent : theme()->surface_pressed, 0);
		lv_obj_set_style_text_color(lv_obj_get_child(search_pills[i], 0), on ? lv_color_white() : theme()->text_primary,
									0);
	}
}

static void search_pill_cb(lv_event_t *e) {
	search_kind_index = (int)(intptr_t)lv_event_get_user_data(e);
	paint_search_pills();
}

static void refresh_search_clear(void) {
	if (!search_clear_btn) {
		return;
	}
	const char *text = lv_textarea_get_text(search_field);
	lv_obj_set_hidden(search_clear_btn, !(text && text[0]));
}

static void search_field_changed_cb(lv_event_t *e) {
	(void)e;
	refresh_search_clear();
}

static void search_clear_cb(lv_event_t *e) {
	(void)e;
	lv_textarea_set_text(search_field, "");
	keyboard_reset(search_keyboard);
	lv_obj_add_state(search_field, LV_STATE_FOCUSED);
	lv_obj_send_event(search_field, LV_EVENT_FOCUSED, NULL);
	refresh_search_clear();
}

static void search_accept_cb(lv_event_t *e) {
	(void)e;
	const char *text = lv_textarea_get_text(search_field);
	if (!text || !*text) {
		return;
	}

	job_t job = {0};
	job.kind = search_kind_index == 1 ? JOB_SEARCH_ALBUMS
									  : (search_kind_index == 2 ? JOB_SEARCH_ARTISTS : JOB_SEARCH_SONGS);
	snprintf(job.text, sizeof(job.text), "%s", text);
	run(&job, "searching");
}

static void open_search_cb(lv_event_t *e) {
	(void)e;
	lv_textarea_set_text(search_field, "");
	keyboard_reset(search_keyboard);
	lv_obj_add_state(search_field, LV_STATE_FOCUSED);
	lv_obj_send_event(search_field, LV_EVENT_FOCUSED, NULL);
	refresh_search_clear();
	switch_screen(search_screen);
}

static void build_search_page(gui_config_t *cfg) {
	search_screen = lv_obj_create(NULL);
	lv_obj_add_style(search_screen, &theme_style_screen, 0);

	settingsrow_title(search_screen, cfg, "subsonic_search");
	int top = settingsrow_content_top(cfg);

	search_field = lv_textarea_create(search_screen);
	lv_textarea_set_one_line(search_field, true);
	lv_textarea_set_placeholder_text(search_field, tr("artist_album_or_track"));
	lv_obj_set_size(search_field, cfg->screen_width - 2 * cfg->padding, 62);
	lv_obj_set_scrollbar_mode(search_field, LV_SCROLLBAR_MODE_OFF);
	lv_obj_align(search_field, LV_ALIGN_TOP_LEFT, cfg->padding, top);
	lv_obj_add_style(search_field, &theme_style_card, 0);
	lv_obj_set_style_radius(search_field, 12, 0);
	lv_obj_set_style_border_width(search_field, 0, 0);
	lv_obj_set_style_shadow_width(search_field, 0, 0);
	lv_obj_set_style_pad_all(search_field, 14, 0);
	lv_obj_set_style_text_font(search_field, &font_ui_24, 0);
	keyboard_style_caret(search_field);
	lv_obj_add_event_cb(search_field, search_field_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

	search_clear_btn = lv_btn_create(search_screen);
	lv_obj_set_size(search_clear_btn, 56, 56);
	lv_obj_align(search_clear_btn, LV_ALIGN_TOP_RIGHT, -cfg->padding - 4, top + (62 - 56) / 2);
	lv_obj_set_style_bg_opa(search_clear_btn, LV_OPA_TRANSP, 0);
	lv_obj_set_style_shadow_width(search_clear_btn, 0, 0);
	lv_obj_set_style_border_width(search_clear_btn, 0, 0);
	lv_obj_set_style_pad_all(search_clear_btn, 0, 0);
	lv_obj_set_hidden(search_clear_btn, true);
	lv_obj_add_event_cb(search_clear_btn, search_clear_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *clear_icon = lv_image_create(search_clear_btn);
	lv_image_set_src(clear_icon, &icon_clear);
	lv_obj_add_style(clear_icon, &theme_style_icon, 0);
	lv_obj_set_style_image_opa(clear_icon, LV_OPA_70, 0);
	lv_obj_center(clear_icon);

	// Room for it, so a long query does not run underneath.
	lv_obj_set_style_pad_right(search_field, 60, 0);

	static const char *const KINDS[] = {"tracks_2", "albums", "artists"};
	for (int i = 0; i < 3; i++) {
		lv_obj_t *pill = lv_btn_create(search_screen);
		lv_obj_set_size(pill, 140, 52);
		lv_obj_set_style_radius(pill, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_shadow_width(pill, 0, 0);
		lv_obj_set_style_border_width(pill, 0, 0);
		lv_obj_align(pill, LV_ALIGN_TOP_LEFT, cfg->padding + i * 150, top + 78);
		lv_obj_add_event_cb(pill, search_pill_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

		lv_obj_t *label = lv_label_create(pill);
		lv_label_set_text(label, tr(KINDS[i]));
		lv_obj_set_style_text_font(label, &font_ui_20, 0);
		lv_obj_center(label);
		search_pills[i] = pill;
	}
	paint_search_pills();

	search_keyboard = keyboard_create(search_screen, cfg->screen_width, 316, search_field, NULL, "ok",
									  search_accept_cb, NULL);
	switcher_attach_back_gesture(search_screen);
	theme_register_refresh(paint_search_pills);
}

// ---------------------------------------------------------------------------
// home
// ---------------------------------------------------------------------------

static void section_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return;
	}
	job_t job = {0};
	job.kind = (job_kind_t)(intptr_t)lv_event_get_user_data(e);
	run(&job, "loading");
}

// The album lists: the section, and the protocol's own type string for it.
static void album_section_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return;
	}
	job_t job = {0};
	job.kind = JOB_ALBUMS;
	snprintf(job.text, sizeof(job.text), "%s", (const char *)lv_event_get_user_data(e));
	run(&job, "loading");
}

static void build_home_sections(void);

static void build_home_sections(void) {
	static const char *const TYPES[] = {"newest", "recent", "frequent", "alphabeticalByName", "random"};
	static const char *const NAMES[] = {"subsonic_recently_added", "subsonic_recently_played",
										"subsonic_most_played", "subsonic_alphabetical", "subsonic_random"};

	for (int i = 0; i < 5; i++) {
		lv_obj_t *row = settingsrow_add(home_container, NAMES[i], NULL, album_section_cb, (void *)TYPES[i]);
		lv_obj_set_style_margin_top(row, 8, 0);
	}

	lv_obj_t *gap = lv_obj_create(home_container);
	lv_obj_set_size(gap, lv_pct(100), 16);
	lv_obj_set_style_bg_opa(gap, 0, 0);
	lv_obj_set_style_border_width(gap, 0, 0);

	settingsrow_add(home_container, "artists", NULL, section_cb, (void *)(intptr_t)JOB_ARTISTS);
	settingsrow_add(home_container, "playlists", NULL, section_cb, (void *)(intptr_t)JOB_PLAYLISTS);
	settingsrow_add(home_container, "subsonic_starred_albums", NULL, section_cb, (void *)(intptr_t)JOB_STARRED_ALBUMS);
	settingsrow_add(home_container, "favourites", NULL, section_cb, (void *)(intptr_t)JOB_STARRED_SONGS);
}

// Whether the network is actually up. There is nothing to show without it:
// every list comes from the server, and with no IP address each request ends in
// a generic error that does not tell the user the one thing they need to know.
static bool wifi_is_connected(void) {
#ifdef HOST_BUILD
	// The simulator runs on a machine that has a network but no wlan0: without
	// this the page would not be testable.
	return true;
#else
	wifi_status_t status;
	wifi_get_status(&status);
	return status.state == WIFI_STATE_CONNECTED && status.ip[0] != '\0';
#endif
}

static void open_wifi_settings_cb(lv_event_t *e) {
	(void)e;
	switch_screen(wifisettings_screen);
}

static void rebuild_home(void) {
	lv_obj_clean(home_container);

	// The network comes before the login, and not the other way round: signing
	// in is a conversation with the server, so with no network the "Sign in" row
	// could only fail.
	if (!wifi_is_connected()) {
		update_corner_buttons();
		lv_obj_t *note = lv_label_create(home_container);
		lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
		lv_obj_set_width(note, lv_pct(100));
		lv_obj_add_style(note, &theme_style_text, 0);
		lv_obj_set_style_text_font(note, &font_ui_22, 0);
		lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_set_style_pad_hor(note, 4, 0);
		lv_obj_set_style_pad_top(note, 14, 0);
		lv_label_set_text(note, tr("enable_wi_fi_first"));
		lv_obj_t *row = settingsrow_add(home_container, "wi_fi_settings", NULL, open_wifi_settings_cb, NULL);
		lv_obj_set_style_margin_top(row, 16, 0);
		return;
	}

	if (!subsonic_logged_in()) {
		update_corner_buttons();
		settingsrow_add(home_container, "subsonic_sign_in", NULL, open_login_cb, NULL);
		return;
	}

	// The server in use, at the top: with more than one music server in a
	// house, "which one am I looking at" is a real question.
	lv_obj_t *where = lv_label_create(home_container);
	lv_label_set_long_mode(where, LV_LABEL_LONG_DOT);
	lv_obj_set_width(where, lv_pct(100));
	lv_obj_add_style(where, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(where, &font_ui_18, 0);
	lv_obj_set_style_pad_left(where, 4, 0);
	lv_obj_set_style_pad_bottom(where, 6, 0);
	lv_label_set_text(where, subsonic_server());

	build_home_sections();
	update_corner_buttons();
}

bool subsonicpage_open_album(const char *album_id, const char *title) {
	if (!album_id || !album_id[0] || !subsonic_logged_in()) {
		return false;
	}
	job_t job = {0};
	job.kind = JOB_ALBUM_SONGS;
	snprintf(job.id, sizeof(job.id), "%s", album_id);
	snprintf(job.text, sizeof(job.text), "%s", title && title[0] ? title : tr("subsonic"));
	run(&job, "loading");
	return true;
}

bool subsonicpage_owns(const char *path) { return subsoniccache_owns(path); }

// ---------------------------------------------------------------------------

static void home_loaded_cb(lv_event_t *e) {
	(void)e;
	// The card may have been inserted or removed since the page was built, and
	// without a card there is nowhere to download to.
	subsoniccache_set_root(storage_sd_root());
	qobuzart_set_root(storage_sd_root());
	rebuild_home();
}

static void list_loaded_cb(lv_event_t *e) {
	(void)e;
	subsoniccache_set_root(storage_sd_root());
	art_request_visible();
}

static void list_unloaded_cb(lv_event_t *e) {
	(void)e;
	art_forget_rows();
}

static void build_list_page(gui_config_t *cfg) {
	list_screen = lv_obj_create(NULL);
	lv_obj_add_event_cb(list_screen, list_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(list_screen, list_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
	list_container = settingsrow_page(list_screen, cfg, "subsonic");
	list_title = settingsrow_page_title(list_screen);

	list_empty = lv_label_create(list_screen);
	lv_label_set_text(list_empty, tr("no_results"));
	lv_obj_set_style_text_align(list_empty, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(list_empty, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(list_empty, &font_ui_24, 0);
	lv_obj_align(list_empty, LV_ALIGN_TOP_MID, 0, settingsrow_content_top(cfg) + 100);
	lv_obj_set_hidden(list_empty, true);

	lv_obj_add_event_cb(list_container, list_scrolled_cb, LV_EVENT_SCROLL, NULL);
	switcher_attach_back_gesture(list_screen);
	// The leftward drag that pulls in the player sheet, as on every other
	// browsable page.
	player_sheet_attach_drag(list_screen, true);
	player_sheet_attach_drag(list_container, true);
}

void subsonicpage_init(gui_config_t *cfg) {
	subsonic_init();

	subsonic_screen = lv_obj_create(NULL);
	home_container = settingsrow_page(subsonic_screen, cfg, "subsonic");

	// Two buttons top right, like the Music gear: settings and search. The title
	// must be told the corner is taken, otherwise its text runs under them.
	settingsrow_title_corner_slots(settingsrow_page_title(subsonic_screen), cfg, 2);
	settings_btn = corner_button(cfg, 0, &icon_music_settings, open_settings_cb);
	search_btn = corner_button(cfg, 1, &icon_search, open_search_cb);

	build_list_page(cfg);
	build_search_page(cfg);
	build_settings_page(cfg);
	build_login_page(cfg);
	build_busy_layer();

	subsoniccache_set_slow_start_cb(NULL);
	device_state_add_prepare_cb(subsonic_prepare_track);

	// Created paused: the first cover request starts it.
	art_timer = lv_timer_create(art_poll_cb, 120, NULL);
	lv_timer_pause(art_timer);

	// The queue watchdog: which track to download next, and the restart of a
	// queue parked on a missing file. One second is enough -- this is about
	// tracks, not frames.
	lv_timer_create(queue_watch_cb, 1000, NULL);
	start_prefetch_thread();

	qobuzart_set_root(storage_sd_root());
	subsoniccache_set_root(storage_sd_root());

	start_worker();
	rebuild_home();

	lv_obj_add_event_cb(subsonic_screen, home_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	switcher_attach_back_gesture(subsonic_screen);
	player_sheet_attach_drag(subsonic_screen, true);
	player_sheet_attach_drag(home_container, true);
}
