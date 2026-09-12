// ============================================================================
// pos.js — Checkout page logic (product grid + cart + checkout + receipt)
// ============================================================================

let allProducts = [];
let cart = [];          // [{ id, name, sku, price, qty, tax_rate, stock }]
let scannerStream = null;

document.addEventListener('DOMContentLoaded', () => {
    loadProducts();
    bindEvents();
});

// ── Load products from API ──────────────────────────────────────────────
async function loadProducts() {
    try {
        const r = await API.get('/api/products');
        if (!r.ok) throw new Error(r.error || 'Failed to load products');
        allProducts = r.products || [];
        populateCategoryFilter();
        renderProducts();
    } catch (e) {
        toast('Error loading products: ' + e.message, 'error');
        document.getElementById('productGrid').innerHTML =
            '<div class="empty">Could not load products. Is PostgreSQL running?</div>';
    }
}

function populateCategoryFilter() {
    const sel = document.getElementById('categoryFilter');
    const cats = [...new Set(allProducts.map(p => p.category).filter(Boolean))].sort();
    sel.innerHTML = '<option value="">All categories</option>' +
        cats.map(c => `<option value="${escHtml(c)}">${escHtml(c)}</option>`).join('');
}

function renderProducts() {
    const grid = document.getElementById('productGrid');
    const q = (document.getElementById('searchInput').value || '').toLowerCase().trim();
    const cat = document.getElementById('categoryFilter').value;

    let filtered = allProducts.filter(p => {
        if (cat && p.category !== cat) return false;
        if (!q) return true;
        return (p.name && p.name.toLowerCase().includes(q))
            || (p.sku && p.sku.toLowerCase().includes(q))
            || (p.barcode && p.barcode.toLowerCase().includes(q));
    });

    if (filtered.length === 0) {
        grid.innerHTML = '<div class="empty">No products match your search</div>';
        return;
    }

    grid.innerHTML = filtered.map(p => {
        const low = parseInt(p.stock) <= 5;
        const out = parseInt(p.stock) <= 0;
        return `
            <div class="product-card ${out ? 'out-of-stock' : ''}" onclick="addToCart(${p.id})">
                <div class="pc-cat">${escHtml(p.category || 'General')}</div>
                <div class="pc-name">${escHtml(p.name)}</div>
                <div class="pc-sku">${escHtml(p.sku)}</div>
                <div class="pc-price">${fmtMoney(p.price)}</div>
                <div class="pc-stock ${low ? 'low' : ''}">${out ? 'OUT OF STOCK' : low ? 'Only ' + p.stock + ' left' : p.stock + ' in stock'}</div>
            </div>
        `;
    }).join('');
}

// ── Cart management ─────────────────────────────────────────────────────
function addToCart(productId) {
    const p = allProducts.find(x => parseInt(x.id) === parseInt(productId));
    if (!p) return;
    if (parseInt(p.stock) <= 0) {
        toast(p.name + ' is out of stock', 'error');
        return;
    }
    const existing = cart.find(c => c.id === p.id);
    if (existing) {
        if (existing.qty >= parseInt(p.stock)) {
            toast('No more stock available for ' + p.name, 'error');
            return;
        }
        existing.qty++;
    } else {
        cart.push({
            id: p.id,
            name: p.name,
            sku: p.sku,
            price: parseFloat(p.price),
            qty: 1,
            tax_rate: parseFloat(p.tax_rate || 0),
            stock: parseInt(p.stock)
        });
    }
    renderCart();
    toast(p.name + ' added to cart', 'success');
}

function removeFromCart(idx) {
    cart.splice(idx, 1);
    renderCart();
}

function changeQty(idx, delta) {
    const item = cart[idx];
    if (!item) return;
    const newQty = item.qty + delta;
    if (newQty <= 0) {
        removeFromCart(idx);
        return;
    }
    if (newQty > item.stock) {
        toast('Insufficient stock (max ' + item.stock + ')', 'error');
        return;
    }
    item.qty = newQty;
    renderCart();
}

