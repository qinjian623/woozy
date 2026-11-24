# std/csv_io

`std/csv_io` is the Native stream and file adapter for the pure `std/csv`
module. Parser state and row encoding have exactly one implementation in
`std/csv`; this module adds `std/io`/`std/os` transport and retained writer
suffixes. Python programs that only parse or serialize strings should import
`std/csv` directly.

## Public API (Native)

```wz
pub enum Error {
    Csv(value csv.Error);
    Io(value os.Error);
    PendingOutput;
    BatchIo(cause os.Error, accepted_rows usize);
}

pub func reader(input io.Reader) Reader;
pub func reader_limited(input io.Reader, limits csv.Limits) Reader;
pub func reader_with(input io.Reader, delimiter string) Reader throws Error;
pub func reader_with_limited(input io.Reader, delimiter string,
    limits csv.Limits) Reader throws Error;
pub func read_row(input Reader) option.Option[Vec[string]] throws Error;
pub func read_all(input Reader) Vec[Vec[string]] throws Error;

pub func writer(output io.Writer) Writer;
pub func writer_with(output io.Writer, delimiter string) Writer throws Error;
pub func write_row(output Writer, row Vec[string]) throws Error;
pub func write_all(output Writer, rows Vec[Vec[string]]) throws Error;
pub func flush(output Writer) throws Error;

pub func read_file(path string) Vec[Vec[string]] throws Error;
pub func read_file_limited(path string, limits csv.Limits)
    Vec[Vec[string]] throws Error;
pub func read_file_with(path string, delimiter string)
    Vec[Vec[string]] throws Error;
pub func read_file_with_limited(path string, delimiter string,
    limits csv.Limits) Vec[Vec[string]] throws Error;
pub func write_file(path string, rows Vec[Vec[string]]) throws Error;
pub func write_file_with(path string, rows Vec[Vec[string]], delimiter string)
    throws Error;
```

`Error.Csv` preserves the typed terminal parser error without duplicating the
grammar errors. Invalid UTF-8 therefore arrives as
`Error.Csv(csv.Error.Syntax(offset, row, column))`, with `offset` at the first
offending raw byte inside the field. I/O failures stay `Error.Io`; a batch
failure on an accepted row is
`Error.BatchIo(cause, accepted_rows)`. A Syntax or limit error is terminal for
a Reader and all aliases. Once `parser_finish` succeeds, every later
`read_row` on any alias returns `None` and `read_all` returns no rows without
reading the underlying input again; the Reader keeps a private shared EOF flag
that is set even when `parser_finish` returns the final unterminated row. A
closed fd or bytes appended after EOF are therefore not observed. I/O errors
remain resumable through `read_row`.

`read_all` is not resumable across an I/O error: it throws and discards rows it
had already collected. Nonblocking callers that can see `WouldBlock` should
loop on `read_row` and retain completed rows themselves.

A Writer accepts a complete encoded row before attempting I/O. It submits the
encoded bytes through `io.write_byte`, whose contract is: append one byte, but
flush first when the 4096-byte buffer is full; if that flush throws, the byte
is not appended and `pending` still holds exactly the unwritten suffix, so a
retry after `flush` neither duplicates nor drops the byte. `Error.Io` means
that row was accepted and its unwritten suffix must be finished with `flush`,
never retried. A later `write_row`/`write_all` with a pending suffix returns
`PendingOutput` before accepting another row. `write_all` counts the retained
row in `accepted_rows`; after `flush`, resume with `rows[accepted_rows:]`.

File writers validate the delimiter before opening with `O_TRUNC`, so an
invalid delimiter cannot destroy an existing file. Row encoding itself is
infallible. `write_file`/`write_file_with` open with
`O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC` and mode `0o666` (masked by the
umask). They own and close the internal Writer and file on every exit path, so
they have no retained suffix and no batch position to resume: an I/O failure is
always `Error.Io`, and a `BatchIo` from `write_all` is mapped to
`Error.Io(cause)`. A failure after the open may leave an existing file
truncated, or the new file empty or partially written. Callers that need to
resume after a failure must use `writer`/`write_all`/`flush` directly.
`reader_with` uses the default limits and `reader_with_limited` takes explicit
limits. File helpers use the same defaults and limits as the stream readers.

## Test coverage

The `tests/stdlib_csv_io/linux_failure` fixture drives the file writer against
Linux `/dev/full`. A small row
that fits in the writer buffer fails in the final flush, and a row larger than
4096 bytes fails while `write_all` is still submitting it. Both must surface
`Error.Io` with `ENOSPC` (28), proving the file writer does not leak
`BatchIo`. A separate check opens `/dev/full` through the public Writer and
confirms the large row reports `BatchIo` followed by `PendingOutput`, while the
pipe-based partial-write and recovery checks continue to cover resumable
Writer behavior. The Native suite also returns a final unterminated record,
then closes the underlying fd or appends to the file, and checks that an alias
still returns `None` without consuming the appended bytes.
