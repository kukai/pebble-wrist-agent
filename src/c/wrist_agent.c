#include <pebble.h>

// ---------------------------------------------------------------------------
// AppMessage key constants (must match appinfo.json messageKeys)
// ---------------------------------------------------------------------------
#define KEY_QUERY    0
#define KEY_RESPONSE 1
#define KEY_STATUS   2
#define KEY_COMMAND  3

// ---------------------------------------------------------------------------
// Buffer sizes
// ---------------------------------------------------------------------------
#define QUERY_BUF_SIZE    512
#define RESPONSE_BUF_SIZE 512

// ---------------------------------------------------------------------------
// Screen states
// ---------------------------------------------------------------------------
typedef enum {
  SCREEN_HOME,
  SCREEN_CONFIRM,
  SCREEN_LOADING,
  SCREEN_ANSWER
} Screen;

// ---------------------------------------------------------------------------
// UI elements
// ---------------------------------------------------------------------------
static Window      *s_window;

// Home
static TextLayer   *s_home_title_layer;
static TextLayer   *s_home_hint_layer;
static TextLayer   *s_home_history_layer;
static TextLayer   *s_home_status_layer;

// Confirm
static TextLayer   *s_confirm_title_layer;
static ScrollLayer *s_confirm_scroll_layer;
static TextLayer   *s_confirm_text_layer;
static TextLayer   *s_confirm_hint_layer;

// Loading
static TextLayer   *s_loading_title_layer;
static TextLayer   *s_loading_msg_layer;

// Answer
static TextLayer   *s_answer_title_layer;
static ScrollLayer *s_answer_scroll_layer;
static TextLayer   *s_answer_text_layer;
static TextLayer   *s_answer_hint_layer;

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static DictationSession *s_dictation_session;
static Screen            s_current_screen;
static int               s_history_count;

static char s_query_buf[QUERY_BUF_SIZE];
static char s_response_buf[RESPONSE_BUF_SIZE];
static char s_history_text[32];

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
static void show_screen(Screen screen);
static void send_query(void);
static void send_reset_command(void);

// ---------------------------------------------------------------------------
// Layer visibility
// ---------------------------------------------------------------------------
static void hide_all_screens(void) {
  // Home
  layer_set_hidden(text_layer_get_layer(s_home_title_layer), true);
  layer_set_hidden(text_layer_get_layer(s_home_hint_layer), true);
  layer_set_hidden(text_layer_get_layer(s_home_history_layer), true);
  layer_set_hidden(text_layer_get_layer(s_home_status_layer), true);
  // Confirm
  layer_set_hidden(text_layer_get_layer(s_confirm_title_layer), true);
  layer_set_hidden(scroll_layer_get_layer(s_confirm_scroll_layer), true);
  layer_set_hidden(text_layer_get_layer(s_confirm_hint_layer), true);
  // Loading
  layer_set_hidden(text_layer_get_layer(s_loading_title_layer), true);
  layer_set_hidden(text_layer_get_layer(s_loading_msg_layer), true);
  // Answer
  layer_set_hidden(text_layer_get_layer(s_answer_title_layer), true);
  layer_set_hidden(scroll_layer_get_layer(s_answer_scroll_layer), true);
  layer_set_hidden(text_layer_get_layer(s_answer_hint_layer), true);
}

// ---------------------------------------------------------------------------
// Click handlers
// ---------------------------------------------------------------------------
static void home_select_click(ClickRecognizerRef r, void *ctx) {
  dictation_session_start(s_dictation_session);
}

static void home_up_long_click(ClickRecognizerRef r, void *ctx) {
  send_reset_command();
}

static void home_back_click(ClickRecognizerRef r, void *ctx) {
  // click_config_provider を設定すると Back のデフォルト終了動作が失われるため明示的に終了
  window_stack_remove(s_window, true);
}

static void home_click_config(void *ctx) {
  window_single_click_subscribe(BUTTON_ID_SELECT, home_select_click);
  window_long_click_subscribe(BUTTON_ID_UP, 700, home_up_long_click, NULL);
  window_single_click_subscribe(BUTTON_ID_BACK, home_back_click);
}

