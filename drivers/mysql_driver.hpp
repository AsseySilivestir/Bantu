#pragma once
/**
 * Bantu v1.2.1 — MySQL Driver (mysqlclient / MariaDB Connector)
 *
 * Real MySQL/MariaDB connectivity for Bantu. When Bantu is compiled
 * with mysqlclient available (`-DHAS_MYSQL` and `mysql.h` on the
 * include path), this driver is used in place of the in-process stub
 * and `sua.mysql.*` calls route to a live `MYSQL*` connection.
 *
 * Build (Linux):
 *   sudo apt install libmysqlclient-dev
 *   cmake .. -DBANTU_MYSQL=ON
 *
 * Build (Windows / vcpkg):
 *   vcpkg install libmysql
 *   cmake .. -DBANTU_MYSQL=ON -DCMAKE_TOOLCHAIN_FILE=...
 */

#ifdef HAS_MYSQL
#include <mysql.h>
#endif

#include <string>
#include <vector>
#include <iostream>
#include <unordered_map>

namespace bantu {

class MysqlDriver {
public:
#ifdef HAS_MYSQL
    MysqlDriver() { mysql_init(&conn_); }
    ~MysqlDriver() { close(); }

    bool connect(const std::string& host, const std::string& user,
                 const std::string& password, const std::string& db, int port) {
        if (!mysql_real_connect(&conn_, host.c_str(), user.c_str(),
                                password.c_str(), db.c_str(), port, nullptr, 0)) {
            std::cerr << "  [MYSQL] connect failed: " << mysql_error(&conn_) << "\n";
            return false;
        }
        std::cout << "  [MYSQL] mysql_real_connect OK (server="
                  << mysql_get_server_info(&conn_) << ")\n";
        return true;
    }

    std::vector<std::unordered_map<std::string, std::string>> query(const std::string& sql) {
        std::vector<std::unordered_map<std::string, std::string>> rows;
        if (mysql_query(&conn_, sql.c_str()) != 0) {
            std::cerr << "  [MYSQL] query failed: " << mysql_error(&conn_) << "\n";
            return rows;
        }
        MYSQL_RES* res = mysql_store_result(&conn_);
        if (!res) return rows; // non-SELECT
        int nFields = mysql_num_fields(res);
        MYSQL_FIELD* fields = mysql_fetch_fields(res);
        while (MYSQL_ROW row = mysql_fetch_row(res)) {
            std::unordered_map<std::string, std::string> r;
            for (int i = 0; i < nFields; ++i) {
                r[fields[i].name] = row[i] ? row[i] : "";
            }
            rows.push_back(std::move(r));
        }
        mysql_free_result(res);
        return rows;
    }

    int exec(const std::string& sql) {
        return mysql_query(&conn_, sql.c_str());
    }

    void close() {
        mysql_close(&conn_);
    }

    bool isConnected() const { return mysql_ping(const_cast<MYSQL*>(&conn_)) == 0; }

private:
    MYSQL conn_;
#else
    // Stub mode (default build): no-op driver. The evaluator's built-in
    // simulation handles `sua.mysql.*` calls in this case.
    bool connect(const std::string&, const std::string&,
                 const std::string&, const std::string&, int) { return false; }
    std::vector<std::unordered_map<std::string, std::string>> query(const std::string&) { return {}; }
    int exec(const std::string&) { return -1; }
    void close() {}
    bool isConnected() const { return false; }
#endif
};

} // namespace bantu
