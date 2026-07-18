// WristAgent companion JS - OpenAI API integration and conversation history management

// ===== API KEY (hardcoded for testing) =====
var HARDCODED_API_KEY = ''; // ← ここに sk-... を記入
// ===========================================

var KEY_QUERY           = 0;
var KEY_RESPONSE        = 1;
var KEY_STATUS          = 2;
var KEY_COMMAND         = 3;
var KEY_TIMER_SET       = 4;
var KEY_TIMER_LABEL     = 5;
var KEY_STOPWATCH_START = 6;

var conversationHistory = [
  { role: 'system', content: 'You are a helpful assistant on a smartwatch. Answer concisely.' }
];
var MAX_HISTORY = 10;

// ES5互換: findIndex の代替
function firstNonSystemIndex() {
  for (var i = 0; i < conversationHistory.length; i++) {
    if (conversationHistory[i].role !== 'system') return i;
  }
  return -1;
}

function countNonSystem() {
  var n = 0;
  for (var i = 0; i < conversationHistory.length; i++) {
    if (conversationHistory[i].role !== 'system') n++;
  }
  return n;
}

function addToHistory(role, content) {
  conversationHistory.push({ role: role, content: content });
  while (countNonSystem() > MAX_HISTORY) {
    var idx = firstNonSystemIndex();
    if (idx === -1) break;
    conversationHistory.splice(idx, 1);
  }
}

function sendWithRetry(dict, label) {
  Pebble.sendAppMessage(dict,
    function() { console.log('[WA] sent: ' + label); },
    function(e) {
      console.log('[WA] nack ' + label + ', retry: ' + JSON.stringify(e));
      setTimeout(function() {
        Pebble.sendAppMessage(dict,
          function() { console.log('[WA] retry ok: ' + label); },
          function(e2) { console.log('[WA] retry fail: ' + JSON.stringify(e2)); }
        );
      }, 500);
    }
  );
}

function sendStatus(msg) {
  console.log('[WA] status len=' + msg.length);
  var dict = {};
  dict[KEY_STATUS] = msg;
  sendWithRetry(dict, 'status');
}

function sendResponse(text) {
  var dict = {};
  dict[KEY_RESPONSE] = text.substring(0, 500);
  sendWithRetry(dict, 'response');
}

// ===========================================================================
// Tools (OpenAI function calling)
// ===========================================================================

var OPENAI_TOOLS = [
  {
    type: 'function',
    'function': {
      name: 'get_weather',
      description: 'Get the weather forecast for a location on a date. ' +
        'Use this for any question about weather, temperature, rain, snow or wind.',
      parameters: {
        type: 'object',
        properties: {
          location: {
            type: 'string',
            description: "Place name, e.g. 'Tokyo' or '大阪'. " +
              "Omit to use the user's current location."
          },
          date: {
            type: 'string',
            description: 'Target date in YYYY-MM-DD. Omit for today. ' +
              'Forecasts are available up to 16 days ahead.'
          }
        }
      }
    }
  },
  {
    type: 'function',
    'function': {
      name: 'set_timer',
      description: 'Set a countdown timer on the smartwatch. ' +
        'Use this when the user asks for a timer, e.g. "12分タイマーして".',
      parameters: {
        type: 'object',
        properties: {
          duration_seconds: {
            type: 'integer',
            description: 'Timer length in seconds. Minimum 30.'
          },
          label: {
            type: 'string',
            description: "Short label for the timer, e.g. 'パスタ'. " +
              'Omit if the user gave no purpose.'
          }
        },
        required: ['duration_seconds']
      }
    }
  },
  {
    type: 'function',
    'function': {
      name: 'start_stopwatch',
      description: 'Start a stopwatch on the smartwatch. ' +
        'Use this when the user asks to start a stopwatch, e.g. "ストップウォッチ開始".',
      parameters: {
        type: 'object',
        properties: {
          label: {
            type: 'string',
            description: 'Short label for the stopwatch. Omit if none given.'
          }
        }
      }
    }
  }
];

// ツール実行→再問い合わせの最大ラウンド数（無限ループ防止）
var MAX_TOOL_ROUNDS = 2;

