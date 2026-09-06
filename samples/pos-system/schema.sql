-- ============================================================================
-- schema.sql — POS System PostgreSQL schema
-- ============================================================================
-- Apply with:
--   psql -U postgres -d pos -f schema.sql
-- Or run via the Bantu server (it auto-applies on startup if DATABASE_URL is set)
-- ============================================================================

-- ── Products ───────────────────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS products (
    id          SERIAL PRIMARY KEY,
    name        TEXT NOT NULL,
    sku         TEXT UNIQUE NOT NULL,
    barcode     TEXT,
    category    TEXT DEFAULT 'General',
    price       NUMERIC(10, 2) NOT NULL DEFAULT 0,
    cost        NUMERIC(10, 2) NOT NULL DEFAULT 0,
    stock       INTEGER NOT NULL DEFAULT 0,
    tax_rate    NUMERIC(5, 2) NOT NULL DEFAULT 18.00,  -- % VAT
    active      BOOLEAN NOT NULL DEFAULT TRUE,
    created_at  TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at  TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP
);

CREATE INDEX IF NOT EXISTS idx_products_category ON products(category);
CREATE INDEX IF NOT EXISTS idx_products_barcode  ON products(barcode);
CREATE INDEX IF NOT EXISTS idx_products_active   ON products(active);

-- ── Customers (optional — walk-in customers use id = 1) ─────────────────
CREATE TABLE IF NOT EXISTS customers (
    id          SERIAL PRIMARY KEY,
    name        TEXT NOT NULL,
    email       TEXT,
    phone       TEXT,
    address     TEXT,
    loyalty_pts INTEGER NOT NULL DEFAULT 0,
    created_at  TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP
);

-- Default walk-in customer (id = 1)
INSERT INTO customers (id, name, email, phone, address)
VALUES (1, 'Walk-in Customer', '', '', '')
ON CONFLICT (id) DO NOTHING;
SELECT setval('customers_id_seq', (SELECT MAX(id) FROM customers));

-- ── Sales (a single checkout = one sale) ────────────────────────────────
CREATE TABLE IF NOT EXISTS sales (
    id              SERIAL PRIMARY KEY,
    receipt_no      TEXT UNIQUE NOT NULL,
    customer_id     INTEGER REFERENCES customers(id) ON DELETE SET DEFAULT DEFAULT 1,
    cashier         TEXT DEFAULT 'admin',
    subtotal        NUMERIC(10, 2) NOT NULL DEFAULT 0,
    tax_total       NUMERIC(10, 2) NOT NULL DEFAULT 0,
    discount        NUMERIC(10, 2) NOT NULL DEFAULT 0,
    total           NUMERIC(10, 2) NOT NULL DEFAULT 0,
    payment_method  TEXT NOT NULL DEFAULT 'cash',     -- cash | card | mpesa | mobile
    payment_amount  NUMERIC(10, 2) NOT NULL DEFAULT 0,
    change_due      NUMERIC(10, 2) NOT NULL DEFAULT 0,
    status          TEXT NOT NULL DEFAULT 'completed',-- completed | refunded | voided
    created_at      TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP
);

CREATE INDEX IF NOT EXISTS idx_sales_created  ON sales(created_at);
CREATE INDEX IF NOT EXISTS idx_sales_customer ON sales(customer_id);

-- ── Sale items (each row = one product sold in one sale) ────────────────
CREATE TABLE IF NOT EXISTS sale_items (
    id          SERIAL PRIMARY KEY,
    sale_id     INTEGER NOT NULL REFERENCES sales(id) ON DELETE CASCADE,
    product_id  INTEGER REFERENCES products(id) ON DELETE SET NULL,
    name        TEXT NOT NULL,
    sku         TEXT NOT NULL,
    price       NUMERIC(10, 2) NOT NULL,
    qty         INTEGER NOT NULL,
    tax_rate    NUMERIC(5, 2) NOT NULL DEFAULT 0,
    line_total  NUMERIC(10, 2) NOT NULL,
    created_at  TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP
);

CREATE INDEX IF NOT EXISTS idx_sale_items_sale    ON sale_items(sale_id);
CREATE INDEX IF NOT EXISTS idx_sale_items_product ON sale_items(product_id);

