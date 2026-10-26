// ─────────────────────────────────────────────────────────────────────────────
//  Tessera · exec.cpp
// ─────────────────────────────────────────────────────────────────────────────
#include "query/exec.hpp"

#include "common/util.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <map>
#include <unordered_map>
#include <unordered_set>

namespace tess::query {
namespace {

// ── Comparisons ─────────────────────────────────────────────────────────────
int sign_of(int value) noexcept { return (value > 0) - (value < 0); }

int compare_strings(std::string_view a, std::string_view b) noexcept {
    const int cmp = a.compare(b);
    return sign_of(cmp);
}

bool apply_comparison(BinOp op, int cmp) noexcept {
    switch (op) {
        case BinOp::Eq: return cmp == 0;
        case BinOp::Ne: return cmp != 0;
        case BinOp::Lt: return cmp < 0;
        case BinOp::Le: return cmp <= 0;
        case BinOp::Gt: return cmp > 0;
        case BinOp::Ge: return cmp >= 0;
        default: return false;
    }
}

BinOp flipped(BinOp op) noexcept {
    switch (op) {
        case BinOp::Lt: return BinOp::Gt;
        case BinOp::Le: return BinOp::Ge;
        case BinOp::Gt: return BinOp::Lt;
        case BinOp::Ge: return BinOp::Le;
        default: return op;
    }
}

bool is_comparison(BinOp op) noexcept {
    switch (op) {
        case BinOp::Eq:
        case BinOp::Ne:
        case BinOp::Lt:
        case BinOp::Le:
        case BinOp::Gt:
        case BinOp::Ge: return true;
        default: return false;
    }
}

// ── Aggregates ──────────────────────────────────────────────────────────────
struct AggState {
    std::uint64_t count = 0;
    double sum = 0.0;
    bool has_value = false;
    Value best;
    std::unordered_set<std::string> distinct;
};

struct AggContext {
    const std::vector<const Expr*>* exprs = nullptr;
    const std::vector<AggState>* states = nullptr;

    [[nodiscard]] const AggState* find(const Expr& e) const {
        if (exprs == nullptr || states == nullptr) return nullptr;
        for (std::size_t i = 0; i < exprs->size(); ++i) {
            if ((*exprs)[i] == &e) return &(*states)[i];
        }
        return nullptr;
    }
};

void collect_aggregates(const Expr& e, std::vector<const Expr*>& out) {
    if (e.kind == Expr::Kind::Func && is_aggregate(e.name)) {
        if (std::find(out.begin(), out.end(), &e) == out.end()) out.push_back(&e);
        return;
    }
    for (const auto& arg : e.args) collect_aggregates(*arg, out);
}

void update_aggregate(AggState& state, const Expr& node, const Value& value) {
    const std::string name = to_lower(node.name);
    if (name == "count") {
        if (node.flag || !value.is_null()) ++state.count;
        return;
    }
    if (value.is_null()) return;
    if (name == "count_distinct") {
        state.distinct.insert(value.display());
        return;
    }
    if (name == "sum" || name == "avg" || name == "mean") {
        state.sum += value.as_double();
        ++state.count;
        return;
    }
    if (name == "min" || name == "max") {
        if (!state.has_value) {
            state.best = value;
            state.has_value = true;
        } else if (compare_values(value, state.best) != 0) {
            const int cmp = compare_values(value, state.best);
            if ((name == "min" && cmp < 0) || (name == "max" && cmp > 0)) state.best = value;
        }
    }
}

Value aggregate_result(const AggState& state, const Expr& node) {
    const std::string name = to_lower(node.name);
    if (name == "count") return Value::integer(static_cast<std::int64_t>(state.count));
    if (name == "count_distinct") return Value::integer(static_cast<std::int64_t>(state.distinct.size()));
    if (name == "sum") {
        if (state.count == 0) return Value::null();
        return Value::real(state.sum);
    }
    if (name == "avg" || name == "mean") {
        if (state.count == 0) return Value::null();
        return Value::real(state.sum / static_cast<double>(state.count));
    }
    if (name == "min" || name == "max") {
        if (!state.has_value) return Value::null();
        return state.best;
    }
    return Value::null();
}

// ── Value construction from cells ───────────────────────────────────────────
Value value_at(const Dataset& dataset, std::uint64_t row, std::uint32_t column) {
    const std::string_view text = dataset.cell(row, column);
    if (text.empty()) return Value::null();
    switch (dataset.column_type(column)) {
        case ColumnType::Integer: {
            std::int64_t v = 0;
            if (parse_int(text, v)) return Value::integer(v);
            break;
        }
        case ColumnType::Float: {
            double d = 0.0;
            if (parse_double(text, d)) return Value::real(d);
            break;
        }
        case ColumnType::Boolean: {
            bool b = false;
            if (parse_bool(text, b)) return Value::boolean(b);
            break;
        }
        case ColumnType::Timestamp: {
            if (const auto t = parse_timestamp(text)) return Value::timestamp(*t, std::string(text));
            break;
        }
        default: break;
    }
    return Value::text(std::string(text));
}

/// Numeric interpretation of a cell, used for the column-vs-literal fast path.
bool numeric_cell(std::string_view text, double& out) {
    const std::string_view trimmed = trim(text);
    if (trimmed.empty()) return false;
    std::int64_t i = 0;
    if (parse_int(trimmed, i)) {
        out = static_cast<double>(i);
        return true;
    }
    return parse_double(trimmed, out);
}

// ── LIKE ────────────────────────────────────────────────────────────────────
char lower_ascii(char c) noexcept { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }

// ── Evaluator ───────────────────────────────────────────────────────────────
class Eval {
public:
    explicit Eval(const Dataset& dataset) : dataset_(dataset) {}

