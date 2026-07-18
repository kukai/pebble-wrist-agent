# WristAgent — CLAUDE.md

Pebble スマートウォッチ向け音声 AI アシスタントアプリ。
ウォッチ側 C ファーム + スマートフォン側 PebbleKit JS + 設定 Web UI の 3 層構成。

## リポジトリ構造

```
pebble-wrist-agent/
├── src/
│   ├── c/wrist_agent.c   # ウォッチ側ファームウェア (Pebble SDK 3)
│   └── pkjs/index.js     # コンパニオン JS (PebbleKit JS / ES5)
├── config/index.html     # 設定 Web UI (GitHub Pages でホスト)
├── appinfo.json          # アプリメタデータ・AppMessage キー定義
├── wscript               # WAF ビルド設定
├── SPECS.md              # 最新仕様書
└── docs/adr/ADR.md       # アーキテクチャ決定記録 (追記専用)
```

## ビルド・開発

```bash
# Pebble SDK がインストールされていること前提
pebble build
pebble install --emulator emery   # エミュレータで実行
pebble logs                       # ログ確認
```

設定 Web UI は `config/index.html` を GitHub Pages
(`https://kukai.github.io/pebble-wrist-agent/config/`) にデプロイして使用。

## AppMessage プロトコル

| キー定数            | 値 | 方向         | 用途                   |
|---------------------|----|--------------|------------------------|
| KEY_QUERY           | 0  | Watch → Phone | 音声認識テキスト送信   |
| KEY_RESPONSE        | 1  | Phone → Watch | AI 応答テキスト返信    |
| KEY_STATUS          | 2  | Phone → Watch | エラー・ステータス通知 |
| KEY_COMMAND         | 3  | Watch → Phone | "reset" コマンド送信   |
| KEY_TIMER_SET       | 4  | Phone → Watch | タイマー秒数（set_timer ツール） |
| KEY_TIMER_LABEL     | 5  | Phone → Watch | タイマー/SW のラベル（空文字可） |
| KEY_STOPWATCH_START | 6  | Phone → Watch | ストップウォッチ開始（start_stopwatch ツール） |

ステータス値の規則:
- `error:<メッセージ>` — エラー発生（ホーム画面に表示）
- `reset_ok` — 会話履歴リセット完了
- `key_saved` — API キー保存完了

## スクリーン遷移

```
HOME (MenuLayer 単一セクション・固定5行: 話す/天気/タイマー/ストップウォッチ/会話履歴)
  ※ タイマー/ストップウォッチ行はインスタンスの有無にかかわらず常に1行のまま。
    状態はサブタイトルで表現（未設定/残り・経過時間）。タイマー・SWとも同時に
    保持できるのは1件まで（ADR-021, ADR-022）
  ※ UP/DOWNは循環スクロール（末尾でDOWN→先頭、先頭でUP→末尾。ADR-022）
HOME「話す」(SELECT) ──▶ [音声録音] ──(認識成功)──▶ LOADING ──▶ ANSWER
  ※ タイマー/SW を音声セットした場合は ANSWER でなく HOME に戻る
HOME「天気」(SELECT、太陽アイコン付き) ──▶ 「今日の天気を教えて」を送信 ──▶ LOADING ──▶ ANSWER
  （「話す」と同じ経路。get_weather ツールが応答。ADR-019, ADR-022）
HOME「タイマー」(SELECT) ──▶ 未設定ならプリセット時間ピッカー(ActionMenu)、
  設定済みならその ActionMenu。新規作成時は作成したスロットの ActionMenu を
  そのまま自動的に開く（ADR-020）
HOME「ストップウォッチ」(SELECT) ──▶ 未設定なら即座に開始、
  設定済みならその ActionMenu。新規作成時は同様に ActionMenu を自動的に開く（ADR-020）
タイマー/SW の ActionMenu (SELECT) ──▶ 一時停止/再開/リセット/削除、
  SWはStart/Stop/Lap/Reset。全てローカル処理
HOME「会話履歴」(SELECT) ──▶ ANSWER（履歴0件なら「履歴なし」表示）
ANSWER ──(SELECT / BACK)──▶ HOME
ANSWER: SELECT長押し(700ms) → 会話リセット → HOME
ANSWER: UP/DOWN短押し → スクロール±30px
ANSWER: UP/DOWN長押し(500ms) → 履歴前後移動
HOME 表示中のみ TickTimerService(SECOND_UNIT) 購読、タイマー/SW 行は描画時にタイムスタンプ差分を再計算
```

## 重要な制約・注意事項

