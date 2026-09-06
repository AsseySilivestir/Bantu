// ============================================================================
// db.b — PostgreSQL data-access layer for the POS system
// ----------------------------------------------------------------------------
// Exposes:
//   $pg               — the global postgres connection handle
//   initSchema()      — runs schema.sql if not already applied
//   fmtNum($n)        — formats a number to 2-decimal-place string
//   escStr($s)        — single-quote escapes a string for SQL literals
//   toInt($v)         — coerces to int (handles NUMERIC coming back as float)
// ============================================================================

$pg = sua.postgres;

// ── Helpers ──────────────────────────────────────────────────────────────

// Format a number as a fixed 2-decimal string (e.g. 1500 -> "1500.00")
def fmtNum($n) {
    if ($n == null) { return "0.00"; }
    $s = "" + $n;
    $dot = indexOf($s, ".");
    if ($dot < 0) { return $s + ".00"; }
    $dec = len($s) - $dot - 1;
    if ($dec == 1) { return $s + "0"; }
    if ($dec == 0) { return $s + "00"; }
    return substr($s, 0, $dot + 3);
}

// Escape single quotes for safe SQL string literals
def escStr($s) {
    if ($s == null) { return ""; }
    $out = "";
    $i = 0;
    while ($i < len($s)) {
        $c = charAt($s, $i);
        if ($c == "'") {
            $out = $out + "''";
        } else {
            $out = $out + $c;
        }
        $i = $i + 1;
    }
    return $out;
}

// Coerce a value to int (handles NUMERIC returning as a float)
// Bantu has no parseInt() builtin — use num() then floor().
def toInt($v) {
    if ($v == null) { return 0; }
    return floor(num($v));
}

// Coerce to float
def toFloat($v) {
    if ($v == null) { return 0.0; }
    return num($v);
}

// ── Schema initialization ────────────────────────────────────────────────

