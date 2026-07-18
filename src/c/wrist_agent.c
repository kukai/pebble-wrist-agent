#include <pebble.h>

// ---------------------------------------------------------------------------
// AppMessage key constants (must match appinfo.json appKeys)
// ---------------------------------------------------------------------------
#define KEY_QUERY           0
#define KEY_RESPONSE        1
#define KEY_STATUS          2
#define KEY_COMMAND         3
#define KEY_TIMER_SET       4
#define KEY_TIMER_LABEL     5
#define KEY_STOPWATCH_START 6

// ---------------------------------------------------------------------------
// Buffer sizes
// ---------------------------------------------------------------------------
#define QUERY_BUF_SIZE    512
#define RESPONSE_BUF_SIZE 512

// ---------------------------------------------------------------------------
// Conversation history (C-side ring buffer)
// ---------------------------------------------------------------------------
#define HIST_CAP    5
#define HIST_Q_SIZE 128
#define HIST_A_SIZE 512
typedef struct { char q[HIST_Q_SIZE]; char a[HIST_A_SIZE]; } HistEntry;

// ---------------------------------------------------------------------------
// Timer / stopwatch slots (persisted)
// ---------------------------------------------------------------------------
#define SLOT_COUNT            4
#define SLOT_LABEL_SIZE       24
#define PERSIST_KEY_SLOT_BASE 100
#define TIMER_MIN_SECONDS     30   // Wakeup API rejects reservations < 30 s
#define WAKEUP_RETRY_MAX      8
#define WAKEUP_RETRY_SHIFT_S  5    // shift on exclusion-window collision

// ---------------------------------------------------------------------------
// Conversation history persistence
// ---------------------------------------------------------------------------
// PersistentStorage caps a single key at 256 bytes, so HIST_A_SIZE (512) must
// be split across two keys per entry.
#define PERSIST_KEY_HIST_META    200
#define PERSIST_KEY_HIST_Q_BASE  210  // +i, i in [0, HIST_CAP)
#define PERSIST_KEY_HIST_A0_BASE 220  // +i, first 256 bytes of a
#define PERSIST_KEY_HIST_A1_BASE 230  // +i, remaining bytes of a
#define HIST_A_CHUNK 256

typedef enum {
  SLOT_EMPTY     = 0,
  SLOT_TIMER     = 1,
  SLOT_STOPWATCH = 2,
} SlotKind;

typedef struct {
  uint8_t kind;       // SlotKind
  uint8_t running;    // 1 = running, 0 = paused
  int32_t duration;   // timer: original length in seconds (for reset)
  time_t  target_ts;  // timer running: expiry time
  int32_t remaining;  // timer paused: seconds left
  time_t  start_ts;   // stopwatch running: origin (accumulated-adjusted)
  int32_t elapsed;    // stopwatch paused: accumulated seconds
  int32_t wakeup_id;  // timer: WakeupId (-1 = none), cookie = slot index
  int32_t last_lap;   // stopwatch: latest lap seconds (0 = none)
  char    label[SLOT_LABEL_SIZE];
} Slot;

static Slot s_slots[SLOT_COUNT];

// ---------------------------------------------------------------------------
// Home menu sections
// ---------------------------------------------------------------------------
#define SECTION_TALK   0
#define SECTION_ACTIVE 1
#define SECTION_MENU   2
#define NUM_SECTIONS   3

typedef void (*FixedMenuCallback)(void);
typedef struct { const char *title; FixedMenuCallback callback; } FixedMenuItem;

static void menu_item_history(void);
static void menu_item_timer(void);
static void menu_item_stopwatch(void);

// セクション3の固定項目。将来は配列に追記するだけで行が増える。
static const FixedMenuItem s_fixed_items[] = {
  { "\xe4\xbc\x9a\xe8\xa9\xb1\xe5\xb1\xa5\xe6\xad\xb4", menu_item_history },
  // UTF-8: "会話履歴"
  { "\xe3\x82\xbf\xe3\x82\xa4\xe3\x83\x9e\xe3\x83\xbc", menu_item_timer },
  // UTF-8: "タイマー"
  { "\xe3\x82\xb9\xe3\x83\x88\xe3\x83\x83\xe3\x83\x97\xe3\x82\xa6\xe3\x82\xa9\xe3\x83\x83\xe3\x83\x81",
    menu_item_stopwatch },
  // UTF-8: "ストップウォッチ"
};

// ---------------------------------------------------------------------------
// Screen states
// ---------------------------------------------------------------------------
typedef enum {
  SCREEN_HOME,
  SCREEN_LOADING,
  SCREEN_ANSWER
} Screen;

// ---------------------------------------------------------------------------
// UI elements
// ---------------------------------------------------------------------------
static Window      *s_window;

// Home
static TextLayer   *s_home_title_layer;
static MenuLayer   *s_home_menu_layer;
static TextLayer   *s_home_status_layer;

// Loading
static TextLayer   *s_loading_title_layer;
static TextLayer   *s_loading_msg_layer;

// Answer
static TextLayer   *s_answer_title_layer;
static ScrollLayer *s_answer_scroll_layer;
static TextLayer   *s_answer_text_layer;
static TextLayer   *s_answer_hint_layer;

// ActionMenu
static ActionMenuLevel *s_am_root;
static int              s_am_slot = -1;
static bool              s_open_timer_picker_pending = false;
static bool              s_open_new_sw_pending = false;

// HOME click config: menu_layer_set_click_config_onto_window() owns UP/DOWN/
// SELECT; we chain onto its provider once and add an explicit BACK handler
// (see home_click_config_provider) instead of relying on default fallback.
static ClickConfigProvider s_home_menu_ccp;

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static DictationSession *s_dictation_session;
static Screen            s_current_screen;

static char      s_query_buf[QUERY_BUF_SIZE];
static char      s_response_buf[RESPONSE_BUF_SIZE];
static char      s_status_buf[64];

static HistEntry s_hist[HIST_CAP];
static int       s_hist_len  = 0;
static int       s_hist_view = 0;
static char      s_answer_display[HIST_Q_SIZE + HIST_A_SIZE + 8];
static char      s_answer_title_text[12];

// 音声経由でタイマー/SWをセットした直後の応答は ANSWER でなく HOME に戻す
static bool      s_pending_home = false;

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
static void show_screen(Screen screen);
static void send_query(void);
static void send_reset_command(void);
static void refresh_answer_screen(void);
static void refresh_home_menu(void);
static void set_home_status(const char *text);
static void open_timer_duration_picker(void);
static void handle_timer_set(int32_t seconds, const char *label, bool set_pending_home);
static void handle_stopwatch_start(const char *label, bool set_pending_home);
static void clear_history(void);
static void persist_history(void);