function pad2(n) { return (n < 10 ? '0' : '') + n; }

function localDateStr(d) {
  return d.getFullYear() + '-' + pad2(d.getMonth() + 1) + '-' + pad2(d.getDate());
}

function httpGetJson(url, timeoutMs, cb) {
  var req = new XMLHttpRequest();
  req.open('GET', url, true);
  req.timeout = timeoutMs;
  req.onload = function() {
    if (req.status === 200) {
      try { cb(null, JSON.parse(req.responseText)); return; } catch (e) {}
    }
    cb('http_' + req.status);
  };
  req.ontimeout = function() { cb('timeout'); };
  req.onerror = function() { cb('network'); };
  req.send(null);
}

// WMO weather code → 日本語表現
function wmoToText(code) {
  if (code === 0) return '快晴';
  if (code === 1 || code === 2) return '晴れ時々曇り';
  if (code === 3) return '曇り';
  if (code === 45 || code === 48) return '霧';
  if (code >= 51 && code <= 57) return '霧雨';
  if (code >= 61 && code <= 67) return '雨';
  if (code >= 71 && code <= 77) return '雪';
  if (code >= 80 && code <= 82) return 'にわか雨';
  if (code === 85 || code === 86) return 'にわか雪';
  if (code >= 95) return '雷雨';
  return '不明(code=' + code + ')';
}

// 地名 → 緯度経度（未指定なら現在地）
function resolveLocation(name, cb) {
  if (name) {
    var url = 'https://geocoding-api.open-meteo.com/v1/search?count=1&language=ja&name=' +
              encodeURIComponent(name);
    httpGetJson(url, 10000, function(err, data) {
      if (err || !data.results || !data.results.length) {
        console.log('[WA] geocode fail: ' + (err || 'no_results'));
        cb('place_not_found');
        return;
      }
      var r = data.results[0];
      cb(null, { lat: r.latitude, lon: r.longitude, name: r.name });
    });
  } else {
    if (!navigator.geolocation) {
      cb('no_geolocation');
      return;
    }
    navigator.geolocation.getCurrentPosition(
      function(pos) {
        cb(null, { lat: pos.coords.latitude, lon: pos.coords.longitude, name: '現在地' });
      },
      function(err) {
        console.log('[WA] geolocation fail: ' + JSON.stringify(err));
        cb('geolocation_failed');
      },
      { timeout: 10000, maximumAge: 600000 }
    );
  }
}

// get_weather ツール本体。結果は tool メッセージ用の JSON 文字列で返す
function getWeather(args, cb) {
  resolveLocation(args.location, function(err, loc) {
    if (err) {
      cb(JSON.stringify({ error: err }));
      return;
    }
    var date = args.date || localDateStr(new Date());
    var url = 'https://api.open-meteo.com/v1/forecast' +
      '?latitude=' + loc.lat + '&longitude=' + loc.lon +
      '&daily=weather_code,temperature_2m_max,temperature_2m_min,' +
      'precipitation_probability_max,wind_speed_10m_max' +
      '&timezone=auto&start_date=' + date + '&end_date=' + date;
    httpGetJson(url, 10000, function(werr, data) {
      if (werr || !data.daily || !data.daily.time || !data.daily.time.length) {
        console.log('[WA] weather fail: ' + (werr || 'no_daily'));
        cb(JSON.stringify({ error: 'weather_unavailable', detail: werr || 'no_data' }));
        return;
      }
      cb(JSON.stringify({
        place: loc.name,
        date: data.daily.time[0],
        weather: wmoToText(data.daily.weather_code[0]),
        temp_max_c: data.daily.temperature_2m_max[0],
        temp_min_c: data.daily.temperature_2m_min[0],
        precip_prob_pct: data.daily.precipitation_probability_max[0],
        wind_max_kmh: data.daily.wind_speed_10m_max[0]
      }));
    });
  });
}