def initSchema() {
    // Products
    $pg.exec("CREATE TABLE IF NOT EXISTS products ("
        + "id SERIAL PRIMARY KEY, "
        + "name TEXT NOT NULL, "
        + "sku TEXT UNIQUE NOT NULL, "
        + "barcode TEXT, "
        + "category TEXT DEFAULT 'General', "
        + "price NUMERIC(10, 2) NOT NULL DEFAULT 0, "
        + "cost NUMERIC(10, 2) NOT NULL DEFAULT 0, "
        + "stock INTEGER NOT NULL DEFAULT 0, "
        + "tax_rate NUMERIC(5, 2) NOT NULL DEFAULT 18.00, "
        + "active BOOLEAN NOT NULL DEFAULT TRUE, "
        + "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP, "
        + "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)");

    $pg.exec("CREATE INDEX IF NOT EXISTS idx_products_category ON products(category)");
    $pg.exec("CREATE INDEX IF NOT EXISTS idx_products_barcode  ON products(barcode)");
    $pg.exec("CREATE INDEX IF NOT EXISTS idx_products_active   ON products(active)");

    // Customers (walk-in is id=1)
    $pg.exec("CREATE TABLE IF NOT EXISTS customers ("
        + "id SERIAL PRIMARY KEY, "
        + "name TEXT NOT NULL, "
        + "email TEXT, "
        + "phone TEXT, "
        + "address TEXT, "
        + "loyalty_pts INTEGER NOT NULL DEFAULT 0, "
        + "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)");

    $pg.exec("INSERT INTO customers (id, name, email, phone, address) "
        + "VALUES (1, 'Walk-in Customer', '', '', '') "
        + "ON CONFLICT (id) DO NOTHING");

    // Sales
    $pg.exec("CREATE TABLE IF NOT EXISTS sales ("
        + "id SERIAL PRIMARY KEY, "
        + "receipt_no TEXT UNIQUE NOT NULL, "
        + "customer_id INTEGER DEFAULT 1, "
        + "cashier TEXT DEFAULT 'admin', "
        + "subtotal NUMERIC(10, 2) NOT NULL DEFAULT 0, "
        + "tax_total NUMERIC(10, 2) NOT NULL DEFAULT 0, "
        + "discount NUMERIC(10, 2) NOT NULL DEFAULT 0, "
        + "total NUMERIC(10, 2) NOT NULL DEFAULT 0, "
        + "payment_method TEXT NOT NULL DEFAULT 'cash', "
        + "payment_amount NUMERIC(10, 2) NOT NULL DEFAULT 0, "
        + "change_due NUMERIC(10, 2) NOT NULL DEFAULT 0, "
        + "status TEXT NOT NULL DEFAULT 'completed', "
        + "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)");

    $pg.exec("CREATE INDEX IF NOT EXISTS idx_sales_created  ON sales(created_at)");
    $pg.exec("CREATE INDEX IF NOT EXISTS idx_sales_customer ON sales(customer_id)");

    // Sale items
    $pg.exec("CREATE TABLE IF NOT EXISTS sale_items ("
        + "id SERIAL PRIMARY KEY, "
        + "sale_id INTEGER NOT NULL REFERENCES sales(id) ON DELETE CASCADE, "
        + "product_id INTEGER REFERENCES products(id) ON DELETE SET NULL, "
        + "name TEXT NOT NULL, "
        + "sku TEXT NOT NULL, "
        + "price NUMERIC(10, 2) NOT NULL, "
        + "qty INTEGER NOT NULL, "
        + "tax_rate NUMERIC(5, 2) NOT NULL DEFAULT 0, "
        + "line_total NUMERIC(10, 2) NOT NULL, "
        + "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)");

    $pg.exec("CREATE INDEX IF NOT EXISTS idx_sale_items_sale    ON sale_items(sale_id)");
    $pg.exec("CREATE INDEX IF NOT EXISTS idx_sale_items_product ON sale_items(product_id)");

    // Seed sample products only if the table is empty
    $cnt = $pg.query("SELECT COUNT(*) AS n FROM products");
    if (toInt($cnt[0]["n"]) == 0) {
        print("[db] seeding sample products...");
        $pg.exec("INSERT INTO products (name, sku, barcode, category, price, cost, stock, tax_rate) VALUES "
            + "('Coca-Cola 500ml',     'BEV-001', '8901234567890', 'Beverages',  1500.00, 1100.00, 120, 18.00), "
            + "('Bottled Water 1L',    'BEV-002', '8901234567891', 'Beverages',  1000.00,  600.00, 200, 18.00), "
            + "('Red Bull 250ml',      'BEV-003', '8901234567892', 'Beverages',  3500.00, 2700.00,  60, 18.00), "
            + "('Fresh Bread',         'BAK-001', '8901234567893', 'Bakery',     2500.00, 1800.00,  40,  0.00), "
            + "('Croissant',           'BAK-002', '8901234567894', 'Bakery',     3000.00, 2000.00,  30,  0.00), "
            + "('Milk 1L',             'DAI-001', '8901234567895', 'Dairy',      2800.00, 2200.00,  80,  0.00), "
            + "('Cheddar Cheese 200g', 'DAI-002', '8901234567896', 'Dairy',      8500.00, 6500.00,  25, 18.00), "
            + "('Eggs (tray of 30)',   'DAI-003', '8901234567897', 'Dairy',     12000.00, 9500.00,  35,  0.00), "
            + "('Rice 5kg',            'GRO-001', '8901234567898', 'Groceries', 12000.00, 9800.00,  50,  0.00), "
            + "('Cooking Oil 2L',      'GRO-002', '8901234567899', 'Groceries',  9500.00, 7500.00,  45, 18.00), "
            + "('Sugar 2kg',           'GRO-003', '8901234567800', 'Groceries',  5500.00, 4200.00,  60,  0.00), "
            + "('Salt 1kg',            'GRO-004', '8901234567801', 'Groceries',  1200.00,  800.00, 100,  0.00), "
            + "('Tea Bags 100pk',      'GRO-005', '8901234567802', 'Groceries',  7500.00, 5800.00,  40, 18.00), "
            + "('Soap Bar',            'HOM-001', '8901234567803', 'Household',  2500.00, 1700.00,  90, 18.00), "
            + "('Toothpaste',          'HOM-002', '8901234567804', 'Household',  4500.00, 3200.00,  55, 18.00), "
            + "('Detergent 1kg',       'HOM-003', '8901234567805', 'Household',  6500.00, 4800.00,  35, 18.00), "
            + "('Notebook A5',         'STA-001', '8901234567806', 'Stationery', 3000.00, 1800.00,  70,  0.00), "
            + "('Ballpoint Pen',       'STA-002', '8901234567807', 'Stationery', 1000.00,  500.00, 150,  0.00), "
            + "('USB Cable Type-C',    'ELE-001', '8901234567808', 'Electronics',8500.00, 5500.00,  40, 18.00), "
            + "('Earphones Wired',     'ELE-002', '8901234567809', 'Electronics',12000.00, 8000.00,  25, 18.00)");
        print("[db] seeded 20 sample products");
    } else {
        print("[db] products table has " + $cnt[0]["n"] + " rows");
    }
}