    [[nodiscard]] Value eval(const Expr& e, std::uint64_t row, const AggContext* agg) const {
        switch (e.kind) {
            case Expr::Kind::Literal: return e.literal;
            case Expr::Kind::Column: return value_at(dataset_, row, e.column);
            case Expr::Kind::Unary: {
                const Value operand = eval(*e.args[0], row, agg);
                if (e.unop == UnOp::Not) return Value::boolean(!operand.as_bool());
                if (operand.is_numeric()) {
                    return operand.kind == ValueKind::Int ? Value::integer(-operand.i) : Value::real(-operand.d);
                }
                return Value::null();
            }
            case Expr::Kind::IsNull: {
                const bool is_null = eval(*e.args[0], row, agg).is_null();
                return Value::boolean(e.flag ? !is_null : is_null);
            }
            case Expr::Kind::In: {
                const Value lhs = eval(*e.args[0], row, agg);
                bool found = false;
                for (std::size_t i = 1; i < e.args.size() && !found; ++i) {
                    const Value item = eval(*e.args[i], row, agg);
                    if (lhs.is_null() || item.is_null()) continue;
                    found = compare_values(lhs, item) == 0;
                }
                return Value::boolean(e.flag ? !found : found);
            }
            case Expr::Kind::Between: {
                const Value v = eval(*e.args[0], row, agg);
                const Value low = eval(*e.args[1], row, agg);
                const Value high = eval(*e.args[2], row, agg);
                if (v.is_null() || low.is_null() || high.is_null()) return Value::boolean(false);
                const bool inside = compare_values(v, low) >= 0 && compare_values(v, high) <= 0;
                return Value::boolean(e.flag ? !inside : inside);
            }
            case Expr::Kind::Binary: return eval_binary(e, row, agg);
            case Expr::Kind::Func: return eval_function(e, row, agg);
        }
        return Value::null();
    }

