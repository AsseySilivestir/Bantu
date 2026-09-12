# Bantu `uuid`

RFC 4122 / RFC 9562 UUID generation and inspection, written **in pure Bantu** on the OS CSPRNG
(`randbytes`) and the [`hash`](hash.md) module. Name-based UUIDs are bit-exact to the RFC vectors.

- **Source:** [`uuid/uuid.b`](../uuid/uuid.b) · **Tests:** [`uuid/uuid_test.b`](../uuid/uuid_test.b)

```bantu
include "./uuid.b" as uuid;

print(uuid.uuid4());                                   // random, e.g. 0cc77e81-6173-4b89-98d4-...
print(uuid.uuid7());                                   // time-ordered (sortable), great for DB keys
print(uuid.uuid5(uuid.NAMESPACE_DNS, "python.org"));   // 886313e1-3b8a-5372-9b90-0c9aee199e5d
```

```sh
bantu run uuid/uuid_test.b   # RFC vectors → RESULT: ALL GREEN
```

---

## Which version should I use?

| Version | Use it for | Notes |
|---|---|---|
| **`uuid4()`** | general unique IDs, tokens, keys | 122 random bits from the **OS CSPRNG** — unpredictable |
| **`uuid7()`** | database primary keys, event IDs | 48-bit ms timestamp + random → **sorts by creation time** |
| **`uuid5(ns, name)`** | stable IDs derived from a name | deterministic (SHA-1); same input → same UUID |
| **`uuid3(ns, name)`** | legacy deterministic IDs | deterministic (MD5); prefer `uuid5` for new code |

`uuid4`/`uuid7` draw from the cryptographically-secure `randbytes`, so they are safe as
unguessable identifiers.

---

## API

| Function | Returns |
|---|---|
| `uuid.uuid4()` | random UUID string (v4) |
| `uuid.uuid7()` | time-ordered UUID string (v7) |
| `uuid.uuid5(namespace, name)` | name-based UUID (v5, SHA-1) |
| `uuid.uuid3(namespace, name)` | name-based UUID (v3, MD5) |
| `uuid.parse(str)` | 16 raw bytes |
| `uuid.format(bytes)` | canonical `8-4-4-4-12` string |
| `uuid.is_valid(str)` | bool |
| `uuid.version_of(str)` | version digit (1–8) |

Predefined namespaces: `uuid.NAMESPACE_DNS`, `NAMESPACE_URL`, `NAMESPACE_OID`, `NAMESPACE_X500`.
A namespace argument may be a UUID string (with dashes) or a 16-byte list.
