// WristAgent companion JS - OpenAI API integration and conversation history management

// Numeric keys must match appinfo.json messageKeys exactly
var KEY_QUERY    = 0;
var KEY_RESPONSE = 1;
var KEY_STATUS   = 2;
var KEY_COMMAND  = 3;

var conversationHistory = [
  { role: 'system', content: 'You are a helpful assistant on a smartwatch. Answer concisely.' }
];
var MAX_HISTORY = 10;

function addToHistory(role, content) {
  conversationHistory.push({ role: role, content: content });
  var nonSystem = conversationHistory.filter(function(m) {
    return m.role !== 'system';
  });
  while (nonSystem.length > MAX_HISTORY) {
    var idx = conversationHistory.findIndex(function(m) {
      return m.role !== 'system';
    });
    conversationHistory.splice(idx, 1);
    nonSystem = conversationHistory.filter(function(m) {
      return m.role !== 'system';
    });
  }
}

// Retry sendAppMessage once on nack to handle transient ACK timing issues
function sendWithRetry(dict, label) {
  Pebble.sendAppMessage(dict,
    function() { console.log('[WristAgent] sent: ' + label); },
    function(e) {
      console.log('[WristAgent] nack for ' + label + ', retrying: ' + JSON.stringify(e));
      setTimeout(function() {
        Pebble.sendAppMessage(dict,
          function() { console.log('[WristAgent] retry ok: ' + label); },
          function(e2) { console.log('[WristAgent] retry failed: ' + JSON.stringify(e2)); }
        );
      }, 500);
    }
  );
}

function sendStatus(msg) {
  var dict = {};
  dict[KEY_STATUS] = msg;
  sendWithRetry(dict, 'status:' + msg);
}

function sendResponse(text) {
  var dict = {};
  dict[KEY_RESPONSE] = text.substring(0, 500);
  sendWithRetry(dict, 'response');
}

function handleQuery(text) {
  var apiKey = localStorage.getItem('openai_api_key');
  if (!apiKey) {
    sendStatus('error:APIキー未設定。設定を開いてください');
    return;
  }

  addToHistory('user', text);

  var req = new XMLHttpRequest();
  req.open('POST', 'https://api.openai.com/v1/chat/completions', true);
  req.setRequestHeader('Content-Type', 'application/json');
  req.setRequestHeader('Authorization', 'Bearer ' + apiKey);
  req.timeout = 15000;

  req.onload = function() {
    if (req.status === 200) {
      try {
        var data = JSON.parse(req.responseText);
        var reply = data.choices[0].message.content;
        addToHistory('assistant', reply);
        sendResponse(reply);
      } catch (err) {
        sendStatus('error:レスポンス解析失敗');
      }
    } else {
      sendStatus('error:API HTTP ' + req.status);
    }
  };

  req.ontimeout = function() {
    sendStatus('error:タイムアウト');
  };

  req.onerror = function() {
    sendStatus('error:ネットワークエラー');
  };

  req.send(JSON.stringify({
    model: 'gpt-4.1-mini',
    max_tokens: 200,
    messages: conversationHistory
  }));
}

Pebble.addEventListener('ready', function() {
  console.log('[WristAgent] PebbleKit JS ready');
});

Pebble.addEventListener('appmessage', function(e) {
  var payload = e.payload;
  console.log('[WristAgent] received: ' + JSON.stringify(payload));

  // Accept both numeric and string key forms
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
  Pebble.openURL('https://kukaidev.github.io/pebble-wrist-agent/config/');
});

Pebble.addEventListener('webviewclosed', function(e) {
  if (e.response && e.response !== 'CANCELLED') {
    try {
      var config = JSON.parse(decodeURIComponent(e.response));
      if (config.apiKey) {
        localStorage.setItem('openai_api_key', config.apiKey);
        console.log('[WristAgent] API key saved');
      }
    } catch (err) {
      console.log('[WristAgent] config parse error: ' + err);
    }
  }
});