    [[nodiscard]] bool truth(const Expr& e, std::uint64_t row, const AggContext* agg) const {
        // Short-circuit boolean trees before touching cells.
        if (e.kind == Expr::Kind::Binary) {
            if (e.op == BinOp::And) return truth(*e.args[0], row, agg) && truth(*e.args[1], row, agg);
            if (e.op == BinOp::Or) return truth(*e.args[0], row, agg) || truth(*e.args[1], row, agg);
            if (is_comparison(e.op)) {
                // Hot path: column vs literal, no Value construction.
                if (e.args[0]->kind == Expr::Kind::Column && e.args[1]->kind == Expr::Kind::Literal) {
                    return compare_cell_literal(e.args[0]->column, e.op, e.args[1]->literal, row);
                }
                if (e.args[1]->kind == Expr::Kind::Column && e.args[0]->kind == Expr::Kind::Literal) {
                    return compare_cell_literal(e.args[1]->column, flipped(e.op), e.args[0]->literal, row);
                }
            }
            if (e.op == BinOp::Like || e.op == BinOp::NotLike) {
                if (e.args[0]->kind == Expr::Kind::Column && e.args[1]->kind == Expr::Kind::Literal &&
                    e.args[1]->literal.kind == ValueKind::Text) {
                    const std::string_view text = dataset_.cell(row, e.args[0]->column);
                    const bool matched = like_match(text, e.args[1]->literal.s);
                    return e.op == BinOp::Like ? matched : !matched;
                }
            }
        }
        if (e.kind == Expr::Kind::Unary && e.unop == UnOp::Not) {
            return !truth(*e.args[0], row, agg);
        }
        if (e.kind == Expr::Kind::IsNull && e.args[0]->kind == Expr::Kind::Column) {
            const bool is_null = dataset_.cell(row, e.args[0]->column).empty();
            return e.flag ? !is_null : is_null;
        }
        const Value value = eval(e, row, agg);
        if (value.is_null()) return false;
        if (value.kind == ValueKind::Bool) return value.b;
        if (value.kind == ValueKind::Int) return value.i != 0;
        if (value.kind == ValueKind::Real) return value.d != 0.0;
        if (value.kind == ValueKind::Timestamp) return true;
        return value.as_bool();
    }

private:
    [[nodiscard]] Value eval_binary(const Expr& e, std::uint64_t row, const AggContext* agg) const {
        if (e.op == BinOp::And) {
            return Value::boolean(truth(*e.args[0], row, agg) && truth(*e.args[1], row, agg));
        }
        if (e.op == BinOp::Or) {
            return Value::boolean(truth(*e.args[0], row, agg) || truth(*e.args[1], row, agg));
        }
        if (e.op == BinOp::Like || e.op == BinOp::NotLike) {
            const Value text = eval(*e.args[0], row, agg);
            const Value pattern = eval(*e.args[1], row, agg);
            if (text.is_null() || pattern.is_null()) return Value::boolean(false);
            const bool matched = like_match(text.display(), pattern.display());
            return Value::boolean(e.op == BinOp::Like ? matched : !matched);
        }
        if (is_comparison(e.op)) {
            const Value a = eval(*e.args[0], row, agg);
            const Value b = eval(*e.args[1], row, agg);
            if (a.is_null() || b.is_null()) return Value::boolean(false);
            return Value::boolean(apply_comparison(e.op, compare_values(a, b)));
        }

        // Arithmetic.
        const Value a = eval(*e.args[0], row, agg);
        const Value b = eval(*e.args[1], row, agg);
        if (a.is_null() || b.is_null()) return Value::null();
        const bool both_int = a.kind == ValueKind::Int && b.kind == ValueKind::Int;
        switch (e.op) {
            case BinOp::Add:
                return both_int ? Value::integer(a.i + b.i) : Value::real(a.as_double() + b.as_double());
            case BinOp::Sub:
                return both_int ? Value::integer(a.i - b.i) : Value::real(a.as_double() - b.as_double());
            case BinOp::Mul:
                return both_int ? Value::integer(a.i * b.i) : Value::real(a.as_double() * b.as_double());
            case BinOp::Div:
                if (b.as_double() == 0.0) return Value::null();
                if (both_int && a.i % b.i == 0) return Value::integer(a.i / b.i);
                return Value::real(a.as_double() / b.as_double());
            case BinOp::Mod:
                if (b.as_double() == 0.0) return Value::null();
                if (both_int) return Value::integer(a.i % b.i);
                return Value::real(std::fmod(a.as_double(), b.as_double()));
            default: return Value::null();
        }
    }

    [[nodiscard]] bool compare_cell_literal(std::uint32_t column, BinOp op, const Value& literal,
                                            std::uint64_t row) const {
        if (literal.kind == ValueKind::Null) return false;
        const std::string_view text = dataset_.cell(row, column);
        const ColumnType type = dataset_.column_type(column);

        // Numeric literals compare numerically when the cell is a number.
        if (literal.is_numeric()) {
            double cell_value = 0.0;
            if (numeric_cell(text, cell_value)) {
                const double target = literal.kind == ValueKind::Int ? static_cast<double>(literal.i) : literal.d;
                int cmp = 0;
                if (cell_value < target) cmp = -1;
                else if (cell_value > target) cmp = 1;
                return apply_comparison(op, cmp);
            }
            if (type != ColumnType::Text && type != ColumnType::Unknown) return false;
        }

        // Timestamp literals compare chronologically.
        if (literal.kind == ValueKind::Text && type == ColumnType::Timestamp) {
            const auto left = parse_timestamp(text);
            const auto right = parse_timestamp(literal.s);
            if (left.has_value() && right.has_value()) {
                const int cmp = (*left < *right) ? -1 : (*left > *right ? 1 : 0);
                return apply_comparison(op, cmp);
            }
        }

        const std::string target_storage = literal.display();
        const std::string_view target = target_storage;
        if (text.empty() && !target.empty()) {
            // Empty cell is NULL: only != and comparisons that exclude it match.
            return op == BinOp::Ne;
        }
        return apply_comparison(op, compare_strings(text, target));
    }

