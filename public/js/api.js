// ═══════════════════════════════════════════════════════════════
// ChatBantu — Frontend API helper
// Pure JavaScript, no frameworks. Talks to the Bantu backend.
// ═══════════════════════════════════════════════════════════════

const api = {
  base: '',  // same origin

  token() {
    return localStorage.getItem('cb_token') || '';
  },

  user() {
    try {
      return JSON.parse(localStorage.getItem('cb_user') || 'null');
    } catch (e) { return null; }
  },

  setSession(token, user) {
    localStorage.setItem('cb_token', token);
    localStorage.setItem('cb_user', JSON.stringify(user));
  },

  clearSession() {
    localStorage.removeItem('cb_token');
    localStorage.removeItem('cb_user');
  },

  async request(method, path, body) {
    const headers = { 'Content-Type': 'application/json' };
    const tok = this.token();
    if (tok) headers['Authorization'] = 'Bearer ' + tok;
    const opts = { method, headers };
    if (body !== undefined && body !== null) {
      opts.body = JSON.stringify(body);
    }
    let res;
    try {
      res = await fetch(this.base + path, opts);
    } catch (e) {
      throw new Error('Network error: ' + e.message);
    }
    let data = null;
    const ct = res.headers.get('Content-Type') || '';
    if (ct.indexOf('json') !== -1) {
      data = await res.json();
    } else {
      data = await res.text();
    }
    if (res.status === 401) {
      // Auto-logout on auth failure
      this.clearSession();
      if (!window.location.pathname.endsWith('index.html') && !window.location.pathname.endsWith('/')) {
        window.location.href = '/';
      }
    }
    return data;
  },

  get(path)  { return this.request('GET',  path); },
  post(path, body) { return this.request('POST', path, body); },
  put(path, body)  { return this.request('PUT',  path, body); },
  del(path)        { return this.request('DELETE', path); },
};

// ─── Utilities ──────────────────────────────────────────────

function avatarLetter(name) {
  if (!name) return '?';
  return name.trim().charAt(0).toUpperCase();
}

function timeAgo(iso) {
  if (!iso) return '';
  const d = new Date(iso.endsWith('Z') ? iso : iso.replace(' ', 'T') + 'Z');
  const now = new Date();
  const diff = Math.floor((now - d) / 1000);
  if (diff < 60)    return 'just now';
  if (diff < 3600)  return Math.floor(diff / 60) + 'm ago';
  if (diff < 86400) return Math.floor(diff / 3600) + 'h ago';
  if (diff < 604800) return Math.floor(diff / 86400) + 'd ago';
  return d.toLocaleDateString();
}

function toast(message, ms = 3000) {
  const el = document.createElement('div');
  el.className = 'toast';
  el.textContent = message;
  document.body.appendChild(el);
  setTimeout(() => el.remove(), ms);
}

function requireAuth() {
  if (!api.token()) {
    window.location.href = '/';
    return false;
  }
  return true;
}

// Inject sidebar into any page that has <div id="sidebar"></div>
function renderSidebar(active) {
  const el = document.getElementById('sidebar');
  if (!el) return;
  const u = api.user() || { displayName: '?', username: '' };
  el.innerHTML = `
    <div class="brand"><span class="logo">💬</span> ChatBantu</div>
    <div class="nav-item ${active === 'feed' ? 'active' : ''}" onclick="location.href='/feed.html'">
      <span class="icon">📰</span><span>Feed</span>
    </div>
    <div class="nav-item ${active === 'chat' ? 'active' : ''}" onclick="location.href='/chat.html'">
      <span class="icon">💬</span><span>Messages</span>
      <span class="badge" id="nav-unread">0</span>
    </div>
    <div class="nav-item ${active === 'people' ? 'active' : ''}" onclick="location.href='/people.html'">
      <span class="icon">👥</span><span>People</span>
    </div>
    <div class="nav-item ${active === 'notifications' ? 'active' : ''}" onclick="location.href='/notifications.html'">
      <span class="icon">🔔</span><span>Notifications</span>
      <span class="badge" id="nav-notif">0</span>
    </div>
    <div class="me">
      <div class="avatar">${avatarLetter(u.displayName)}</div>
      <div class="info">
        <div class="name">${escapeHtml(u.displayName || '')}</div>
        <div class="uname">@${escapeHtml(u.username || '')}</div>
      </div>
      <button class="logout" title="Sign out" onclick="doLogout()">⏻</button>
    </div>
  `;
  refreshBadges();
  // Poll unread counts every 15s
  setInterval(refreshBadges, 15000);
}

