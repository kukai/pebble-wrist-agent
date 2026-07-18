# WristAgent 仕様書 (SPECS)

> このファイルは現行仕様のスナップショット。仕様変更時に上書き更新する。
> 決定の経緯は `docs/adr/ADR.md` を参照。

最終更新: 2026-07-19

---

## 1. アプリ概要

| 項目           | 値                          |
|----------------|-----------------------------|
| アプリ名       | WristAgent                  |
| UUID           | 7388c494-40c6-41fc-931a-f01c7b6d47e8 |
| バージョン     | 1.0.0                       |
| SDK バージョン | 3                           |
| ターゲット機種 | Pebble Emery                |
| 言語           | C (watch) / ES5 JS (phone)  |

## 2. アーキテクチャ概要

```
[Pebble Watch]                    [Smartphone]
  wrist_agent.c                     index.js (PebbleKit JS)
  ┌──────────────┐                 ┌──────────────────────┐
  │ Dictation    │──KEY_QUERY───▶ │ handleQuery()        │
  │ Session      │                 │   ├── localStorage   │
  │              │◀──KEY_RESPONSE─│   └── OpenAI API     │
  │ Ring Buffer  │◀──KEY_STATUS───│         ↕             │
  │ (5 entries)  │──KEY_COMMAND──▶│   "reset"            │
  └──────────────┘                 └──────────────────────┘
         ▲                                   ▲
         │ Pebble app store / sideload       │ GitHub Pages
         │                            config/index.html
         │                            (API Key 設定 UI)
```

## 3. スクリーン仕様

### 3.1 HOME（MenuLayer ダッシュボード）

タイトルバー（"WristAgent"）と下部ステータス行（エラー・通知表示用）の間に
MenuLayer を配置した**単一セクション・固定 5 行**構成（`s_home_rows[]`）。
「話す」「天気」「タイマー」「ストップウォッチ」「会話履歴」はすべて同列の
固定行として並び、タイマー/SW もインスタンスの有無にかかわらず**常に1行の
まま存在する**（ADR-021。旧 S1/S2/S3 の 3 セクション構成、および S2 の
インスタンス数だけ行が増減する設計から変更）。タイマー・SW とも同時に保持
できるのは 1 件までに制限している（ADR-022）。

| 行 | タイトル | アイコン | サブタイトル |
|----|---------|---------|-------------|
| 1 | 話す | なし | "Selectで音声入力"（固定） |
| 2 | 天気 | 太陽アイコン（`resources/images/icon_weather.png`） | なし |
| 3 | タイマー | なし | 未設定なら "未設定"。設定済みなら残り時間 `MM:SS`（1h以上は`H:MM:SS`）+ ラベル（あれば） |
| 4 | ストップウォッチ | なし | タイマーと同様（経過時間、Lap があれば "Lap MM:SS" も付記） |
| 5 | 会話履歴 | なし | なし |

- HOME 表示のたびに「話す」（行0）を初期選択行に設定し、Select 即押しで
  音声入力へ（起動感を維持）。
- **UP/DOWN は循環スクロール**: 末尾の行で DOWN を押すと先頭へ、先頭の行で
  UP を押すと末尾へ戻る（`home_up_click`/`home_down_click` が MenuLayer の
  既定の「端で止まる」挙動を上書き）。
- タイマー/SW 行のサブタイトルはカウンタを持たず、TickTimerService
  （SECOND_UNIT、HOME 表示中のみ購読）で `layer_mark_dirty` → 描画時に
  タイムスタンプ差分から都度再計算。
- 「天気」Select — 音声を使わず「今日の天気を教えて」を `KEY_QUERY` として送信し、
  通常の音声質問と全く同じ経路（LOADING → ANSWER）で `get_weather` ツール（7.1 参照）
  の応答を表示する。新しい AppMessage キーは追加しない。
- 「タイマー」Select — タイマーが未設定ならタイマー設定画面（3.5）へ自動遷移し、
  作成後はそのタイマーの ActionMenu（3.4）をそのまま開く。設定済みならその
  ActionMenu（3.4）を開く。
- 「ストップウォッチ」Select — SW が未設定ならボタン操作のみで即座に開始
  （ラベルなし）し、開始後はその SW の ActionMenu（3.4）をそのまま開く。
  設定済みならその ActionMenu（3.4）を開く。
- 「会話履歴」Select で ANSWER 画面へ（履歴 0 件時はステータス行に「履歴なし」）。