static void confirm_select_click(ClickRecognizerRef r, void *ctx) {
  send_query();
}

static void confirm_back_click(ClickRecognizerRef r, void *ctx) {
  show_screen(SCREEN_HOME);
}

static void confirm_click_config(void *ctx) {
  window_single_click_subscribe(BUTTON_ID_SELECT, confirm_select_click);
  window_single_click_subscribe(BUTTON_ID_BACK, confirm_back_click);
}

static void answer_select_click(ClickRecognizerRef r, void *ctx) {
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

static void answer_click_config(void *ctx) {
  window_single_click_subscribe(BUTTON_ID_SELECT, answer_select_click);
  window_single_click_subscribe(BUTTON_ID_BACK, answer_back_click);
  window_single_click_subscribe(BUTTON_ID_UP, answer_up_click);
  window_single_click_subscribe(BUTTON_ID_DOWN, answer_down_click);
}

static void loading_click_config(void *ctx) {}

// ---------------------------------------------------------------------------
// show_screen
// ---------------------------------------------------------------------------
static void show_screen(Screen screen) {
  s_current_screen = screen;
  hide_all_screens();

  switch (screen) {
    case SCREEN_HOME:
      snprintf(s_history_text, sizeof(s_history_text), "[履歴: %d件]", s_history_count);
      text_layer_set_text(s_home_history_layer, s_history_text);
      layer_set_hidden(text_layer_get_layer(s_home_title_layer), false);
      layer_set_hidden(text_layer_get_layer(s_home_hint_layer), false);
      layer_set_hidden(text_layer_get_layer(s_home_history_layer), false);
      layer_set_hidden(text_layer_get_layer(s_home_status_layer), false);
      window_set_click_config_provider(s_window, home_click_config);
      break;

    case SCREEN_CONFIRM: {
      text_layer_set_text(s_confirm_text_layer, s_query_buf);
      GRect scroll_bounds = layer_get_bounds(scroll_layer_get_layer(s_confirm_scroll_layer));
      GSize text_size = text_layer_get_content_size(s_confirm_text_layer);
      text_size.h += 8;
      if (text_size.h < scroll_bounds.size.h) text_size.h = scroll_bounds.size.h;
      text_layer_set_size(s_confirm_text_layer, GSize(scroll_bounds.size.w, text_size.h));
      scroll_layer_set_content_size(s_confirm_scroll_layer, GSize(scroll_bounds.size.w, text_size.h));
      scroll_layer_set_content_offset(s_confirm_scroll_layer, GPointZero, false);
      layer_set_hidden(text_layer_get_layer(s_confirm_title_layer), false);
      layer_set_hidden(scroll_layer_get_layer(s_confirm_scroll_layer), false);
      layer_set_hidden(text_layer_get_layer(s_confirm_hint_layer), false);
      window_set_click_config_provider(s_window, confirm_click_config);
      break;
    }

    case SCREEN_LOADING:
      layer_set_hidden(text_layer_get_layer(s_loading_title_layer), false);
      layer_set_hidden(text_layer_get_layer(s_loading_msg_layer), false);
      window_set_click_config_provider(s_window, loading_click_config);
      break;

    case SCREEN_ANSWER: {
      text_layer_set_text(s_answer_text_layer, s_response_buf);
      GRect scroll_bounds = layer_get_bounds(scroll_layer_get_layer(s_answer_scroll_layer));
      GSize text_size = text_layer_get_content_size(s_answer_text_layer);
      text_size.h += 8;
      if (text_size.h < scroll_bounds.size.h) text_size.h = scroll_bounds.size.h;
      text_layer_set_size(s_answer_text_layer, GSize(scroll_bounds.size.w, text_size.h));
      scroll_layer_set_content_size(s_answer_scroll_layer, GSize(scroll_bounds.size.w, text_size.h));
      scroll_layer_set_content_offset(s_answer_scroll_layer, GPointZero, false);
      layer_set_hidden(text_layer_get_layer(s_answer_title_layer), false);
      layer_set_hidden(scroll_layer_get_layer(s_answer_scroll_layer), false);
      layer_set_hidden(text_layer_get_layer(s_answer_hint_layer), false);
      window_set_click_config_provider(s_window, answer_click_config);
      break;
    }
  }
}

// ---------------------------------------------------------------------------
// AppMessage send helpers
// ---------------------------------------------------------------------------
static void send_query(void) {
  DictionaryIterator *out;
  AppMessageResult result = app_message_outbox_begin(&out);
  if (result != APP_MSG_OK) {
    snprintf(s_response_buf, sizeof(s_response_buf), "送信エラー: %d", (int)result);
    show_screen(SCREEN_ANSWER);
    return;
  }
  dict_write_cstring(out, KEY_QUERY, s_query_buf);
  app_message_outbox_send();
  show_screen(SCREEN_LOADING);
}

static void send_reset_command(void) {
  DictionaryIterator *out;
  if (app_message_outbox_begin(&out) != APP_MSG_OK) return;
  dict_write_cstring(out, KEY_COMMAND, "reset");
  app_message_outbox_send();
  s_history_count = 0;
  text_layer_set_text(s_home_status_layer, "リセット中...");
}

// ---------------------------------------------------------------------------
// AppMessage receive
// ---------------------------------------------------------------------------
static void inbox_received_handler(DictionaryIterator *iter, void *ctx) {
  Tuple *resp_tuple   = dict_find(iter, KEY_RESPONSE);
  Tuple *status_tuple = dict_find(iter, KEY_STATUS);

  if (resp_tuple) {
    strncpy(s_response_buf, resp_tuple->value->cstring, RESPONSE_BUF_SIZE - 1);
    s_response_buf[RESPONSE_BUF_SIZE - 1] = '\0';
    s_history_count += 2;
    show_screen(SCREEN_ANSWER);
  }

  if (status_tuple) {
    const char *status = status_tuple->value->cstring;
    if (strncmp(status, "error:", 6) == 0) {
      strncpy(s_response_buf, status + 6, RESPONSE_BUF_SIZE - 1);
      s_response_buf[RESPONSE_BUF_SIZE - 1] = '\0';
      show_screen(SCREEN_ANSWER);
    } else if (strcmp(status, "reset_ok") == 0) {
      s_history_count = 0;
      text_layer_set_text(s_home_status_layer, "Up長押し:リセット");
      show_screen(SCREEN_HOME);
    }
  }
}

static void outbox_failed_handler(DictionaryIterator *iter, AppMessageResult reason, void *ctx) {
  snprintf(s_response_buf, sizeof(s_response_buf), "送信失敗 (%d)", (int)reason);
  show_screen(SCREEN_ANSWER);
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
    show_screen(SCREEN_CONFIRM);
  } else {
    text_layer_set_text(s_home_status_layer, "認識失敗。再試行してください");
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

  // ── Home ──────────────────────────────────────────────────────────────────
  s_home_title_layer = make_title_bar(root, bounds, "WristAgent");

  s_home_hint_layer = text_layer_create(GRect(0, 40, bounds.size.w, 60));
  text_layer_set_font(s_home_hint_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18));
  text_layer_set_text_alignment(s_home_hint_layer, GTextAlignmentCenter);
  text_layer_set_text(s_home_hint_layer, "Selectで質問");
  layer_add_child(root, text_layer_get_layer(s_home_hint_layer));

  s_home_history_layer = text_layer_create(GRect(0, 110, bounds.size.w, 30));
  text_layer_set_font(s_home_history_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14));
  text_layer_set_text_alignment(s_home_history_layer, GTextAlignmentCenter);
  text_layer_set_text(s_home_history_layer, "[履歴: 0件]");
  layer_add_child(root, text_layer_get_layer(s_home_history_layer));

  s_home_status_layer = make_bottom_hint(root, bounds, "Up長押し:リセット");

  // ── Confirm ───────────────────────────────────────────────────────────────
  s_confirm_title_layer = make_title_bar(root, bounds, "確認");

  GRect confirm_scroll_frame = GRect(0, content_top, bounds.size.w, content_h);
  s_confirm_scroll_layer = scroll_layer_create(confirm_scroll_frame);
  scroll_layer_set_shadow_hidden(s_confirm_scroll_layer, true);

  s_confirm_text_layer = text_layer_create(
    GRect(4, 4, confirm_scroll_frame.size.w - 8, confirm_scroll_frame.size.h));
  text_layer_set_font(s_confirm_text_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18));
  text_layer_set_overflow_mode(s_confirm_text_layer, GTextOverflowModeWordWrap);
  scroll_layer_add_child(s_confirm_scroll_layer, text_layer_get_layer(s_confirm_text_layer));
  layer_add_child(root, scroll_layer_get_layer(s_confirm_scroll_layer));

  s_confirm_hint_layer = make_bottom_hint(root, bounds, "\xe2\x9c\x93\xe9\x80\x81\xe4\xbf\xa1"
                                                         "  \xe2\x9c\x97\xe3\x82\x84\xe3\x82\x8a"
                                                         "\xe7\x9b\xb4\xe3\x81\x97");
  // UTF-8 for "✓送信  ✗やり直し"

  // ── Loading ───────────────────────────────────────────────────────────────
  s_loading_title_layer = make_title_bar(root, bounds, "WristAgent");

  s_loading_msg_layer = text_layer_create(GRect(0, 80, bounds.size.w, 60));
  text_layer_set_font(s_loading_msg_layer, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  text_layer_set_text_alignment(s_loading_msg_layer, GTextAlignmentCenter);
  text_layer_set_text(s_loading_msg_layer, "\xe8\x80\x83\xe3\x81\x88\xe4\xb8\xad...");
  // UTF-8 for "考え中..."
  layer_add_child(root, text_layer_get_layer(s_loading_msg_layer));

  // ── Answer ────────────────────────────────────────────────────────────────
  s_answer_title_layer = make_title_bar(root, bounds, "AI\xe8\xbf\x94\xe7\xad\x94");
  // UTF-8 for "AI返答"

  GRect answer_scroll_frame = GRect(0, content_top, bounds.size.w, content_h);
  s_answer_scroll_layer = scroll_layer_create(answer_scroll_frame);
  scroll_layer_set_shadow_hidden(s_answer_scroll_layer, true);

  s_answer_text_layer = text_layer_create(
    GRect(4, 4, answer_scroll_frame.size.w - 8, answer_scroll_frame.size.h));
  text_layer_set_font(s_answer_text_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18));
  text_layer_set_overflow_mode(s_answer_text_layer, GTextOverflowModeWordWrap);
  scroll_layer_add_child(s_answer_scroll_layer, text_layer_get_layer(s_answer_text_layer));
  layer_add_child(root, scroll_layer_get_layer(s_answer_scroll_layer));

  s_answer_hint_layer = make_bottom_hint(root, bounds,
    "\xe2\x86\xa9 \xe6\xac\xa1\xe3\x81\xae\xe8\xb3\xaa\xe5\x95\x8f\xe3\x81\xb8");
  // UTF-8 for "↩ 次の質問へ"

  // ── Show initial screen ───────────────────────────────────────────────────
  show_screen(SCREEN_HOME);
}

static void window_unload(Window *window) {
  text_layer_destroy(s_home_title_layer);
  text_layer_destroy(s_home_hint_layer);
  text_layer_destroy(s_home_history_layer);
  text_layer_destroy(s_home_status_layer);

  text_layer_destroy(s_confirm_title_layer);
  text_layer_destroy(s_confirm_text_layer);
  scroll_layer_destroy(s_confirm_scroll_layer);
  text_layer_destroy(s_confirm_hint_layer);

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
}

static void deinit(void) {
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