async function refreshBadges() {
  try {
    const r = await api.get('/api/unread');
    if (r && !r.error) {
      const m = document.getElementById('nav-unread');
      const n = document.getElementById('nav-notif');
      if (m) m.textContent = r.unreadMessages > 0 ? r.unreadMessages : '';
      if (n) n.textContent = r.unreadNotifications > 0 ? r.unreadNotifications : '';
    }
  } catch (e) { /* ignore */ }
}

function doLogout() {
  api.clearSession();
  window.location.href = '/';
}

function escapeHtml(s) {
  if (s == null) return '';
  return String(s)
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;')
    .replace(/'/g, '&#39;');
}

// Heartbeat — keep us marked online
setInterval(() => {
  if (api.token()) {
    api.post('/api/presence', {}).catch(() => {});
  }
}, 30000);
// Send an immediate heartbeat on page load
if (api.token()) {
  api.post('/api/presence', {}).catch(() => {});
}

// ─── Incoming-call watcher ──────────────────────────────────
// Rings an Accept/Decline banner on ANY authed page when someone calls us.
// It PEEKS GET /api/call/incoming (non-consuming); Accept opens the callee
// call window (dir=in), which then performs the real, consuming offer fetch.
// This is the fix for "call gets stuck after pickup": previously nothing ever
// launched the callee/answerer path, so no answer was ever created.
(function () {
  // Don't ring on the call page itself — we're already in a call there.
  if (location.pathname.endsWith('/call.html')) return;

  let ringingFor = null;      // fromId currently shown in the banner
  const handled = new Set();  // peers we've accepted/declined this ring cycle

  function dismissBanner() {
    const el = document.getElementById('incoming-call-banner');
    if (el) el.remove();
    ringingFor = null;
  }

  function showBanner(fromId, fromName, hasVideo) {
    if (ringingFor === fromId || document.getElementById('incoming-call-banner')) return;
    ringingFor = fromId;
    const kind = hasVideo ? 'video' : 'voice';
    const el = document.createElement('div');
    el.id = 'incoming-call-banner';
    el.className = 'incoming-call-banner';
    el.innerHTML =
      '<div class="ic-avatar">' + avatarLetter(fromName) + '</div>' +
      '<div class="ic-body">' +
        '<div class="ic-name">' + escapeHtml(fromName) + '</div>' +
        '<div class="ic-sub">Incoming ' + kind + ' call…</div>' +
      '</div>' +
      '<button class="ic-btn accept" id="ic-accept">Accept</button>' +
      '<button class="ic-btn decline" id="ic-decline">Decline</button>';
    document.body.appendChild(el);

    document.getElementById('ic-accept').onclick = function () {
      handled.add(fromId);
      const params = new URLSearchParams({
        peer: fromId, name: fromName, video: hasVideo ? '1' : '0', dir: 'in'
      });
      window.open('/call.html?' + params.toString(), '_blank', 'width=1100,height=800');
      dismissBanner();
    };
    document.getElementById('ic-decline').onclick = function () {
      handled.add(fromId);
      api.post('/api/call/hangup/' + fromId, {}).catch(() => {});
      dismissBanner();
    };
  }

  async function pollIncoming() {
    if (!api.token()) return;
    try {
      const r = await api.get('/api/call/incoming');
      if (r && r.incoming) {
        if (!handled.has(r.fromId)) showBanner(r.fromId, r.fromName, r.hasVideo);
      } else {
        // No pending offer (caller hung up, or callee consumed it) — reset.
        if (ringingFor !== null) dismissBanner();
        handled.clear();
      }
    } catch (e) { /* ignore transient errors */ }
  }

  setInterval(pollIncoming, 2000);
  if (api.token()) pollIncoming();
})();
