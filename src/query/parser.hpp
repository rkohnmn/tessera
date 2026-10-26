// ─────────────────────────────────────────────────────────────────────────────
//  Tessera · parser.hpp
//  A small, deliberately boring SQL subset. No joins, no subqueries, no
//  surprises: SELECT list, WHERE, GROUP BY, ORDER BY, LIMIT/OFFSET over one
//  file. The grammar is documented in docs/QUERY.md and is stable API — people
//  will script against it, so it must not grow dialects of convenience.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "core/dataset.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tess::query {

enum class ValueKind { Null, Int, Real, Bool, Text, Timestamp };

struct Value {
    ValueKind kind = ValueKind::Null;
    std::int64_t i = 0;
    double d = 0.0;
    bool b = false;
    std::string s; ///< text, or the original timestamp spelling

    [[nodiscard]] static Value null() { return {}; }
    [[nodiscard]] static Value integer(std::int64_t v);
    [[nodiscard]] static Value real(double v);
    [[nodiscard]] static Value boolean(bool v);
    [[nodiscard]] static Value text(std::string v);
    [[nodiscard]] static Value timestamp(std::int64_t epoch, std::string original);

    [[nodiscard]] bool is_null() const noexcept { return kind == ValueKind::Null; }
    [[nodiscard]] bool is_numeric() const noexcept { return kind == ValueKind::Int || kind == ValueKind::Real; }
    [[nodiscard]] double as_double() const noexcept;
    [[nodiscard]] std::int64_t as_int() const noexcept;
    [[nodiscard]] bool as_bool() const noexcept;
    [[nodiscard]] std::string display() const;
};

enum class BinOp {
    Add, Sub, Mul, Div, Mod,
    Eq, Ne, Lt, Le, Gt, Ge,
    And, Or,
    Like, NotLike,
};

enum class UnOp { Neg, Not };

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

struct Expr {
    enum class Kind { Literal, Column, Unary, Binary, Func, IsNull, In, Between };

    Kind kind = Kind::Literal;
    Value literal;
    std::uint32_t column = 0;
    std::string name; ///< column name or function name
    BinOp op = BinOp::Eq;
    UnOp unop = UnOp::Not;
    bool flag = false; ///< NOT for IS NULL/IN/BETWEEN, star argument for count(*)
    std::vector<ExprPtr> args;
    /// Original source text, used for result headers and error messages.
    std::string source;

    static ExprPtr literal_value(Value v);
    static ExprPtr column_ref(std::uint32_t index, std::string name);
};

struct SelectItem {
    ExprPtr expr;
    std::string alias;
    bool star = false;
};

struct OrderKey {
    ExprPtr expr;
    bool descending = false;
};

struct Query {
    bool distinct = false;
    std::vector<SelectItem> items;
    ExprPtr where;
    std::vector<ExprPtr> group_by;
    std::vector<OrderKey> order_by;
    std::optional<std::uint64_t> limit;
    std::uint64_t offset = 0;
    bool has_aggregates = false;
    std::string source_text;
};

/// Parses a full statement. `dataset` resolves column names; pass the open
/// dataset so `WHERE region = 'EU'` can report a typo precisely.
[[nodiscard]] std::optional<Query> parse_query(std::string_view sql, const Dataset& dataset, std::string& error);

/// Parses a bare boolean expression (used by the TUI filter bar).
[[nodiscard]] ExprPtr parse_expression(std::string_view text, const Dataset& dataset, std::string& error);

/// True when `name` is a known aggregate function.
[[nodiscard]] bool is_aggregate(std::string_view name) noexcept;

/// Column names in a natural display order, for error suggestions.
[[nodiscard]] std::string suggest_column(std::string_view name, const Dataset& dataset);

} // namespace tess::query
