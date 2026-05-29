// WristAgent companion JS - OpenAI API integration and conversation history management

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

function handleQuery(text) {
  var apiKey = localStorage.getItem('openai_api_key');
  if (!apiKey) {
    Pebble.sendAppMessage(
      { 'KEY_STATUS': 'error:No API key. Open settings.' },
      function() {},
      function(e) { console.log('Send failed: ' + JSON.stringify(e)); }
    );
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
      var data = JSON.parse(req.responseText);
      var reply = data.choices[0].message.content;
      addToHistory('assistant', reply);
      // Truncate to 500 chars to fit within AppMessage 512-byte limit with key overhead
      var truncated = reply.substring(0, 500);
      Pebble.sendAppMessage(
        { 'KEY_RESPONSE': truncated },
        function() { console.log('Response sent'); },
        function(e) { console.log('Response send failed: ' + JSON.stringify(e)); }
      );
    } else {
      Pebble.sendAppMessage(
        { 'KEY_STATUS': 'error:API ' + req.status },
        function() {},
        function(e) { console.log('Status send failed: ' + JSON.stringify(e)); }
      );
    }
  };

  req.ontimeout = function() {
    Pebble.sendAppMessage(
      { 'KEY_STATUS': 'error:Timeout' },
      function() {},
      function(e) { console.log('Timeout send failed: ' + JSON.stringify(e)); }
    );
  };

  req.onerror = function() {
    Pebble.sendAppMessage(
      { 'KEY_STATUS': 'error:Network error' },
      function() {},
      function(e) { console.log('Error send failed: ' + JSON.stringify(e)); }
    );
  };

  req.send(JSON.stringify({
    model: 'gpt-4.1-mini',
    max_tokens: 200,
    messages: conversationHistory
  }));
}

Pebble.addEventListener('ready', function() {
  console.log('WristAgent PebbleKit JS ready');
});

Pebble.addEventListener('appmessage', function(e) {
  var payload = e.payload;
  console.log('Received: ' + JSON.stringify(payload));

  if (payload.KEY_QUERY !== undefined) {
    handleQuery(payload.KEY_QUERY);
  }

  if (payload.KEY_COMMAND === 'reset') {
    // Keep only system prompt
    conversationHistory = [conversationHistory[0]];
    Pebble.sendAppMessage(
      { 'KEY_STATUS': 'reset_ok' },
      function() { console.log('Reset acknowledged'); },
      function(e) { console.log('Reset ack failed: ' + JSON.stringify(e)); }
    );
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
        console.log('API key saved');
      }
    } catch (err) {
      console.log('Config parse error: ' + err);
    }
  }
});