// set_timer ツール本体。ウォッチに AppMessage を送り、ack は待たず楽観的に
// tool 結果を返す（Wakeup 予約の失敗はウォッチ側ステータス表示で通知される）
function setTimerOnWatch(args, cb) {
  var secs = parseInt(args.duration_seconds, 10);
  if (!secs || secs <= 0) {
    cb(JSON.stringify({ error: 'invalid_duration' }));
    return;
  }
  var dict = {};
  dict[KEY_TIMER_SET] = secs;
  dict[KEY_TIMER_LABEL] = args.label ? String(args.label) : '';
  sendWithRetry(dict, 'timer_set');
  cb(JSON.stringify({ status: 'scheduled', duration_seconds: secs }));
}

// start_stopwatch ツール本体。同じく楽観的に即時返却する
function startStopwatchOnWatch(args, cb) {
  var dict = {};
  dict[KEY_STOPWATCH_START] = 1;
  dict[KEY_TIMER_LABEL] = args.label ? String(args.label) : '';
  sendWithRetry(dict, 'stopwatch_start');
  cb(JSON.stringify({ status: 'started' }));
}

// assistant の tool_calls をすべて実行し、結果を添えて再問い合わせする
function executeToolCalls(assistantMsg, apiKey, extraMessages, round) {
  extraMessages.push(assistantMsg);
  var calls = assistantMsg.tool_calls;
  var results = [];
  var pending = calls.length;

  function finish(idx, content) {
    results[idx] = { role: 'tool', tool_call_id: calls[idx].id, content: content };
    pending--;
    if (pending === 0) {
      for (var k = 0; k < results.length; k++) extraMessages.push(results[k]);
      runCompletion(apiKey, extraMessages, round + 1);
    }
  }

  for (var i = 0; i < calls.length; i++) {
    (function(idx) {
      var fn = calls[idx]['function'] || {};
      console.log('[WA] tool call: ' + fn.name + ' args=' + fn.arguments);
      var args = {};
      try { args = JSON.parse(fn.arguments || '{}'); } catch (e) {}
      if (fn.name === 'get_weather') {
        getWeather(args, function(content) { finish(idx, content); });
      } else if (fn.name === 'set_timer') {
        setTimerOnWatch(args, function(content) { finish(idx, content); });
      } else if (fn.name === 'start_stopwatch') {
        startStopwatchOnWatch(args, function(content) { finish(idx, content); });
      } else {
        finish(idx, JSON.stringify({ error: 'unknown_tool' }));
      }
    })(i);
  }
}

// conversationHistory + 今回のツール往復（extraMessages）から messages を組み立てる。
// tool 往復は履歴に永続化しない（トリムで assistant/tool ペアが壊れるのを防ぐ）。
// system メッセージには相対日付（明日・週末等）解決用に今日の日付を注入する。
function buildMessages(extraMessages) {
  var now = new Date();
  var days = ['Sun', 'Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat'];
  var messages = [{
    role: 'system',
    content: conversationHistory[0].content +
      ' Today is ' + localDateStr(now) + ' (' + days[now.getDay()] + ').'
  }];
  for (var i = 1; i < conversationHistory.length; i++) messages.push(conversationHistory[i]);
  for (var j = 0; j < extraMessages.length; j++) messages.push(extraMessages[j]);
  return messages;
}

function runCompletion(apiKey, extraMessages, round) {
  var body = JSON.stringify({
    model: 'gpt-4o-mini',
    max_tokens: 200,
    messages: buildMessages(extraMessages),
    tools: OPENAI_TOOLS
  });
  console.log('[WA] request body length: ' + body.length + ' round=' + round);

  var req = new XMLHttpRequest();
  req.open('POST', 'https://api.openai.com/v1/chat/completions', true);
  req.setRequestHeader('Content-Type', 'application/json');
  req.setRequestHeader('Authorization', 'Bearer ' + apiKey);
  req.timeout = 15000;

  req.onload = function() {
    console.log('[WA] onload status: ' + req.status);
    console.log('[WA] response len=' + req.responseText.length);
    if (req.status === 200) {
      try {
        var data = JSON.parse(req.responseText);
        var msg = data.choices[0].message;
        if (msg.tool_calls && msg.tool_calls.length > 0) {
          if (round >= MAX_TOOL_ROUNDS) {
            console.log('[WA] tool round limit exceeded');
            sendStatus('error:tool_loop');
            return;
          }
          executeToolCalls(msg, apiKey, extraMessages, round);
          return;
        }
        var reply = msg.content || '';
        console.log('[WA] reply len=' + reply.length);
        addToHistory('assistant', reply);
        sendResponse(reply);
      } catch (err) {
        console.log('[WA] parse error: ' + err);
        sendStatus('error:parse_fail');
      }
    } else {
      // エラー内容の先頭をウォッチに表示
      var errMsg = 'HTTP ' + req.status;
      try {
        var errData = JSON.parse(req.responseText);
        if (errData.error && errData.error.message) {
          errMsg = errData.error.message.substring(0, 60);
        }
      } catch (e2) {}
      sendStatus('error:' + errMsg);
    }
  };

  req.ontimeout = function() {
    console.log('[WA] timeout');
    sendStatus('error:timeout');
  };

  req.onerror = function() {
    console.log('[WA] onerror');
    sendStatus('error:network');
  };

  req.send(body);
}

