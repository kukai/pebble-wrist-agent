// WristAgent companion JS - OpenAI API integration and conversation history management

// ===== API KEY (hardcoded for testing) =====
var HARDCODED_API_KEY = ''; // ← ここに sk-... を記入
// ===========================================

var KEY_QUERY    = 0;
var KEY_RESPONSE = 1;
var KEY_STATUS   = 2;
var KEY_COMMAND  = 3;

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

function handleQuery(text) {
  console.log('[WA] handleQuery len=' + text.length);

  var apiKey = HARDCODED_API_KEY || localStorage.getItem('openai_api_key');
  if (!apiKey) {
    sendStatus('error:no_api_key');
    return;
  }
  console.log('[WA] apiKey prefix: ' + apiKey.substring(0, 7));

  addToHistory('user', text);

  var body = JSON.stringify({
    model: 'gpt-4o-mini',
    max_tokens: 200,
    messages: conversationHistory
  });
  console.log('[WA] request body length: ' + body.length);

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
        var reply = data.choices[0].message.content;
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

Pebble.addEventListener('ready', function() {
  console.log('[WA] PebbleKit JS ready');
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
            '?v=6&current=' + encodeURIComponent(masked);
  console.log('[WA] openURL called');
  Pebble.openURL(url);
});

Pebble.addEventListener('webviewclosed', function(e) {
  console.log('[WA] webviewclosed len=' + (e.response ? e.response.length : 0));
  if (!e.response || e.response.length === 0) return;

  try {
    var config = JSON.parse(decodeURIComponent(e.response));
    if (config.cancelled) return;
    if (config.apiKey) {
      localStorage.setItem('openai_api_key', config.apiKey);
      console.log('[WA] key saved');
      sendStatus('key_saved');
    }
  } catch (err) {
    console.log('[WA] webviewclosed parse err');
  }
});
