# The Tessera query language

Tessera's query language is a **deliberately small, frozen** subset of SQL over exactly one file. It
is stable API: people script against it in CI, so it must not grow dialects of convenience.

The header comment in [`src/query/parser.hpp`](../src/query/parser.hpp) states the rule. This
document is the contract.

## The shape of it

There is one table, and it is called **`t`**. A `FROM` clause is accepted — and its name ignored — so
that SQL pasted from elsewhere still runs, but there is nothing to join to.

```sql
SELECT <items>
[FROM t]
[WHERE <condition>]
[GROUP BY <expr> [, <expr>]...]
[ORDER BY <expr> [ASC|DESC] [, ...]]
[LIMIT <count> | LIMIT ALL] [OFFSET <count>]
```

A statement that does not begin with `SELECT` is read as a bare condition over all columns — the
form the TUI's filter bar uses:

```console
tessera query orders.csv "region = 'EU' AND revenue > 5000"
```

is exactly equivalent to

```console
tessera query orders.csv "SELECT * WHERE region = 'EU' AND revenue > 5000"
```

### What is intentionally absent

| Not supported | Why, and what to do instead |
|---|---|
| `JOIN`, `UNION` | One file is the entire mental model. Use DuckDB when you need two. |
| Subqueries | Compose in the shell: `tessera query a.csv "…" -f csv \| tessera query b.csv "…"`. |
| `HAVING` | Rejected with that explanation. Filter the aggregate in your pipeline. |
| `INSERT` / `UPDATE` / `DELETE` / `CREATE` | Tessera opens files read-only. It never writes to your data. |
| Window functions | Ask a database. This is a viewer with a query engine, not a warehouse. |
| Multiple statements | One statement per invocation. |

Attempting an unsupported clause produces a message saying so, rather than a confusing parse error.

## Selecting

```sql
SELECT *                                   -- every column, in file order
SELECT region, revenue                     -- a subset
SELECT region, sum(revenue) AS revenue     -- aliases; AS is optional
SELECT DISTINCT status                     -- de-duplicate the projected rows
SELECT upper(region) AS r, units * 2 AS u  -- expressions in the projection
SELECT count(*)                            -- aggregates without GROUP BY
```

With `GROUP BY`, a column that is neither grouped nor aggregated — including `*` — takes its
value from the **first row of its group**. In a table used as a lookup (`country` is functionally
dependent on `region`) that is exactly what you want; elsewhere, group or aggregate it explicitly.
Values are never random: the first row of a group is always the earliest row in file order.

**Column names** are the header row when the file has one. Files without a header get `column_1`,
`column_2`, … Names are matched case-insensitively, and a name that is a SQL keyword is fine here
because column resolution happens before keyword interpretation in expression position.

**Aliases** declared in `SELECT` are usable in `GROUP BY` and `ORDER BY`:

```sql
SELECT year(created_at) y, count(*) n FROM t GROUP BY y ORDER BY n DESC
```

`ORDER BY <n>` with an integer literal refers to the *n*th select item, as in standard SQL, so this
is also valid:

```sql
SELECT region, count(*) FROM t GROUP BY region ORDER BY 2 DESC
```

## Filtering

`WHERE` takes a boolean expression. Values are compared with their column's inferred type, so
`revenue > 5000` compares numbers, not strings.

### Comparison

| Operator | Meaning |
|---|---|
| `=` `<>` `!=` | equal, not equal |
| `<` `<=` `>` `>=` | ordering |
| `AND` `OR` `NOT` | boolean logic, `AND` binds tighter than `OR` |
| `LIKE` `NOT LIKE` | wildcard match: `%` = any run, `_` = one character, `\` escapes |
| `IN (…)` `NOT IN (…)` | membership in a literal list |
| `BETWEEN a AND b` `NOT BETWEEN` | inclusive range |
| `IS NULL` `IS NOT NULL` | empty or missing cell |

`LIKE` is case-insensitive for ASCII, which is what people mean when they filter a status column:

```sql
SELECT * WHERE notes LIKE '%renewal%'
SELECT * WHERE channel LIKE 'web%'
SELECT * WHERE order_id NOT LIKE 'ORD-0%'
```

`IN` and `BETWEEN` accept the usual lists and ranges:

```sql
SELECT * WHERE region IN ('EU', 'MEA', 'APAC')
SELECT * WHERE units BETWEEN 10 AND 20
SELECT * WHERE customer_id NOT IN ('C0', 'C1')
```

### Arithmetic and string operators

`+ - * / %` work on numbers; `||` concatenates. Integer arithmetic stays integer, `/` by zero yields
NULL rather than an error, and `%` is the remainder. Comparison against NULL is always false, as in
SQL — use `IS NULL`.

```sql
SELECT order_id, revenue * (1 - discount_pct / 100.0) AS net
  FROM t
 WHERE net > 1000
```

Unlike most SQL engines, Tessera resolves `SELECT` aliases in `WHERE` as well as in `GROUP BY` and
`ORDER BY`, so a long calculated column only has to be written once. An alias that is not defined in
the select list is reported as an unknown column, with the name echoed back.

## Aggregating

An aggregate in the select list switches the query into grouped mode. With no `GROUP BY`, there is
one output row for the whole file:

```sql
SELECT count(*), sum(revenue), avg(units), min(created_at), max(created_at) FROM t
```

| Aggregate | Notes |
|---|---|
| `count(*)` | number of rows |
| `count(expr)` | number of rows whose expression is not NULL |
| `count_distinct(expr)` | exact distinct count |
| `sum(expr)` | sum of the non-NULL values |
| `avg(expr)` / `mean(expr)` | arithmetic mean |
| `min(expr)` / `max(expr)` | typed minimum and maximum, works for text and timestamps too |

```sql
SELECT region, channel, count(*) orders, sum(revenue) revenue, avg(units) avg_units
  FROM t
 GROUP BY region, channel
 ORDER BY revenue DESC
 LIMIT 20