function handleQuery(text) {
  console.log('[WA] handleQuery len=' + text.length);

  var lsKey = localStorage.getItem('openai_api_key');
  console.log('[WA] ls_key=' + (lsKey ? lsKey.substring(0, 7) + '...' : 'null'));
  var apiKey = HARDCODED_API_KEY || lsKey;
  if (!apiKey) {
    sendStatus('error:no_api_key');
    return;
  }
  console.log('[WA] using key prefix: ' + apiKey.substring(0, 7));

  addToHistory('user', text);
  runCompletion(apiKey, [], 0);
}

Pebble.addEventListener('ready', function() {
  var stored = localStorage.getItem('openai_api_key');
  console.log('[WA] ready. ls_key=' + (stored ? stored.substring(0, 7) + '...(len=' + stored.length + ')' : 'null'));
});

Pebble.addEventListener('appmessage', function(e) {
  var payload = e.payload;
  console.log('[WA] appmessage keys=' + Object.keys(payload).join(','));

  var query   = payload[KEY_QUERY]   !== undefined ? payload[KEY_QUERY]   : payload.KEY_QUERY;
  var command = payload[KEY_COMMAND] !== undefined ? payload[KEY_COMMAND] : payload.KEY_COMMAND;

  if (query !== undefined) {
    handleQuery(String(query));
  }

  if (command === 'reset') {
    conversationHistory = [conversationHistory[0]];
    sendStatus('reset_ok');
  }

});

Pebble.addEventListener('showConfiguration', function() {
  console.log('[WA] showConfiguration fired');
  var apiKey = localStorage.getItem('openai_api_key') || '';
  var masked = apiKey
    ? apiKey.substring(0, 7) + '****...' + apiKey.slice(-4)
    : '';
  var url = 'https://kukai.github.io/pebble-wrist-agent/config/' +
            '?v=7' +
            '&return_to=' + encodeURIComponent('pebblejs://close') +
            '&current=' + encodeURIComponent(masked);
  console.log('[WA] openURL called');
  Pebble.openURL(url);
});

Pebble.addEventListener('webviewclosed', function(e) {
  console.log('[WA] webviewclosed len=' + (e.response ? e.response.length : 0));
  if (!e.response || e.response.length === 0) return;

  try {
    var raw = e.response.replace(/^#/, '');
    console.log('[WA] webviewclosed raw len=' + raw.length + ' prefix=' + raw.substring(0, 20));
    var config = JSON.parse(decodeURIComponent(raw));
    if (config.cancelled) { console.log('[WA] cancelled'); return; }
    if (config.apiKey) {
      console.log('[WA] writing key prefix=' + config.apiKey.substring(0, 7));
      localStorage.setItem('openai_api_key', config.apiKey);
      var verify = localStorage.getItem('openai_api_key');
      console.log('[WA] verify read=' + (verify ? verify.substring(0, 7) + '...' : 'null'));
      if (verify === config.apiKey) {
        sendStatus('key_saved');
      } else {
        console.log('[WA] ls write FAILED');
        sendStatus('error:ls_write_fail');
      }
    }
  } catch (err) {
    console.log('[WA] webviewclosed parse err: ' + err);
    sendStatus('error:wvc_parse');
  }
});
