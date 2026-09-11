// Shared realtime channel: connects to /ws, reconnects on drop, and fans
// out each {type:"status", status, variables} payload to listeners
// registered by the page-specific scripts (dashboard.js, etc).
(function () {
  const listeners = [];
  let socket = null;

  function connect() {
    // The bearer token goes in the QUERY STRING, because the browser
    // WebSocket API gives no way to set an Authorization header. The gateway
    // authenticates the upgrade in its onaccept handler and closes the
    // connection if the token is missing, expired or lacks DEVICE_READ; see
    // the note on that handler in src/WebServer.cpp for why this is the
    // weakest link in the scheme.
    const token = window.HsfAuth ? window.HsfAuth.token() : null;
    const scheme = location.protocol === "https:" ? "wss://" : "ws://";
    const url = scheme + location.host + "/ws" + (token ? "?token=" + encodeURIComponent(token) : "");
    socket = new WebSocket(url);

    socket.onmessage = (event) => {
      let payload;
      try {
        payload = JSON.parse(event.data);
      } catch (e) {
        return;
      }
      listeners.forEach((fn) => fn(payload));
    };

    socket.onclose = () => {
      // A rejected upgrade closes immediately, so a page left open after its
      // token expired would otherwise reconnect every 2s forever. auth.js
      // redirects to the login page on the next REST 401; until then, back off
      // rather than hammering a socket that will keep being refused.
      const authed = !window.HsfAuth || !window.HsfAuth.authEnabled() || window.HsfAuth.isAuthenticated();
      setTimeout(connect, authed ? 2000 : 15000);
    };
  }

  connect();

  window.HsfWs = {
    onMessage(fn) {
      listeners.push(fn);
    },
  };
})();
