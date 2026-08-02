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
// タイマー・SW とも同時に保持できるのは 1 件まで（ADR-022）。スロット自体は
// 種別ごとに 1 つずつ、計 2 件で足りる。
#define SLOT_COUNT            2
#define SLOT_LABEL_SIZE       24
#define PERSIST_KEY_SLOT_BASE 100
#define TIMER_MIN_SECONDS     30   // Wakeup API rejects reservations < 30 s
#define WAKEUP_RETRY_MAX      8
#define WAKEUP_RETRY_SHIFT_S  5    // shift on exclusion-window collision
// SW の Lap 履歴。上限を超えたら最古を捨てて詰める（HIST_CAP と同じ考え方）。
#define MAX_LAPS               8

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
  int32_t laps[MAX_LAPS];  // stopwatch: 記録した Lap 履歴（古い順）
  uint8_t lap_count;       // 0..MAX_LAPS
} Slot;

static Slot s_slots[SLOT_COUNT];

// ---------------------------------------------------------------------------
// Home menu (単一セクション・固定行リスト)
// ---------------------------------------------------------------------------
// 「話す」「天気」「タイマー」「ストップウォッチ」「会話履歴」はすべて同列の
// 固定行として並ぶ。タイマー/SW は動的にインスタンスの数だけ行が増えることは
// せず、常に1行のまま存在し、サブタイトルで状態（未設定/残り・経過時間）を
// 表現する（ADR-021 参照）。同時に保持できるのはタイマー・SW とも 1 件まで
// （ADR-022）。
typedef void (*MenuRowCallback)(void);
typedef const char *(*MenuRowSubtitleFn)(void);  // NULL を返せばサブタイトルなし

typedef struct {
  const char *title;
  MenuRowCallback callback;
  MenuRowSubtitleFn subtitle;
  GBitmap **icon;  // NULL = アイコンなし。実行時に読み込んだ GBitmap* へのポインタ
} MenuRow;

static void menu_row_talk(void);
static void menu_item_timer(void);
static void menu_item_stopwatch(void);
static void menu_item_history(void);
static void menu_item_weather(void);
static const char *talk_subtitle(void);
static const char *timer_row_subtitle(void);
static const char *stopwatch_row_subtitle(void);
static const char *no_subtitle(void);

static GBitmap *s_icon_weather;

// 将来の項目追加は配列への追記だけで済む。
static const MenuRow s_home_rows[] = {
  { "\xe8\xa9\xb1\xe3\x81\x99", menu_row_talk, talk_subtitle, NULL },
  // UTF-8: "話す"
  { "\xe5\xa4\xa9\xe6\xb0\x97", menu_item_weather, no_subtitle, &s_icon_weather },
  // UTF-8: "天気"
  { "\xe3\x82\xbf\xe3\x82\xa4\xe3\x83\x9e\xe3\x83\xbc", menu_item_timer, timer_row_subtitle, NULL },
  // UTF-8: "タイマー"
  { "\xe3\x82\xb9\xe3\x83\x88\xe3\x83\x83\xe3\x83\x97\xe3\x82\xa6\xe3\x82\xa9\xe3\x83\x83\xe3\x83\x81",
    menu_item_stopwatch, stopwatch_row_subtitle, NULL },
  // UTF-8: "ストップウォッチ"
  { "\xe4\xbc\x9a\xe8\xa9\xb1\xe5\xb1\xa5\xe6\xad\xb4", menu_item_history, no_subtitle, NULL },
  // UTF-8: "会話履歴"
};