ボタン操作:
- **UP / DOWN** — メニュー行移動（循環）
- **SELECT** — 選択行の実行（話す = 音声入力開始）
- **BACK** — アプリ終了（`home_click_config_provider` で明示的にサブスクライブ。ADR-016 参照）

### 3.2 LOADING

"考え中..." を表示。操作無効（ボタン設定なし）。

### 3.3 ANSWER（会話履歴）

Q&A ペアをスクロール表示。タイトルバーに "現在位置/総件数" を表示。
HOME の「会話履歴」項目からも遷移可能。

- **SELECT / BACK** — HOME へ戻る
- **SELECT 長押し (700 ms)** — 会話リセット（JS 側コンテキスト含む）→ HOME へ
- **UP / DOWN 短押し** — スクロール ±30 px
- **UP 長押し (500 ms)** — 履歴を 1 件前へ
- **DOWN 長押し (500 ms)** — 履歴を 1 件後へ

### 3.4 ActionMenu（「タイマー」「ストップウォッチ」行選択時）

Pebble 標準 ActionMenu API を使用。すべてローカル処理で完結し、LLM 往復を挟まない。

| 種別 | 状態 | アクション |
|------|------|-----------|
| タイマー | 実行中 | 一時停止 / リセット / 削除 |
| タイマー | 停止中 | 再開 / リセット / 削除 |
| ストップウォッチ | 実行中 | ストップ / Lap / リセット / 削除 |
| ストップウォッチ | 停止中 | スタート / リセット / 削除 |

- タイマーの一時停止 = Wakeup キャンセル + 残り秒保存。再開 = 残り秒で再スケジュール。
  リセット = 元の長さ（duration）で再スケジュール。
- SW の Lap は直近 Lap 秒を記録し「ストップウォッチ」行のサブタイトルに表示。
  リセットは 0 秒・停止状態へ。
- タイマー/SW を新規作成した直後は、作成したスロットの ActionMenu をそのまま開く
  （タイマー設定画面 3.5 の `did_close` 後に `s_open_slot_pending` 経由で遅延オープン）。
- タイマー・SW とも同時に保持できるのは 1 件までのため（ADR-022）、「新しいタイマー/
  ストップウォッチ」を追加する導線はない。ActionMenu の容量は最大アクション数
  （SW実行中の4件: ストップ/Lap/リセット/削除）に合わせて確保。

### 3.5 タイマー設定画面（プリセットピッカー）

ActionMenu によるプリセット時間選択（数値入力 UI は使わない）。
HOME「タイマー」行が未設定のときに遷移する。

- プリセット: 1分 / 3分 / 5分 / 10分 / 15分 / 20分 / 30分
- 選択したプリセットで `handle_timer_set()` をローカル実行（ラベルなし。音声経由の
  `set_timer` ツールと同じ内部関数を再利用）。既にタイマーが1件ある場合は
  `handle_timer_set()` 内部のガードにより作成できず、ステータス行に
  「タイマーは1つまでです」を表示する（音声経由でも同じ制限がかかる）。

## 4. AppMessage プロトコル

### 4.1 キー定義

```json
{
  "KEY_QUERY":           0,
  "KEY_RESPONSE":        1,
  "KEY_STATUS":          2,
  "KEY_COMMAND":         3,
  "KEY_TIMER_SET":       4,
  "KEY_TIMER_LABEL":     5,
  "KEY_STOPWATCH_START": 6
}
```

### 4.2 メッセージフロー

```
Watch → Phone: { KEY_QUERY: "<音声認識テキスト>" }
Phone → Watch: { KEY_RESPONSE: "<AI応答テキスト (≤500文字)>" }

Watch → Phone: { KEY_COMMAND: "reset" }
Phone → Watch: { KEY_STATUS: "reset_ok" }

Phone → Watch: { KEY_STATUS: "error:<詳細>" }
Phone → Watch: { KEY_STATUS: "key_saved" }

Phone → Watch: { KEY_TIMER_SET: <秒数:int>, KEY_TIMER_LABEL: "<ラベル|空文字>" }
Phone → Watch: { KEY_STOPWATCH_START: 1, KEY_TIMER_LABEL: "<ラベル|空文字>" }
```

タイマー/SW メッセージは LLM の function call 実行時にウォッチ応答（KEY_RESPONSE）
より先に送信される。ウォッチはこれを受けてスロット作成・Wakeup 予約を行い、
直後の KEY_RESPONSE 受信時は ANSWER でなく HOME に戻る（応答は履歴に保存）。

### 4.3 バッファサイズ

