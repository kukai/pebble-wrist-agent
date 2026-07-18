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

| キー定数     | 値 | 方向         | 用途                   |
|--------------|----|--------------|------------------------|
| KEY_QUERY    | 0  | Watch → Phone | 音声認識テキスト送信   |
| KEY_RESPONSE | 1  | Phone → Watch | AI 応答テキスト返信    |
| KEY_STATUS   | 2  | Phone → Watch | エラー・ステータス通知 |
| KEY_COMMAND  | 3  | Watch → Phone | "reset" コマンド送信   |

ステータス値の規則:
- `error:<メッセージ>` — エラー発生（ホーム画面に表示）
- `reset_ok` — 会話履歴リセット完了
- `key_saved` — API キー保存完了

## スクリーン遷移

```
HOME ──(SELECT)──▶ [音声録音] ──(認識成功)──▶ LOADING ──▶ ANSWER
ANSWER ──(SELECT / BACK)──▶ HOME
HOME: UP長押し(700ms) → 会話リセット送信
ANSWER: UP/DOWN短押し → スクロール±30px
ANSWER: UP/DOWN長押し(500ms) → 履歴前後移動
```

## 重要な制約・注意事項

- **ES5 必須**: コンパニオン JS は PebbleKit JS ランタイムが ES5 のため、アロー関数・`const`/`let`・`Array.prototype.findIndex` 等は使用不可。
- **バッファサイズ**: クエリ 512 B、レスポンス 512 B（AppMessage 上限に合わせた最大値で open）。応答は送信前に 500 文字にトリム。
- **ウォッチ側履歴**: リングバッファ 5 件（`HIST_CAP`）。JS 側の会話コンテキストは最大 10 ターン（非 system メッセージ）。
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

## GitHub 運用ルール

- PR の本文（description）は **日本語** で記述する。タイトルは英語。

## ドキュメント運用

- **SPECS.md** — 現行仕様のスナップショット。仕様変更時に上書き更新。
- **docs/adr/ADR.md** — 追記専用。決定を覆した場合も旧エントリは削除せず新エントリを追加。
