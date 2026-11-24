# std/csv

`std/csv` is the pure CSV grammar and encoding module. It imports only
`std/option`, `std/text`, and `std/vec`, so the same source runs on Native and
Python. Stream, pipe, and file adapters are in `std/csv_io` and reuse the
`Parser`/`Encoder` state defined here.

## Public API

```wz
pub const DELIMITER string = ",";
pub const DEFAULT_MAX_FIELD_BYTES usize = 1048576;
pub const DEFAULT_MAX_ROW_BYTES usize = 16777216;
pub const DEFAULT_MAX_ROW_FIELDS usize = 16384;

pub struct Limits {
    pub max_field_bytes usize;
    pub max_row_bytes usize;
    pub max_row_fields usize;
}

pub func default_limits() Limits;
pub func limits(max_field_bytes usize, max_row_bytes usize,
    max_row_fields usize) Limits;

pub func parse(value string) Vec[Vec[string]] throws Error;
pub func parse_limited(value string, limits Limits)
    Vec[Vec[string]] throws Error;
pub func parse_with(value string, delimiter string)
    Vec[Vec[string]] throws Error;
pub func parse_with_limited(value string, delimiter string, limits Limits)
    Vec[Vec[string]] throws Error;

pub func serialize(rows Vec[Vec[string]]) string;
pub func serialize_with(rows Vec[Vec[string]], delimiter string)
    string throws Error;

pub struct Parser;   // opaque byte-at-a-time parser state
pub struct Encoder;  // opaque validated delimiter

pub func parser() Parser;
pub func parser_limited(limits Limits) Parser;
pub func parser_with(delimiter string) Parser throws Error;
pub func parser_with_limited(delimiter string, limits Limits)
    Parser throws Error;
pub func parser_error(parser Parser) option.Option[Error];
pub func parser_feed(parser Parser, byte u8)
    option.Option[Vec[string]] throws Error;
pub func parser_finish(parser Parser)
    option.Option[Vec[string]] throws Error;

pub func encoder() Encoder;
pub func encoder_with(delimiter string) Encoder throws Error;
pub func encode_row(state Encoder, out Vec[u8], row Vec[string]);
```

`parser()` uses the comma dialect and default limits. `parser_limited` keeps
the comma dialect and takes explicit limits. `parser_with` takes one custom
ASCII delimiter and uses the default limits; `parser_with_limited` takes both.
This mirrors `parse`/`parse_limited`/`parse_with`/`parse_with_limited`.
`parser_feed` consumes one byte and returns a row when that byte completes a
record. `parser_finish` ends the input and returns the final unterminated
record, if any. Copies of a `Parser` are aliases: they share its position,
completed-row buffer, finished flag, and first error.

After a successful `parser_finish`, the parser is terminal and
`parser_finish` is idempotent: every later call returns `None`.
`parser_feed` after that success throws `FeedAfterEnd(offset)`, where `offset`
is the number of bytes already consumed; it does not change the finished
state, so `parser_finish` still returns `None` afterwards. `parser_error`
reports only a sticky Syntax or limit error, on every alias, and that error is
rethrown by `parser_feed` and `parser_finish` before any feed-after-end check.
A feed-after-end is caller misuse after completion, not a parse failure, so it
is not stored in `parser_error`.

`encoder()` uses the default comma delimiter. `encoder_with` validates one
ASCII delimiter: tab or printable ASCII, excluding the quote, CR, LF, and NUL,
matching the `*_with` functions. `encode_row` appends a complete encoded row,
including its LF, to an existing `Vec[u8]` owned by the caller.

`DELIMITER` is a string so default calls match the `*_with` forms. The raw
delimiter and quote bytes are private. `Error` is:

```wz
Syntax(offset, row, column)
FieldTooLarge(offset, row, column, limit)
RowTooLarge(offset, row, column, limit)
TooManyFields(offset, row, column, limit)
InvalidDelimiter(value)
FeedAfterEnd(offset)
```

Offsets are zero-based byte offsets into the original encoded input, including
raw bytes removed by doubled-quote decoding. Rows and columns are one-based.
All parser failures are terminal for a `Parser` and every copy of it. A
successful `parser_finish` is also terminal, but not a failure: repeated
`parser_finish` calls return `None`, and `parser_feed` throws
`FeedAfterEnd` without overwriting the finished state.

## Limits

A limit is a maximum, not a special "unlimited" value. Exactly the limit is
accepted; the next byte/field is rejected. Zero forbids that item, except that
a blank line still has zero fields.

- `max_field_bytes` counts decoded field content.
- `max_row_bytes` counts original encoded row bytes, including delimiters and
  quotes but excluding the LF/CRLF/CR record terminator.
- `max_row_fields` counts fields in one row.

The parser checks these limits before growing field bytes, the lazy raw-offset
map, the row field vector, or the row-byte count. This bounds an unterminated
quote or a hostile many-column row instead of relying on allocation Panic.

## Serialization

`serialize` is infallible. Every byte comes from a valid `string` or ASCII
punctuation, so it encodes once into `Vec[u8]` and validates once at the end.
That conversion cannot fail; if the internal invariant is ever broken, the
module forces a Panic instead of swallowing an error or returning invalid
UTF-8. It does not build the result with repeated string concatenation.
`serialize_with` validates the delimiter and can throw `InvalidDelimiter`.

## Historical cases

`tests/stdlib_csv` retains the original layout, quote, text-only, malformed,
custom-delimiter and round-trip checks (codes 10-83) and adds field, row, and
column limit checks at, below, and above each limit (codes 160-178). Codes
180-215 cover the exact exported defaults and `limits` fields, incremental
`parser()`/`parser_limited`/`parser_with`/`parser_with_limited` behavior,
aliases sharing position and errors, repeated EOF, feed-after-end, sticky
errors, and `encoder()`/`encoder_with`/`encode_row`. The Native stream suite in
`tests/stdlib_csv_io` retains the original terminal Syntax, invalid UTF-8
raw-offset, pipe reader/writer, fixed 5000-byte buffered file, I/O error,
partial write, PendingOutput, and batch first/middle `accepted_rows` checks. It
adds repeated EOF on reader aliases (225-232) and Linux `/dev/full` checks: the
file writer reports `Error.Io` for both a small buffered row and a row larger
than the 4096-byte writer buffer (233-234), while the public resumable Writer
still reports `BatchIo` and then `PendingOutput` for the same large row
(235-236).