| 種別             | サイズ |
|------------------|--------|
| AppMessage inbox | min(max, 512) bytes |
| AppMessage outbox| min(max, 512) bytes |
| C クエリバッファ | 512 bytes |
| C レスポンスバッファ | 512 bytes |
| JS 応答トリム上限 | 500 文字 |

## 5. 会話履歴

### 5.1 ウォッチ側（C）

- リングバッファ、容量 `HIST_CAP = 5` 件
- Q: 最大 128 bytes、A: 最大 512 bytes
- 上限超過時は最古エントリを押し出し
- ANSWER 画面で `s_hist_view` インデックスで閲覧
- `reset` コマンドで `s_hist_len = 0` にリセット
- **PersistentStorage に永続化**（ユーザーがリセットするまでアプリ再起動をまたいで保持。
  ADR-018 参照）。1 キー最大 256 bytes の制約があるため、エントリごとに
  Q 用 1 キー（≤128B）+ A 用 2 キー（各 256B）に分割して保存し、件数はメタキー
  （`PERSIST_KEY_HIST_META`）に保存する。`push_history()` のたびに全件書き直す。

### 5.2 スマートフォン側（JS）

- `conversationHistory` 配列（system メッセージを先頭に常駐）
- 非 system メッセージの最大保持数: `MAX_HISTORY = 10`
- 上限超過時は最古の非 system メッセージを削除
- `reset` コマンドで system メッセージのみ残してリセット

## 6. タイマー・ストップウォッチ（スロット管理）

### 6.1 スロット

- 固定長スロット `s_slots[SLOT_COUNT]`、**SLOT_COUNT = 2**（タイマー用・SW用に
  1つずつ）。タイマー・SW とも同時に保持できるのは **1 件まで**（ADR-022）。
  この制限は `handle_timer_set()`/`handle_stopwatch_start()` 自身が入口で
  チェックするため、ボタン操作・音声（LLM の関数呼び出し）のどちらの経路でも
  一貫して適用される。既に1件ある状態で追加しようとした場合はステータス行に
  「タイマーは1つまでです」/「ストップウォッチは1つまでです」を表示して拒否する
  （LLM へのフィードバックはなし。音声で「もう一つタイマーをセットして」と
  言われても、実際には作成されない）。
- 各スロット: `kind`（TIMER/STOPWATCH）、`running`、`duration`（タイマー元秒数）、
  `target_ts`（実行中タイマーの満了時刻）、`remaining`（停止中タイマーの残り秒）、
  `start_ts`（実行中 SW の起点）、`elapsed`（停止中 SW の累積秒）、`wakeup_id`、
  `last_lap`、`label`（最大 23 bytes、UTF-8 境界で安全に切り詰め）。
- PersistentStorage キー `100 + スロット番号` に構造体ごと保存。変更のたびに書き込み、
  削除時は `persist_delete`。起動時に全スロットをロード。

### 6.2 Wakeup（タイマー満了通知）

- `wakeup_schedule(満了時刻, cookie=スロット番号, notify_if_missed=true)`。
- 最短 30 秒（`TIMER_MIN_SECONDS`。それ未満の指定は 30 秒に切り上げ）。
- 予約失敗（排他ウィンドウ衝突等で負値）時は **5 秒ずらして最大 8 回リトライ**。
  全滅時はスロットを作らず「予約失敗」を表示。
- 満了時: バイブ（double pulse）+ 該当スロット削除 + ステータス行に「タイマー終了 <ラベル>」。
  - アプリがフォアグラウンド: `wakeup_service_subscribe` ハンドラで処理。
  - アプリ非起動: システムがアプリを Wakeup 起動し、`launch_reason() == APP_LAUNCH_WAKEUP`
    経路で同処理。
- 起動時サニタイズ: 実行中タイマーの `wakeup_query` が無効な場合、満了済みなら削除、
  未満了なら残り時間で再スケジュール（失敗時は削除）。
- 一時停止中のタイマーは Wakeup を持たない（再開時に再スケジュール）。
- ストップウォッチは自動終了せず、ユーザーが明示的に削除するまで保持。

## 7. OpenAI API 連携

| 項目           | 値                                              |
|----------------|-------------------------------------------------|
| エンドポイント | `https://api.openai.com/v1/chat/completions`    |
| モデル         | `gpt-4o-mini`                                   |
| max_tokens     | 200                                             |
| タイムアウト   | 15,000 ms                                       |
| system 指示    | "You are a helpful assistant on a smartwatch. Answer concisely." + 今日の日付 |
| リトライ       | NACK 時 500 ms 後に 1 回再送（sendWithRetry）   |
| ツール         | `get_weather` / `set_timer` / `start_stopwatch`（7.1〜7.2 参照）。実行は最大 2 ラウンド |

