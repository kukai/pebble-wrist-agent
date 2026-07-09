# WristAgent 仕様書 (SPECS)

> このファイルは現行仕様のスナップショット。仕様変更時に上書き更新する。
> 決定の経緯は `docs/adr/ADR.md` を参照。

最終更新: 2026-07-09

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

### 3.1 HOME

| 要素           | 内容                              |
|----------------|-----------------------------------|
| タイトルバー   | "WristAgent"（黒背景・白文字）    |
| ヒントテキスト | "Selectで質問"                    |
| 履歴カウンタ   | "[履歴: N件]"                     |
| ステータス行   | エラー・操作ガイド（可変）         |

ボタン操作:
- **SELECT** — 音声入力開始
- **UP 長押し (700 ms)** — 会話リセット（JS 側コンテキストも含む）
- **BACK** — アプリ終了

### 3.2 LOADING

"考え中..." を表示。操作無効（ボタン設定なし）。

### 3.3 ANSWER

Q&A ペアをスクロール表示。タイトルバーに "現在位置/総件数" を表示。

- **SELECT / BACK** — HOME へ戻る
- **UP / DOWN 短押し** — スクロール ±30 px
- **UP 長押し (500 ms)** — 履歴を 1 件前へ
- **DOWN 長押し (500 ms)** — 履歴を 1 件後へ

## 4. AppMessage プロトコル

### 4.1 キー定義

```json
{
  "KEY_QUERY":    0,
  "KEY_RESPONSE": 1,
  "KEY_STATUS":   2,
  "KEY_COMMAND":  3
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
```

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

### 5.2 スマートフォン側（JS）

- `conversationHistory` 配列（system メッセージを先頭に常駐）
- 非 system メッセージの最大保持数: `MAX_HISTORY = 10`
- 上限超過時は最古の非 system メッセージを削除
- `reset` コマンドで system メッセージのみ残してリセット

## 6. OpenAI API 連携

| 項目           | 値                                              |
|----------------|-------------------------------------------------|
| エンドポイント | `https://api.openai.com/v1/chat/completions`    |
| モデル         | `gpt-4o-mini`                                   |
| max_tokens     | 200                                             |
| タイムアウト   | 15,000 ms                                       |
| system 指示    | "You are a helpful assistant on a smartwatch. Answer concisely." + 今日の日付 |
| リトライ       | NACK 時 500 ms 後に 1 回再送（sendWithRetry）   |
| ツール         | `get_weather`（6.1 参照）。実行は最大 2 ラウンド |

### 6.1 get_weather ツール

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

tool 往復メッセージ（assistant の `tool_calls` / role=`tool`）は会話履歴に
永続化せず、履歴には user と最終 assistant 応答のみ保存する
（MAX_HISTORY トリムでペアが分断されると OpenAI API がエラーになるため）。

## 7. API キー管理

1. 設定 UI (`config/index.html`) でユーザーが入力
2. `doSave()` が `pebblejs://close#<URLencoded JSON>` にリダイレクト
3. `webviewclosed` イベントで JS が受け取り `localStorage.setItem('openai_api_key', key)` に保存
4. 書き込み検証: `getItem` で読み返して一致チェック
5. 成功時 `key_saved`、失敗時 `error:ls_write_fail` を送信
6. `HARDCODED_API_KEY` 変数（開発用、空文字がデフォルト）が localStorage より優先

注意: PebbleKit JS の localStorage はアプリ UUID ごとに分離されるため、
`appinfo.json` の UUID を変更すると保存済みキーは参照できなくなる。
UUID 変更後は設定 UI からキーを再入力すること（ADR-012 参照）。

## 8. 設定 Web UI

- ホスト: `https://kukai.github.io/pebble-wrist-agent/config/`
- URL パラメータ:
  - `?v=N` — キャッシュバスト用バージョン番号（現在 v=7）
  - `&return_to=pebblejs://close` — 保存後のリダイレクト先
  - `&current=<masked>` — 設定済みキーのマスク表示用

## 9. ビルドターゲット

| 項目           | 値        |
|----------------|-----------|
| プラットフォーム | emery    |
| watchface      | false     |
| enableMultiJS  | true      |
| capabilities   | configurable |
| ビルドツール   | WAF (wscript) |
