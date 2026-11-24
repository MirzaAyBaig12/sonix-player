#include "podcastdownloads.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/confirm.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/gui/streaming/podcastauto.h"
#include "src/system/core/lang.h"
#include "src/system/streaming/podcast.h"
#include "src/system/streaming/podcastdl.h"
#include "src/system/streaming/podcastoptions.h"

lv_obj_t *podcastdownloads_screen;

#define LIST_MAX 24

static const int KEEP_VALUES[] = {5, 10, 15, 20, 25};
#define KEEP_COUNT ((int)(sizeof(KEEP_VALUES) / sizeof(KEEP_VALUES[0])))

static long long setting_feed_id;
static char setting_title[PODCAST_TITLE_MAX];

static lv_obj_t *name_label;
static lv_obj_t *no_feed_note;
static lv_obj_t *last_sync_label;
static lv_obj_t *auto_row;
static lv_obj_t *auto_switch;
static lv_obj_t *heard_row;
static lv_obj_t *heard_switch;
static lv_obj_t *auto_keep_row;
static lv_obj_t *auto_keep_pills[KEEP_COUNT];
static lv_obj_t *manual_keep_row;
static lv_obj_t *manual_keep_pills[KEEP_COUNT];
static lv_obj_t *scope_row;
static lv_obj_t *scope_pills[2];
static lv_obj_t *keep_note;

// The list of one kind of download. Its rows are made once: rebuilding them
// every time the page opens would leak the action records settingsrow_add()
// keeps for the life of the run.
static lv_obj_t *list_screen;
static lv_obj_t *list_container;
static lv_obj_t *list_title;
static lv_obj_t *list_empty;
static lv_obj_t *list_rows[LIST_MAX];
static podcastdl_file_t list_items[LIST_MAX];
static int list_count;
static podcastdl_kind_t list_kind;

// ---------------------------------------------------------------------------
// the main page
// ---------------------------------------------------------------------------

static void refresh_rows(void) {
	bool online = setting_feed_id > 0;
	lv_obj_set_hidden(name_label, !online);
	lv_obj_set_hidden(no_feed_note, online);
	lv_obj_set_hidden(auto_row, !online);
	lv_obj_set_hidden(heard_row, !online);
	lv_obj_set_hidden(auto_keep_row, !online);
	lv_obj_set_hidden(manual_keep_row, !online);
	lv_obj_set_hidden(scope_row, !online);
	lv_obj_set_hidden(keep_note, !online);

	if (name_label) {
		lv_label_set_text(name_label, setting_title[0] ? setting_title : tr("podcasts"));
	}
	if (last_sync_label) {
		time_t last = podcastauto_last_sync();
		char value[32];
		if (last <= 0) {
			snprintf(value, sizeof(value), "%s", tr("never"));
		} else {
			struct tm *tm = localtime(&last);
			if (tm && strftime(value, sizeof(value), "%Y-%m-%d %H:%M", tm) > 0) {
				// absolute, so the row is useful even after a reboot
			} else {
				snprintf(value, sizeof(value), "%s", tr("unknown"));
			}
		}
		char text[96];
		snprintf(text, sizeof(text), tr("podcast_last_sync"), value);
		lv_label_set_text(last_sync_label, text);
	}
	if (auto_switch) {
		if (podcast_options_auto(setting_feed_id)) {
			lv_obj_add_state(auto_switch, LV_STATE_CHECKED);
		} else {
			lv_obj_remove_state(auto_switch, LV_STATE_CHECKED);
		}
	}
	if (heard_switch) {
		if (podcast_options_delete_heard(setting_feed_id)) {
			lv_obj_add_state(heard_switch, LV_STATE_CHECKED);
		} else {
			lv_obj_remove_state(heard_switch, LV_STATE_CHECKED);
		}
	}
	int auto_keep = podcast_options_keep_auto(setting_feed_id);
	int manual_keep = podcast_options_keep_manual(setting_feed_id);
	for (int i = 0; i < KEEP_COUNT; i++) {
		settingsrow_pill_active(auto_keep_pills[i], KEEP_VALUES[i] == auto_keep);
		settingsrow_pill_active(manual_keep_pills[i], KEEP_VALUES[i] == manual_keep);
	}
	bool all = podcast_options_delete_all(setting_feed_id);
	settingsrow_pill_active(scope_pills[0], !all);
	settingsrow_pill_active(scope_pills[1], all);
}

