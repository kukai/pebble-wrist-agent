# Architecture Decision Records (ADR)

> **運用ルール**: このファイルは追記専用。過去の決定は変更・削除しない。
> 決定を覆した場合は新しいエントリを追加し、旧エントリの番号を参照する。

---

## ADR-001: ターゲットプラットフォームを Pebble Emery に限定する

- **日付**: 2026-06-14（コミット履歴より再構成）
- **状態**: 採用
- **決定**: `appinfo.json` の `targetPlatforms` を `["emery"]` のみとする。
- **理由**: Emery はマイクを搭載した Pebble 最上位機種であり、Dictation API を使う本アプリには必須。他機種では `PBL_MICROPHONE` が未定義となり音声入力が動作しない。
- **トレードオフ**: Pebble Time 等の旧機種ユーザーは対象外になる。

---

## ADR-002: SDK バージョンを 2 から 3 に変更する

- **日付**: 2026-06-14（コミット `7469b26` より）
- **状態**: 採用
- **決定**: `appinfo.json` の `sdkVersion` を `"3"` に設定する。
- **理由**: Dictation API は SDK 3 以降でのみ利用可能。また `enableMultiJS: true` も SDK 3 が必要。
- **トレードオフ**: SDK 2 ビルド環境との互換性を失う。

---

## ADR-003: コンパニオン JS を ES5 で記述する

- **日付**: 2026-06-14（コミット `7469b26` より）
- **状態**: 採用
- **決定**: `src/pkjs/index.js` は ES5 構文のみ使用。アロー関数・`const`/`let`・`Array.prototype.findIndex` 等は不使用。
- **理由**: PebbleKit JS ランタイムは ES5 準拠であり、ES6+ 構文は実行時エラーになる。
- **トレードオフ**: コードが冗長になる（`firstNonSystemIndex()` 等の手動実装が必要）。

---

## ADR-004: AppMessage バッファサイズを 512 bytes に設定する

- **日付**: 2026-06-14（コミット履歴より再構成）
- **状態**: 採用
- **決定**: `app_message_open` の inbox/outbox サイズを `min(platform_max, 512)` とする。
- **理由**: Pebble のプラットフォーム上限が小さい環境でもクラッシュしないよう上限チェックを入れつつ、実用的なテキスト長を確保するために 512 bytes を選択。
- **トレードオフ**: 長い応答は JS 側で 500 文字にトリムされる（`text.substring(0, 500)`）。

---

## ADR-005: ウォッチ側履歴をリングバッファ 5 件とする

- **日付**: 2026-06-14（コミット `e36353f` より）
- **状態**: 採用
- **決定**: C 側の `HistEntry s_hist[5]` でウォッチ画面内の閲覧履歴を管理する。JS 側の会話コンテキスト（10 ターン）とは独立して管理する。
- **理由**: ウォッチのメモリ制約上、無制限に履歴を保持できない。Q: 128 B + A: 512 B = 640 B × 5 = 3.2 KB が現実的な上限。
- **トレードオフ**: ウォッチで閲覧できる過去 Q&A は最大 5 件のみ。JS 側の会話コンテキスト（10 ターン）より少ない。

---

## ADR-006: OpenAI モデルとして gpt-4o-mini を採用する

- **日付**: 2026-06-14（コミット履歴より再構成）
- **状態**: 採用
- **決定**: API リクエストのモデルを `gpt-4o-mini` に固定する。`max_tokens: 200`。
- **理由**: スマートウォッチの小画面では長文応答は不要。`gpt-4o-mini` は低レイテンシかつ低コストであり、腕時計の UX に適する。
- **トレードオフ**: 複雑な推論タスクや長文生成には不向き。

---

## ADR-007: API キーの永続化に localStorage を使用する

- **日付**: 2026-06-14（コミット `5f639a7` より）
- **状態**: 採用（`5f639a7` で競合する保存場所を localStorage に一本化）
- **決定**: PebbleKit JS の `localStorage.setItem('openai_api_key', key)` のみを正とし、他の保存手段（`Pebble.getLocalStorageItem` 等）は使わない。
- **理由**: PebbleKit JS の `Pebble.getLocalStorageItem` と `localStorage` が別ストレージであることによる二重管理バグを解消するため。
- **トレードオフ**: `HARDCODED_API_KEY` 変数は開発テスト用にのみ残す（本番では空文字）。

---

## ADR-008: 設定 UI の return_to を pebblejs://close プロトコルに統一する

- **日付**: 2026-06-14（コミット `e8b5812` より）
- **状態**: 採用
- **決定**: `showConfiguration` イベントで開く URL の `return_to` パラメータを常に `pebblejs://close` とする。
- **理由**: 他のプロトコル（`pebble-js-app://` 等）では `webviewclosed` イベントが発火しないケースがあり、API キーが JS に返却されない不具合が発生していた。
- **トレードオフ**: なし（`pebblejs://close` が PebbleKit JS の公式手順）。

---

## ADR-009: 設定 UI を GitHub Pages でホストする

- **日付**: 2026-06-14（コミット履歴より再構成）
- **状態**: 採用
- **決定**: `config/index.html` を `https://kukai.github.io/pebble-wrist-agent/config/` で公開する。JS 側はこの URL を `?v=N` のキャッシュバスト付きでオープンする。
- **理由**: Pebble の `showConfiguration` はリモート URL を必要とし、アプリバンドル内の HTML を直接参照できない。GitHub Pages は無料かつ静的ファイルのホスティングに適する。
- **トレードオフ**: 設定 UI の更新はリポジトリへの push が必要。スマートフォンのキャッシュ対策として `?v=N` を手動インクリメントする運用が必要。

---

## ADR-010: localStorage diagnostics コマンドを本番コードから除去する

- **日付**: 2026-06-14（コミット `d0bf49d` より）
- **状態**: 採用
- **決定**: デバッグ用の `lscheck` / `lsclear` コマンドハンドラを `index.js` から削除する。
- **理由**: 本番アプリに不要なデバッグ UI はコードサイズを増やし、誤操作リスクになる。診断は `pebble logs` で十分。
- **トレードオフ**: localStorage の状態を手動確認する手段が失われる（開発時は `pebble logs` で代替）。