// ── Product queries ──────────────────────────────────────────────────────

def listProducts($filter) {
    $sql = "SELECT id, name, sku, barcode, category, "
        + "price, cost, stock, tax_rate, active, "
        + "TO_CHAR(created_at, 'YYYY-MM-DD') AS created_at "
        + "FROM products WHERE active = TRUE ";
    if ($filter != null && $filter != "") {
        $f = escStr($filter);
        $sql = $sql + "AND (LOWER(name) LIKE LOWER('%" + $f + "%') "
            + "OR LOWER(sku) LIKE LOWER('%" + $f + "%') "
            + "OR LOWER(barcode) LIKE LOWER('%" + $f + "%') "
            + "OR LOWER(category) LIKE LOWER('%" + $f + "%')) ";
    }
    $sql = $sql + "ORDER BY category ASC, name ASC LIMIT 500";
    return $pg.query($sql);
}

def getProduct($id) {
    $rows = $pg.query("SELECT id, name, sku, barcode, category, price, cost, "
        + "stock, tax_rate, active, "
        + "TO_CHAR(created_at, 'YYYY-MM-DD') AS created_at "
        + "FROM products WHERE id = " + toInt($id));
    if (len($rows) == 0) { return null; }
    return $rows[0];
}

def createProduct($p) {
    $name     = escStr($p["name"]);
    $sku      = escStr($p["sku"]);
    $barcode  = escStr($p["barcode"]);
    $category = escStr($p["category"]);
    $price    = fmtNum(toFloat($p["price"]));
    $cost     = fmtNum(toFloat($p["cost"]));
    $stock    = toInt($p["stock"]);
    $taxRate  = fmtNum(toFloat($p["tax_rate"]));
    $pg.exec("INSERT INTO products (name, sku, barcode, category, price, cost, stock, tax_rate) "
        + "VALUES ('" + $name + "', '" + $sku + "', '" + $barcode + "', '" + $category + "', "
        + $price + ", " + $cost + ", " + $stock + ", " + $taxRate + ")");
    $rows = $pg.query("SELECT id, name, sku, barcode, category, price, cost, stock, tax_rate "
        + "FROM products WHERE sku = '" + $sku + "' ORDER BY id DESC LIMIT 1");
    if (len($rows) == 0) { return null; }
    return $rows[0];
}

def updateProduct($id, $p) {
    $name     = escStr($p["name"]);
    $sku      = escStr($p["sku"]);
    $barcode  = escStr($p["barcode"]);
    $category = escStr($p["category"]);
    $price    = fmtNum(toFloat($p["price"]));
    $cost     = fmtNum(toFloat($p["cost"]));
    $stock    = toInt($p["stock"]);
    $taxRate  = fmtNum(toFloat($p["tax_rate"]));
    $active   = $p["active"];
    $activeStr = "TRUE";
    if ($active == false || $active == "false" || $active == 0) { $activeStr = "FALSE"; }
    $pg.exec("UPDATE products SET "
        + "name = '" + $name + "', "
        + "sku = '" + $sku + "', "
        + "barcode = '" + $barcode + "', "
        + "category = '" + $category + "', "
        + "price = " + $price + ", "
        + "cost = " + $cost + ", "
        + "stock = " + $stock + ", "
        + "tax_rate = " + $taxRate + ", "
        + "active = " + $activeStr + ", "
        + "updated_at = CURRENT_TIMESTAMP "
        + "WHERE id = " + toInt($id));
    return getProduct($id);
}

def deleteProduct($id) {
    $pg.exec("UPDATE products SET active = FALSE, updated_at = CURRENT_TIMESTAMP WHERE id = " + toInt($id));
    return true;
}