static void auto_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return;
	}
	bool on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
	podcast_options_set_auto(setting_feed_id, on);
	// Switching it on here should not wait for the next time Wi-Fi comes up.
	if (on) {
		podcastauto_sync_feed(setting_feed_id, setting_title);
	}
	refresh_rows();
}

static void heard_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return;
	}
	podcast_options_set_delete_heard(setting_feed_id, lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
	refresh_rows();
}

static void auto_keep_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return;
	}
	int keep = (int)(intptr_t)lv_event_get_user_data(e);
	podcast_options_set_keep_auto(setting_feed_id, keep);
	podcastdl_trim(setting_title, PODCASTDL_KIND_AUTO, keep);
	refresh_rows();
}

static void manual_keep_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return;
	}
	int keep = (int)(intptr_t)lv_event_get_user_data(e);
	podcast_options_set_keep_manual(setting_feed_id, keep);
	podcastdl_trim(setting_title, PODCASTDL_KIND_MANUAL, keep);
	refresh_rows();
}

static void scope_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return;
	}
	podcast_options_set_delete_all(setting_feed_id, (intptr_t)lv_event_get_user_data(e) != 0);
	refresh_rows();
}

// ---------------------------------------------------------------------------
// the two manage lists
// ---------------------------------------------------------------------------

static void refresh_list(void) {
	list_count = podcastdl_list(setting_title, list_kind, list_items, LIST_MAX);
	for (int i = 0; i < LIST_MAX; i++) {
		lv_obj_set_hidden(list_rows[i], i >= list_count);
		if (i < list_count) {
			lv_label_set_text(settingsrow_name_label(list_rows[i]), list_items[i].title);
		}
	}
	lv_obj_set_hidden(list_empty, list_count > 0);
}

static void manage_open(podcastdl_kind_t kind) {
	list_kind = kind;
	lv_label_set_text(list_title,
					  tr(kind == PODCASTDL_KIND_AUTO ? "podcast_manage_auto" : "podcast_manage_manual"));
	lv_obj_scroll_to_y(list_screen, 0, LV_ANIM_OFF);
	refresh_list();
	switch_screen(list_screen);
}

static void manage_auto_cb(lv_event_t *e) {
	(void)e;
	if (switcher_back_drag_active()) {
		return;
	}
	manage_open(PODCASTDL_KIND_AUTO);
}

static void manage_manual_cb(lv_event_t *e) {
	(void)e;
	if (switcher_back_drag_active()) {
		return;
	}
	manage_open(PODCASTDL_KIND_MANUAL);
}

static void delete_one_done(void *user) {
	int i = (int)(intptr_t)user;
	if (i >= 0 && i < list_count && podcastdl_delete_download(list_items[i].path)) {
		toast_success("podcast_download_deleted");
	}
	refresh_list();
}

static void delete_one_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return;
	}
	int i = (int)(intptr_t)lv_event_get_user_data(e);
	if (i < 0 || i >= list_count) {
		return;
	}
	confirm_show(tr("podcast_delete_one"), list_items[i].title, tr("delete"), delete_one_done, (void *)(intptr_t)i);
}

// ---------------------------------------------------------------------------
// the bulk delete at the end of the page
// ---------------------------------------------------------------------------

static void bulk_delete_done(void *user) {
	(void)user;
	podcastdl_kind_t kind = podcast_options_delete_all(setting_feed_id) ? PODCASTDL_KIND_ALL : PODCASTDL_KIND_AUTO;
	int removed = podcastdl_delete_all(setting_title, kind);
	char text[96];
	snprintf(text, sizeof(text), tr("podcast_downloads_deleted"), removed);
	toast_success(text);
	refresh_rows();
}

static void bulk_delete_cb(lv_event_t *e) {
	(void)e;
	if (switcher_back_drag_active()) {
		return;
	}
	int automatic = podcastdl_count(setting_title, PODCASTDL_KIND_AUTO);
	int manual = podcastdl_count(setting_title, PODCASTDL_KIND_MANUAL);
	char message[160];
	snprintf(message, sizeof(message), tr("podcast_delete_downloads_msg"), automatic, manual);
	confirm_show(tr("podcast_delete_downloads"), message, tr("delete"), bulk_delete_done, NULL);
}

// ---------------------------------------------------------------------------
// build
// ---------------------------------------------------------------------------

static void loaded_cb(lv_event_t *e) {
	(void)e;
	refresh_rows();
}

void podcastdownloads_open(long long feed_id, const char *title) {
	setting_feed_id = feed_id > 0 ? feed_id : 0;
	snprintf(setting_title, sizeof(setting_title), "%s", title ? title : "");
	lv_obj_scroll_to_y(podcastdownloads_screen, 0, LV_ANIM_OFF);
	refresh_rows();
	switch_screen(podcastdownloads_screen);
}