    [[nodiscard]] Value eval_function(const Expr& e, std::uint64_t row, const AggContext* agg) const {
        if (const AggState* state = agg != nullptr ? agg->find(e) : nullptr) {
            return aggregate_result(*state, e);
        }
        const std::string name = to_lower(e.name);
        const auto arg = [&](std::size_t i) { return eval(*e.args[i], row, agg); };

        if (name == "coalesce" || name == "ifnull") {
            for (std::size_t i = 0; i < e.args.size(); ++i) {
                Value v = arg(i);
                if (!v.is_null()) return v;
            }
            return Value::null();
        }
        if (name == "concat") {
            std::string out;
            for (std::size_t i = 0; i < e.args.size(); ++i) {
                const Value v = arg(i);
                if (v.is_null()) continue;
                out += v.display();
            }
            return Value::text(std::move(out));
        }
        if (name == "lower" || name == "upper" || name == "trim" || name == "ltrim" || name == "rtrim") {
            const Value v = arg(0);
            if (v.is_null()) return Value::null();
            std::string text = v.display();
            if (name == "lower") {
                for (char& c : text) c = lower_ascii(c);
            } else if (name == "upper") {
                for (char& c : text) {
                    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
                }
            } else if (name == "trim") {
                text = std::string(trim(text));
            } else if (name == "ltrim") {
                text = std::string(lstrip(text));
            } else {
                text = std::string(rstrip(text));
            }
            return Value::text(std::move(text));
        }
        if (name == "len" || name == "length") {
            const Value v = arg(0);
            if (v.is_null()) return Value::null();
            return Value::integer(static_cast<std::int64_t>(rune_count(v.display())));
        }
        if (name == "substr" || name == "substring") {
            const Value v = arg(0);
            if (v.is_null()) return Value::null();
            const std::string text = v.display();
            const std::int64_t start = arg(1).as_int();
            std::int64_t length = std::numeric_limits<std::int64_t>::max();
            if (e.args.size() >= 3) length = arg(2).as_int();
            const std::int64_t from = std::max<std::int64_t>(1, start) - 1;
            std::size_t byte_start = text.size();
            std::size_t byte_end = text.size();
            std::int64_t index = 0;
            for (std::size_t i = 0; i < text.size();) {
                const Rune r = decode_rune(text, i);
                if (index == from) byte_start = i;
                if (index == from + length) {
                    byte_end = i;
                    break;
                }
                ++index;
                i += static_cast<std::size_t>(r.length);
            }
            if (byte_start == text.size()) return Value::text(std::string());
            return Value::text(text.substr(byte_start, byte_end - byte_start));
        }
        if (name == "replace") {
            const Value v = arg(0);
            if (v.is_null()) return Value::null();
            std::string text = v.display();
            const std::string from = arg(1).display();
            const std::string to = arg(2).display();
            if (!from.empty()) {
                std::size_t pos = 0;
                while ((pos = text.find(from, pos)) != std::string::npos) {
                    text.replace(pos, from.size(), to);
                    pos += to.size();
                }
            }
            return Value::text(std::move(text));
        }
        if (name == "abs") {
            const Value v = arg(0);
            if (!v.is_numeric()) return Value::null();
            return v.kind == ValueKind::Int ? Value::integer(std::llabs(v.i)) : Value::real(std::fabs(v.d));
        }
        if (name == "round" || name == "floor" || name == "ceil" || name == "ceiling") {
            const Value v = arg(0);
            if (!v.is_numeric()) return Value::null();
            double value = v.as_double();
            if (name == "floor") return Value::real(std::floor(value));
            if (name == "ceil" || name == "ceiling") return Value::real(std::ceil(value));
            int digits = 0;
            if (e.args.size() >= 2) digits = static_cast<int>(arg(1).as_int());
            const double scale = std::pow(10.0, digits);
            value = std::round(value * scale) / scale;
            if (digits <= 0 && std::fabs(value) < 9.0e15) {
                return Value::integer(static_cast<std::int64_t>(value));
            }
            return Value::real(value);
        }
        if (name == "int" || name == "integer") return Value::integer(arg(0).as_int());
        if (name == "float" || name == "real" || name == "double") return Value::real(arg(0).as_double());
        if (name == "text" || name == "str") return Value::text(arg(0).display());
        if (name == "startswith" || name == "endswith" || name == "contains") {
            const std::string haystack = to_lower(arg(0).display());
            const std::string needle = to_lower(arg(1).display());
            if (name == "startswith") return Value::boolean(haystack.rfind(needle, 0) == 0);
            if (name == "endswith") {
                return Value::boolean(haystack.size() >= needle.size() &&
                                      haystack.compare(haystack.size() - needle.size(), needle.size(), needle) == 0);
            }
            return Value::boolean(haystack.find(needle) != std::string::npos);
        }
        if (name == "year" || name == "month" || name == "day" || name == "date") {
            const Value v = arg(0);
            const auto stamp = parse_timestamp(v.display());
            if (!stamp.has_value()) return Value::null();
            if (name == "date") return Value::timestamp(*stamp, format_timestamp(*stamp, true));
            const std::string formatted = format_timestamp(*stamp);
            const std::size_t part = (name == "year") ? 0 : (name == "month" ? 5 : 8);
            const std::size_t width = (name == "year") ? 4 : 2;
            std::int64_t numeric = 0;
            if (!parse_int(std::string_view(formatted).substr(part, width), numeric)) return Value::null();
            return Value::integer(numeric);
        }
        return Value::null();
    }