// Find a product by barcode or SKU (for scanner input)
def findByCode($code) {
    $c = escStr($code);
    $rows = $pg.query("SELECT id, name, sku, barcode, category, price, stock, tax_rate "
        + "FROM products WHERE active = TRUE AND "
        + "(barcode = '" + $c + "' OR sku = '" + $c + "') LIMIT 1");
    if (len($rows) == 0) { return null; }
    return $rows[0];
}

// ── Sales queries ────────────────────────────────────────────────────────

def listSales($limit) {
    $lim = toInt($limit);
    if ($lim <= 0 || $lim > 500) { $lim = 100; }
    return $pg.query("SELECT s.id, s.receipt_no, s.customer_id, c.name AS customer_name, "
        + "s.subtotal, s.tax_total, s.discount, s.total, "
        + "s.payment_method, s.payment_amount, s.change_due, s.status, "
        + "TO_CHAR(s.created_at, 'YYYY-MM-DD HH24:MI') AS created_at "
        + "FROM sales s LEFT JOIN customers c ON c.id = s.customer_id "
        + "ORDER BY s.id DESC LIMIT " + $lim);
}

def getSale($id) {
    $rows = $pg.query("SELECT s.id, s.receipt_no, s.customer_id, c.name AS customer_name, "
        + "s.subtotal, s.tax_total, s.discount, s.total, "
        + "s.payment_method, s.payment_amount, s.change_due, s.status, s.cashier, "
        + "TO_CHAR(s.created_at, 'YYYY-MM-DD HH24:MI') AS created_at "
        + "FROM sales s LEFT JOIN customers c ON c.id = s.customer_id "
        + "WHERE s.id = " + toInt($id));
    if (len($rows) == 0) { return null; }
    $sale = $rows[0];
    $items = $pg.query("SELECT si.id, si.product_id, si.name, si.sku, si.price, si.qty, "
        + "si.tax_rate, si.line_total "
        + "FROM sale_items si WHERE si.sale_id = " + toInt($id) + " ORDER BY si.id");
    $sale["items"] = $items;
    return $sale;
}

// Create a sale. Expects a payload like:
//   { "items": [{ "id":1, "qty":2 }, ...], "payment_method":"cash",
//     "payment_amount":5000, "discount":0, "customer_id":1 }
// Returns the new sale record (with items) or null on failure.
def createSale($payload) {
    $items = $payload["items"];
    if ($items == null || len($items) == 0) { return null; }

    // ── Compute totals server-side (don't trust the client) ────────────
    $subtotal = 0.0;
    $taxTotal = 0.0;
    $lineItems = [];       // resolved rows ready for INSERT
    $i = 0;
    while ($i < len($items)) {
        $it = $items[$i];
        $prod = getProduct(toInt($it["id"]));
        if ($prod != null) {
            $qty = toInt($it["qty"]);
            if ($qty <= 0) { $qty = 1; }
            $price = toFloat($prod["price"]);
            $taxRate = toFloat($prod["tax_rate"]);
            $lineTax = ($price * $qty) * ($taxRate / 100.0);
            $lineTotal = ($price * $qty);  // line_total is pre-tax; tax added on top
            $subtotal = $subtotal + $lineTotal;
            $taxTotal = $taxTotal + $lineTax;
            $lineItems = push($lineItems, {
                "product_id": toInt($prod["id"]),
                "name": $prod["name"],
                "sku": $prod["sku"],
                "price": $price,
                "qty": $qty,
                "tax_rate": $taxRate,
                "line_total": $lineTotal
            });
        }
        $i = $i + 1;
    }

    if (len($lineItems) == 0) { return null; }

    $discount = toFloat($payload["discount"]);
    if ($discount < 0) { $discount = 0; }
    $total = $subtotal + $taxTotal - $discount;
    if ($total < 0) { $total = 0; }

    $payMethod = "cash";
    if ($payload["payment_method"] != null && $payload["payment_method"] != "") {
        $payMethod = escStr($payload["payment_method"]);
    }
    $payAmount = toFloat($payload["payment_amount"]);
    if ($payAmount < $total) { $payAmount = $total; }  // for card/mpesa exact amount
    $changeDue = $payAmount - $total;
    if ($changeDue < 0) { $changeDue = 0; }

    $custId = toInt($payload["customer_id"]);
    if ($custId <= 0) { $custId = 1; }

    // Generate receipt number: POS-YYYYMMDD-HHMMSS-NNN
    $now = $pg.query("SELECT TO_CHAR(NOW(), 'YYYYMMDDHH24MISS') AS ts");
    $ts = $now[0]["ts"];
    $receiptNo = "POS-" + $ts;

    // Insert sale row
    $pg.exec("INSERT INTO sales (receipt_no, customer_id, subtotal, tax_total, discount, total, "
        + "payment_method, payment_amount, change_due, status) VALUES "
        + "('" + $receiptNo + "', " + $custId + ", "
        + fmtNum($subtotal) + ", " + fmtNum($taxTotal) + ", " + fmtNum($discount) + ", "
        + fmtNum($total) + ", '" + $payMethod + "', " + fmtNum($payAmount) + ", "
        + fmtNum($changeDue) + ", 'completed')");

    // Fetch the sale id back
    $saleRows = $pg.query("SELECT id FROM sales WHERE receipt_no = '" + $receiptNo + "'");
    if (len($saleRows) == 0) { return null; }
    $saleId = toInt($saleRows[0]["id"]);

    // Insert sale items + decrement stock
    $j = 0;
    while ($j < len($lineItems)) {
        $li = $lineItems[$j];
        $pg.exec("INSERT INTO sale_items (sale_id, product_id, name, sku, price, qty, tax_rate, line_total) VALUES "
            + "(" + $saleId + ", " + $li["product_id"] + ", '" + escStr($li["name"]) + "', '"
            + escStr($li["sku"]) + "', " + fmtNum($li["price"]) + ", " + $li["qty"] + ", "
            + fmtNum($li["tax_rate"]) + ", " + fmtNum($li["line_total"]) + ")");
        // Decrement stock (never below 0)
        $pg.exec("UPDATE products SET stock = GREATEST(stock - " + $li["qty"] + ", 0) "
            + "WHERE id = " + $li["product_id"]);
        $j = $j + 1;
    }

    return getSale($saleId);
}

