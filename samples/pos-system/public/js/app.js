// ============================================================================
// app.js — Shared utilities for the Bantu POS frontend
// ============================================================================

const API = {
    get:  (path)            => fetch(path,                       { method: 'GET'  }).then(r => r.json()),
    post: (path, body)      => fetch(path, { method: 'POST',   headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) }).then(r => r.json()),
    put:  (path, body)      => fetch(path, { method: 'PUT',    headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) }).then(r => r.json()),
    del:  (path)            => fetch(path, { method: 'DELETE' }).then(r => r.json())
};

// Format a number as currency with thousands separator.
// Default currency: TZS (Tanzanian Shilling) — no decimals shown for large amounts.
// Override with localStorage['posCurrency'] = 'USD' | 'KES' | 'TZS' etc.
function fmtMoney(n) {
    if (n == null || isNaN(n)) n = 0;
    n = parseFloat(n);
    const cur = localStorage.getItem('posCurrency') || 'TZS';
    if (cur === 'USD' || cur === 'EUR' || cur === 'GBP') {
        return cur + ' ' + n.toFixed(2).replace(/\B(?=(\d{3})+(?!\d))/g, ',');
    }
    // TZS, KES, UGX — whole-shilling currencies
    return cur + ' ' + Math.round(n).toLocaleString('en-US');
}

// Format a raw number to 2-decimal string (for input fields)
function fmtNum(n) {
    if (n == null || isNaN(n)) return '0.00';
    return parseFloat(n).toFixed(2);
}

// Toast notification
function toast(msg, type) {
    const el = document.getElementById('toast');
    if (!el) { alert(msg); return; }
    el.textContent = msg;
    el.className = 'toast ' + (type || '');
    el.classList.remove('hidden');
    clearTimeout(window._toastTimer);
    window._toastTimer = setTimeout(() => el.classList.add('hidden'), 3000);
}

// Live clock in the top bar
function startClock() {
    const el = document.getElementById('clock');
    if (!el) return;
    const tick = () => {
        const d = new Date();
        const pad = (n) => String(n).padStart(2, '0');
        el.textContent = `${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}`;
    };
    tick();
    setInterval(tick, 1000);
}

// Escape HTML to prevent XSS in dynamically-built strings
function escHtml(s) {
    if (s == null) return '';
    return String(s)
        .replace(/&/g, '&amp;')
        .replace(/</g, '&lt;')
        .replace(/>/g, '&gt;')
        .replace(/"/g, '&quot;')
        .replace(/'/g, '&#039;');
}

document.addEventListener('DOMContentLoaded', startClock);