    const Dataset& dataset_;
};

// ── Output description ──────────────────────────────────────────────────────
struct Output {
    const Expr* expr = nullptr;
    std::uint32_t star_column = 0;
    bool star = false;
    std::string header;
    ColumnType type = ColumnType::Text;
};

std::vector<Output> build_outputs(const Dataset& dataset, const Query& query) {
    std::vector<Output> outputs;
    for (const auto& item : query.items) {
        if (item.star) {
            for (std::uint32_t c = 0; c < dataset.column_count(); ++c) {
                Output out;
                out.star = true;
                out.star_column = c;
                out.header = std::string(dataset.column_name(c));
                out.type = dataset.column_type(c);
                outputs.push_back(std::move(out));
            }
            continue;
        }
        Output out;
        out.expr = item.expr.get();
        out.header = item.alias.empty() ? item.expr->source : item.alias;
        out.type = infer_output_type(*item.expr, dataset);
        outputs.push_back(std::move(out));
    }
    return outputs;
}

std::string join_row(const std::vector<std::string>& cells) {
    std::string out;
    for (std::size_t i = 0; i < cells.size(); ++i) {
        if (i) out.push_back('\x1f');
        out.append(cells[i]);
    }
    return out;
}

} // namespace

int compare_values(const Value& a, const Value& b) noexcept {
    if (a.is_null() && b.is_null()) return 0;
    if (a.is_null()) return 1;  // nulls sort last
    if (b.is_null()) return -1;

    if (a.is_numeric() && b.is_numeric()) {
        if (a.kind == ValueKind::Int && b.kind == ValueKind::Int) return (a.i > b.i) - (a.i < b.i);
        const double left = a.as_double();
        const double right = b.as_double();
        return (left > right) - (left < right);
    }
    if (a.kind == ValueKind::Timestamp && b.kind == ValueKind::Timestamp) {
        return (a.i > b.i) - (a.i < b.i);
    }
    if (a.kind == ValueKind::Bool && b.kind == ValueKind::Bool) {
        return (static_cast<int>(a.b) > static_cast<int>(b.b)) - (static_cast<int>(a.b) < static_cast<int>(b.b));
    }
    const std::string left = a.display();
    const std::string right = b.display();
    return compare_strings(left, right);
}

bool like_match(std::string_view text, std::string_view pattern) noexcept {
    // Iterative wildcard matcher with single-star backtracking: no recursion,
    // no allocation, linear in the common case.
    std::size_t ti = 0, pi = 0;
    std::size_t star_pi = std::string_view::npos;
    std::size_t star_ti = 0;
    const auto equals = [](char a, char b) { return lower_ascii(a) == lower_ascii(b); };

    while (ti < text.size()) {
        if (pi < pattern.size() && pattern[pi] == '\\' && pi + 1 < pattern.size()) {
            if (equals(text[ti], pattern[pi + 1])) {
                ti += 1;
                pi += 2;
                continue;
            }
        } else if (pi < pattern.size() && pattern[pi] == '%') {
            star_pi = pi++;
            star_ti = ti;
            continue;
        } else if (pi < pattern.size() && (pattern[pi] == '_' || equals(text[ti], pattern[pi]))) {
            ++ti;
            ++pi;
            continue;
        }
        if (star_pi != std::string_view::npos) {
            pi = star_pi + 1;
            ti = ++star_ti;
            continue;
        }
        return false;
    }
    while (pi < pattern.size() && pattern[pi] == '%') ++pi;
    return pi == pattern.size();
}

ColumnType infer_output_type(const Expr& expr, const Dataset& dataset) {
    switch (expr.kind) {
        case Expr::Kind::Column: return dataset.column_type(expr.column);
        case Expr::Kind::Literal: {
            switch (expr.literal.kind) {
                case ValueKind::Int: return ColumnType::Integer;
                case ValueKind::Real: return ColumnType::Float;
                case ValueKind::Bool: return ColumnType::Boolean;
                case ValueKind::Timestamp: return ColumnType::Timestamp;
                default: return ColumnType::Text;
            }
        }
        case Expr::Kind::IsNull:
        case Expr::Kind::In:
        case Expr::Kind::Between: return ColumnType::Boolean;
        case Expr::Kind::Unary:
            return expr.unop == UnOp::Not ? ColumnType::Boolean : infer_output_type(*expr.args[0], dataset);
        case Expr::Kind::Binary: {
            switch (expr.op) {
                case BinOp::And:
                case BinOp::Or:
                case BinOp::Eq:
                case BinOp::Ne:
                case BinOp::Lt:
                case BinOp::Le:
                case BinOp::Gt:
                case BinOp::Ge:
                case BinOp::Like:
                case BinOp::NotLike: return ColumnType::Boolean;
                default: {
                    const ColumnType a = infer_output_type(*expr.args[0], dataset);
                    const ColumnType b = infer_output_type(*expr.args[1], dataset);
                    if (a == ColumnType::Float || b == ColumnType::Float) return ColumnType::Float;
                    if (a == ColumnType::Integer && b == ColumnType::Integer) return ColumnType::Integer;
                    return ColumnType::Float;
                }
            }
        }
        case Expr::Kind::Func: {
            const std::string name = to_lower(expr.name);
            if (name == "count" || name == "count_distinct" || name == "len" || name == "length" ||
                name == "year" || name == "month" || name == "day" || name == "int" || name == "integer") {
                return ColumnType::Integer;
            }
            if (name == "sum" || name == "avg" || name == "mean" || name == "round" || name == "abs" ||
                name == "floor" || name == "ceil" || name == "ceiling" || name == "float" || name == "real") {
                return ColumnType::Float;
            }
            if (name == "startswith" || name == "endswith" || name == "contains") return ColumnType::Boolean;
            if ((name == "min" || name == "max") && !expr.args.empty()) {
                return infer_output_type(*expr.args[0], dataset);
            }
            return ColumnType::Text;
        }
    }
    return ColumnType::Text;
}

// ── Filtering ───────────────────────────────────────────────────────────────
std::vector<std::uint64_t> all_rows(const Dataset& dataset) {
    std::vector<std::uint64_t> rows;
    rows.resize(static_cast<std::size_t>(dataset.row_count()));
    for (std::uint64_t i = 0; i < dataset.row_count(); ++i) rows[static_cast<std::size_t>(i)] = i;
    return rows;
}

std::vector<std::uint64_t> filter_rows(const Dataset& dataset, const Expr& predicate,
                                       const RunOptions& options, double* seconds) {
    const double started = now_seconds();
    const Eval eval(dataset);
    const std::uint64_t total = dataset.row_count();
    std::vector<std::uint64_t> rows;
    rows.reserve(1024);

    constexpr std::uint64_t kProgressEvery = 262144;
    for (std::uint64_t row = 0; row < total; ++row) {
        if ((row & (kProgressEvery - 1)) == 0) {
            if (options.cancel != nullptr) options.cancel->throw_if_canceled();
            if (options.progress && total > 0) {
                options.progress(static_cast<double>(row) / static_cast<double>(total), "filtering");
            }
        }
        if (eval.truth(predicate, row, nullptr)) rows.push_back(row);
    }
    if (options.cancel != nullptr) options.cancel->throw_if_canceled();
    if (options.progress) options.progress(1.0, "filtering");
    if (seconds != nullptr) *seconds = now_seconds() - started;
    return rows;
}

std::vector<std::uint64_t> search_rows(const Dataset& dataset, std::string_view needle,
                                       const std::vector<std::uint32_t>& columns, const RunOptions& options,
                                       double* seconds) {
    const double started = now_seconds();
    std::vector<std::uint64_t> rows;
    const std::uint64_t total = dataset.row_count();
    const std::string lower_needle = to_lower(needle);
    if (lower_needle.empty()) {
        rows = all_rows(dataset);
        if (seconds != nullptr) *seconds = now_seconds() - started;
        return rows;
    }
    std::vector<std::uint32_t> targets = columns;
    if (targets.empty()) {
        for (std::uint32_t c = 0; c < dataset.column_count(); ++c) targets.push_back(c);
    }
    for (std::uint64_t row = 0; row < total; ++row) {
        if ((row & 262143u) == 0) {
            if (options.cancel != nullptr) options.cancel->throw_if_canceled();
            if (options.progress && total > 0) {
                options.progress(static_cast<double>(row) / static_cast<double>(total), "searching");
            }
        }
        const std::string_view text = dataset.row_text(row);
        bool hit = false;
        for (const std::uint32_t column : targets) {
            FieldSpan span;
            if (!field_at(text, dataset.dialect().delimiter, dataset.dialect().quote, column, span)) continue;
            const std::string_view cell_text = text.substr(span.offset, span.length);
            if (contains_icase(cell_text, lower_needle)) {
                hit = true;
                break;
            }
        }
        if (hit) rows.push_back(row);
    }
    if (seconds != nullptr) *seconds = now_seconds() - started;
    return rows;
}

// ── Sorting ─────────────────────────────────────────────────────────────────
void sort_rows(const Dataset& dataset, std::vector<std::uint64_t>& rows, const std::vector<OrderKey>& keys,
               const RunOptions& options) {
    if (keys.empty() || rows.size() < 2) return;
    if (options.cancel != nullptr) options.cancel->throw_if_canceled();
    const Eval eval(dataset);

    // Decorate-sort-undecorate, stored column-wise for cache-friendly compare.
    const std::size_t key_count = keys.size();
    std::vector<std::vector<Value>> columns(key_count);
    for (std::size_t k = 0; k < key_count; ++k) {
        columns[k].resize(rows.size());
        for (std::size_t i = 0; i < rows.size(); ++i) {
            columns[k][i] = eval.eval(*keys[k].expr, rows[i], nullptr);
        }
    }

    std::vector<std::size_t> order(rows.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;

    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        for (std::size_t k = 0; k < key_count; ++k) {
            const Value& left = columns[k][a];
            const Value& right = columns[k][b];
            const bool left_null = left.is_null();
            const bool right_null = right.is_null();
            if (left_null || right_null) {
                if (left_null && right_null) continue;
                return right_null; // nulls last regardless of direction
            }
            const int cmp = compare_values(left, right);
            if (cmp == 0) continue;
            return keys[k].descending ? cmp > 0 : cmp < 0;
        }
        return false;
    });