// ---------------------------------------------------------------------------
// Slot helpers
// ---------------------------------------------------------------------------
static int active_slot_count(void) {
  int n = 0;
  for (int i = 0; i < SLOT_COUNT; i++) {
    if (s_slots[i].kind != SLOT_EMPTY) n++;
  }
  return n;
}

// n 番目 (0-origin) のアクティブスロットの実インデックスを返す
static int active_slot_index(int n) {
  for (int i = 0; i < SLOT_COUNT; i++) {
    if (s_slots[i].kind != SLOT_EMPTY && n-- == 0) return i;
  }
  return -1;
}

static int find_free_slot(void) {
  for (int i = 0; i < SLOT_COUNT; i++) {
    if (s_slots[i].kind == SLOT_EMPTY) return i;
  }
  return -1;
}

static void persist_slot(int idx) {
  if (s_slots[idx].kind == SLOT_EMPTY) {
    persist_delete(PERSIST_KEY_SLOT_BASE + idx);
  } else {
    persist_write_data(PERSIST_KEY_SLOT_BASE + idx, &s_slots[idx], sizeof(Slot));
  }
}

static void clear_slot(int idx) {
  memset(&s_slots[idx], 0, sizeof(Slot));
  s_slots[idx].wakeup_id = -1;
  persist_slot(idx);
}

static void slots_load(void) {
  for (int i = 0; i < SLOT_COUNT; i++) {
    memset(&s_slots[i], 0, sizeof(Slot));
    s_slots[i].wakeup_id = -1;
    if (persist_exists(PERSIST_KEY_SLOT_BASE + i)) {
      persist_read_data(PERSIST_KEY_SLOT_BASE + i, &s_slots[i], sizeof(Slot));
    }
  }
}

// 表示用の秒数。カウンタは持たず、描画のたびにタイムスタンプ差分から再計算する
static int32_t slot_display_seconds(const Slot *s) {
  time_t now = time(NULL);
  int32_t v = 0;
  if (s->kind == SLOT_TIMER) {
    v = s->running ? (int32_t)(s->target_ts - now) : s->remaining;
  } else if (s->kind == SLOT_STOPWATCH) {
    v = s->running ? (int32_t)(now - s->start_ts) : s->elapsed;
  }
  return v < 0 ? 0 : v;
}

static void format_hms(char *buf, size_t len, int32_t secs) {
  if (secs >= 3600) {
    snprintf(buf, len, "%d:%02d:%02d",
             (int)(secs / 3600), (int)((secs / 60) % 60), (int)(secs % 60));
  } else {
    snprintf(buf, len, "%02d:%02d", (int)(secs / 60), (int)(secs % 60));
  }
}

// UTF-8 の継続バイトで切れないように末尾を整える
static void trim_utf8_tail(char *buf) {
  size_t len = strlen(buf);
  while (len > 0 && (buf[len - 1] & 0xC0) == 0x80) {
    buf[--len] = '\0';
  }
}

// ---------------------------------------------------------------------------
// Wakeup (timers)
// ---------------------------------------------------------------------------
// 排他ウィンドウ衝突 (負値エラー) 時は数秒ずらして再試行する
static bool schedule_timer_wakeup(int idx, int32_t seconds) {
  if (seconds < TIMER_MIN_SECONDS) seconds = TIMER_MIN_SECONDS;
  time_t target = time(NULL) + seconds;
  for (int attempt = 0; attempt < WAKEUP_RETRY_MAX; attempt++) {
    WakeupId id = wakeup_schedule(target, idx, true);
    if (id >= 0) {
      s_slots[idx].wakeup_id = id;
      s_slots[idx].target_ts = target;
      return true;
    }
    APP_LOG(APP_LOG_LEVEL_WARNING, "wakeup_schedule failed (%d), shifting", (int)id);
    target += WAKEUP_RETRY_SHIFT_S;
  }
  return false;
}

static void handle_timer_fired(int idx, bool vibrate) {
  if (idx < 0 || idx >= SLOT_COUNT || s_slots[idx].kind != SLOT_TIMER) return;
  if (vibrate) vibes_double_pulse();
  snprintf(s_status_buf, sizeof(s_status_buf),
           "\xe3\x82\xbf\xe3\x82\xa4\xe3\x83\x9e\xe3\x83\xbc\xe7\xb5\x82\xe4\xba\x86 %s",
           s_slots[idx].label[0] ? s_slots[idx].label : "");
  // UTF-8: "タイマー終了 <label>"
  clear_slot(idx);
  set_home_status(s_status_buf);
  refresh_home_menu();
}

static void wakeup_handler(WakeupId id, int32_t cookie) {
  int idx = (int)cookie;
  if (idx < 0 || idx >= SLOT_COUNT) return;
  if (s_slots[idx].kind != SLOT_TIMER || s_slots[idx].wakeup_id != (int32_t)id) return;
  handle_timer_fired(idx, true);
}