### 7.1 get_weather ツール

OpenAI function calling で宣言。天気・気温・降水・風に関する質問で LLM が呼び出す。

| 項目       | 内容                                                          |
|------------|---------------------------------------------------------------|
| 引数       | `location`（省略時は現在地）, `date`（YYYY-MM-DD、省略時は当日）|
| 現在地     | `navigator.geolocation`（appinfo `capabilities: ["location"]`）|
| ジオコーディング | Open-Meteo Geocoding API（`language=ja`, `count=1`）     |
| 予報       | Open-Meteo Forecast API（daily: 天気コード・最高/最低気温・最大降水確率・最大風速、`timezone=auto`）|
| 期間       | 16 日先まで（Open-Meteo の予報範囲）                           |
| API キー   | 不要                                                          |
| タイムアウト | 各 HTTP 10 s                                                 |
| エラー     | 失敗内容を tool 結果として LLM に渡し文章化させる。ツールラウンド超過時のみ `error:tool_loop` |

### 7.2 set_timer / start_stopwatch ツール

音声からタイマー・ストップウォッチを操作するための function calling 定義。

| ツール | 引数 | JS 側の処理 |
|--------|------|-------------|
| `set_timer` | `duration_seconds`（必須, integer）, `label`（任意） | `{ KEY_TIMER_SET, KEY_TIMER_LABEL }` をウォッチへ送信 |
| `start_stopwatch` | `label`（任意） | `{ KEY_STOPWATCH_START: 1, KEY_TIMER_LABEL }` をウォッチへ送信 |

- tool 結果は**ウォッチの ack を待たず楽観的に即時返却**する
  （`{"status":"scheduled",...}` / `{"status":"started"}`）。Wakeup 予約失敗や
  スロット満杯はウォッチ側のステータス行表示で通知され、LLM には伝わらない。
- `duration_seconds` が不正（0 以下・非数値）の場合は `{"error":"invalid_duration"}` を返す。
- ウォッチ側の実処理（スロット作成・Wakeup 予約・HOME 復帰）は 4.2 / 6 章を参照。

tool 往復メッセージ（assistant の `tool_calls` / role=`tool`）は会話履歴に
永続化せず、履歴には user と最終 assistant 応答のみ保存する
（MAX_HISTORY トリムでペアが分断されると OpenAI API がエラーになるため）。

## 8. API キー管理

1. 設定 UI (`config/index.html`) でユーザーが入力
2. `doSave()` が `pebblejs://close#<URLencoded JSON>` にリダイレクト
3. `webviewclosed` イベントで JS が受け取り `localStorage.setItem('openai_api_key', key)` に保存
4. 書き込み検証: `getItem` で読み返して一致チェック
5. 成功時 `key_saved`、失敗時 `error:ls_write_fail` を送信
6. `HARDCODED_API_KEY` 変数（開発用、空文字がデフォルト）が localStorage より優先

注意: PebbleKit JS の localStorage はアプリ UUID ごとに分離されるため、
`appinfo.json` の UUID を変更すると保存済みキーは参照できなくなる。
UUID 変更後は設定 UI からキーを再入力すること（ADR-012 参照）。

## 9. 設定 Web UI

- ホスト: `https://kukai.github.io/pebble-wrist-agent/config/`
- URL パラメータ:
  - `?v=N` — キャッシュバスト用バージョン番号（現在 v=7）
  - `&return_to=pebblejs://close` — 保存後のリダイレクト先
  - `&current=<masked>` — 設定済みキーのマスク表示用

## 10. ビルドターゲット

| 項目           | 値        |
|----------------|-----------|
| プラットフォーム | emery    |
| watchface      | false     |
| enableMultiJS  | true      |
| capabilities   | configurable |
| ビルドツール   | WAF (wscript) |

## 11. リソース

| リソース ID | ファイル | 用途 |
|-------------|---------|------|
| `RESOURCE_ID_ICON_WEATHER` | `resources/images/icon_weather.png`（25x25、透過PNG） | HOME「天気」行のアイコン |

`appinfo.json` の `resources.media` に登録し、Pebble のビルドパイプラインが
`RESOURCE_ID_<name>` マクロを自動生成する。`gbitmap_create_with_resource()`
で読み込み、`window_unload` で `gbitmap_destroy()` する。