-- ── Seed sample products (only if products table is empty) ──────────────
INSERT INTO products (name, sku, barcode, category, price, cost, stock, tax_rate)
SELECT * FROM (VALUES
    ('Coca-Cola 500ml',     'BEV-001', '8901234567890', 'Beverages',  1500.00, 1100.00, 120, 18.00),
    ('Bottled Water 1L',    'BEV-002', '8901234567891', 'Beverages',  1000.00,  600.00, 200, 18.00),
    ('Red Bull 250ml',      'BEV-003', '8901234567892', 'Beverages',  3500.00, 2700.00,  60, 18.00),
    ('Fresh Bread',         'BAK-001', '8901234567893', 'Bakery',     2500.00, 1800.00,  40,  0.00),
    ('Croissant',           'BAK-002', '8901234567894', 'Bakery',     3000.00, 2000.00,  30,  0.00),
    ('Milk 1L',             'DAI-001', '8901234567895', 'Dairy',      2800.00, 2200.00,  80,  0.00),
    ('Cheddar Cheese 200g', 'DAI-002', '8901234567896', 'Dairy',      8500.00, 6500.00,  25, 18.00),
    ('Eggs (tray of 30)',   'DAI-003', '8901234567897', 'Dairy',     12000.00, 9500.00,  35,  0.00),
    ('Rice 5kg',            'GRO-001', '8901234567898', 'Groceries', 12000.00, 9800.00,  50,  0.00),
    ('Cooking Oil 2L',      'GRO-002', '8901234567899', 'Groceries',  9500.00, 7500.00,  45, 18.00),
    ('Sugar 2kg',           'GRO-003', '8901234567800', 'Groceries',  5500.00, 4200.00,  60,  0.00),
    ('Salt 1kg',            'GRO-004', '8901234567801', 'Groceries',  1200.00,  800.00, 100,  0.00),
    ('Tea Bags 100pk',      'GRO-005', '8901234567802', 'Groceries',  7500.00, 5800.00,  40, 18.00),
    ('Soap Bar',            'HOM-001', '8901234567803', 'Household',  2500.00, 1700.00,  90, 18.00),
    ('Toothpaste',          'HOM-002', '8901234567804', 'Household',  4500.00, 3200.00,  55, 18.00),
    ('Detergent 1kg',       'HOM-003', '8901234567805', 'Household',  6500.00, 4800.00,  35, 18.00),
    ('Notebook A5',         'STA-001', '8901234567806', 'Stationery', 3000.00, 1800.00,  70,  0.00),
    ('Ballpoint Pen',       'STA-002', '8901234567807', 'Stationery', 1000.00,  500.00, 150,  0.00),
    ('USB Cable Type-C',    'ELE-001', '8901234567808', 'Electronics',8500.00, 5500.00,  40, 18.00),
    ('Earphones Wired',     'ELE-002', '8901234567809', 'Electronics',12000.00, 8000.00,  25, 18.00)
) AS t(name, sku, barcode, category, price, cost, stock, tax_rate)
WHERE NOT EXISTS (SELECT 1 FROM products LIMIT 1);

-- ── Views for dashboard queries ─────────────────────────────────────────
CREATE OR REPLACE VIEW v_daily_sales AS
SELECT
    DATE(created_at)                                   AS day,
    COUNT(*)                                           AS sales_count,
    SUM(subtotal)                                      AS subtotal,
    SUM(tax_total)                                     AS tax,
    SUM(total)                                         AS total,
    SUM(payment_amount - change_due)                   AS cash_received
FROM sales
WHERE status = 'completed'
GROUP BY DATE(created_at)
ORDER BY day DESC;

CREATE OR REPLACE VIEW v_top_products AS
SELECT
    p.name,
    p.sku,
    p.category,
    SUM(si.qty)                       AS units_sold,
    SUM(si.line_total)                AS revenue,
    AVG(si.price)                     AS avg_price
FROM sale_items si
LEFT JOIN products p ON p.id = si.product_id
GROUP BY p.name, p.sku, p.category
ORDER BY revenue DESC NULLS LAST;
