// ============================================================================
// dashboard.js — Dashboard page (KPIs + bar chart + top items)
// ============================================================================

document.addEventListener('DOMContentLoaded', () => {
    loadDashboard();
    document.getElementById('refreshBtn').addEventListener('click', loadDashboard);
});

async function loadDashboard() {
    try {
        const r = await API.get('/api/dashboard');
        if (!r.ok) throw new Error(r.error);
        renderKPIs(r.metrics);
        renderLast7(r.metrics.last7 || []);
        renderTopItems(r.metrics.top_items || []);
    } catch (e) {
        toast('Error: ' + e.message, 'error');
    }
}

function renderKPIs(m) {
    const today    = m.today    || {};
    const week     = m.week     || {};
    const month    = m.month    || {};
    const products = m.products || {};

    document.getElementById('todayRevenue').textContent = fmtMoney(today.revenue || 0);
    document.getElementById('todaySales').textContent   = (today.sales || 0) + ' sales';

    document.getElementById('weekRevenue').textContent  = fmtMoney(week.revenue || 0);
    document.getElementById('weekSales').textContent    = (week.sales || 0) + ' sales';

    document.getElementById('monthRevenue').textContent = fmtMoney(month.revenue || 0);
    document.getElementById('monthSales').textContent   = (month.sales || 0) + ' sales';

    document.getElementById('invValue').textContent     = fmtMoney(products.inventory_value || 0);
    document.getElementById('invProducts').textContent  = (products.total || 0) + ' products';

    document.getElementById('lowStock').textContent     = (products.low_stock || 0);
}

function renderLast7(rows) {
    const el = document.getElementById('last7Chart');
    if (!rows || rows.length === 0) {
        el.innerHTML = '<div class="empty">No data</div>';
        return;
    }
    const maxRev = Math.max(...rows.map(r => parseFloat(r.revenue) || 0), 1);
    el.innerHTML = rows.map(r => {
        const rev = parseFloat(r.revenue) || 0;
        const heightPct = (rev / maxRev) * 100;
        const label = (r.day || '').slice(5);  // MM-DD
        return `
            <div class="bar-wrap">
                <div class="bar" style="height:${heightPct}%" data-value="${fmtMoney(rev)}"></div>
                <div class="bar-label">${label}</div>
            </div>
        `;
    }).join('');
}

function renderTopItems(items) {
    const tbody = document.getElementById('topItemsBody');
    if (!items || items.length === 0) {
        tbody.innerHTML = '<tr><td colspan="3" class="empty">No sales yet</td></tr>';
        return;
    }
    tbody.innerHTML = items.map(it => `
        <tr>
            <td>${escHtml(it.name || '-')}</td>
            <td class="num">${it.units || 0}</td>
            <td class="num">${fmtMoney(it.revenue || 0)}</td>
        </tr>
    `).join('');
}
