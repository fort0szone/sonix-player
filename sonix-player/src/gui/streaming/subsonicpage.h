#ifndef SUBSONICPAGE_H
#define SUBSONICPAGE_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The Subsonic page: sign in, then browse the server's albums, artists and
// playlists, search it, and play from it.
//
// The shape is Qobuz's, deliberately: a home page of rows that each open a
// list, one list page reused for every kind of result, a search page and a
// settings page. A user who has used one service already knows the other.
extern lv_obj_t *subsonic_screen;

void subsonicpage_init(gui_config_t *cfg);

// Opens an album's track list, for "show album" in the player. False when not
// signed in.
bool subsonicpage_open_album(const char *album_id, const char *title);

// Whether a path is one of this page's cached tracks, for the player's badge
// and its "show album" row.
bool subsonicpage_owns(const char *path);

#endif /* SUBSONICPAGE_H */