    std::vector<std::uint64_t> sorted(rows.size());
    for (std::size_t i = 0; i < order.size(); ++i) sorted[i] = rows[order[i]];
    rows.swap(sorted);
}

// ── Execution ───────────────────────────────────────────────────────────────
ResultSet execute(const Dataset& dataset, const Query& query, const RunOptions& run) {
    const double started = now_seconds();
    ResultSet result;
    const Eval eval(dataset);
    const auto outputs = build_outputs(dataset, query);
    result.headers.reserve(outputs.size());
    result.types.reserve(outputs.size());
    for (const auto& out : outputs) {
        result.headers.push_back(out.header);
        result.types.push_back(out.type);
    }

    const bool grouped = query.has_aggregates || !query.group_by.empty();
    const std::uint64_t total_rows = dataset.row_count();

    std::uint64_t cap = query.limit.value_or(0);
    if (run.max_rows > 0) cap = (cap == 0) ? run.max_rows : std::min(cap, run.max_rows);
    const bool limited = cap > 0;

    const auto evaluate_outputs = [&](std::uint64_t row, const AggContext* agg) {
        std::vector<std::string> cells;
        cells.reserve(outputs.size());
        for (const auto& out : outputs) {
            if (out.star) {
                cells.push_back(std::string(dataset.cell(row, out.star_column)));
            } else {
                cells.push_back(eval.eval(*out.expr, row, agg).display());
            }
        }
        return cells;
    };

    if (!grouped) {
        std::vector<std::uint64_t> matched_rows;
        std::uint64_t scanned = 0;

        if (query.order_by.empty()) {
            // Streaming path: project and stop as soon as the LIMIT is met.
            for (std::uint64_t row = 0; row < total_rows; ++row) {
                if ((row & 262143u) == 0) {
                    if (run.cancel != nullptr) run.cancel->throw_if_canceled();
                    if (run.progress && total_rows > 0) {
                        run.progress(static_cast<double>(row) / static_cast<double>(total_rows), "scanning");
                    }
                }
                ++scanned;
                if (query.where && !eval.truth(*query.where, row, nullptr)) continue;
                ++result.matched;
                if (result.matched <= query.offset) continue;
                std::vector<std::string> cells = evaluate_outputs(row, nullptr);
                result.rows.push_back(std::move(cells));
                if (limited && result.rows.size() >= cap) {
                    result.truncated = true;
                    break;
                }
            }
        } else {
            matched_rows.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(total_rows, 4096)));
            for (std::uint64_t row = 0; row < total_rows; ++row) {
                if ((row & 262143u) == 0) {
                    if (run.cancel != nullptr) run.cancel->throw_if_canceled();
                    if (run.progress && total_rows > 0) {
                        run.progress(static_cast<double>(row) / static_cast<double>(total_rows), "scanning");
                    }
                }
                ++scanned;
                if (query.where && !eval.truth(*query.where, row, nullptr)) continue;
                matched_rows.push_back(row);
            }
            result.matched = matched_rows.size();
            sort_rows(dataset, matched_rows, query.order_by, run);
            const std::size_t begin = static_cast<std::size_t>(std::min<std::uint64_t>(query.offset, matched_rows.size()));
            const std::size_t end = limited
                                        ? std::min<std::size_t>(matched_rows.size(), begin + static_cast<std::size_t>(cap))
                                        : matched_rows.size();
            for (std::size_t i = begin; i < end; ++i) {
                result.rows.push_back(evaluate_outputs(matched_rows[i], nullptr));
            }
            result.truncated = limited && (matched_rows.size() > end);
        }
        result.scanned = scanned;

        if (query.distinct && !result.rows.empty()) {
            std::unordered_set<std::string> seen;
            std::vector<std::vector<std::string>> unique;
            unique.reserve(result.rows.size());
            for (auto& row : result.rows) {
                if (seen.insert(join_row(row)).second) unique.push_back(std::move(row));
            }
            result.rows.swap(unique);
        }
    } else {
        // ── Aggregation ─────────────────────────────────────────────────────
        std::vector<const Expr*> agg_exprs;
        for (const auto& out : outputs) {
            if (out.expr != nullptr) collect_aggregates(*out.expr, agg_exprs);
        }
        for (const auto& key : query.order_by) collect_aggregates(*key.expr, agg_exprs);

        struct Group {
            std::uint64_t first_row = 0;
            bool has_row = false;
            std::vector<AggState> states;
        };
        std::unordered_map<std::string, Group> groups;
        groups.reserve(256);

        std::uint64_t scanned = 0;
        for (std::uint64_t row = 0; row < total_rows; ++row) {
            if ((row & 262143u) == 0) {
                if (run.cancel != nullptr) run.cancel->throw_if_canceled();
                if (run.progress && total_rows > 0) {
                    run.progress(static_cast<double>(row) / static_cast<double>(total_rows), "aggregating");
                }
            }
            ++scanned;
            if (query.where && !eval.truth(*query.where, row, nullptr)) continue;
            ++result.matched;

            std::string key;
            for (const auto& group_expr : query.group_by) {
                key.append(eval.eval(*group_expr, row, nullptr).display());
                key.push_back('\x1f');
            }
            auto [it, inserted] = groups.try_emplace(key, Group{});
            Group& group = it->second;
            if (inserted) {
                group.states.resize(agg_exprs.size());
                group.first_row = row;
                group.has_row = true;
            }
            for (std::size_t a = 0; a < agg_exprs.size(); ++a) {
                const Expr& node = *agg_exprs[a];
                if (node.flag && to_lower(node.name) == "count") {
                    ++group.states[a].count;
                    continue;
                }
                if (node.args.empty()) continue;
                const Value value = eval.eval(*node.args[0], row, nullptr);
                update_aggregate(group.states[a], node, value);
            }
        }

        result.scanned = scanned;
        result.groups = groups.size();

        if (groups.empty() && query.group_by.empty()) {
            // SQL semantics: an aggregate over zero rows yields one row.
            Group empty_group;
            empty_group.states.resize(agg_exprs.size());
            groups.emplace(std::string(), std::move(empty_group));
        }

        // Emit groups, honouring ORDER BY when present.
        struct Emitted {
            std::vector<std::string> cells;
            std::vector<Value> sort_keys;
            std::uint64_t first_row = 0;
        };
        std::vector<Emitted> emitted;
        emitted.reserve(groups.size());

        for (const auto& [key, group] : groups) {
            AggContext context;
            context.exprs = &agg_exprs;
            context.states = &group.states;

            Emitted row_result;
            row_result.first_row = group.first_row;
            if (outputs.empty()) continue;
            for (const auto& out : outputs) {
                if (out.star) {
                    row_result.cells.push_back(group.has_row ? std::string(dataset.cell(group.first_row, out.star_column))
                                                             : std::string());
                } else if (group.has_row) {
                    row_result.cells.push_back(eval.eval(*out.expr, group.first_row, &context).display());
                } else {
                    // No input rows: only aggregates produce values.
                    bool is_agg = out.expr->kind == Expr::Kind::Func && is_aggregate(out.expr->name);
                    row_result.cells.push_back(is_agg ? eval.eval(*out.expr, 0, &context).display() : std::string());
                }
            }
            for (const auto& order_key : query.order_by) {
                row_result.sort_keys.push_back(group.has_row ? eval.eval(*order_key.expr, group.first_row, &context)
                                                             : Value::null());
            }
            emitted.push_back(std::move(row_result));
        }

        if (!query.order_by.empty()) {
            std::stable_sort(emitted.begin(), emitted.end(), [&](const Emitted& a, const Emitted& b) {
                for (std::size_t k = 0; k < query.order_by.size(); ++k) {
                    const bool an = a.sort_keys[k].is_null();
                    const bool bn = b.sort_keys[k].is_null();
                    if (an || bn) {
                        if (an && bn) continue;
                        return bn;
                    }
                    const int cmp = compare_values(a.sort_keys[k], b.sort_keys[k]);
                    if (cmp == 0) continue;
                    return query.order_by[k].descending ? cmp > 0 : cmp < 0;
                }
                return a.first_row < b.first_row;
            });
        } else {
            std::stable_sort(emitted.begin(), emitted.end(),
                             [](const Emitted& a, const Emitted& b) { return a.first_row < b.first_row; });
        }

        const std::size_t begin = static_cast<std::size_t>(std::min<std::uint64_t>(query.offset, emitted.size()));
        const std::size_t end = limited ? std::min<std::size_t>(emitted.size(), begin + static_cast<std::size_t>(cap))
                                        : emitted.size();
        for (std::size_t i = begin; i < end; ++i) result.rows.push_back(std::move(emitted[i].cells));
        result.truncated = limited && emitted.size() > end;
    }

    if (run.cancel != nullptr) run.cancel->throw_if_canceled();
    if (run.progress) run.progress(1.0, "done");
    result.seconds = now_seconds() - started;
    return result;
}

} // namespace tess::query
