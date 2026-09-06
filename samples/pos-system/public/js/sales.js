// ============================================================================
// sales.js — Sales history page (list + view receipt)
// ============================================================================

document.addEventListener('DOMContentLoaded', () => {
    loadSales();
    bindEvents();
});

async function loadSales() {
    try {
        const r = await API.get('/api/sales?limit=200');
        if (!r.ok) throw new Error(r.error);
        renderTable(r.sales || []);
    } catch (e) {
        toast('Error: ' + e.message, 'error');
        document.getElementById('salesTableBody').innerHTML =
            '<tr><td colspan="9" class="empty">Could not load sales</td></tr>';
    }
}

function renderTable(sales) {
    const tbody = document.getElementById('salesTableBody');
    if (sales.length === 0) {
        tbody.innerHTML = '<tr><td colspan="9" class="empty">No sales yet. Make your first sale from the Checkout page.</td></tr>';
        return;
    }
    tbody.innerHTML = sales.map(s => {
        const payIcon = {
            cash: '💵', card: '💳', mpesa: '📱', mobile: '📱'
        }[s.payment_method] || '💰';
        return `
            <tr>
                <td><code>${escHtml(s.receipt_no)}</code></td>
                <td>${escHtml(s.created_at)}</td>
                <td>${escHtml(s.customer_name || 'Walk-in')}</td>
                <td class="num">${s.items_count || '-'}</td>
                <td class="num">${fmtMoney(s.subtotal)}</td>
                <td class="num">${fmtMoney(s.tax_total)}</td>
                <td class="num" style="font-weight:600;color:#1e3a8a">${fmtMoney(s.total)}</td>
                <td>${payIcon} ${escHtml(s.payment_method)}</td>
                <td><button class="btn btn-ghost btn-sm" onclick="viewSale(${s.id})">View</button></td>
            </tr>
        `;
    }).join('');
}

async function viewSale(id) {
    try {
        const r = await API.get('/api/sales/' + id);
        if (!r.ok) throw new Error(r.error);
        showSaleDetail(r.sale);
    } catch (e) {
        toast('Error: ' + e.message, 'error');
    }
}

function showSaleDetail(sale) {
    const items = sale.items || [];
    const changeDue = parseFloat(sale.change_due) || 0;
    const el = document.getElementById('saleDetail');
    el.innerHTML = `
        <div class="receipt">
            <div class="r-header">
                <div class="r-store">BANTU POS</div>
                <div class="r-meta">Receipt #${escHtml(sale.receipt_no)}</div>
                <div class="r-meta">${escHtml(sale.created_at)}</div>
                <div class="r-meta">Cashier: ${escHtml(sale.cashier || 'admin')}</div>
                <div class="r-meta">Customer: ${escHtml(sale.customer_name || 'Walk-in')}</div>
            </div>
            <div class="r-items">
                ${items.map(it => `
                    <div class="r-item">
                        <div class="r-item-head">
                            <span>${escHtml(it.name)}</span>
                            <span>${fmtMoney(it.line_total)}</span>
                        </div>
                        <div class="r-item-sub">
                            <span>${escHtml(it.sku)} × ${it.qty} @ ${fmtMoney(it.price)}</span>
                            <span>tax ${fmtNum(it.tax_rate)}%</span>
                        </div>
                    </div>
                `).join('')}
            </div>
            <div class="r-totals">
                <div class="r-row"><span>Subtotal</span><span>${fmtMoney(sale.subtotal)}</span></div>
                <div class="r-row"><span>Tax (VAT)</span><span>${fmtMoney(sale.tax_total)}</span></div>
                <div class="r-row"><span>Discount</span><span>-${fmtMoney(sale.discount)}</span></div>
                <div class="r-total-row"><span>TOTAL</span><span>${fmtMoney(sale.total)}</span></div>
                <div class="r-row" style="margin-top:8px;"><span>Payment (${escHtml(sale.payment_method)})</span><span>${fmtMoney(sale.payment_amount)}</span></div>
                ${changeDue > 0 ? `<div class="r-row"><span>Change Due</span><span>${fmtMoney(changeDue)}</span></div>` : ''}
            </div>
            <div class="r-footer">
                Thank you for shopping with us!<br>
                Returns accepted within 7 days with receipt.<br>
                Powered by Bantu v1.2.2
            </div>
        </div>
    `;
    document.getElementById('saleModal').classList.remove('hidden');
}

function bindEvents() {
    document.getElementById('refreshBtn').addEventListener('click', loadSales);
    document.getElementById('closeSaleBtn').addEventListener('click', () => {
        document.getElementById('saleModal').classList.add('hidden');
    });
    document.addEventListener('keydown', (e) => {
        if (e.key === 'Escape') {
            document.getElementById('saleModal').classList.add('hidden');
        }
    });
}
