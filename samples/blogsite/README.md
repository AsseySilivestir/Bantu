# Bantu Blogsite Sample

A modular blog backend demonstrating Bantu v1.2.2's `include` keyword for splitting a real app across `server.b`, `db.b`, `controller.b`, `routes.b`, and `login.b`.

## Structure

```
blogsite/
├── server.b          # Entry point — includes the other modules
├── db.b              # SQLite connection + queries (listPosts, getPost, createPost)
├── controller.b      # Request handlers (postController.index / show / create)
├── routes.b          # Route registration (routes.registerAll(sua))
├── login.b           # Authentication module ($USERS, authenticate($req))
├── public/
│   └── index.html    # Landing page served by sua.server.static
└── bantu.json        # Project manifest
```

## Run

```bash
bantu run server.b
# → http://localhost:3000
```

## API

| Method | Path | Description |
|---|---|---|
| `GET`  | `/api/health` | Health check |
| `GET`  | `/api/posts` | List all posts |
| `GET`  | `/api/posts/:id` | Fetch one post |
| `POST` | `/api/posts` | Create a post |

## What this sample proves

1. **Modular include** — `server.b` uses both direct (`include "./db.b";`) and namespaced (`include "./routes.b" as routes;`) forms.
2. **Sua HTTP** — `sua.server.get/post/static/listen` work end-to-end.
3. **Sua SQLite** — `sua.sqlite.connect/exec/query` persist posts to `blogsite.db`.
4. **Real app shape** — handlers are organized like Express (controller + routes + db layers).