```

GROUP BY keys may be expressions, and an aggregate's result can be ordered by its alias or its
position.

**A bare column next to an aggregate requires `GROUP BY`.** `SELECT region, count(*) FROM t` names a
column that the aggregation collapses, so it is refused with a message telling you to add `GROUP BY`
or drop the column — the alternative, silently reporting the first row's region beside a whole-file
count, is a number that looks authoritative and means nothing. Constants and pure aggregate
expressions (`SELECT count(*), sum(revenue) / count(*) FROM t`) are fine without `GROUP BY`.

## Sorting and slicing

```sql
ORDER BY created_at                 -- ascending, the default
ORDER BY revenue DESC
ORDER BY region ASC, revenue DESC   -- multiple keys, evaluated left to right
ORDER BY 2 DESC                     -- by select-item position
LIMIT 100 OFFSET 200               -- paging
LIMIT ALL                           -- explicitly no limit
```

`ORDER BY` is a **stable** sort, so ties keep file order — which is usually what you want when you
sort an export by a low-cardinality column. `LIMIT` combines with `ORDER BY` to give top-N results
without materialising the full output.

## Functions

### Scalars — text

| Function | Result |
|---|---|
| `lower(s)` `upper(s)` | case conversion (ASCII) |
| `trim(s)` `ltrim(s)` `rtrim(s)` | strip surrounding whitespace |
| `len(s)` / `length(s)` | byte length |
| `substr(s, start[, len])` | 1-based substring, also spelled `substring` |
| `replace(s, from, to)` | replace all occurrences |
| `concat(a, b, …)` | concatenate 1–64 arguments |
| `startswith(s, prefix)` | boolean |
| `endswith(s, suffix)` | boolean |
| `contains(s, needle)` | boolean, case-insensitive |

### Scalars — numbers

| Function | Result |
|---|---|
| `abs(x)` | absolute value |
| `round(x[, places])` | round to *places* (default 0) |
| `floor(x)` `ceil(x)` / `ceiling(x)` | round down / up |

### Scalars — dates

`year(t)` `month(t)` `day(t)` `date(t)` operate on ISO-8601-ish timestamps and return an integer (or
the date part as text for `date`). They are cheap because timestamps are stored as their original
bytes plus a parsed epoch value.

```sql
SELECT year(created_at) y, count(*) FROM t GROUP BY y ORDER BY y
SELECT * WHERE month(created_at) = 12
```

### Conversion and null handling

| Function | Result |
|---|---|
| `int(x)` / `integer(x)` | parse as integer |
| `float(x)` / `real(x)` / `double(x)` | parse as double |
| `text(x)` / `str(x)` | stringify |
| `coalesce(a, b, …)` | first non-NULL argument |
| `ifnull(a, b)` | `b` when `a` is NULL |

```sql
SELECT coalesce(notes, '(none)') AS note FROM t
SELECT customer_id, sum(revenue) revenue FROM t GROUP BY customer_id ORDER BY revenue DESC LIMIT 10
```

## Types

Columns are inferred from a sample of records as one of `text`, `integer`, `float`, `boolean` or
`timestamp`. Functions and comparisons respect that type, but the engine is forgiving where a strict
type system would be annoying: a numeric column with a handful of bad cells still compares
numerically, and the bad cells read as NULL.

## Errors

Errors are meant to be fixed in seconds:

```console
$ tessera query sales.csv "SELECT regoin FROM t"
error query error: unknown column 'regoin'. Did you mean 'region'?
  SELECT regoin FROM t
                ^
```

```console
$ tessera query sales.csv "SELECT * WHERE revenue > "
error query error: unexpected end of statement in expression
  SELECT * WHERE revenue > 
                           ^
```

```console
$ tessera query sales.csv "SELECT region, count(*) FROM t"
error query error: SELECT mixes aggregates with bare columns but has no GROUP BY; add the columns to
GROUP BY, or remove them and keep the aggregates
  SELECT region, count(*) FROM t
                                ^
```

Every diagnostic echoes the statement and marks the offending position.


## Worked examples

```console
# Top ten customers by revenue, revenue per customer
tessera query orders10m.csv \
  "SELECT customer_id, count(*) orders, sum(revenue) revenue FROM t GROUP BY customer_id ORDER BY revenue DESC LIMIT 10"

# Monthly trend
tessera query events.jsonl \
  "SELECT year(created_at) y, month(created_at) m, count(*) n FROM t GROUP BY y, m ORDER BY y, m"

# Anything suspicious in a log file
tessera query app.log.jsonl \
  "SELECT * WHERE status = 'failed' OR latency_ms > 3000 ORDER BY latency_ms DESC LIMIT 50"

# Streamed, not materialised — 1M rows to disk as JSONL
tessera export orders10m.csv \
  -q "SELECT order_id, revenue FROM t WHERE units > 36" -f jsonl -o big_orders.jsonl

# Clean CSV on stdout for the next tool in the pipe
tessera query orders.csv "SELECT region, avg(revenue) FROM t GROUP BY region" | sort -t, -k2 -g
```

## Where to go from here

* [`ARCHITECTURE.md`](ARCHITECTURE.md) — how these queries are executed over the row index.
* [`DESIGN.md`](DESIGN.md) — why the language is deliberately small.
* [`BENCHMARKS.md`](BENCHMARKS.md) — what each of these costs on a ten-million-row file.