- **ES5 必須**: コンパニオン JS は PebbleKit JS ランタイムが ES5 のため、アロー関数・`const`/`let`・`Array.prototype.findIndex` 等は使用不可。
- **バッファサイズ**: クエリ 512 B、レスポンス 512 B（AppMessage 上限に合わせた最大値で open）。応答は送信前に 500 文字にトリム。
- **ウォッチ側履歴**: リングバッファ 5 件（`HIST_CAP`）。ユーザーがリセットするまで PersistentStorage に永続化（1 キー最大 256 bytes 制約のため a は 2 キーに分割。ADR-018 参照）。JS 側の会話コンテキストは最大 10 ターン（非 system メッセージ、こちらは永続化しない）。
- **タイマー/SW スロット**: `SLOT_COUNT=2`（タイマー用・SW用に1つずつ）。タイマー・SWとも同時に保持できるのは1件までで、`handle_timer_set()`/`handle_stopwatch_start()` 自身がボタン操作・音声どちらの経路でもこの制限をガードする（ADR-022）。PersistentStorage キー 100+n に永続化。タイマーは Wakeup API（最短 30 秒、予約衝突時は 5 秒ずらし最大 8 回リトライ、cookie=スロット番号）。
- **HOME の BACK ボタン**: `menu_layer_set_click_config_onto_window` のデフォルト任せにせず、`home_click_config_provider` で明示的に BACK をサブスクライブする（ADR-016 参照）。BACK 長押しは Pebble OS が常にアプリを強制終了する仕様でありアプリ側で変更不可。
- **マイク依存**: `PBL_MICROPHONE` 定義がない場合、Dictation Session は作成されない（エミュレータ注意）。
- **日本語リテラル**: C ファイル内の日本語は UTF-8 エスケープ `\xNN` で記述（ツールチェーン互換性のため）。

## API キー管理

1. 設定 UI から入力 → `pebblejs://close#<JSON>` で JS に返却
2. JS 側で `localStorage.setItem('openai_api_key', key)` に保存
3. `HARDCODED_API_KEY` 変数（開発テスト用、デフォルト空文字）が優先
4. キー未設定時は `error:no_api_key` をウォッチに送信

**注意**: PebbleKit JS の localStorage はアプリ UUID ごとに分離される。
`appinfo.json` の UUID を変更すると保存済みキーは参照できなくなり、
設定 UI からの再入力が必要（ADR-012 参照）。

## OpenAI 連携

- モデル: `gpt-4o-mini`
- `max_tokens`: 200
- タイムアウト: 15,000 ms
- リトライ: NACK 時に 500 ms 後 1 回のみ再送
- system プロンプト: `"You are a helpful assistant on a smartwatch. Answer concisely."`（毎リクエスト、相対日付解決用に今日の日付を付加）
- ツール (function calling): `get_weather(location?, date?)` — Open-Meteo（ジオコーディング + 予報、API キー不要）
  - 場所未指定時は `navigator.geolocation` の現在地を使用（appinfo の `capabilities` に `location` が必須）
  - ツール実行 → 再問い合わせは最大 2 ラウンド（超過で `error:tool_loop`）
  - tool 往復メッセージは会話履歴に永続化しない（トリムで assistant/tool ペアが壊れるのを防ぐ。ADR-013 参照）
- ツール: `set_timer(duration_seconds, label?)` / `start_stopwatch(label?)` — ウォッチへ AppMessage 送信、ack を待たず tool 結果を楽観的に即時返却（ADR-015 参照）

## タイマー/SW のローカル追加（ボタン操作のみ、LLM 非経由）

- HOME「タイマー」/「ストップウォッチ」から、音声を使わずボタン操作のみでも追加できる（ADR-017 参照）。
- タイマーの時間指定は ActionMenu によるプリセット選択（1/3/5/10/15/20/30分）。数値入力 UI は使わない。
- 新規作成直後は作成したスロットの ActionMenu を自動的に開く
  （`s_open_slot_pending` で `did_close` 後に遅延オープン。ADR-020 参照）。
- HOME の「タイマー」/「ストップウォッチ」行はインスタンス数にかかわらず常に1行のまま
  （増減しない。ADR-021 参照。旧 S2 の「インスタンスごとに行が増える」設計から変更）。
- タイマー・SW とも同時に保持できるのは 1 件までで、既に1件ある状態でボタン操作・音声の
  どちらから追加しようとしても拒否される（「新しいタイマー/SW」の ActionMenu 項目は
  廃止。ADR-022 参照）。

## 天気のローカル送信（ボタン操作のみ、音声非経由）

- HOME「天気」から、音声を使わず「今日の天気を教えて」を送信できる（ADR-019 参照）。
- 新しい AppMessage キーは追加せず、`KEY_QUERY` の定型文送信として既存の `send_query()`
  経路をそのまま再利用する（get_weather ツールは ADR-013 で実装済み）。

## GitHub 運用ルール

- PR の本文（description）は **日本語** で記述する。タイトルは英語。
- **main にマージする前に必ず `staging` ブランチ経由で CloudPebble ビルドを確認する**。
  手順は `.claude/skills/pre-merge-staging/SKILL.md` を参照（skill 名: `pre-merge-staging`）。

## ドキュメント運用

- **SPECS.md** — 現行仕様のスナップショット。仕様変更時に上書き更新。
- **docs/adr/ADR.md** — 追記専用。決定を覆した場合も旧エントリは削除せず新エントリを追加。