function setQty(idx, val) {
    const item = cart[idx];
    if (!item) return;
    let n = parseInt(val) || 1;
    if (n <= 0) { removeFromCart(idx); return; }
    if (n > item.stock) {
        toast('Insufficient stock (max ' + item.stock + ')', 'error');
        n = item.stock;
    }
    item.qty = n;
    renderCart();
}

function clearCart() {
    if (cart.length === 0) return;
    if (!confirm('Clear all items from the cart?')) return;
    cart = [];
    renderCart();
}

function renderCart() {
    const el = document.getElementById('cartItems');
    if (cart.length === 0) {
        el.innerHTML = '<div class="empty">Scan or click products to add to cart</div>';
    } else {
        el.innerHTML = cart.map((c, i) => `
            <div class="cart-item">
                <div>
                    <div class="ci-name">${escHtml(c.name)}</div>
                    <div class="ci-meta">${escHtml(c.sku)} • ${fmtMoney(c.price)} × ${c.qty}</div>
                </div>
                <div class="ci-qty">
                    <button onclick="changeQty(${i}, -1)">−</button>
                    <input type="number" min="1" value="${c.qty}" onchange="setQty(${i}, this.value)">
                    <button onclick="changeQty(${i}, 1)">+</button>
                </div>
                <div>
                    <div class="ci-price">${fmtMoney(c.price * c.qty)}</div>
                    <button class="ci-remove" onclick="removeFromCart(${i})" title="Remove">✕</button>
                </div>
            </div>
        `).join('');
    }

    // Totals
    let subtotal = 0, taxTotal = 0;
    cart.forEach(c => {
        const line = c.price * c.qty;
        subtotal += line;
        taxTotal += line * (c.tax_rate / 100);
    });
    const discount = parseFloat(document.getElementById('discountInput').value) || 0;
    const total = subtotal + taxTotal - discount;

    document.getElementById('subtotal').textContent  = fmtMoney(subtotal);
    document.getElementById('taxTotal').textContent  = fmtMoney(taxTotal);
    document.getElementById('grandTotal').textContent = fmtMoney(total);

    // Auto-fill payment amount if it's 0 or less than total
    const payInput = document.getElementById('paymentAmount');
    const cur = parseFloat(payInput.value) || 0;
    if (cur < total) payInput.value = total.toFixed(2);
}

// ── Checkout ────────────────────────────────────────────────────────────
async function checkout() {
    if (cart.length === 0) {
        toast('Cart is empty', 'error');
        return;
    }

    let subtotal = 0, taxTotal = 0;
    cart.forEach(c => {
        subtotal += c.price * c.qty;
        taxTotal += (c.price * c.qty) * (c.tax_rate / 100);
    });
    const discount = parseFloat(document.getElementById('discountInput').value) || 0;
    const total = subtotal + taxTotal - discount;
    const payMethod = document.getElementById('paymentMethod').value;
    const payAmount = parseFloat(document.getElementById('paymentAmount').value) || 0;

    if (payMethod === 'cash' && payAmount < total) {
        toast('Insufficient cash tendered', 'error');
        return;
    }

    const payload = {
        items: cart.map(c => ({ id: c.id, qty: c.qty })),
        payment_method: payMethod,
        payment_amount: payAmount,
        discount: discount,
        customer_id: 1
    };

    try {
        const r = await API.post('/api/checkout', payload);
        if (!r.ok) throw new Error(r.error || 'Checkout failed');
        showReceipt(r.sale);
        cart = [];
        renderCart();
        // Refresh products (stock changed)
        loadProducts();
    } catch (e) {
        toast('Checkout failed: ' + e.message, 'error');
    }
}