// ---------------------------------------------------------------------------
// Screen states
// ---------------------------------------------------------------------------
// 「会話履歴」（ANSWER）に続き、「天気」「タイマー/ストップウォッチ」
// （SLOT）「タイマー設定」もそれぞれ専用のフルスクリーンビューを持つ。
// いずれも Pebble Window は増やさず、単一の s_window 内でレイヤーの
// 表示/非表示と Click Config Provider を切り替えるだけの既存方式を踏襲する。
typedef enum {
  SCREEN_HOME,
  SCREEN_LOADING,
  SCREEN_ANSWER,
  SCREEN_WEATHER,
  SCREEN_SLOT,
  SCREEN_TIMER_SET,
  SCREEN_TIMER_CONFIRM,
  SCREEN_LAPS,
  SCREEN_ALARM,
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

// Answer / Weather (会話履歴と天気は同じスクロール可能テキスト表示を共有する)
static TextLayer   *s_answer_title_layer;
static ScrollLayer *s_answer_scroll_layer;
static TextLayer   *s_answer_text_layer;
static TextLayer   *s_answer_hint_layer;

// Slot view (タイマー/ストップウォッチ共通の操作ビュー)
static TextLayer   *s_slot_title_layer;
static TextLayer   *s_slot_time_layer;
static TextLayer   *s_slot_sub_layer;
static TextLayer   *s_slot_hint_layer;
static int          s_open_slot = -1;  // SCREEN_SLOT が表示中のスロット番号

// Timer set picker (分秒ピッカー)
static TextLayer   *s_tset_title_layer;
static TextLayer   *s_tset_min_layer;
static TextLayer   *s_tset_colon_layer;
static TextLayer   *s_tset_sec_layer;
static TextLayer   *s_tset_hint_layer;
// 新規作成・既存タイマーの「時間設定」いずれも、この初期値から始める。
// 既存タイマーの現在の長さをプリフィルしない（「リセット」という言葉から
// 期待される「まっさらな初期状態に戻る」という直感に合わせるため。ADR-031）。
#define TSET_DEFAULT_MINUTES 5
#define TSET_DEFAULT_SECONDS 0
static int          s_ts_minutes = TSET_DEFAULT_MINUTES;
static int          s_ts_seconds = TSET_DEFAULT_SECONDS;
static int          s_ts_field   = 0;  // 0 = 分選択中, 1 = 秒選択中

// Alarm (タイマー満了。ユーザーが止めるまでバイブを繰り返す)
static TextLayer   *s_alarm_title_layer;
static TextLayer   *s_alarm_msg_layer;
static TextLayer   *s_alarm_hint_layer;
static AppTimer     *s_alarm_timer;
static char          s_alarm_label[SLOT_LABEL_SIZE];
#define ALARM_VIBE_INTERVAL_MS 1000

// HOME click config: menu_layer_set_click_config_onto_window() owns SELECT;
// we chain onto its provider once and add an explicit BACK handler (see
// home_click_config_provider) instead of relying on default fallback, and
// override UP/DOWN with cyclic-scroll versions (home_up_click/home_down_click).
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
static char      s_answer_title_text[16];

// 音声経由でタイマー/SWをセットした直後の応答は ANSWER でなく HOME に戻す
static bool      s_pending_home = false;
// 「天気」経由の応答は ANSWER でなく SCREEN_WEATHER に表示する
static bool      s_pending_weather = false;

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
static void show_screen(Screen screen);
static void send_query(void);
static void send_reset_command(void);
static void refresh_answer_screen(void);
static void refresh_weather_screen(void);
static void refresh_slot_screen(void);
static void refresh_timer_set_screen(void);
static void refresh_timer_confirm_screen(void);
static void refresh_laps_screen(void);
static void refresh_alarm_screen(void);
static void refresh_home_menu(void);
static void set_home_status(const char *text);
static int handle_timer_set(int32_t seconds, const char *label, bool set_pending_home);
static int handle_stopwatch_start(const char *label, bool set_pending_home);
static void clear_history(void);
static void persist_history(void);

// ---------------------------------------------------------------------------
// Slot helpers
// ---------------------------------------------------------------------------
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

// タイマー満了時のバイブを一定間隔で繰り返す（ユーザーが SCREEN_ALARM で
// 何かボタンを押して止めるまで鳴り続ける）。
static void alarm_vibe_timer_cb(void *ctx) {
  vibes_double_pulse();
  s_alarm_timer = app_timer_register(ALARM_VIBE_INTERVAL_MS, alarm_vibe_timer_cb, NULL);
}

static void start_alarm_vibration(void) {
  vibes_double_pulse();
  s_alarm_timer = app_timer_register(ALARM_VIBE_INTERVAL_MS, alarm_vibe_timer_cb, NULL);
}

static void stop_alarm_vibration(void) {
  if (s_alarm_timer) {
    app_timer_cancel(s_alarm_timer);
    s_alarm_timer = NULL;
  }
}

static void handle_timer_fired(int idx, bool vibrate) {
  if (idx < 0 || idx >= SLOT_COUNT || s_slots[idx].kind != SLOT_TIMER) return;
  strncpy(s_alarm_label, s_slots[idx].label, SLOT_LABEL_SIZE - 1);
  s_alarm_label[SLOT_LABEL_SIZE - 1] = '\0';
  clear_slot(idx);
  refresh_home_menu();
  if (vibrate) {
    start_alarm_vibration();
    show_screen(SCREEN_ALARM);
  } else {
    snprintf(s_status_buf, sizeof(s_status_buf),
             "\xe3\x82\xbf\xe3\x82\xa4\xe3\x83\x9e\xe3\x83\xbc\xe7\xb5\x82\xe4\xba\x86 %s",
             s_alarm_label[0] ? s_alarm_label : "");
    // UTF-8: "タイマー終了 <label>"
    set_home_status(s_status_buf);
  }
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

// ---------------------------------------------------------------------------
// Answer / Weather (共有のスクロール可能テキスト表示)
// ---------------------------------------------------------------------------
// 会話履歴 (ANSWER) と天気 (WEATHER) は同じレイヤー構成（タイトル+スクロール
// 本文+下部ヒント）を使い回す。タイトル・本文・ヒントの中身だけ差し替える。
static void set_answer_style_content(const char *title, const char *body, const char *hint) {
  text_layer_set_text(s_answer_title_layer, title);
  strncpy(s_answer_display, body, sizeof(s_answer_display) - 1);
  s_answer_display[sizeof(s_answer_display) - 1] = '\0';
  text_layer_set_text(s_answer_text_layer, s_answer_display);
  text_layer_set_text(s_answer_hint_layer, hint);

  GRect scroll_bounds = layer_get_bounds(scroll_layer_get_layer(s_answer_scroll_layer));
  GSize text_size = text_layer_get_content_size(s_answer_text_layer);
  text_size.h += 8;
  if (text_size.h < scroll_bounds.size.h) text_size.h = scroll_bounds.size.h;
  text_layer_set_size(s_answer_text_layer, GSize(scroll_bounds.size.w, text_size.h));
  scroll_layer_set_content_size(s_answer_scroll_layer, GSize(scroll_bounds.size.w, text_size.h));
  scroll_layer_set_content_offset(s_answer_scroll_layer, GPointZero, false);
}

static void refresh_answer_screen(void) {
  if (s_hist_len == 0) return;
  HistEntry *e = &s_hist[s_hist_view];
  char body[HIST_Q_SIZE + HIST_A_SIZE + 8];
  snprintf(body, sizeof(body), "Q: %s\n\nA: %s", e->q, e->a);
  snprintf(s_answer_title_text, sizeof(s_answer_title_text),
           "%d/%d", s_hist_view + 1, s_hist_len);
  set_answer_style_content(s_answer_title_text, body,
    "UP/DN\xe9\x95\xb7:\xe5\x89\x8d\xe5\xbe\x8c SEL\xe9\x95\xb7:\xe3\x83\xaa\xe3\x82\xbb\xe3\x83\x83\xe3\x83\x88");
  // UTF-8: "UP/DN長:前後 SEL長:リセット"
}

static void refresh_weather_screen(void) {
  char body[HIST_Q_SIZE + HIST_A_SIZE + 8];
  snprintf(body, sizeof(body), "%s", s_response_buf);
  set_answer_style_content(
    "\xe5\xa4\xa9\xe6\xb0\x97",  // UTF-8: "天気"
    body,
    "BACK/SEL: \xe6\x88\xbb\xe3\x82\x8b");  // UTF-8: "BACK/SEL: 戻る"
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
  return 1;
}

static uint16_t menu_get_num_rows(MenuLayer *menu, uint16_t section, void *ctx) {
  return (uint16_t)ARRAY_LENGTH(s_home_rows);
}

static void menu_draw_row(GContext *ctx, const Layer *cell_layer,
                          MenuIndex *index, void *data) {
  if (index->row >= ARRAY_LENGTH(s_home_rows)) return;
  const MenuRow *row = &s_home_rows[index->row];
  GBitmap *icon = row->icon ? *row->icon : NULL;
  menu_cell_basic_draw(ctx, cell_layer, row->title, row->subtitle(), icon);
}

// ---------------------------------------------------------------------------
// 行サブタイトル（タイマー/SW は 1 件までなので、あればその状態、なければ
// "未設定" を表示するだけでよい。行自体は増減しない）
// ---------------------------------------------------------------------------
static const char *no_subtitle(void) { return NULL; }

static const char *talk_subtitle(void) {
  return "Select\xe3\x81\xa7\xe9\x9f\xb3\xe5\xa3\xb0\xe5\x85\xa5\xe5\x8a\x9b";
  // UTF-8: "Selectで音声入力"
}

static const char *slot_kind_row_subtitle(SlotKind kind) {
  static char buf[48];
  int idx = -1;
  for (int i = 0; i < SLOT_COUNT; i++) {
    if (s_slots[i].kind == kind) { idx = i; break; }
  }
  if (idx < 0) {
    return "\xe6\x9c\xaa\xe8\xa8\xad\xe5\xae\x9a";  // UTF-8: "未設定"
  }
  Slot *s = &s_slots[idx];
  char tbuf[12];
  format_hms(tbuf, sizeof(tbuf), slot_display_seconds(s));
  const char *paused = "(\xe5\x81\x9c\xe6\xad\xa2)";  // UTF-8: "(停止)"
  char state[32];
  if (kind == SLOT_STOPWATCH && s->last_lap > 0) {
    char lbuf[12];
    format_hms(lbuf, sizeof(lbuf), s->last_lap);
    snprintf(state, sizeof(state), "%s%s%s Lap %s",
             tbuf, s->running ? "" : " ", s->running ? "" : paused, lbuf);
  } else if (s->running) {
    snprintf(state, sizeof(state), "%s", tbuf);
  } else {
    snprintf(state, sizeof(state), "%s %s", tbuf, paused);
  }
  if (s->label[0]) {
    snprintf(buf, sizeof(buf), "%s %s", s->label, state);
  } else {
    snprintf(buf, sizeof(buf), "%s", state);
  }
  return buf;
}

static const char *timer_row_subtitle(void) {
  return slot_kind_row_subtitle(SLOT_TIMER);
}

static const char *stopwatch_row_subtitle(void) {
  return slot_kind_row_subtitle(SLOT_STOPWATCH);
}

// ---------------------------------------------------------------------------
// Slot actions (タイマー/SW 操作。SCREEN_SLOT のクリックハンドラから直接
// 呼ばれる。all local — no LLM round-trip)
// ---------------------------------------------------------------------------
static void slot_toggle(int idx) {
  Slot *s = &s_slots[idx];
  if (s->kind == SLOT_TIMER) {
    if (s->running) {
      wakeup_cancel(s->wakeup_id);
      s->wakeup_id = -1;
      int32_t rem = (int32_t)(s->target_ts - time(NULL));
      s->remaining = rem < 1 ? 1 : rem;
      s->running = 0;
    } else {
      if (schedule_timer_wakeup(idx, s->remaining)) {
        s->running = 1;
      } else {
        set_home_status("\xe4\xba\x88\xe7\xb4\x84\xe5\xa4\xb1\xe6\x95\x97");  // "予約失敗"
      }
    }
  } else if (s->kind == SLOT_STOPWATCH) {
    time_t now = time(NULL);
    if (s->running) {
      s->elapsed = (int32_t)(now - s->start_ts);
      s->running = 0;
    } else {
      s->start_ts = now - s->elapsed;
      s->running  = 1;
    }
  }
  persist_slot(idx);
  refresh_home_menu();
}

// SW のリセット（0秒・停止状態・Lap履歴クリア）。
// タイマー側はここでは扱わない — 「リセットしても元の時間に戻るだけで
// 新しい時間を設定できない」との指摘を受け、SELECT長押しは分秒ピッカー
// （現在の時間をプリセットした状態）を開く方式に変更した
// （slot_select_long_click 参照）。
static void slot_reset(int idx) {
  Slot *s = &s_slots[idx];
  if (s->kind != SLOT_STOPWATCH) return;
  s->running   = 0;
  s->elapsed   = 0;
  s->start_ts  = 0;
  s->last_lap  = 0;
  s->lap_count = 0;
  persist_slot(idx);
  refresh_home_menu();
}

// Lap は履歴として複数件保持する（MAX_LAPS 超過時は最古を捨てて詰める）。
static void slot_lap(int idx) {
  Slot *s = &s_slots[idx];
  if (s->kind != SLOT_STOPWATCH || !s->running) return;
  int32_t t = slot_display_seconds(s);
  s->last_lap = t;
  if (s->lap_count < MAX_LAPS) {
    s->laps[s->lap_count] = t;
    s->lap_count++;
  } else {
    memmove(s->laps, s->laps + 1, sizeof(int32_t) * (MAX_LAPS - 1));
    s->laps[MAX_LAPS - 1] = t;
  }
  persist_slot(idx);
  refresh_home_menu();
}

static void slot_delete(int idx) {
  Slot *s = &s_slots[idx];
  if (s->kind == SLOT_TIMER && s->running && s->wakeup_id >= 0) {
    wakeup_cancel(s->wakeup_id);
  }
  clear_slot(idx);
  refresh_home_menu();
}

// ---------------------------------------------------------------------------
// SCREEN_SLOT (タイマー/ストップウォッチ専用ビュー)
// ---------------------------------------------------------------------------
static void refresh_slot_screen(void) {
  if (s_open_slot < 0 || s_open_slot >= SLOT_COUNT) return;
  Slot *s = &s_slots[s_open_slot];
  if (s->kind == SLOT_EMPTY) return;

  bool is_timer = (s->kind == SLOT_TIMER);
  text_layer_set_text(s_slot_title_layer,
    is_timer ? "\xe3\x82\xbf\xe3\x82\xa4\xe3\x83\x9e\xe3\x83\xbc"           // "タイマー"
             : "\xe3\x82\xb9\xe3\x83\x88\xe3\x83\x83\xe3\x83\x97\xe3\x82"
               "\xa6\xe3\x82\xa9\xe3\x83\x83\xe3\x83\x81");                // "ストップウォッチ"

  static char time_buf[16];
  format_hms(time_buf, sizeof(time_buf), slot_display_seconds(s));
  text_layer_set_text(s_slot_time_layer, time_buf);

  static char sub_buf[64];
  const char *paused = "\xe5\x81\x9c\xe6\xad\xa2\xe4\xb8\xad";  // "停止中"
  const char *running_timer = "\xe6\xae\x8b\xe3\x82\x8a";        // "残り"
  const char *running_sw    = "\xe8\xa8\x88\xe6\xb8\xac\xe4\xb8\xad";  // "計測中"
  if (s->label[0]) {
    if (s->running) {
      snprintf(sub_buf, sizeof(sub_buf), "%s %s", s->label, is_timer ? running_timer : running_sw);
    } else {
      snprintf(sub_buf, sizeof(sub_buf), "%s %s", s->label, paused);
    }
  } else {
    snprintf(sub_buf, sizeof(sub_buf), "%s", s->running ? (is_timer ? running_timer : running_sw) : paused);
  }
  if (!is_timer && s->last_lap > 0) {
    char lbuf[12];
    format_hms(lbuf, sizeof(lbuf), s->last_lap);
    size_t len = strlen(sub_buf);
    snprintf(sub_buf + len, sizeof(sub_buf) - len, " Lap %s", lbuf);
  }
  text_layer_set_text(s_slot_sub_layer, sub_buf);

  text_layer_set_text(s_slot_hint_layer, is_timer
    ? "SEL:\xe4\xb8\x80\xe6\x99\x82\xe5\x81\x9c\xe6\xad\xa2/\xe5\x86\x8d\xe9\x96\x8b "
      "\xe9\x95\xb7:\xe6\x99\x82\xe9\x96\x93\xe8\xa8\xad\xe5\xae\x9a"
      // "SEL:一時停止/再開 長:時間設定"
    : "SEL:\xe9\x96\x8b\xe5\xa7\x8b/\xe5\x81\x9c\xe6\xad\xa2 UP:Lap DOWN:\xe4\xb8\x80\xe8\xa6\xa7 "
      "\xe9\x95\xb7:\xe3\x83\xaa\xe3\x82\xbb\xe3\x83\x83\xe3\x83\x88");
      // "SEL:開始/停止 UP:Lap DOWN:一覧 長:リセット"
}

static void slot_select_click(ClickRecognizerRef r, void *ctx) {
  slot_toggle(s_open_slot);
  refresh_slot_screen();
}

// タイマーは「時間を設定し直す」(分秒ピッカーを現在の長さで開く) へ、
// SW は従来どおり即座にリセットする。
static void slot_select_long_click(ClickRecognizerRef r, void *ctx) {
  Slot *s = &s_slots[s_open_slot];
  if (s->kind == SLOT_TIMER) {
    // 既存の長さはプリフィルせず、常にデフォルト値から時間設定を開始する
    // （ADR-031）。
    s_ts_minutes = TSET_DEFAULT_MINUTES;
    s_ts_seconds = TSET_DEFAULT_SECONDS;
    s_ts_field   = 0;
    show_screen(SCREEN_TIMER_SET);
  } else {
    slot_reset(s_open_slot);
    refresh_slot_screen();
  }
}

static void slot_up_click(ClickRecognizerRef r, void *ctx) {
  slot_lap(s_open_slot);
  refresh_slot_screen();
}

// SW の Lap 履歴一覧 (SCREEN_LAPS) へ。タイマーには Lap がないため無視する。
static void slot_down_click(ClickRecognizerRef r, void *ctx) {
  if (s_open_slot < 0 || s_slots[s_open_slot].kind != SLOT_STOPWATCH) return;
  show_screen(SCREEN_LAPS);
}

static void slot_back_click(ClickRecognizerRef r, void *ctx) {
  show_screen(SCREEN_HOME);
}

static void slot_back_long_click(ClickRecognizerRef r, void *ctx) {
  slot_delete(s_open_slot);
  show_screen(SCREEN_HOME);
}

static void slot_click_config(void *ctx) {
  window_single_click_subscribe(BUTTON_ID_SELECT, slot_select_click);
  window_long_click_subscribe(BUTTON_ID_SELECT, 700, slot_select_long_click, NULL);
  window_single_click_subscribe(BUTTON_ID_UP, slot_up_click);
  window_single_click_subscribe(BUTTON_ID_DOWN, slot_down_click);
  window_single_click_subscribe(BUTTON_ID_BACK, slot_back_click);
  window_long_click_subscribe(BUTTON_ID_BACK, 700, slot_back_long_click, NULL);
}

// ---------------------------------------------------------------------------
// SCREEN_LAPS (SW の Lap 履歴一覧。ANSWER/WEATHER と同じ共有レイヤーを使う)
// ---------------------------------------------------------------------------
static void refresh_laps_screen(void) {
  if (s_open_slot < 0 || s_slots[s_open_slot].kind != SLOT_STOPWATCH) return;
  Slot *s = &s_slots[s_open_slot];
  char body[512];
  size_t len = 0;
  body[0] = '\0';
  if (s->lap_count == 0) {
    snprintf(body, sizeof(body), "\xe3\x83\xa9\xe3\x83\x83\xe3\x83\x97\xe3\x81\xaa\xe3\x81\x97");
    // UTF-8: "ラップなし"
  } else {
    for (int i = 0; i < s->lap_count; i++) {
      char lbuf[12];
      format_hms(lbuf, sizeof(lbuf), s->laps[i]);
      len += snprintf(body + len, sizeof(body) - len, "Lap %d: %s\n", i + 1, lbuf);
      if (len >= sizeof(body)) break;
    }
  }
  set_answer_style_content("Lap", body, "BACK: \xe6\x88\xbb\xe3\x82\x8b");  // UTF-8: "BACK: 戻る"
}

// ---------------------------------------------------------------------------
// SCREEN_TIMER_SET (タイマー設定, 分秒ピッカー)
// ---------------------------------------------------------------------------
static void refresh_timer_set_screen(void) {
  static char min_buf[4];
  static char sec_buf[4];
  snprintf(min_buf, sizeof(min_buf), "%02d", s_ts_minutes);
  snprintf(sec_buf, sizeof(sec_buf), "%02d", s_ts_seconds);
  text_layer_set_text(s_tset_min_layer, min_buf);
  text_layer_set_text(s_tset_sec_layer, sec_buf);

  bool min_selected = (s_ts_field == 0);
  text_layer_set_background_color(s_tset_min_layer, min_selected ? GColorBlack : GColorWhite);
  text_layer_set_text_color(s_tset_min_layer, min_selected ? GColorWhite : GColorBlack);
  text_layer_set_background_color(s_tset_sec_layer, min_selected ? GColorWhite : GColorBlack);
  text_layer_set_text_color(s_tset_sec_layer, min_selected ? GColorBlack : GColorWhite);

  // ヒントは編集中フィールドで SEL/BACK の行き先が変わるので都度更新する
  text_layer_set_text(s_tset_hint_layer, min_selected
    ? "UP/DN:\xe5\xa2\x97\xe6\xb8\x9b SEL:\xe7\xa7\x92\xe3\x81\xb8 "
      "BACK:\xe3\x82\xad\xe3\x83\xa3\xe3\x83\xb3\xe3\x82\xbb\xe3\x83\xab"
      // "UP/DN:増減 SEL:秒へ BACK:キャンセル"
    : "UP/DN:\xe5\xa2\x97\xe6\xb8\x9b SEL:\xe7\xa2\xba\xe8\xaa\x8d\xe3\x81\xb8 "
      "BACK:\xe5\x88\x86\xe3\x81\xb8\xe6\x88\xbb\xe3\x82\x8b");
      // "UP/DN:増減 SEL:確認へ BACK:分へ戻る"
}

// 秒は10秒刻み（0/10/.../50）で選ぶ。
#define TSET_SECOND_STEP 10

// Wakeup API は30秒未満を予約できず、handle_timer_set() がその場合
// TIMER_MIN_SECONDS(30) に黙って切り上げてしまう。ピッカーで30秒未満を
// 選べてしまうと「10秒を選んだのに30秒になる」という見た目と結果の食い違い
// が起きるため、分=0のときは秒を30未満に選べないようにする。
static void tset_clamp_min_duration(void) {
  if (s_ts_minutes == 0 && s_ts_seconds < TIMER_MIN_SECONDS) {
    s_ts_seconds = TIMER_MIN_SECONDS;
  }
}

static void tset_up_click(ClickRecognizerRef r, void *ctx) {
  if (s_ts_field == 0) {
    s_ts_minutes = (s_ts_minutes + 1) % 181;
  } else {
    s_ts_seconds = (s_ts_seconds + TSET_SECOND_STEP) % 60;
  }
  tset_clamp_min_duration();
  refresh_timer_set_screen();
}

static void tset_down_click(ClickRecognizerRef r, void *ctx) {
  if (s_ts_field == 0) {
    s_ts_minutes = (s_ts_minutes + 180) % 181;
  } else {
    s_ts_seconds = (s_ts_seconds + (60 - TSET_SECOND_STEP)) % 60;
  }
  tset_clamp_min_duration();
  refresh_timer_set_screen();
}

// SELECT 短押しは「分→秒→確認画面」と前に進むだけにする（長押しでの確定は
// 直感に反するという指摘を受け、専用の確認ビュー SCREEN_TIMER_CONFIRM へ
// 遷移させる方式に変更した）。確認画面で BACK を押すと分の入力からやり直せる。
static void tset_select_click(ClickRecognizerRef r, void *ctx) {
  if (s_ts_field == 0) {
    s_ts_field = 1;
    refresh_timer_set_screen();
  } else {
    show_screen(SCREEN_TIMER_CONFIRM);
  }
}

// 秒を編集中の BACK は一段階戻って分編集へ（キャンセルではない）。
// 分編集中の BACK のみ HOME へ戻る（キャンセル）。
static void tset_back_click(ClickRecognizerRef r, void *ctx) {
  if (s_ts_field == 1) {
    s_ts_field = 0;
    refresh_timer_set_screen();
  } else {
    show_screen(SCREEN_HOME);
  }
}

static void tset_click_config(void *ctx) {
  window_single_click_subscribe(BUTTON_ID_UP, tset_up_click);
  window_single_click_subscribe(BUTTON_ID_DOWN, tset_down_click);
  window_single_click_subscribe(BUTTON_ID_SELECT, tset_select_click);
  window_single_click_subscribe(BUTTON_ID_BACK, tset_back_click);
}

// ---------------------------------------------------------------------------
// SCREEN_TIMER_CONFIRM (分秒ピッカーの確定画面。長押しの代わりに明示的な
// 確認ステップを設ける)。表示は SCREEN_SLOT と同じレイヤーを再利用する
// （幅の狭い s_tset_min_layer 等を流用すると ADR-026 と同じ省略記号の
// 問題が再発するため、フル幅の s_slot_* 側を使う）。
// ---------------------------------------------------------------------------
static void refresh_timer_confirm_screen(void) {
  text_layer_set_text(s_slot_title_layer,
    "\xe7\xa2\xba\xe8\xaa\x8d");  // UTF-8: "確認"
  static char value_buf[16];
  snprintf(value_buf, sizeof(value_buf), "%02d:%02d", s_ts_minutes, s_ts_seconds);
  text_layer_set_text(s_slot_time_layer, value_buf);
  text_layer_set_text(s_slot_sub_layer,
    "\xe3\x81\x93\xe3\x81\xae\xe6\x99\x82\xe9\x96\x93\xe3\x81\xa7\xe9\x96\x8b"
    "\xe5\xa7\x8b\xe3\x81\x97\xe3\x81\xbe\xe3\x81\x99\xe3\x81\x8b\xef\xbc\x9f");
    // UTF-8: "この時間で開始しますか？"
  text_layer_set_text(s_slot_hint_layer,
    "SEL:\xe9\x96\x8b\xe5\xa7\x8b BACK:\xe3\x82\x84\xe3\x82\x8a\xe7\x9b\xb4\xe3\x81\x99");
    // UTF-8: "SEL:開始 BACK:やり直す"
}

static void tconfirm_select_click(ClickRecognizerRef r, void *ctx) {
  int32_t seconds = (int32_t)(s_ts_minutes * 60 + s_ts_seconds);
  if (s_open_slot >= 0 && s_slots[s_open_slot].kind == SLOT_TIMER) {
    // 既存タイマーの時間を設定し直す（SLOT の SELECT長押しから来たケース）。
    // handle_timer_set() は「タイマーは1件まで」のガードで既存分を弾いて
    // しまうため、ここでは直接スロットを書き換える。
    Slot *s = &s_slots[s_open_slot];
    if (s->running && s->wakeup_id >= 0) wakeup_cancel(s->wakeup_id);
    s->wakeup_id = -1;
    s->duration  = seconds < TIMER_MIN_SECONDS ? TIMER_MIN_SECONDS : seconds;
    s->running   = 0;
    s->remaining = s->duration;
    persist_slot(s_open_slot);
    refresh_home_menu();
    show_screen(SCREEN_SLOT);
  } else {
    int idx = handle_timer_set(seconds, NULL, false);
    if (idx >= 0) {
      s_open_slot = idx;
      show_screen(SCREEN_SLOT);
    } else {
      show_screen(SCREEN_HOME);
    }
  }
}

// やり直す場合は分の入力からやり直す（値は保持したまま）
static void tconfirm_back_click(ClickRecognizerRef r, void *ctx) {
  s_ts_field = 0;
  show_screen(SCREEN_TIMER_SET);
}

static void tconfirm_click_config(void *ctx) {
  window_single_click_subscribe(BUTTON_ID_SELECT, tconfirm_select_click);
  window_single_click_subscribe(BUTTON_ID_BACK, tconfirm_back_click);
}

// ---------------------------------------------------------------------------
// SCREEN_ALARM (タイマー満了。止めるまでバイブを繰り返す)
// ---------------------------------------------------------------------------
static void refresh_alarm_screen(void) {
  text_layer_set_text(s_alarm_msg_layer,
    s_alarm_label[0] ? s_alarm_label
                      : "\xe3\x82\xbf\xe3\x82\xa4\xe3\x83\x9e\xe3\x83\xbc\xe7\xb5\x82\xe4\xba\x86");
                      // "タイマー終了"
}

static void alarm_dismiss_click(ClickRecognizerRef r, void *ctx) {
  stop_alarm_vibration();
  show_screen(SCREEN_HOME);
}

static void alarm_click_config(void *ctx) {
  window_single_click_subscribe(BUTTON_ID_SELECT, alarm_dismiss_click);
  window_single_click_subscribe(BUTTON_ID_BACK, alarm_dismiss_click);
  window_single_click_subscribe(BUTTON_ID_UP, alarm_dismiss_click);
  window_single_click_subscribe(BUTTON_ID_DOWN, alarm_dismiss_click);
}

// ---------------------------------------------------------------------------
// Menu row callbacks
// ---------------------------------------------------------------------------
static void menu_row_talk(void) {
  if (s_dictation_session) {
    dictation_session_start(s_dictation_session);
  } else {
    set_home_status("\xe3\x83\x9e\xe3\x82\xa4\xe3\x82\xaf\xe9\x9d\x9e\xe5\xaf\xbe\xe5\xbf\x9c");
    // UTF-8: "マイク非対応"
  }
}

static void menu_item_history(void) {
  if (s_hist_len > 0) {
    s_hist_view = s_hist_len - 1;
    show_screen(SCREEN_ANSWER);
  } else {
    set_home_status("\xe5\xb1\xa5\xe6\xad\xb4\xe3\x81\xaa\xe3\x81\x97");  // "履歴なし"
  }
}

// 未設定ならタイマー設定画面(分秒ピッカー)へ、設定済みなら専用ビュー
// (SCREEN_SLOT) を開く。タイマーは同時に1件までしか保持できない。
static void menu_item_timer(void) {
  for (int i = 0; i < SLOT_COUNT; i++) {
    if (s_slots[i].kind == SLOT_TIMER) {
      s_open_slot = i;
      show_screen(SCREEN_SLOT);
      return;
    }
  }
  s_open_slot  = -1;  // 新規作成であって既存タイマーの編集ではないことを明示
  s_ts_minutes = TSET_DEFAULT_MINUTES;
  s_ts_seconds = TSET_DEFAULT_SECONDS;
  s_ts_field   = 0;
  show_screen(SCREEN_TIMER_SET);
}

// 未設定なら即座にストップウォッチを開始、設定済みなら専用ビューを開く
static void menu_item_stopwatch(void) {
  for (int i = 0; i < SLOT_COUNT; i++) {
    if (s_slots[i].kind == SLOT_STOPWATCH) {
      s_open_slot = i;
      show_screen(SCREEN_SLOT);
      return;
    }
  }
  int idx = handle_stopwatch_start(NULL, false);
  if (idx >= 0) {
    s_open_slot = idx;
    show_screen(SCREEN_SLOT);
  }
}

// 音声を使わず「今日の天気を教えて」を送信する。既存の get_weather ツール（JS 側）が
// 応答するため、ここでは通常の音声質問と同じ send_query() の経路をそのまま再利用する。
// 応答表示は ANSWER でなく専用の SCREEN_WEATHER に出す (s_pending_weather 参照)。
static void menu_item_weather(void) {
  snprintf(s_query_buf, QUERY_BUF_SIZE, "%s",
    "\xe4\xbb\x8a\xe6\x97\xa5\xe3\x81\xae\xe5\xa4\xa9\xe6\xb0\x97\xe3\x82\x92"
    "\xe6\x95\x99\xe3\x81\x88\xe3\x81\xa6");
  // UTF-8: "今日の天気を教えて"
  s_pending_weather = true;
  send_query();
}

static void menu_select_callback(MenuLayer *menu, MenuIndex *index, void *ctx) {
  if (index->row >= ARRAY_LENGTH(s_home_rows)) return;
  s_home_rows[index->row].callback();
}

// ---------------------------------------------------------------------------
// TickTimerService (home / slot-view foreground only; redraw recomputes from
// timestamps)
// ---------------------------------------------------------------------------
static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
  if (s_current_screen == SCREEN_HOME && s_home_menu_layer) {
    layer_mark_dirty(menu_layer_get_layer(s_home_menu_layer));
  } else if (s_current_screen == SCREEN_SLOT) {
    refresh_slot_screen();
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
  layer_set_hidden(text_layer_get_layer(s_slot_title_layer), true);
  layer_set_hidden(text_layer_get_layer(s_slot_time_layer), true);
  layer_set_hidden(text_layer_get_layer(s_slot_sub_layer), true);
  layer_set_hidden(text_layer_get_layer(s_slot_hint_layer), true);
  layer_set_hidden(text_layer_get_layer(s_tset_title_layer), true);
  layer_set_hidden(text_layer_get_layer(s_tset_min_layer), true);
  layer_set_hidden(text_layer_get_layer(s_tset_colon_layer), true);
  layer_set_hidden(text_layer_get_layer(s_tset_sec_layer), true);
  layer_set_hidden(text_layer_get_layer(s_tset_hint_layer), true);
  layer_set_hidden(text_layer_get_layer(s_alarm_title_layer), true);
  layer_set_hidden(text_layer_get_layer(s_alarm_msg_layer), true);
  layer_set_hidden(text_layer_get_layer(s_alarm_hint_layer), true);
}

// ---------------------------------------------------------------------------
// Click handlers (ANSWER / WEATHER / LOADING)
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

// 天気は単発の応答表示のため、履歴前後移動・リセットの長押しはバインドしない
static void weather_click_config(void *ctx) {
  window_single_click_subscribe(BUTTON_ID_SELECT, answer_select_click);
  window_single_click_subscribe(BUTTON_ID_BACK, answer_back_click);
  window_single_click_subscribe(BUTTON_ID_UP, answer_up_click);
  window_single_click_subscribe(BUTTON_ID_DOWN, answer_down_click);
}

// Lap 一覧は HOME でなく SLOT (ストップウォッチ操作画面) へ戻る
static void laps_back_click(ClickRecognizerRef r, void *ctx) {
  show_screen(SCREEN_SLOT);
}

static void laps_click_config(void *ctx) {
  window_single_click_subscribe(BUTTON_ID_SELECT, laps_back_click);
  window_single_click_subscribe(BUTTON_ID_BACK, laps_back_click);
  window_single_click_subscribe(BUTTON_ID_UP, answer_up_click);
  window_single_click_subscribe(BUTTON_ID_DOWN, answer_down_click);
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

// UP/DOWN は循環スクロールにする（最後の行で DOWN → 先頭へ、先頭の行で UP → 末尾へ）。
// MenuLayer の既定の UP/DOWN（末尾/先頭で止まる）を上書きする。
static void home_up_click(ClickRecognizerRef r, void *ctx) {
  MenuIndex idx = menu_layer_get_selected_index(s_home_menu_layer);
  idx.row = (idx.row == 0) ? (uint16_t)(ARRAY_LENGTH(s_home_rows) - 1) : idx.row - 1;
  // MenuRowAlignNone だと選択行が切り替わってもビューポートがスクロールせず、
  // どの行が選ばれているか画面上わからなくなるため Center を指定する。
  menu_layer_set_selected_index(s_home_menu_layer, idx, MenuRowAlignCenter, true);
}

static void home_down_click(ClickRecognizerRef r, void *ctx) {
  MenuIndex idx = menu_layer_get_selected_index(s_home_menu_layer);
  idx.row = ((size_t)idx.row + 1 >= ARRAY_LENGTH(s_home_rows)) ? 0 : idx.row + 1;
  menu_layer_set_selected_index(s_home_menu_layer, idx, MenuRowAlignCenter, true);
}

static void home_click_config_provider(void *context) {
  if (s_home_menu_ccp) {
    s_home_menu_ccp(context);
  }
  window_single_click_subscribe(BUTTON_ID_BACK, home_back_click);
  window_single_click_subscribe(BUTTON_ID_UP, home_up_click);
  window_single_click_subscribe(BUTTON_ID_DOWN, home_down_click);
}

// ---------------------------------------------------------------------------
// show_screen
// ---------------------------------------------------------------------------
static void show_screen(Screen screen) {
  s_current_screen = screen;
  hide_all_screens();

  if (screen == SCREEN_HOME || screen == SCREEN_SLOT) {
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
                                    MenuIndex(0, 0),
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

    case SCREEN_WEATHER:
      refresh_weather_screen();
      layer_set_hidden(text_layer_get_layer(s_answer_title_layer), false);
      layer_set_hidden(scroll_layer_get_layer(s_answer_scroll_layer), false);
      layer_set_hidden(text_layer_get_layer(s_answer_hint_layer), false);
      window_set_click_config_provider(s_window, weather_click_config);
      break;

    case SCREEN_SLOT:
      refresh_slot_screen();
      layer_set_hidden(text_layer_get_layer(s_slot_title_layer), false);
      layer_set_hidden(text_layer_get_layer(s_slot_time_layer), false);
      layer_set_hidden(text_layer_get_layer(s_slot_sub_layer), false);
      layer_set_hidden(text_layer_get_layer(s_slot_hint_layer), false);
      window_set_click_config_provider(s_window, slot_click_config);
      break;

    case SCREEN_TIMER_SET:
      refresh_timer_set_screen();
      layer_set_hidden(text_layer_get_layer(s_tset_title_layer), false);
      layer_set_hidden(text_layer_get_layer(s_tset_min_layer), false);
      layer_set_hidden(text_layer_get_layer(s_tset_colon_layer), false);
      layer_set_hidden(text_layer_get_layer(s_tset_sec_layer), false);
      layer_set_hidden(text_layer_get_layer(s_tset_hint_layer), false);
      window_set_click_config_provider(s_window, tset_click_config);
      break;

    case SCREEN_TIMER_CONFIRM:
      refresh_timer_confirm_screen();
      layer_set_hidden(text_layer_get_layer(s_slot_title_layer), false);
      layer_set_hidden(text_layer_get_layer(s_slot_time_layer), false);
      layer_set_hidden(text_layer_get_layer(s_slot_sub_layer), false);
      layer_set_hidden(text_layer_get_layer(s_slot_hint_layer), false);
      window_set_click_config_provider(s_window, tconfirm_click_config);
      break;

    case SCREEN_LAPS:
      refresh_laps_screen();
      layer_set_hidden(text_layer_get_layer(s_answer_title_layer), false);
      layer_set_hidden(scroll_layer_get_layer(s_answer_scroll_layer), false);
      layer_set_hidden(text_layer_get_layer(s_answer_hint_layer), false);
      window_set_click_config_provider(s_window, laps_click_config);
      break;

    case SCREEN_ALARM:
      refresh_alarm_screen();
      layer_set_hidden(text_layer_get_layer(s_alarm_title_layer), false);
      layer_set_hidden(text_layer_get_layer(s_alarm_msg_layer), false);
      layer_set_hidden(text_layer_get_layer(s_alarm_hint_layer), false);
      window_set_click_config_provider(s_window, alarm_click_config);
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
    s_pending_weather = false;
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
// 同時に保持できるタイマーは 1 件まで（ADR-022）。音声経由（LLM の
// set_timer 関数呼び出し）でも同じ制限を適用するため、この関数自身でガードする。
// 成功時は作成したスロット番号、失敗時は -1 を返す
static int handle_timer_set(int32_t seconds, const char *label, bool set_pending_home) {
  for (int i = 0; i < SLOT_COUNT; i++) {
    if (s_slots[i].kind == SLOT_TIMER) {
      set_home_status("\xe3\x82\xbf\xe3\x82\xa4\xe3\x83\x9e\xe3\x83\xbc\xe3\x81\xaf"
                      "1\xe3\x81\xa4\xe3\x81\xbe\xe3\x81\xa7\xe3\x81\xa7\xe3\x81\x99");
      // UTF-8: "タイマーは1つまでです"
      return -1;
    }
  }
  int idx = find_free_slot();
  if (idx < 0) {
    set_home_status("\xe3\x82\xbf\xe3\x82\xa4\xe3\x83\x9e\xe3\x83\xbc\xe6\x9e\xa0"
                    "\xe6\xba\x80\xe6\x9d\xaf");  // UTF-8: "タイマー枠満杯"
    return -1;
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
    return idx;
  }
  clear_slot(idx);
  set_home_status("\xe4\xba\x88\xe7\xb4\x84\xe5\xa4\xb1\xe6\x95\x97");  // "予約失敗"
  return -1;
}

// 同時に保持できる SW は 1 件まで（ADR-022）。音声経由（LLM の
// start_stopwatch 関数呼び出し）でも同じ制限を適用するため、この関数自身でガードする。
// 成功時は作成したスロット番号、失敗時は -1 を返す
static int handle_stopwatch_start(const char *label, bool set_pending_home) {
  for (int i = 0; i < SLOT_COUNT; i++) {
    if (s_slots[i].kind == SLOT_STOPWATCH) {
      set_home_status("\xe3\x82\xb9\xe3\x83\x88\xe3\x83\x83\xe3\x83\x97\xe3\x82\xa6"
                      "\xe3\x82\xa9\xe3\x83\x83\xe3\x83\x81\xe3\x81\xaf"
                      "1\xe3\x81\xa4\xe3\x81\xbe\xe3\x81\xa7\xe3\x81\xa7\xe3\x81\x99");
      // UTF-8: "ストップウォッチは1つまでです"
      return -1;
    }
  }
  int idx = find_free_slot();
  if (idx < 0) {
    set_home_status("\xe3\x82\xbf\xe3\x82\xa4\xe3\x83\x9e\xe3\x83\xbc\xe6\x9e\xa0"
                    "\xe6\xba\x80\xe6\x9d\xaf");  // UTF-8: "タイマー枠満杯"
    return -1;
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
  return idx;
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
    } else if (s_pending_weather) {
      s_pending_weather = false;
      show_screen(SCREEN_WEATHER);
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
      s_pending_weather = false;
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
  s_pending_weather = false;
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

  s_icon_weather = gbitmap_create_with_resource(RESOURCE_ID_ICON_WEATHER);

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

  // ── Answer / Weather (共有レイヤー) ─────────────────────────────────────
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

  s_answer_hint_layer = make_bottom_hint(root, bounds, "");

  // ── Slot view (タイマー/ストップウォッチ) ───────────────────────────────
  // 時間表示+サブテキストのブロックをコンテンツ領域内で上下中央に配置する。
  s_slot_title_layer = make_title_bar(root, bounds, "");

  const int slot_time_h  = 50;
  const int slot_sub_h   = 30;
  const int slot_gap     = 4;
  int slot_block_h = slot_time_h + slot_gap + slot_sub_h;
  int slot_block_y = content_top + (content_h - slot_block_h) / 2;

  s_slot_time_layer = text_layer_create(GRect(0, slot_block_y, bounds.size.w, slot_time_h));
  text_layer_set_font(s_slot_time_layer, fonts_get_system_font(FONT_KEY_BITHAM_42_BOLD));
  text_layer_set_text_alignment(s_slot_time_layer, GTextAlignmentCenter);
  layer_add_child(root, text_layer_get_layer(s_slot_time_layer));

  s_slot_sub_layer = text_layer_create(
    GRect(0, slot_block_y + slot_time_h + slot_gap, bounds.size.w, slot_sub_h));
  text_layer_set_font(s_slot_sub_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18));
  text_layer_set_text_alignment(s_slot_sub_layer, GTextAlignmentCenter);
  layer_add_child(root, text_layer_get_layer(s_slot_sub_layer));

  s_slot_hint_layer = make_bottom_hint(root, bounds, "");

  // ── Timer set picker (分秒ピッカー) ──────────────────────────────────────
  // 分・コロン・秒をまとめて画面中央に配置し、選択中フィールドのハイライト
  // (背景反転) が数字の幅にぴったり収まる（画面端まで伸びない）ようにする。
  s_tset_title_layer = make_title_bar(root, bounds,
    "\xe3\x82\xbf\xe3\x82\xa4\xe3\x83\x9e\xe3\x83\xbc\xe8\xa8\xad\xe5\xae\x9a");
  // UTF-8: "タイマー設定"

  // digit_w は BITHAM_42_BOLD で2桁がちょうど収まる幅。狭すぎると Pebble の
  // TextLayer が描画しきれず "..." (省略記号) になってしまう（ADR-026）。
  const int tset_digit_w = 70;
  const int tset_colon_w = 20;
  const int tset_row_h   = 50;
  int tset_total_w = tset_digit_w * 2 + tset_colon_w;
  int tset_start_x = (bounds.size.w - tset_total_w) / 2;
  int tset_row_y   = content_top + (content_h - tset_row_h) / 2;

  s_tset_min_layer = text_layer_create(GRect(tset_start_x, tset_row_y, tset_digit_w, tset_row_h));
  text_layer_set_font(s_tset_min_layer, fonts_get_system_font(FONT_KEY_BITHAM_42_BOLD));
  text_layer_set_text_alignment(s_tset_min_layer, GTextAlignmentCenter);
  layer_add_child(root, text_layer_get_layer(s_tset_min_layer));

  s_tset_colon_layer = text_layer_create(
    GRect(tset_start_x + tset_digit_w, tset_row_y, tset_colon_w, tset_row_h));
  text_layer_set_font(s_tset_colon_layer, fonts_get_system_font(FONT_KEY_BITHAM_42_BOLD));
  text_layer_set_text_alignment(s_tset_colon_layer, GTextAlignmentCenter);
  text_layer_set_text(s_tset_colon_layer, ":");
  layer_add_child(root, text_layer_get_layer(s_tset_colon_layer));

  s_tset_sec_layer = text_layer_create(
    GRect(tset_start_x + tset_digit_w + tset_colon_w, tset_row_y, tset_digit_w, tset_row_h));
  text_layer_set_font(s_tset_sec_layer, fonts_get_system_font(FONT_KEY_BITHAM_42_BOLD));
  text_layer_set_text_alignment(s_tset_sec_layer, GTextAlignmentCenter);
  layer_add_child(root, text_layer_get_layer(s_tset_sec_layer));

  s_tset_hint_layer = make_bottom_hint(root, bounds, "");
  // 実際のヒント文言は refresh_timer_set_screen() が編集中フィールドに
  // 応じて都度設定する

  // ── Alarm (タイマー満了) ──────────────────────────────────────────────────
  s_alarm_title_layer = make_title_bar(root, bounds,
    "\xe3\x82\xbf\xe3\x82\xa4\xe3\x83\x9e\xe3\x83\xbc\xe7\xb5\x82\xe4\xba\x86");
  // UTF-8: "タイマー終了"

  const int alarm_msg_h = 60;
  s_alarm_msg_layer = text_layer_create(
    GRect(0, content_top + (content_h - alarm_msg_h) / 2, bounds.size.w, alarm_msg_h));
  text_layer_set_font(s_alarm_msg_layer, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  text_layer_set_text_alignment(s_alarm_msg_layer, GTextAlignmentCenter);
  layer_add_child(root, text_layer_get_layer(s_alarm_msg_layer));

  s_alarm_hint_layer = make_bottom_hint(root, bounds,
    "\xe3\x81\xa9\xe3\x82\x8c\xe3\x81\x8b\xe3\x81\xae\xe3\x83\x9c\xe3\x82\xbf\xe3\x83\xb3"
    "\xe3\x81\xa7\xe5\x81\x9c\xe6\xad\xa2");
  // UTF-8: "どれかのボタンで停止"

  // ── Show initial screen ───────────────────────────────────────────────────
  show_screen(SCREEN_HOME);
}

static void window_unload(Window *window) {
  gbitmap_destroy(s_icon_weather);
  s_icon_weather = NULL;

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

  text_layer_destroy(s_slot_title_layer);
  text_layer_destroy(s_slot_time_layer);
  text_layer_destroy(s_slot_sub_layer);
  text_layer_destroy(s_slot_hint_layer);

  text_layer_destroy(s_tset_title_layer);
  text_layer_destroy(s_tset_min_layer);
  text_layer_destroy(s_tset_colon_layer);
  text_layer_destroy(s_tset_sec_layer);
  text_layer_destroy(s_tset_hint_layer);

  text_layer_destroy(s_alarm_title_layer);
  text_layer_destroy(s_alarm_msg_layer);
  text_layer_destroy(s_alarm_hint_layer);
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
  stop_alarm_vibration();
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