// ── Dashboard metrics ────────────────────────────────────────────────────

def dashboard() {
    $today = $pg.query("SELECT COUNT(*) AS sales, COALESCE(SUM(total),0) AS revenue, "
        + "COALESCE(SUM(subtotal),0) AS subtotal, COALESCE(SUM(tax_total),0) AS tax "
        + "FROM sales WHERE status = 'completed' AND DATE(created_at) = CURRENT_DATE");
    $week = $pg.query("SELECT COUNT(*) AS sales, COALESCE(SUM(total),0) AS revenue "
        + "FROM sales WHERE status = 'completed' AND created_at >= NOW() - INTERVAL '7 days'");
    $month = $pg.query("SELECT COUNT(*) AS sales, COALESCE(SUM(total),0) AS revenue "
        + "FROM sales WHERE status = 'completed' AND created_at >= NOW() - INTERVAL '30 days'");
    $products = $pg.query("SELECT COUNT(*) AS total, "
        + "COALESCE(SUM(CASE WHEN stock <= 5 THEN 1 ELSE 0 END), 0) AS low_stock, "
        + "COALESCE(SUM(stock * cost), 0) AS inventory_value "
        + "FROM products WHERE active = TRUE");
    $top = $pg.query("SELECT p.name, p.sku, COALESCE(SUM(si.qty), 0) AS units, "
        + "COALESCE(SUM(si.line_total), 0) AS revenue "
        + "FROM products p LEFT JOIN sale_items si ON si.product_id = p.id "
        + "WHERE p.active = TRUE "
        + "GROUP BY p.id, p.name, p.sku "
        + "ORDER BY revenue DESC NULLS LAST LIMIT 5");
    $last7 = $pg.query("SELECT TO_CHAR(d.day, 'YYYY-MM-DD') AS day, "
        + "COALESCE(COUNT(s.id), 0) AS sales, COALESCE(SUM(s.total), 0) AS revenue "
        + "FROM generate_series(CURRENT_DATE - INTERVAL '6 days', CURRENT_DATE, '1 day') AS d(day) "
        + "LEFT JOIN sales s ON DATE(s.created_at) = d.day AND s.status = 'completed' "
        + "GROUP BY d.day ORDER BY d.day ASC");

    return {
        "today":     $today[0],
        "week":      $week[0],
        "month":     $month[0],
        "products":  $products[0],
        "top_items": $top,
        "last7":     $last7
    };
}