// ── Receipt ─────────────────────────────────────────────────────────────
function showReceipt(sale) {
    const el = document.getElementById('receiptContent');
    const items = sale.items || [];
    const changeDue = parseFloat(sale.change_due) || 0;

    el.innerHTML = `
        <div class="receipt">
            <div class="r-header">
                <div class="r-store">BANTU POS</div>
                <div class="r-meta">Receipt #${escHtml(sale.receipt_no)}</div>
                <div class="r-meta">${escHtml(sale.created_at)}</div>
                <div class="r-meta">Cashier: ${escHtml(sale.cashier || 'admin')}</div>
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
    document.getElementById('receiptModal').classList.remove('hidden');
}

// ── Barcode scanner (uses native BarcodeDetector if available) ──────────
async function openScanner() {
    const modal = document.getElementById('scannerModal');
    const video = document.getElementById('scannerVideo');
    const statusEl = document.getElementById('scannerStatus');
    modal.classList.remove('hidden');

    if (!('BarcodeDetector' in window)) {
        statusEl.textContent = 'BarcodeDetector not supported in this browser. Type the barcode manually below.';
        return;
    }

    try {
        scannerStream = await navigator.mediaDevices.getUserMedia({
            video: { facingMode: 'environment' }
        });
        video.srcObject = scannerStream;
        const detector = new BarcodeDetector({ formats: ['ean_13', 'ean_8', 'code_128', 'code_39', 'upc_a', 'upc_e'] });
        statusEl.textContent = 'Point camera at a barcode...';

        const scan = async () => {
            try {
                const codes = await detector.detect(video);
                if (codes.length > 0) {
                    const code = codes[0].rawValue;
                    statusEl.textContent = 'Scanned: ' + code;
                    await lookupByCode(code);
                    closeScanner();
                    return;
                }
            } catch (e) { /* ignore detection errors */ }
            if (!modal.classList.contains('hidden')) {
                requestAnimationFrame(scan);
            }
        };
        scan();
    } catch (e) {
        statusEl.textContent = 'Camera access denied. Type the barcode manually below.';
    }
}

function closeScanner() {
    document.getElementById('scannerModal').classList.add('hidden');
    if (scannerStream) {
        scannerStream.getTracks().forEach(t => t.stop());
        scannerStream = null;
    }
}

async function lookupByCode(code) {
    try {
        const r = await API.get('/api/products/lookup/' + encodeURIComponent(code));
        if (!r.ok) {
            toast('No product matches code: ' + code, 'error');
            return;
        }
        addToCart(r.product.id);
    } catch (e) {
        toast('Lookup failed: ' + e.message, 'error');
    }
}

// ── Event bindings ──────────────────────────────────────────────────────
function bindEvents() {
    document.getElementById('searchInput').addEventListener('input', renderProducts);
    document.getElementById('categoryFilter').addEventListener('change', renderProducts);
    document.getElementById('scanBtn').addEventListener('click', openScanner);
    document.getElementById('closeScannerBtn').addEventListener('click', closeScanner);
    document.getElementById('manualBarcode').addEventListener('keydown', (e) => {
        if (e.key === 'Enter') {
            const v = e.target.value.trim();
            if (v) {
                lookupByCode(v);
                e.target.value = '';
                closeScanner();
            }
        }
    });
    document.getElementById('clearCartBtn').addEventListener('click', clearCart);
    document.getElementById('discountInput').addEventListener('input', renderCart);
    document.getElementById('checkoutBtn').addEventListener('click', checkout);
    document.getElementById('newSaleBtn').addEventListener('click', () => {
        document.getElementById('receiptModal').classList.add('hidden');
    });

    // Quick keyboard shortcuts:
    //   F2 → focus search
    //   F9 → checkout
    //   Esc → close modal
    document.addEventListener('keydown', (e) => {
        if (e.key === 'F2') { e.preventDefault(); document.getElementById('searchInput').focus(); }
        if (e.key === 'F9') { e.preventDefault(); checkout(); }
        if (e.key === 'Escape') {
            document.getElementById('receiptModal').classList.add('hidden');
            closeScanner();
        }
    });
}