// 起動時: Wakeup が失われた/発火済みのタイマーを整理する
static void sanitize_slots(void) {
  time_t now = time(NULL);
  for (int i = 0; i < SLOT_COUNT; i++) {
    Slot *s = &s_slots[i];
    if (s->kind != SLOT_TIMER || !s->running) continue;
    if (wakeup_query(s->wakeup_id, NULL)) continue;
    if (now >= s->target_ts) {
      // アプリ外で発火済み (Wakeup 起動経路で通知済みのはず)
      clear_slot(i);
    } else {
      // 予約が失われている → 残り時間で再スケジュール
      if (!schedule_timer_wakeup(i, (int32_t)(s->target_ts - now))) {
        clear_slot(i);
      } else {
        persist_slot(i);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// History management
// ---------------------------------------------------------------------------
// 会話履歴はユーザーがリセットするまで永続化する（ADR-005 の HIST_CAP=5 方針を踏襲）。
// 1 キー最大 256 bytes の制約があるため、a (512 bytes) は 2 キーに分割する。
static void persist_history(void) {
  persist_write_int(PERSIST_KEY_HIST_META, s_hist_len);
  for (int i = 0; i < HIST_CAP; i++) {
    if (i < s_hist_len) {
      persist_write_data(PERSIST_KEY_HIST_Q_BASE + i, s_hist[i].q, sizeof(s_hist[i].q));
      persist_write_data(PERSIST_KEY_HIST_A0_BASE + i, s_hist[i].a, HIST_A_CHUNK);
      persist_write_data(PERSIST_KEY_HIST_A1_BASE + i, s_hist[i].a + HIST_A_CHUNK,
                         HIST_A_SIZE - HIST_A_CHUNK);
    } else {
      persist_delete(PERSIST_KEY_HIST_Q_BASE + i);
      persist_delete(PERSIST_KEY_HIST_A0_BASE + i);
      persist_delete(PERSIST_KEY_HIST_A1_BASE + i);
    }
  }
}

static void history_load(void) {
  s_hist_len = 0;
  if (persist_exists(PERSIST_KEY_HIST_META)) {
    int32_t len = persist_read_int(PERSIST_KEY_HIST_META);
    if (len < 0) len = 0;
    if (len > HIST_CAP) len = HIST_CAP;
    s_hist_len = len;
  }
  for (int i = 0; i < s_hist_len; i++) {
    memset(&s_hist[i], 0, sizeof(HistEntry));
    if (persist_exists(PERSIST_KEY_HIST_Q_BASE + i)) {
      persist_read_data(PERSIST_KEY_HIST_Q_BASE + i, s_hist[i].q, sizeof(s_hist[i].q));
      s_hist[i].q[HIST_Q_SIZE - 1] = '\0';
    }
    if (persist_exists(PERSIST_KEY_HIST_A0_BASE + i)) {
      persist_read_data(PERSIST_KEY_HIST_A0_BASE + i, s_hist[i].a, HIST_A_CHUNK);
    }
    if (persist_exists(PERSIST_KEY_HIST_A1_BASE + i)) {
      persist_read_data(PERSIST_KEY_HIST_A1_BASE + i, s_hist[i].a + HIST_A_CHUNK,
                        HIST_A_SIZE - HIST_A_CHUNK);
    }
    s_hist[i].a[HIST_A_SIZE - 1] = '\0';
  }
  s_hist_view = s_hist_len > 0 ? s_hist_len - 1 : 0;
}

static void clear_history(void) {
  s_hist_len  = 0;
  s_hist_view = 0;
  persist_history();
}

static void push_history(const char *q, const char *a) {
  if (s_hist_len == HIST_CAP) {
    memmove(s_hist, s_hist + 1, sizeof(HistEntry) * (HIST_CAP - 1));
    s_hist_len--;
  }
  strncpy(s_hist[s_hist_len].q, q, HIST_Q_SIZE - 1);
  s_hist[s_hist_len].q[HIST_Q_SIZE - 1] = '\0';
  strncpy(s_hist[s_hist_len].a, a, HIST_A_SIZE - 1);
  s_hist[s_hist_len].a[HIST_A_SIZE - 1] = '\0';
  s_hist_len++;
  s_hist_view = s_hist_len - 1;
  persist_history();
}

static void refresh_answer_screen(void) {
  if (s_hist_len == 0) return;
  HistEntry *e = &s_hist[s_hist_view];
  snprintf(s_answer_display, sizeof(s_answer_display),
           "Q: %s\n\nA: %s", e->q, e->a);
  snprintf(s_answer_title_text, sizeof(s_answer_title_text),
           "%d/%d", s_hist_view + 1, s_hist_len);
  text_layer_set_text(s_answer_title_layer, s_answer_title_text);
  text_layer_set_text(s_answer_text_layer, s_answer_display);
  GRect scroll_bounds = layer_get_bounds(scroll_layer_get_layer(s_answer_scroll_layer));
  GSize text_size = text_layer_get_content_size(s_answer_text_layer);
  text_size.h += 8;
  if (text_size.h < scroll_bounds.size.h) text_size.h = scroll_bounds.size.h;
  text_layer_set_size(s_answer_text_layer, GSize(scroll_bounds.size.w, text_size.h));
  scroll_layer_set_content_size(s_answer_scroll_layer, GSize(scroll_bounds.size.w, text_size.h));
  scroll_layer_set_content_offset(s_answer_scroll_layer, GPointZero, false);
}

// ---------------------------------------------------------------------------
// Home menu (MenuLayer)
// ---------------------------------------------------------------------------
static void set_home_status(const char *text) {
  if (s_home_status_layer) text_layer_set_text(s_home_status_layer, text);
}

static void refresh_home_menu(void) {
  if (!s_home_menu_layer) return;
  menu_layer_reload_data(s_home_menu_layer);
  layer_mark_dirty(menu_layer_get_layer(s_home_menu_layer));
}

static uint16_t menu_get_num_sections(MenuLayer *menu, void *ctx) {
  return NUM_SECTIONS;
}

static uint16_t menu_get_num_rows(MenuLayer *menu, uint16_t section, void *ctx) {
  switch (section) {
    case SECTION_TALK:   return 1;
    case SECTION_ACTIVE: return (uint16_t)active_slot_count();
    case SECTION_MENU:   return (uint16_t)ARRAY_LENGTH(s_fixed_items);
    default:             return 0;
  }
}

static void menu_draw_row(GContext *ctx, const Layer *cell_layer,
                          MenuIndex *index, void *data) {
  switch (index->section) {
    case SECTION_TALK:
      menu_cell_basic_draw(ctx, cell_layer,
        "\xe8\xa9\xb1\xe3\x81\x99",  // UTF-8: "話す"
        "Select\xe3\x81\xa7\xe9\x9f\xb3\xe5\xa3\xb0\xe5\x85\xa5\xe5\x8a\x9b",
        // UTF-8: "Selectで音声入力"
        NULL);
      break;

    case SECTION_ACTIVE: {
      int idx = active_slot_index(index->row);
      if (idx < 0) break;
      Slot *s = &s_slots[idx];
      char tbuf[12];
      char sub[48];
      format_hms(tbuf, sizeof(tbuf), slot_display_seconds(s));
      const char *paused = "(\xe5\x81\x9c\xe6\xad\xa2)";  // UTF-8: "(停止)"
      if (s->kind == SLOT_STOPWATCH && s->last_lap > 0) {
        char lbuf[12];
        format_hms(lbuf, sizeof(lbuf), s->last_lap);
        snprintf(sub, sizeof(sub), "%s%s%s Lap %s",
                 tbuf, s->running ? "" : " ", s->running ? "" : paused, lbuf);
      } else if (s->running) {
        snprintf(sub, sizeof(sub), "%s", tbuf);
      } else {
        snprintf(sub, sizeof(sub), "%s %s", tbuf, paused);
      }
      const char *title;
      if (s->label[0]) {
        title = s->label;
      } else if (s->kind == SLOT_TIMER) {
        title = "\xe3\x82\xbf\xe3\x82\xa4\xe3\x83\x9e\xe3\x83\xbc";  // "タイマー"
      } else {
        title = "\xe3\x82\xb9\xe3\x83\x88\xe3\x83\x83\xe3\x83\x97"
                "\xe3\x82\xa6\xe3\x82\xa9\xe3\x83\x83\xe3\x83\x81";  // "ストップウォッチ"
      }
      menu_cell_basic_draw(ctx, cell_layer, title, sub, NULL);
      break;
    }

    case SECTION_MENU:
      if (index->row < ARRAY_LENGTH(s_fixed_items)) {
        menu_cell_basic_draw(ctx, cell_layer, s_fixed_items[index->row].title, NULL, NULL);
      }
      break;
  }
}

// ---------------------------------------------------------------------------
// ActionMenu (section-2 row operations, all local — no LLM round-trip)
// ---------------------------------------------------------------------------
static void am_did_close(ActionMenu *menu, const ActionMenuItem *item, void *context) {
  if (s_am_root) {
    action_menu_hierarchy_destroy(s_am_root, NULL, NULL);
    s_am_root = NULL;
  }
  // 「新しいタイマー/SW」が選ばれていた場合、この ActionMenu が完全に閉じてから
  // 次の ActionMenu を開く（開いたままの二重起動を避けるため did_close まで遅延する）
  if (s_open_timer_picker_pending) {
    s_open_timer_picker_pending = false;
    open_timer_duration_picker();
  } else if (s_open_new_sw_pending) {
    s_open_new_sw_pending = false;
    handle_stopwatch_start(NULL, false);
  }
}

static void am_timer_toggle(ActionMenu *am, const ActionMenuItem *item, void *ctx) {
  Slot *s = &s_slots[s_am_slot];
  if (s->kind != SLOT_TIMER) return;
  if (s->running) {
    wakeup_cancel(s->wakeup_id);
    s->wakeup_id = -1;
    int32_t rem = (int32_t)(s->target_ts - time(NULL));
    s->remaining = rem < 1 ? 1 : rem;
    s->running = 0;
  } else {
    if (schedule_timer_wakeup(s_am_slot, s->remaining)) {
      s->running = 1;
    } else {
      set_home_status("\xe4\xba\x88\xe7\xb4\x84\xe5\xa4\xb1\xe6\x95\x97");  // "予約失敗"
    }
  }
  persist_slot(s_am_slot);
  refresh_home_menu();
}

static void am_timer_reset(ActionMenu *am, const ActionMenuItem *item, void *ctx) {
  Slot *s = &s_slots[s_am_slot];
  if (s->kind != SLOT_TIMER) return;
  if (s->running && s->wakeup_id >= 0) wakeup_cancel(s->wakeup_id);
  s->wakeup_id = -1;
  if (schedule_timer_wakeup(s_am_slot, s->duration)) {
    s->running = 1;
  } else {
    s->running   = 0;
    s->remaining = s->duration;
    set_home_status("\xe4\xba\x88\xe7\xb4\x84\xe5\xa4\xb1\xe6\x95\x97");  // "予約失敗"
  }
  persist_slot(s_am_slot);
  refresh_home_menu();
}

static void am_delete(ActionMenu *am, const ActionMenuItem *item, void *ctx) {
  Slot *s = &s_slots[s_am_slot];
  if (s->kind == SLOT_TIMER && s->running && s->wakeup_id >= 0) {
    wakeup_cancel(s->wakeup_id);
  }
  clear_slot(s_am_slot);
  refresh_home_menu();
}

static void am_sw_toggle(ActionMenu *am, const ActionMenuItem *item, void *ctx) {
  Slot *s = &s_slots[s_am_slot];
  if (s->kind != SLOT_STOPWATCH) return;
  time_t now = time(NULL);
  if (s->running) {
    s->elapsed = (int32_t)(now - s->start_ts);
    s->running = 0;
  } else {
    s->start_ts = now - s->elapsed;
    s->running  = 1;
  }
  persist_slot(s_am_slot);
  refresh_home_menu();
}

static void am_sw_lap(ActionMenu *am, const ActionMenuItem *item, void *ctx) {
  Slot *s = &s_slots[s_am_slot];
  if (s->kind != SLOT_STOPWATCH || !s->running) return;
  s->last_lap = slot_display_seconds(s);
  persist_slot(s_am_slot);
  refresh_home_menu();
}

static void am_sw_reset(ActionMenu *am, const ActionMenuItem *item, void *ctx) {
  Slot *s = &s_slots[s_am_slot];
  if (s->kind != SLOT_STOPWATCH) return;
  s->running  = 0;
  s->elapsed  = 0;
  s->start_ts = 0;
  s->last_lap = 0;
  persist_slot(s_am_slot);
  refresh_home_menu();
}

static void am_new_timer(ActionMenu *am, const ActionMenuItem *item, void *ctx) {
  s_open_timer_picker_pending = true;
}

static void am_new_stopwatch(ActionMenu *am, const ActionMenuItem *item, void *ctx) {
  s_open_new_sw_pending = true;
}

// タイマー/SW 実行中の最大アクション数（SW実行中: Stop+Lap+Reset+削除+新規 = 5）に合わせた容量
#define ACTION_MENU_CAPACITY 5

static void open_slot_action_menu(int slot_idx) {
  Slot *s = &s_slots[slot_idx];
  if (s->kind == SLOT_EMPTY) return;
  s_am_slot = slot_idx;
  s_am_root = action_menu_level_create(ACTION_MENU_CAPACITY);
  bool has_free_slot = find_free_slot() >= 0;

  if (s->kind == SLOT_TIMER) {
    action_menu_level_add_action(s_am_root,
      s->running ? "\xe4\xb8\x80\xe6\x99\x82\xe5\x81\x9c\xe6\xad\xa2"   // "一時停止"
                 : "\xe5\x86\x8d\xe9\x96\x8b",                          // "再開"
      am_timer_toggle, NULL);
    action_menu_level_add_action(s_am_root,
      "\xe3\x83\xaa\xe3\x82\xbb\xe3\x83\x83\xe3\x83\x88",               // "リセット"
      am_timer_reset, NULL);
    action_menu_level_add_action(s_am_root,
      "\xe5\x89\x8a\xe9\x99\xa4",                                       // "削除"
      am_delete, NULL);
    if (has_free_slot) {
      action_menu_level_add_action(s_am_root,
        "\xe6\x96\xb0\xe3\x81\x97\xe3\x81\x84\xe3\x82\xbf\xe3\x82\xa4\xe3\x83\x9e\xe3\x83\xbc",
        am_new_timer, NULL);  // UTF-8: "新しいタイマー"
    }
  } else {
    action_menu_level_add_action(s_am_root,
      s->running ? "\xe3\x82\xb9\xe3\x83\x88\xe3\x83\x83\xe3\x83\x97"   // "ストップ"
                 : "\xe3\x82\xb9\xe3\x82\xbf\xe3\x83\xbc\xe3\x83\x88",  // "スタート"
      am_sw_toggle, NULL);
    if (s->running) {
      action_menu_level_add_action(s_am_root, "Lap", am_sw_lap, NULL);
    }
    action_menu_level_add_action(s_am_root,
      "\xe3\x83\xaa\xe3\x82\xbb\xe3\x83\x83\xe3\x83\x88",               // "リセット"
      am_sw_reset, NULL);
    action_menu_level_add_action(s_am_root,
      "\xe5\x89\x8a\xe9\x99\xa4",                                       // "削除"
      am_delete, NULL);
    if (has_free_slot) {
      action_menu_level_add_action(s_am_root,
        "\xe6\x96\xb0\xe3\x81\x97\xe3\x81\x84\xe3\x82\xb9\xe3\x83\x88\xe3\x83\x83\xe3\x83"
        "\x97\xe3\x82\xa6\xe3\x82\xa9\xe3\x83\x83\xe3\x83\x81",
        am_new_stopwatch, NULL);  // UTF-8: "新しいストップウォッチ"
    }
  }

  ActionMenuConfig config = (ActionMenuConfig) {
    .root_level = s_am_root,
    .colors = { .background = GColorWhite, .foreground = GColorBlack },
    .did_close = am_did_close,
  };
  action_menu_open(&config);
}

// ---------------------------------------------------------------------------
// Timer duration picker (S3「タイマー」から未設定時に自動遷移、または
// ActionMenu の「新しいタイマー」から遷移。プリセット時間を ActionMenu で選ばせる)
// ---------------------------------------------------------------------------
typedef struct { const char *label; int32_t seconds; } TimerPreset;

static const TimerPreset s_timer_presets[] = {
  { "1\xe5\x88\x86",  60 },
  { "3\xe5\x88\x86",  180 },
  { "5\xe5\x88\x86",  300 },
  { "10\xe5\x88\x86", 600 },
  { "15\xe5\x88\x86", 900 },
  { "20\xe5\x88\x86", 1200 },
  { "30\xe5\x88\x86", 1800 },
  // UTF-8: "分"
};

static void tp_action(ActionMenu *am, const ActionMenuItem *item, void *ctx) {
  int32_t seconds = (int32_t)(intptr_t)action_menu_item_get_action_data(item);
  handle_timer_set(seconds, NULL, false);
}

static void open_timer_duration_picker(void) {
  if (find_free_slot() < 0) {
    set_home_status("\xe3\x82\xbf\xe3\x82\xa4\xe3\x83\x9e\xe3\x83\xbc\xe6\x9e\xa0"
                    "\xe6\xba\x80\xe6\x9d\xaf");  // UTF-8: "タイマー枠満杯"
    return;
  }
  s_am_root = action_menu_level_create(ARRAY_LENGTH(s_timer_presets));
  for (size_t i = 0; i < ARRAY_LENGTH(s_timer_presets); i++) {
    action_menu_level_add_action(s_am_root, s_timer_presets[i].label, tp_action,
                                 (void *)(intptr_t)s_timer_presets[i].seconds);
  }
  ActionMenuConfig config = (ActionMenuConfig) {
    .root_level = s_am_root,
    .colors = { .background = GColorWhite, .foreground = GColorBlack },
    .did_close = am_did_close,
  };
  action_menu_open(&config);
}

// ---------------------------------------------------------------------------
// Menu select / fixed items
// ---------------------------------------------------------------------------
static void menu_item_history(void) {
  if (s_hist_len > 0) {
    s_hist_view = s_hist_len - 1;
    show_screen(SCREEN_ANSWER);
  } else {
    set_home_status("\xe5\xb1\xa5\xe6\xad\xb4\xe3\x81\xaa\xe3\x81\x97");  // "履歴なし"
  }
}

// 未設定ならタイマー設定画面(プリセット選択)へ、設定済みなら既存の ActionMenu
// (一時停止/リセット/削除 + 新しいタイマー) を開く。複数ある場合は最初の1件を対象とする。
static void menu_item_timer(void) {
  for (int i = 0; i < SLOT_COUNT; i++) {
    if (s_slots[i].kind == SLOT_TIMER) {
      open_slot_action_menu(i);
      return;
    }
  }
  open_timer_duration_picker();
}

// 未設定なら即座にストップウォッチを開始、設定済みなら既存の ActionMenu を開く
static void menu_item_stopwatch(void) {
  for (int i = 0; i < SLOT_COUNT; i++) {
    if (s_slots[i].kind == SLOT_STOPWATCH) {
      open_slot_action_menu(i);
      return;
    }
  }
  handle_stopwatch_start(NULL, false);
}

static void menu_select_callback(MenuLayer *menu, MenuIndex *index, void *ctx) {
  switch (index->section) {
    case SECTION_TALK:
      if (s_dictation_session) {
        dictation_session_start(s_dictation_session);
      } else {
        set_home_status("\xe3\x83\x9e\xe3\x82\xa4\xe3\x82\xaf\xe9\x9d\x9e\xe5\xaf\xbe\xe5\xbf\x9c");
        // UTF-8: "マイク非対応"
      }
      break;
    case SECTION_ACTIVE: {
      int idx = active_slot_index(index->row);
      if (idx >= 0) open_slot_action_menu(idx);
      break;
    }
    case SECTION_MENU:
      if (index->row < ARRAY_LENGTH(s_fixed_items)) {
        s_fixed_items[index->row].callback();
      }
      break;
  }
}

// ---------------------------------------------------------------------------
// TickTimerService (home foreground only; redraw recomputes from timestamps)
// ---------------------------------------------------------------------------
static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
  if (s_home_menu_layer) {
    layer_mark_dirty(menu_layer_get_layer(s_home_menu_layer));
  }
}

// ---------------------------------------------------------------------------
// Layer visibility
// ---------------------------------------------------------------------------
static void hide_all_screens(void) {
  layer_set_hidden(text_layer_get_layer(s_home_title_layer), true);
  layer_set_hidden(menu_layer_get_layer(s_home_menu_layer), true);
  layer_set_hidden(text_layer_get_layer(s_home_status_layer), true);
  layer_set_hidden(text_layer_get_layer(s_loading_title_layer), true);
  layer_set_hidden(text_layer_get_layer(s_loading_msg_layer), true);
  layer_set_hidden(text_layer_get_layer(s_answer_title_layer), true);
  layer_set_hidden(scroll_layer_get_layer(s_answer_scroll_layer), true);
  layer_set_hidden(text_layer_get_layer(s_answer_hint_layer), true);
}

// ---------------------------------------------------------------------------
// Click handlers (ANSWER / LOADING)
// ---------------------------------------------------------------------------
static void answer_select_click(ClickRecognizerRef r, void *ctx) {
  show_screen(SCREEN_HOME);
}

static void answer_select_long_click(ClickRecognizerRef r, void *ctx) {
  send_reset_command();
  show_screen(SCREEN_HOME);
}

static void answer_back_click(ClickRecognizerRef r, void *ctx) {
  show_screen(SCREEN_HOME);
}

static void answer_up_click(ClickRecognizerRef r, void *ctx) {
  GPoint offset = scroll_layer_get_content_offset(s_answer_scroll_layer);
  offset.y += 30;
  if (offset.y > 0) offset.y = 0;
  scroll_layer_set_content_offset(s_answer_scroll_layer, offset, true);
}

static void answer_down_click(ClickRecognizerRef r, void *ctx) {
  GPoint offset = scroll_layer_get_content_offset(s_answer_scroll_layer);
  offset.y -= 30;
  scroll_layer_set_content_offset(s_answer_scroll_layer, offset, true);
}

static void answer_up_long_click(ClickRecognizerRef r, void *ctx) {
  if (s_hist_view > 0) {
    s_hist_view--;
    refresh_answer_screen();
  }
}

static void answer_down_long_click(ClickRecognizerRef r, void *ctx) {
  if (s_hist_view < s_hist_len - 1) {
    s_hist_view++;
    refresh_answer_screen();
  }
}

static void answer_click_config(void *ctx) {
  window_single_click_subscribe(BUTTON_ID_SELECT, answer_select_click);
  window_long_click_subscribe(BUTTON_ID_SELECT, 700, answer_select_long_click, NULL);
  window_single_click_subscribe(BUTTON_ID_BACK, answer_back_click);
  window_single_click_subscribe(BUTTON_ID_UP, answer_up_click);
  window_single_click_subscribe(BUTTON_ID_DOWN, answer_down_click);
  window_long_click_subscribe(BUTTON_ID_UP, 500, answer_up_long_click, NULL);
  window_long_click_subscribe(BUTTON_ID_DOWN, 500, answer_down_long_click, NULL);
}

// LOADING 中は「操作無効」の仕様どおり BACK も含めて何も起きないようにする。
// 何もサブスクライブしないと BACK はデフォルトの「ウィンドウを閉じる」動作にフォール
// バックしてしまい、リクエスト中にアプリが終了できてしまうため明示的に無効化する。
static void noop_click(ClickRecognizerRef r, void *ctx) {}

static void loading_click_config(void *ctx) {
  window_single_click_subscribe(BUTTON_ID_BACK, noop_click);
}

// HOME: menu_layer_set_click_config_onto_window() が上書きする Click Config
// Provider に対し、BACK ボタンだけ明示的にサブスクライブし直す。BACK は本来
// 何もバインドしなければデフォルトで「ウィンドウを閉じる」動作にフォールバック
// するはずだが、音声認識サイクルを繰り返した後に反応しなくなる不具合が実機で
// 報告されたため、暗黙のデフォルト挙動に頼らず明示的なハンドラを持たせる
// （MenuLayer + カスタム BACK の定石パターン。参考: Pebble SDK issue の回避策）。
static void home_back_click(ClickRecognizerRef r, void *ctx) {
  window_stack_remove(s_window, true);
}

static void home_click_config_provider(void *context) {
  if (s_home_menu_ccp) {
    s_home_menu_ccp(context);
  }
  window_single_click_subscribe(BUTTON_ID_BACK, home_back_click);
}

// ---------------------------------------------------------------------------
// show_screen
// ---------------------------------------------------------------------------
static void show_screen(Screen screen) {
  s_current_screen = screen;
  hide_all_screens();

  if (screen == SCREEN_HOME) {
    tick_timer_service_subscribe(SECOND_UNIT, tick_handler);
  } else {
    tick_timer_service_unsubscribe();
  }

  switch (screen) {
    case SCREEN_HOME:
      layer_set_hidden(text_layer_get_layer(s_home_title_layer), false);
      layer_set_hidden(menu_layer_get_layer(s_home_menu_layer), false);
      layer_set_hidden(text_layer_get_layer(s_home_status_layer), false);
      menu_layer_reload_data(s_home_menu_layer);
      // 常に「話す」を初期選択にし、Select 即発話の起動感を維持する
      menu_layer_set_selected_index(s_home_menu_layer,
                                    MenuIndex(SECTION_TALK, 0),
                                    MenuRowAlignTop, false);
      window_set_click_config_provider_with_context(
        s_window, home_click_config_provider, s_home_menu_layer);
      break;

    case SCREEN_LOADING:
      layer_set_hidden(text_layer_get_layer(s_loading_title_layer), false);
      layer_set_hidden(text_layer_get_layer(s_loading_msg_layer), false);
      window_set_click_config_provider(s_window, loading_click_config);
      break;

    case SCREEN_ANSWER:
      refresh_answer_screen();
      layer_set_hidden(text_layer_get_layer(s_answer_title_layer), false);
      layer_set_hidden(scroll_layer_get_layer(s_answer_scroll_layer), false);
      layer_set_hidden(text_layer_get_layer(s_answer_hint_layer), false);
      window_set_click_config_provider(s_window, answer_click_config);
      break;
  }
}

// ---------------------------------------------------------------------------
// AppMessage send helpers
// ---------------------------------------------------------------------------
static void send_query(void) {
  DictionaryIterator *out;
  AppMessageResult result = app_message_outbox_begin(&out);
  if (result != APP_MSG_OK) {
    snprintf(s_status_buf, sizeof(s_status_buf), "send err %d", (int)result);
    set_home_status(s_status_buf);
    show_screen(SCREEN_HOME);
    return;
  }
  s_pending_home = false;
  dict_write_cstring(out, KEY_QUERY, s_query_buf);
  app_message_outbox_send();
  show_screen(SCREEN_LOADING);
}

static void send_reset_command(void) {
  DictionaryIterator *out;
  if (app_message_outbox_begin(&out) != APP_MSG_OK) return;
  dict_write_cstring(out, KEY_COMMAND, "reset");
  app_message_outbox_send();
  clear_history();
  set_home_status("リセット中...");
}

// ---------------------------------------------------------------------------
// AppMessage receive
// ---------------------------------------------------------------------------
static void handle_timer_set(int32_t seconds, const char *label, bool set_pending_home) {
  int idx = find_free_slot();
  if (idx < 0) {
    set_home_status("\xe3\x82\xbf\xe3\x82\xa4\xe3\x83\x9e\xe3\x83\xbc\xe6\x9e\xa0"
                    "\xe6\xba\x80\xe6\x9d\xaf");  // UTF-8: "タイマー枠満杯"
    return;
  }
  Slot *s = &s_slots[idx];
  memset(s, 0, sizeof(Slot));
  s->kind      = SLOT_TIMER;
  s->duration  = seconds < TIMER_MIN_SECONDS ? TIMER_MIN_SECONDS : seconds;
  s->wakeup_id = -1;
  if (label) {
    strncpy(s->label, label, SLOT_LABEL_SIZE - 1);
    s->label[SLOT_LABEL_SIZE - 1] = '\0';
    trim_utf8_tail(s->label);
  }
  if (schedule_timer_wakeup(idx, seconds)) {
    s->running = 1;
    persist_slot(idx);
    if (set_pending_home) s_pending_home = true;
    refresh_home_menu();
  } else {
    clear_slot(idx);
    set_home_status("\xe4\xba\x88\xe7\xb4\x84\xe5\xa4\xb1\xe6\x95\x97");  // "予約失敗"
  }
}

static void handle_stopwatch_start(const char *label, bool set_pending_home) {
  int idx = find_free_slot();
  if (idx < 0) {
    set_home_status("\xe3\x82\xbf\xe3\x82\xa4\xe3\x83\x9e\xe3\x83\xbc\xe6\x9e\xa0"
                    "\xe6\xba\x80\xe6\x9d\xaf");  // UTF-8: "タイマー枠満杯"
    return;
  }
  Slot *s = &s_slots[idx];
  memset(s, 0, sizeof(Slot));
  s->kind      = SLOT_STOPWATCH;
  s->running   = 1;
  s->start_ts  = time(NULL);
  s->wakeup_id = -1;
  if (label) {
    strncpy(s->label, label, SLOT_LABEL_SIZE - 1);
    s->label[SLOT_LABEL_SIZE - 1] = '\0';
    trim_utf8_tail(s->label);
  }
  persist_slot(idx);
  if (set_pending_home) s_pending_home = true;
  refresh_home_menu();
}

static void inbox_received_handler(DictionaryIterator *iter, void *ctx) {
  Tuple *resp_tuple   = dict_find(iter, KEY_RESPONSE);
  Tuple *status_tuple = dict_find(iter, KEY_STATUS);
  Tuple *timer_tuple  = dict_find(iter, KEY_TIMER_SET);
  Tuple *label_tuple  = dict_find(iter, KEY_TIMER_LABEL);
  Tuple *sw_tuple     = dict_find(iter, KEY_STOPWATCH_START);

  const char *label = NULL;
  if (label_tuple && label_tuple->value->cstring[0] != '\0') {
    label = label_tuple->value->cstring;
  }

  if (timer_tuple) {
    handle_timer_set(timer_tuple->value->int32, label, true);
  }

  if (sw_tuple) {
    handle_stopwatch_start(label, true);
  }

  if (resp_tuple) {
    strncpy(s_response_buf, resp_tuple->value->cstring, RESPONSE_BUF_SIZE - 1);
    s_response_buf[RESPONSE_BUF_SIZE - 1] = '\0';
    push_history(s_query_buf, s_response_buf);
    if (s_pending_home) {
      // タイマー/SW セット時は ANSWER でなくホームに戻り、セクション2で確認できるようにする
      s_pending_home = false;
      set_home_status("\xe3\x82\xbb\xe3\x83\x83\xe3\x83\x88\xe5\xae\x8c\xe4\xba\x86");
      // UTF-8: "セット完了"
      show_screen(SCREEN_HOME);
    } else {
      show_screen(SCREEN_ANSWER);
    }
  }

  if (status_tuple) {
    const char *status = status_tuple->value->cstring;
    if (strncmp(status, "error:", 6) == 0) {
      strncpy(s_status_buf, status + 6, sizeof(s_status_buf) - 1);
      s_status_buf[sizeof(s_status_buf) - 1] = '\0';
      set_home_status(s_status_buf);
      show_screen(SCREEN_HOME);
    } else if (strcmp(status, "reset_ok") == 0) {
      clear_history();
      set_home_status("\xe3\x83\xaa\xe3\x82\xbb\xe3\x83\x83\xe3\x83\x88\xe5\xae\x8c\xe4\xba\x86");
      // UTF-8: "リセット完了"
      show_screen(SCREEN_HOME);
    } else if (strcmp(status, "key_saved") == 0) {
      set_home_status("APIキー保存完了!");
      show_screen(SCREEN_HOME);
    }
  }
}

static void outbox_failed_handler(DictionaryIterator *iter, AppMessageResult reason, void *ctx) {
  snprintf(s_status_buf, sizeof(s_status_buf), "outbox err %d", (int)reason);
  set_home_status(s_status_buf);
  show_screen(SCREEN_HOME);
}

// ---------------------------------------------------------------------------
// Dictation callback
// ---------------------------------------------------------------------------
static void dictation_session_callback(DictationSession *session,
                                       DictationSessionStatus status,
                                       char *transcription,
                                       void *context) {
  if (status == DictationSessionStatusSuccess) {
    strncpy(s_query_buf, transcription, QUERY_BUF_SIZE - 1);
    s_query_buf[QUERY_BUF_SIZE - 1] = '\0';
    send_query();
  } else {
    set_home_status("認識失敗。再試行を");
    show_screen(SCREEN_HOME);
  }
}

// ---------------------------------------------------------------------------
// Window load / unload
// ---------------------------------------------------------------------------
static TextLayer *make_title_bar(Layer *root, GRect bounds, const char *title) {
  TextLayer *tl = text_layer_create(GRect(0, 0, bounds.size.w, 30));
  text_layer_set_background_color(tl, GColorBlack);
  text_layer_set_text_color(tl, GColorWhite);
  text_layer_set_font(tl, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
  text_layer_set_text_alignment(tl, GTextAlignmentCenter);
  text_layer_set_text(tl, title);
  layer_add_child(root, text_layer_get_layer(tl));
  return tl;
}

static TextLayer *make_bottom_hint(Layer *root, GRect bounds, const char *text) {
  TextLayer *tl = text_layer_create(GRect(0, bounds.size.h - 30, bounds.size.w, 30));
  text_layer_set_background_color(tl, GColorLightGray);
  text_layer_set_font(tl, fonts_get_system_font(FONT_KEY_GOTHIC_14));
  text_layer_set_text_alignment(tl, GTextAlignmentCenter);
  text_layer_set_text(tl, text);
  layer_add_child(root, text_layer_get_layer(tl));
  return tl;
}

static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect  bounds = layer_get_bounds(root);
  int    content_top = 30;
  int    content_bot = bounds.size.h - 30;
  int    content_h   = content_bot - content_top;

  window_set_background_color(window, GColorWhite);

  // ── Home (MenuLayer dashboard) ────────────────────────────────────────────
  s_home_title_layer = make_title_bar(root, bounds, "WristAgent");

  s_home_menu_layer = menu_layer_create(GRect(0, content_top, bounds.size.w, content_h));
  menu_layer_set_callbacks(s_home_menu_layer, NULL, (MenuLayerCallbacks) {
    .get_num_sections  = menu_get_num_sections,
    .get_num_rows      = menu_get_num_rows,
    .draw_row          = menu_draw_row,
    .select_click      = menu_select_callback,
  });
  layer_add_child(root, menu_layer_get_layer(s_home_menu_layer));

  // MenuLayer 自身の Click Config Provider を一度だけ取得し、以後は
  // home_click_config_provider でラップして使う（BACK ハンドラを補強するため）。
  menu_layer_set_click_config_onto_window(s_home_menu_layer, s_window);
  s_home_menu_ccp = window_get_click_config_provider(s_window);

  s_home_status_layer = make_bottom_hint(root, bounds, "");

  // ── Loading ───────────────────────────────────────────────────────────────
  s_loading_title_layer = make_title_bar(root, bounds, "WristAgent");

  s_loading_msg_layer = text_layer_create(GRect(0, 80, bounds.size.w, 60));
  text_layer_set_font(s_loading_msg_layer, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  text_layer_set_text_alignment(s_loading_msg_layer, GTextAlignmentCenter);
  text_layer_set_text(s_loading_msg_layer, "\xe8\x80\x83\xe3\x81\x88\xe4\xb8\xad...");
  // UTF-8: "考え中..."
  layer_add_child(root, text_layer_get_layer(s_loading_msg_layer));

  // ── Answer ────────────────────────────────────────────────────────────────
  s_answer_title_layer = make_title_bar(root, bounds, "");

  GRect answer_scroll_frame = GRect(0, content_top, bounds.size.w, content_h);
  s_answer_scroll_layer = scroll_layer_create(answer_scroll_frame);
  scroll_layer_set_shadow_hidden(s_answer_scroll_layer, true);

  s_answer_text_layer = text_layer_create(
    GRect(4, 4, answer_scroll_frame.size.w - 8, answer_scroll_frame.size.h));
  text_layer_set_font(s_answer_text_layer, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  text_layer_set_overflow_mode(s_answer_text_layer, GTextOverflowModeWordWrap);
  scroll_layer_add_child(s_answer_scroll_layer, text_layer_get_layer(s_answer_text_layer));
  layer_add_child(root, scroll_layer_get_layer(s_answer_scroll_layer));

  s_answer_hint_layer = make_bottom_hint(root, bounds,
    "UP/DN\xe9\x95\xb7:\xe5\x89\x8d\xe5\xbe\x8c SEL\xe9\x95\xb7:\xe3\x83\xaa\xe3\x82\xbb\xe3\x83\x83\xe3\x83\x88");
  // UTF-8: "UP/DN長:前後 SEL長:リセット"

  // ── Show initial screen ───────────────────────────────────────────────────
  show_screen(SCREEN_HOME);
}

static void window_unload(Window *window) {
  text_layer_destroy(s_home_title_layer);
  menu_layer_destroy(s_home_menu_layer);
  s_home_menu_layer = NULL;
  text_layer_destroy(s_home_status_layer);
  s_home_status_layer = NULL;

  text_layer_destroy(s_loading_title_layer);
  text_layer_destroy(s_loading_msg_layer);

  text_layer_destroy(s_answer_title_layer);
  text_layer_destroy(s_answer_text_layer);
  scroll_layer_destroy(s_answer_scroll_layer);
  text_layer_destroy(s_answer_hint_layer);
}

// ---------------------------------------------------------------------------
// App init / deinit
// ---------------------------------------------------------------------------
static void init(void) {
  slots_load();
  history_load();

  app_message_register_inbox_received(inbox_received_handler);
  app_message_register_outbox_failed(outbox_failed_handler);
  {
    uint32_t inbox  = app_message_inbox_size_maximum();
    uint32_t outbox = app_message_outbox_size_maximum();
    app_message_open(inbox  < 512 ? inbox  : 512,
                     outbox < 512 ? outbox : 512);
  }

#if defined(PBL_MICROPHONE)
  s_dictation_session = dictation_session_create(QUERY_BUF_SIZE,
                                                 dictation_session_callback,
                                                 NULL);
#endif

  s_window = window_create();
  window_set_window_handlers(s_window, (WindowHandlers){
    .load   = window_load,
    .unload = window_unload,
  });
  window_stack_push(s_window, true);

  // Wakeup 起動 (タイマー満了) の処理はレイヤー生成後に行う
  if (launch_reason() == APP_LAUNCH_WAKEUP) {
    WakeupId id;
    int32_t  cookie;
    if (wakeup_get_launch_event(&id, &cookie)) {
      handle_timer_fired((int)cookie, true);
    }
  }
  sanitize_slots();
  refresh_home_menu();
  wakeup_service_subscribe(wakeup_handler);
}

static void deinit(void) {
  tick_timer_service_unsubscribe();
#if defined(PBL_MICROPHONE)
  if (s_dictation_session) {
    dictation_session_destroy(s_dictation_session);
  }
#endif
  window_destroy(s_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
