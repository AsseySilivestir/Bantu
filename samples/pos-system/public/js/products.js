// ============================================================================
// products.js — Products management page (CRUD)
// ============================================================================

let allProducts = [];

document.addEventListener('DOMContentLoaded', () => {
    loadProducts();
    bindEvents();
});

async function loadProducts() {
    try {
        const r = await API.get('/api/products');
        if (!r.ok) throw new Error(r.error || 'Failed to load products');
        allProducts = r.products || [];
        renderTable();
    } catch (e) {
        toast('Error: ' + e.message, 'error');
        document.getElementById('productsTableBody').innerHTML =
            '<tr><td colspan="8" class="empty">Could not load products</td></tr>';
    }
}

function renderTable() {
    const tbody = document.getElementById('productsTableBody');
    const q = (document.getElementById('searchInput').value || '').toLowerCase().trim();

    const filtered = allProducts.filter(p => {
        if (!q) return true;
        return (p.name && p.name.toLowerCase().includes(q))
            || (p.sku && p.sku.toLowerCase().includes(q))
            || (p.category && p.category.toLowerCase().includes(q));
    });

    if (filtered.length === 0) {
        tbody.innerHTML = '<tr><td colspan="8" class="empty">No products found</td></tr>';
        return;
    }

    tbody.innerHTML = filtered.map(p => {
        const low = parseInt(p.stock) <= 5;
        return `
            <tr>
                <td><code>${escHtml(p.sku)}</code></td>
                <td>${escHtml(p.name)}</td>
                <td><span class="pc-cat">${escHtml(p.category || 'General')}</span></td>
                <td class="num">${fmtMoney(p.price)}</td>
                <td class="num">${fmtMoney(p.cost || 0)}</td>
                <td class="num ${low ? 'low' : ''}" style="${low ? 'color:#dc2626;font-weight:600' : ''}">${p.stock}</td>
                <td class="num">${fmtNum(p.tax_rate)}</td>
                <td>
                    <button class="btn btn-ghost btn-sm" onclick="editProduct(${p.id})">Edit</button>
                    <button class="btn btn-ghost btn-sm" onclick="deleteProduct(${p.id})" style="color:#dc2626">Delete</button>
                </td>
            </tr>
        `;
    }).join('');
}

function openModal(product) {
    const modal = document.getElementById('productModal');
    document.getElementById('modalTitle').textContent = product ? 'Edit Product' : 'New Product';
    document.getElementById('productId').value    = product ? product.id : '';
    document.getElementById('pName').value        = product ? product.name : '';
    document.getElementById('pSku').value         = product ? product.sku : '';
    document.getElementById('pBarcode').value     = product ? (product.barcode || '') : '';
    document.getElementById('pCategory').value    = product ? (product.category || 'General') : 'General';
    document.getElementById('pPrice').value       = product ? product.price : '';
    document.getElementById('pCost').value        = product ? (product.cost || 0) : 0;
    document.getElementById('pStock').value       = product ? product.stock : 0;
    document.getElementById('pTax').value         = product ? (product.tax_rate || 18) : 18;
    modal.classList.remove('hidden');
    document.getElementById('pName').focus();
}

function closeModal() {
    document.getElementById('productModal').classList.add('hidden');
}

async function editProduct(id) {
    try {
        const r = await API.get('/api/products/' + id);
        if (!r.ok) throw new Error(r.error);
        openModal(r.product);
    } catch (e) {
        toast('Error: ' + e.message, 'error');
    }
}

async function deleteProduct(id) {
    const p = allProducts.find(x => parseInt(x.id) === parseInt(id));
    if (!p) return;
    if (!confirm('Delete "' + p.name + '"?\n(This soft-deletes the product — it stays in sales history.)')) return;
    try {
        const r = await API.del('/api/products/' + id);
        if (!r.ok) throw new Error(r.error);
        toast(p.name + ' deleted', 'success');
        loadProducts();
    } catch (e) {
        toast('Error: ' + e.message, 'error');
    }
}

async function saveProduct(e) {
    e.preventDefault();
    const id = document.getElementById('productId').value;
    const payload = {
        name:     document.getElementById('pName').value.trim(),
        sku:      document.getElementById('pSku').value.trim(),
        barcode:  document.getElementById('pBarcode').value.trim(),
        category: document.getElementById('pCategory').value.trim() || 'General',
        price:    parseFloat(document.getElementById('pPrice').value) || 0,
        cost:     parseFloat(document.getElementById('pCost').value) || 0,
        stock:    parseInt(document.getElementById('pStock').value) || 0,
        tax_rate: parseFloat(document.getElementById('pTax').value) || 0,
        active:   true
    };
    if (!payload.name || !payload.sku) {
        toast('Name and SKU are required', 'error');
        return;
    }
    try {
        let r;
        if (id) {
            r = await API.put('/api/products/' + id, payload);
        } else {
            r = await API.post('/api/products', payload);
        }
        if (!r.ok) throw new Error(r.error);
        toast(id ? 'Product updated' : 'Product created', 'success');
        closeModal();
        loadProducts();
    } catch (e) {
        toast('Save failed: ' + e.message, 'error');
    }
}

function bindEvents() {
    document.getElementById('searchInput').addEventListener('input', renderTable);
    document.getElementById('newProductBtn').addEventListener('click', () => openModal(null));
    document.getElementById('cancelProductBtn').addEventListener('click', closeModal);
    document.getElementById('productForm').addEventListener('submit', saveProduct);
    document.addEventListener('keydown', (e) => {
        if (e.key === 'Escape') closeModal();
    });
}
