# std/json_io

Native-only convenience layer over `std/json` and `std/fs` for Linux x86_64
and macOS arm64. Pure `std/json` APIs do not import this package or any other
Native-only module.

| API | Behaviour |
| --- | --- |
| `read_file(name string) json.Value throws Error` | Reads and UTF-8-decodes the whole file with `std/fs`, then parses it with `std/json`. |
| `write_file(name string, value json.Value, mode u32) throws Error` | Writes compact JSON. |
| `write_file_pretty(name string, value json.Value, mode u32) throws Error` | Writes JSON with fixed two-space indentation and no trailing newline. |

`Error` has exactly two variants:

```wz
pub enum Error {
    Io(value os.Error);
    Json(value json.Error);
}
```

`read_file` maps file open/read/UTF-8 decode failures to `Error.Io` and
syntactically malformed JSON text (valid UTF-8) to `Error.Json`. `write_file`
and `write_file_pretty` map file creation/write failures to `Error.Io` and
encoding failures to `Error.Json`.

## Ordering and limits

Object member order is preserved exactly as stored in `json.Value.Object`.
Numbers keep their original decimal lexeme. The parser's duplicate-key policy
(the last value wins, at the first key position) applies before writing.
Nested containers are limited to 256 levels; exceeding that in a parsed or
manually constructed value reports `json.Error.DepthLimit` (wrapped as
`Error.Json`).

## Ownership and failure ordering

`read_file` returns a newly parsed `json.Value`; it retains no file handle and
does not borrow the file bytes. `write_file` and `write_file_pretty` borrow the
value for the call and do not consume it. The value remains valid after the
call.

Writing first finishes encoding in memory, then calls `std/fs.write_text`.
Therefore an invalid `json.Value.Number` lexeme or a depth-limit failure
returns `Error.Json` before the destination is opened, and an existing output
file is not truncated. A filesystem failure after a successful encode can
still leave the destination truncated or partially written; these helpers do
not implement atomic replacement, `fsync`, file locking, or concurrent-writer
coordination. The `mode` argument has the same creation semantics as
`std/fs.write_text` and does not change permissions of an existing file.