void podcastdownloads_init(gui_config_t *cfg) {
	podcastdownloads_screen = lv_obj_create(NULL);
	lv_obj_add_style(podcastdownloads_screen, &theme_style_screen, 0);
	lv_obj_t *container = settingsrow_page(podcastdownloads_screen, cfg, "podcast_downloads");

	name_label = lv_label_create(container);
	lv_label_set_long_mode(name_label, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(name_label, lv_pct(100));
	lv_obj_add_style(name_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(name_label, &font_ui_20, 0);
	lv_label_set_text(name_label, "");

	no_feed_note = lv_label_create(container);
	lv_label_set_long_mode(no_feed_note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(no_feed_note, lv_pct(100));
	lv_obj_add_style(no_feed_note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(no_feed_note, &font_ui_18, 0);
	lv_label_set_text(no_feed_note, tr("podcast_feed_settings_no_feed"));
	lv_obj_set_hidden(no_feed_note, true);

	last_sync_label = lv_label_create(container);
	lv_label_set_long_mode(last_sync_label, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(last_sync_label, lv_pct(100));
	lv_obj_add_style(last_sync_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(last_sync_label, &font_ui_18, 0);
	lv_label_set_text(last_sync_label, "");

	auto_row = settingsrow_toggle(container, "podcast_auto_download", &auto_switch, auto_cb);
	heard_row = settingsrow_toggle(container, "podcast_delete_heard", &heard_switch, heard_cb);

	settingsrow_action(container, "podcast_manage_auto", manage_auto_cb, NULL);
	settingsrow_action(container, "podcast_manage_manual", manage_manual_cb, NULL);

	settingsrow_pills(container, "podcast_auto_keep", &auto_keep_row);
	for (int i = 0; i < KEEP_COUNT; i++) {
		char text[8];
		snprintf(text, sizeof(text), "%d", KEEP_VALUES[i]);
		auto_keep_pills[i] = settingsrow_pill_text(auto_keep_row, text, KEEP_VALUES[i], auto_keep_cb);
	}
	settingsrow_pills(container, "podcast_manual_keep", &manual_keep_row);
	for (int i = 0; i < KEEP_COUNT; i++) {
		char text[8];
		snprintf(text, sizeof(text), "%d", KEEP_VALUES[i]);
		manual_keep_pills[i] = settingsrow_pill_text(manual_keep_row, text, KEEP_VALUES[i], manual_keep_cb);
	}

	keep_note = lv_label_create(container);
	lv_label_set_long_mode(keep_note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(keep_note, lv_pct(100));
	lv_obj_add_style(keep_note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(keep_note, &font_ui_18, 0);
	lv_label_set_text(keep_note, tr("podcast_keep_episodes_note"));

	// At the very end: what the bulk delete is allowed to remove, then the
	// delete itself.
	settingsrow_pills(container, "podcast_only_delete", &scope_row);
	scope_pills[0] = settingsrow_pill_text(scope_row, tr("podcast_only_auto"), 0, scope_cb);
	scope_pills[1] = settingsrow_pill_text(scope_row, tr("podcast_only_all"), 1, scope_cb);
	settingsrow_action(container, "podcast_delete_downloads", bulk_delete_cb, NULL);

	lv_obj_add_event_cb(podcastdownloads_screen, loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	theme_register_refresh(refresh_rows);
	switcher_attach_back_gesture(podcastdownloads_screen);

	// The manage page: a fixed pool of rows, hidden or named as the folder reads.
	list_screen = lv_obj_create(NULL);
	lv_obj_add_style(list_screen, &theme_style_screen, 0);
	list_container = settingsrow_page(list_screen, cfg, "podcast_manage_downloads");
	list_title = settingsrow_page_title(list_screen);
	for (int i = 0; i < LIST_MAX; i++) {
		list_rows[i] = settingsrow_action(list_container, "", delete_one_cb, (void *)(intptr_t)i);
		lv_obj_set_hidden(list_rows[i], true);
	}
	list_empty = lv_label_create(list_container);
	lv_label_set_long_mode(list_empty, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(list_empty, lv_pct(100));
	lv_obj_add_style(list_empty, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(list_empty, &font_ui_18, 0);
	lv_label_set_text(list_empty, tr("podcast_no_downloads"));
	lv_obj_set_hidden(list_empty, true);
	switcher_attach_back_gesture(list_screen);

	refresh_rows();
}
