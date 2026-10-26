// ─────────────────────────────────────────────────────────────────────────────
//  Tessera · parser.cpp
// ─────────────────────────────────────────────────────────────────────────────
#include "query/parser.hpp"

#include "common/util.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <unordered_set>

namespace tess::query {
namespace {

enum class TokKind { End, Ident, Number, String, Punct };

struct Token {
    TokKind kind = TokKind::End;
    std::string text;
    double number = 0.0;
    std::int64_t integer = 0;
    bool is_integer = true;
    std::size_t begin = 0;
    std::size_t end = 0;
};

struct ParseFailure {
    std::string message;
    std::size_t position = 0;
};

[[noreturn]] void fail_at(const std::string& message, std::size_t position) {
    throw ParseFailure{message, position};
}

class Lexer {
public:
    explicit Lexer(std::string_view src) : src_(src), pos_(0) { advance(); }

    [[nodiscard]] const Token& current() const { return current_; }
    [[nodiscard]] std::size_t position() const { return current_.begin; }

    void advance() { current_ = next(); }

    [[nodiscard]] bool at_end() const { return current_.kind == TokKind::End; }

    [[nodiscard]] bool is_punct(std::string_view p) const {
        return current_.kind == TokKind::Punct && current_.text == p;
    }

    [[nodiscard]] bool is_keyword(std::string_view kw) const {
        return current_.kind == TokKind::Ident && iequals(current_.text, kw);
    }

    bool accept_punct(std::string_view p) {
        if (is_punct(p)) {
            advance();
            return true;
        }
        return false;
    }

    bool accept_keyword(std::string_view kw) {
        if (is_keyword(kw)) {
            advance();
            return true;
        }
        return false;
    }

    void expect_punct(std::string_view p) {
        if (!accept_punct(p)) {
            fail_at("expected '" + std::string(p) + "' but found " + describe(current_), position());
        }
    }

    void expect_keyword(std::string_view kw) {
        if (!accept_keyword(kw)) {
            fail_at("expected " + std::string(kw) + " but found " + describe(current_), position());
        }
    }

    [[nodiscard]] static std::string describe(const Token& t) {
        switch (t.kind) {
            case TokKind::End: return "end of statement";
            case TokKind::String: return "string literal";
            case TokKind::Number: return "number " + t.text;
            default: return "'" + t.text + "'";
        }
    }

    [[nodiscard]] Token take_identifier() {
        if (current_.kind != TokKind::Ident) {
            fail_at("expected a column name but found " + describe(current_), position());
        }
        Token t = current_;
        advance();
        return t;
    }

    [[nodiscard]] std::string_view source() const { return src_; }

private:
    Token next() {
        while (pos_ < src_.size()) {
            const char c = src_[pos_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++pos_;
            } else if (c == '-' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '-') {
                while (pos_ < src_.size() && src_[pos_] != '\n') ++pos_;
            } else {
                break;
            }
        }
        Token t;
        t.begin = pos_;
        if (pos_ >= src_.size()) {
            t.kind = TokKind::End;
            t.end = pos_;
            return t;
        }
        const char c = src_[pos_];

        // String literal: single quotes, '' escapes a quote.
        if (c == '\'') {
            ++pos_;
            std::string value;
            while (pos_ < src_.size()) {
                const char ch = src_[pos_];
                if (ch == '\'') {
                    if (pos_ + 1 < src_.size() && src_[pos_ + 1] == '\'') {
                        value.push_back('\'');
                        pos_ += 2;
                        continue;
                    }
                    ++pos_;
                    break;
                }
                value.push_back(ch);
                ++pos_;
            }
            t.kind = TokKind::String;
            t.text = std::move(value);
            t.end = pos_;
            return t;
        }

        // Quoted identifier: double quotes, "" escapes a quote.
        if (c == '"' || c == '`' || c == '[') {
            const char close = (c == '[') ? ']' : c;
            ++pos_;
            std::string value;
            while (pos_ < src_.size()) {
                const char ch = src_[pos_];
                if (ch == close) {
                    if (close != ']' && pos_ + 1 < src_.size() && src_[pos_ + 1] == close) {
                        value.push_back(close);
                        pos_ += 2;
                        continue;
                    }
                    ++pos_;
                    break;
                }
                value.push_back(ch);
                ++pos_;
            }
            t.kind = TokKind::Ident;
            t.text = std::move(value);
            t.end = pos_;
            return t;
        }

        if (std::isdigit(static_cast<unsigned char>(c)) ||
            (c == '.' && pos_ + 1 < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_ + 1])))) {
            const std::size_t start = pos_;
            bool is_int = true;
            while (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_]))) ++pos_;
            if (pos_ < src_.size() && src_[pos_] == '.') {
                is_int = false;
                ++pos_;
                while (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_]))) ++pos_;
            }
            if (pos_ < src_.size() && (src_[pos_] == 'e' || src_[pos_] == 'E')) {
                const std::size_t save = pos_;
                ++pos_;
                if (pos_ < src_.size() && (src_[pos_] == '+' || src_[pos_] == '-')) ++pos_;
                if (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_]))) {
                    is_int = false;
                    while (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_]))) ++pos_;
                } else {
                    pos_ = save;
                }
            }
            t.kind = TokKind::Number;
            t.text = std::string(src_.substr(start, pos_ - start));
            t.is_integer = is_int;
            if (is_int && !parse_int(t.text, t.integer)) {
                t.is_integer = false;
                if (!parse_double(t.text, t.number)) t.number = 0.0;
            } else if (!is_int) {
                if (!parse_double(t.text, t.number)) t.number = 0.0;
            }
            t.end = pos_;
            return t;
        }

        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '$' || c == '#') {
            const std::size_t start = pos_;
            while (pos_ < src_.size()) {
                const char ch = src_[pos_];
                if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '$' || ch == '#' || ch == '.') {
                    ++pos_;
                } else {
                    break;
                }
            }
            t.kind = TokKind::Ident;
            t.text = std::string(src_.substr(start, pos_ - start));
            t.end = pos_;
            return t;
        }

        // Operators and punctuation (longest match first).
        static constexpr std::string_view kTwoChar[] = {"<=", ">=", "<>", "!=", "==", "||"};
        for (const auto op : kTwoChar) {
            if (src_.compare(pos_, 2, op) == 0) {
                t.kind = TokKind::Punct;
                t.text = std::string(op);
                pos_ += 2;
                t.end = pos_;
                return t;
            }
        }
        t.kind = TokKind::Punct;
        t.text = std::string(1, c);
        ++pos_;
        t.end = pos_;
        return t;
    }

    std::string_view src_;
    std::size_t pos_;
    Token current_;
};

const std::unordered_set<std::string>& scalar_functions() {
    static const std::unordered_set<std::string> names = {
        "lower", "upper", "trim", "ltrim", "rtrim", "len", "length", "substr", "substring",
        "replace", "concat", "abs", "round", "floor", "ceil", "ceiling", "int", "integer",
        "float", "real", "double", "text", "str", "coalesce", "ifnull", "year", "month", "day",
        "date", "startswith", "endswith", "contains",
    };
    return names;
}

const std::unordered_set<std::string>& aggregate_functions() {
    static const std::unordered_set<std::string> names = {
        "count", "count_distinct", "sum", "avg", "mean", "min", "max",
    };
    return names;
}

ExprPtr clone_expr(const Expr& src) {
    auto e = std::make_unique<Expr>();
    e->kind = src.kind;
    e->literal = src.literal;
    e->column = src.column;
    e->name = src.name;
    e->op = src.op;
    e->unop = src.unop;
    e->flag = src.flag;
    e->source = src.source;
    e->args.reserve(src.args.size());
    for (const auto& arg : src.args) e->args.push_back(clone_expr(*arg));
    return e;
}

bool expr_has_aggregate(const Expr& e) {
    if (e.kind == Expr::Kind::Func && is_aggregate(e.name)) return true;
    for (const auto& arg : e.args) {
        if (expr_has_aggregate(*arg)) return true;
    }
    return false;
}

/// True when the expression reads a column at any depth. Used to reject a bare
/// column sitting next to an aggregate with no GROUP BY to define it.
bool expr_has_column(const Expr& e) {
    if (e.kind == Expr::Kind::Column) return true;
    for (const auto& arg : e.args) {
        if (expr_has_column(*arg)) return true;
    }
    return false;
}

class Parser {
public:
    Parser(std::string_view sql, const Dataset& dataset) : lexer_(sql), dataset_(dataset), sql_(sql) {}

    Query parse_statement() {
        Query q;
        q.source_text = std::string(sql_);
        if (lexer_.is_keyword("SELECT")) {
            lexer_.advance();
            q.distinct = lexer_.accept_keyword("DISTINCT");
            parse_select_list(q);
            aliases_ = &q.items;
            if (lexer_.accept_keyword("FROM")) {
                // The file is the only relation; accept and ignore any name so
                // pasted SQL keeps working.
                if (!lexer_.at_end()) lexer_.advance();
            }
            if (lexer_.accept_keyword("WHERE")) {
                q.where = parse_expression();
            }
            if (lexer_.is_keyword("GROUP")) {
                lexer_.advance();
                lexer_.expect_keyword("BY");
                do {
                    prefer_aliases_ = true;
                    q.group_by.push_back(parse_expression());
                    prefer_aliases_ = false;
                } while (lexer_.accept_punct(","));
            }
            if (lexer_.is_keyword("HAVING")) {
                fail_at("HAVING is not supported; use a subquery in your shell pipeline instead",
                        lexer_.position());
            }
            if (lexer_.is_keyword("ORDER")) {
                lexer_.advance();
                lexer_.expect_keyword("BY");
                do {
                    OrderKey key;
                    prefer_aliases_ = true;
                    key.expr = parse_expression();
                    prefer_aliases_ = false;
                    // ORDER BY 2 refers to the second select item, as in SQL.
                    if (key.expr->kind == Expr::Kind::Literal && key.expr->literal.kind == ValueKind::Int) {
                        const std::int64_t position = key.expr->literal.i;
                        if (position >= 1 && position <= static_cast<std::int64_t>(q.items.size())) {
                            const SelectItem& item = q.items[static_cast<std::size_t>(position - 1)];
                            if (!item.star && item.expr) key.expr = clone_expr(*item.expr);
                        }
                    }
                    if (lexer_.accept_keyword("DESC")) key.descending = true;
                    else lexer_.accept_keyword("ASC");
                    q.order_by.push_back(std::move(key));
                } while (lexer_.accept_punct(","));
            }
            if (lexer_.accept_keyword("LIMIT")) {
                if (!lexer_.accept_keyword("ALL")) q.limit = parse_limit_value();
                if (lexer_.accept_keyword("OFFSET")) {
                    q.offset = parse_limit_value();
                }
            }
            if (!lexer_.at_end()) {
                fail_at("unexpected " + Lexer::describe(lexer_.current()) + " after the end of the statement",
                        lexer_.position());
            }
        } else {
            // A bare expression is treated as a WHERE clause over all columns.
            q.where = parse_expression();
            SelectItem star;
            star.star = true;
            q.items.push_back(std::move(star));
            if (!lexer_.at_end()) {
                fail_at("unexpected " + Lexer::describe(lexer_.current()), lexer_.position());
            }
        }

        for (const auto& item : q.items) {
            if (item.expr && expr_has_aggregate(*item.expr)) q.has_aggregates = true;
        }
        for (const auto& key : q.order_by) {
            if (key.expr && expr_has_aggregate(*key.expr)) q.has_aggregates = true;
        }
        if (q.where && expr_has_aggregate(*q.where)) {
            fail_at("aggregate functions are not allowed in WHERE; use GROUP BY with a HAVING clause in a pipeline",
                    lexer_.position());
        }

        // `SELECT region, count(*) FROM t` has no defined value for `region`: it
        // names a column that the aggregation collapses. Standard SQL refuses
        // this rather than guessing, and so do we — returning the first row's
        // region beside a whole-file count looks like an answer and is not one.
        if (q.has_aggregates && q.group_by.empty()) {
            for (const auto& item : q.items) {
                const bool bare_column =
                    item.expr && expr_has_column(*item.expr) && !expr_has_aggregate(*item.expr);
                if (item.star || bare_column) {
                    fail_at("SELECT mixes aggregates with bare columns but has no GROUP BY; "
                            "add the columns to GROUP BY, or remove them and keep the aggregates",
                            lexer_.position());
                }
            }
        }
        if (q.items.empty()) {
            SelectItem star;
            star.star = true;
            q.items.push_back(std::move(star));
        }
        return q;
    }

    ExprPtr parse_standalone_expression() {
        auto e = parse_expression();
        if (!lexer_.at_end()) {
            fail_at("unexpected " + Lexer::describe(lexer_.current()), lexer_.position());
        }
        return e;
    }

private:
    std::uint64_t parse_limit_value() {
        if (lexer_.current().kind == TokKind::Number && lexer_.current().is_integer) {
            const auto value = static_cast<std::uint64_t>(std::max<std::int64_t>(0, lexer_.current().integer));
            lexer_.advance();
            return value;
        }
        if (lexer_.is_keyword("ALL")) {
            lexer_.advance();
            return 0;
        }
        fail_at("LIMIT expects a whole number", lexer_.position());
    }

    void parse_select_list(Query& q) {
        do {
            SelectItem item;
            if (lexer_.accept_punct("*")) {
                item.star = true;
                q.items.push_back(std::move(item));
                continue;
            }
            item.expr = parse_expression();
            if (lexer_.accept_keyword("AS")) {
                item.alias = lexer_.take_identifier().text;
            } else if (lexer_.current().kind == TokKind::Ident && !is_reserved(lexer_.current().text)) {
                item.alias = lexer_.current().text;
                lexer_.advance();
            }
            if (item.alias.empty()) {
                item.alias = item.expr->source;
            }
            q.items.push_back(std::move(item));
        } while (lexer_.accept_punct(","));
    }

    static bool is_reserved(const std::string& word) {
        static const std::unordered_set<std::string> reserved = {
            "from", "where", "group", "by", "order", "limit", "offset", "having", "asc", "desc",
            "and", "or", "not", "is", "in", "between", "like", "as", "on", "join", "union",
        };
        return reserved.count(to_lower(word)) > 0;
    }

    ExprPtr parse_expression() { return parse_or(); }

    ExprPtr parse_or() {
        auto left = parse_and();
        while (lexer_.is_keyword("OR")) {
            lexer_.advance();
            auto right = parse_and();
            auto node = std::make_unique<Expr>();
            node->kind = Expr::Kind::Binary;
            node->op = BinOp::Or;
            node->source = left->source + " OR " + right->source;
            node->args.push_back(std::move(left));
            node->args.push_back(std::move(right));
            left = std::move(node);
        }
        return left;
    }

    ExprPtr parse_and() {
        auto left = parse_not();
        while (lexer_.is_keyword("AND")) {
            lexer_.advance();
            auto right = parse_not();
            auto node = std::make_unique<Expr>();
            node->kind = Expr::Kind::Binary;
            node->op = BinOp::And;
            node->source = left->source + " AND " + right->source;
            node->args.push_back(std::move(left));
            node->args.push_back(std::move(right));
            left = std::move(node);
        }
        return left;
    }

    ExprPtr parse_not() {
        if (lexer_.is_keyword("NOT")) {
            lexer_.advance();
            auto operand = parse_not();
            auto node = std::make_unique<Expr>();
            node->kind = Expr::Kind::Unary;
            node->unop = UnOp::Not;
            node->source = "NOT " + operand->source;
            node->args.push_back(std::move(operand));
            return node;
        }
        return parse_comparison();
    }

    ExprPtr parse_comparison() {
        auto left = parse_additive();

        if (lexer_.accept_keyword("IS")) {
            const bool negated = lexer_.accept_keyword("NOT");
            lexer_.expect_keyword("NULL");
            auto node = std::make_unique<Expr>();
            node->kind = Expr::Kind::IsNull;
            node->flag = negated;
            node->source = left->source + (negated ? " IS NOT NULL" : " IS NULL");
            node->args.push_back(std::move(left));
            return node;
        }

        bool negated_op = false;
        if (lexer_.is_keyword("NOT")) {
            // Only NOT LIKE / NOT IN / NOT BETWEEN are valid here.
            const std::size_t pos = lexer_.position();
            lexer_.advance();
            if (lexer_.is_keyword("LIKE") || lexer_.is_keyword("IN") || lexer_.is_keyword("BETWEEN")) {
                negated_op = true;
            } else {
                fail_at("expected LIKE, IN or BETWEEN after NOT", pos);
            }
        }

        if (lexer_.accept_keyword("LIKE")) {
            auto pattern = parse_additive();
            auto node = std::make_unique<Expr>();
            node->kind = Expr::Kind::Binary;
            node->op = negated_op ? BinOp::NotLike : BinOp::Like;
            node->source = left->source + (negated_op ? " NOT LIKE " : " LIKE ") + pattern->source;
            node->args.push_back(std::move(left));
            node->args.push_back(std::move(pattern));
            return node;
        }

        if (lexer_.accept_keyword("IN")) {
            lexer_.expect_punct("(");
            auto node = std::make_unique<Expr>();
            node->kind = Expr::Kind::In;
            node->flag = negated_op;
            std::string source = left->source + (negated_op ? " NOT IN (" : " IN (");
            node->args.push_back(std::move(left));
            do {
                node->args.push_back(parse_additive());
                source += node->args.back()->source;
                if (lexer_.is_punct(",")) source += ", ";
            } while (lexer_.accept_punct(","));
            lexer_.expect_punct(")");
            source += ")";
            node->source = source;
            return node;
        }

        if (lexer_.accept_keyword("BETWEEN")) {
            auto low = parse_additive();
            lexer_.expect_keyword("AND");
            auto high = parse_additive();
            auto node = std::make_unique<Expr>();
            node->kind = Expr::Kind::Between;
            node->flag = negated_op;
            node->source = left->source + (negated_op ? " NOT BETWEEN " : " BETWEEN ") + low->source + " AND " +
                           high->source;
            node->args.push_back(std::move(left));
            node->args.push_back(std::move(low));
            node->args.push_back(std::move(high));
            return node;
        }

        if (lexer_.current().kind == TokKind::Punct) {
            BinOp op;
            bool matched = true;
            const std::string& p = lexer_.current().text;
            if (p == "=" || p == "==") op = BinOp::Eq;
            else if (p == "!=" || p == "<>") op = BinOp::Ne;
            else if (p == "<") op = BinOp::Lt;
            else if (p == "<=") op = BinOp::Le;
            else if (p == ">") op = BinOp::Gt;
            else if (p == ">=") op = BinOp::Ge;
            else matched = false;
            if (matched) {
                const std::string op_text = p;
                lexer_.advance();
                auto right = parse_additive();
                auto node = std::make_unique<Expr>();
                node->kind = Expr::Kind::Binary;
                node->op = op;
                node->source = left->source + " " + op_text + " " + right->source;
                node->args.push_back(std::move(left));
                node->args.push_back(std::move(right));
                return node;
            }
        }
        return left;
    }

    ExprPtr parse_additive() {
        auto left = parse_multiplicative();
        while (lexer_.is_punct("+") || lexer_.is_punct("-") || lexer_.is_punct("||")) {
            const std::string op = lexer_.current().text;
            lexer_.advance();
            auto right = parse_multiplicative();
            auto node = std::make_unique<Expr>();
            if (op == "||") {
                node->kind = Expr::Kind::Func;
                node->name = "concat";
                node->source = left->source + " || " + right->source;
                node->args.push_back(std::move(left));
                node->args.push_back(std::move(right));
            } else {
                node->kind = Expr::Kind::Binary;
                node->op = (op == "+") ? BinOp::Add : BinOp::Sub;
                node->source = left->source + " " + op + " " + right->source;
                node->args.push_back(std::move(left));
                node->args.push_back(std::move(right));
            }
            left = std::move(node);
        }
        return left;
    }

    ExprPtr parse_multiplicative() {
        auto left = parse_unary();
        while (lexer_.is_punct("*") || lexer_.is_punct("/") || lexer_.is_punct("%")) {
            const std::string op = lexer_.current().text;
            lexer_.advance();
            auto right = parse_unary();
            auto node = std::make_unique<Expr>();
            node->kind = Expr::Kind::Binary;
            node->op = (op == "*") ? BinOp::Mul : (op == "/" ? BinOp::Div : BinOp::Mod);
            node->source = left->source + " " + op + " " + right->source;
            node->args.push_back(std::move(left));
            node->args.push_back(std::move(right));
            left = std::move(node);
        }
        return left;
    }

    ExprPtr parse_unary() {
        if (lexer_.is_punct("-")) {
            lexer_.advance();
            auto operand = parse_unary();
            auto node = std::make_unique<Expr>();
            node->kind = Expr::Kind::Unary;
            node->unop = UnOp::Neg;
            node->source = "-" + operand->source;
            node->args.push_back(std::move(operand));
            return node;
        }
        if (lexer_.is_punct("+")) {
            lexer_.advance();
            return parse_unary();
        }
        return parse_primary();
    }

    ExprPtr parse_primary() {
        const Token& tok = lexer_.current();
        if (tok.kind == TokKind::Number) {
            auto node = std::make_unique<Expr>();
            node->kind = Expr::Kind::Literal;
            node->literal = tok.is_integer ? Value::integer(tok.integer) : Value::real(tok.number);
            node->source = tok.text;
            lexer_.advance();
            return node;
        }
        if (tok.kind == TokKind::String) {
            auto node = std::make_unique<Expr>();
            node->kind = Expr::Kind::Literal;
            node->literal = Value::text(tok.text);
            node->source = "'" + tok.text + "'";
            lexer_.advance();
            return node;
        }
        if (lexer_.accept_punct("(")) {
            auto inner = parse_expression();
            lexer_.expect_punct(")");
            return inner;
        }
        if (tok.kind == TokKind::Ident) {
            const std::string name = tok.text;
            if (iequals(name, "NULL")) {
                lexer_.advance();
                auto node = std::make_unique<Expr>();
                node->kind = Expr::Kind::Literal;
                node->literal = Value::null();
                node->source = "NULL";
                return node;
            }
            if (iequals(name, "TRUE") || iequals(name, "FALSE")) {
                lexer_.advance();
                auto node = std::make_unique<Expr>();
                node->kind = Expr::Kind::Literal;
                node->literal = Value::boolean(iequals(name, "TRUE"));
                node->source = to_upper(name);
                return node;
            }

            lexer_.advance();
            if (lexer_.is_punct("(")) {
                return parse_function_call(name);
            }
            return resolve_column(name);
        }
        fail_at("unexpected " + Lexer::describe(tok) + " in expression", lexer_.position());
    }

    ExprPtr resolve_column(const std::string& raw_name) {
        // Accept `table.column` by resolving the suffix.
        std::string name = raw_name;
        const std::size_t dot = name.find_last_of('.');
        if (dot != std::string::npos) name = name.substr(dot + 1);
        // In GROUP BY / ORDER BY an output alias shadows a stored column of the
        // same name, which is what SQL engines do.
        if (prefer_aliases_ && aliases_ != nullptr) {
            for (const auto& item : *aliases_) {
                if (item.star || !item.expr) continue;
                const std::string& label = item.alias.empty() ? item.expr->source : item.alias;
                if (iequals(label, name)) return clone_expr(*item.expr);
            }
        }
        const auto index = dataset_.column_index(name);
        if (!index.has_value()) {
            if (aliases_ != nullptr) {
                for (const auto& item : *aliases_) {
                    if (item.star || !item.expr) continue;
                    const std::string& label = item.alias.empty() ? item.expr->source : item.alias;
                    if (iequals(label, name)) return clone_expr(*item.expr);
                }
            }
            fail_at("unknown column '" + name + "'" + suggest_column(name, dataset_), lexer_.position());
        }
        auto node = std::make_unique<Expr>();
        node->kind = Expr::Kind::Column;
        node->column = *index;
        node->name = std::string(dataset_.column_name(*index));
        node->source = node->name;
        return node;
    }

    ExprPtr parse_function_call(const std::string& raw_name) {
        const std::string name = to_lower(raw_name);
        lexer_.expect_punct("(");
        auto node = std::make_unique<Expr>();
        node->kind = Expr::Kind::Func;
        node->name = name;
        node->source = name + "(";

        const bool known_scalar = scalar_functions().count(name) > 0;
        const bool known_aggregate = aggregate_functions().count(name) > 0;
        if (!known_scalar && !known_aggregate) {
            fail_at("unknown function '" + raw_name + "'", lexer_.position());
        }

        if (name == "count" && lexer_.is_punct("*")) {
            lexer_.advance();
            node->flag = true;
            node->source += "*";
            lexer_.expect_punct(")");
            node->source += ")";
            return node;
        }

        if (!lexer_.is_punct(")")) {
            do {
                node->args.push_back(parse_expression());
                node->source += node->args.back()->source;
                if (lexer_.is_punct(",")) node->source += ", ";
            } while (lexer_.accept_punct(","));
        }
        lexer_.expect_punct(")");
        node->source += ")";

        // Cheap arity checks catch the mistakes people actually make.
        const std::size_t argc = node->args.size();
        const auto require = [&](std::size_t lo, std::size_t hi) {
            if (argc < lo || argc > hi) {
                fail_at("function " + name + " expects " + std::to_string(lo) +
                            (hi != lo ? "–" + std::to_string(hi) : "") + " argument(s), got " +
                            std::to_string(argc),
                        lexer_.position());
            }
        };
        if (name == "substr" || name == "substring") require(2, 3);
        else if (name == "replace") require(3, 3);
        else if (name == "round") require(1, 2);
        else if (name == "coalesce" || name == "concat") require(1, 64);
        else if (name == "ifnull") require(2, 2);
        else if (known_aggregate && name != "count") require(1, 1);
        else if (known_scalar) require(1, 2);
        return node;
    }

    Lexer lexer_;
    const Dataset& dataset_;
    std::string_view sql_;
    const std::vector<SelectItem>* aliases_ = nullptr;
    bool prefer_aliases_ = false;
};

} // namespace

// ── Value helpers ───────────────────────────────────────────────────────────
Value Value::integer(std::int64_t v) {
    Value value;
    value.kind = ValueKind::Int;
    value.i = v;
    return value;
}

Value Value::real(double v) {
    Value value;
    value.kind = ValueKind::Real;
    value.d = v;
    return value;
}

Value Value::boolean(bool v) {
    Value value;
    value.kind = ValueKind::Bool;
    value.b = v;
    return value;
}

Value Value::text(std::string v) {
    Value value;
    value.kind = ValueKind::Text;
    value.s = std::move(v);
    return value;
}

Value Value::timestamp(std::int64_t epoch, std::string original) {
    Value value;
    value.kind = ValueKind::Timestamp;
    value.i = epoch;
    value.s = std::move(original);
    return value;
}

double Value::as_double() const noexcept {
    switch (kind) {
        case ValueKind::Int: return static_cast<double>(i);
        case ValueKind::Real: return d;
        case ValueKind::Bool: return b ? 1.0 : 0.0;
        case ValueKind::Timestamp: return static_cast<double>(i);
        default: return 0.0;
    }
}

std::int64_t Value::as_int() const noexcept {
    switch (kind) {
        case ValueKind::Int: return i;
        case ValueKind::Real: return static_cast<std::int64_t>(d);
        case ValueKind::Bool: return b ? 1 : 0;
        case ValueKind::Timestamp: return i;
        default: return 0;
    }
}

bool Value::as_bool() const noexcept {
    switch (kind) {
        case ValueKind::Null: return false;
        case ValueKind::Int: return i != 0;
        case ValueKind::Real: return d != 0.0;
        case ValueKind::Bool: return b;
        case ValueKind::Timestamp: return i != 0;
        case ValueKind::Text: return !s.empty() && !iequals(s, "false") && s != "0";
    }
    return false;
}

std::string Value::display() const {
    switch (kind) {
        case ValueKind::Null: return {};
        case ValueKind::Int: return std::to_string(i);
        case ValueKind::Real: return format_double(d, 12);
        case ValueKind::Bool: return b ? "true" : "false";
        case ValueKind::Timestamp: return s.empty() ? format_timestamp(i) : s;
        case ValueKind::Text: return s;
    }
    return {};
}

ExprPtr Expr::literal_value(Value v) {
    auto node = std::make_unique<Expr>();
    node->kind = Kind::Literal;
    node->literal = std::move(v);
    node->source = node->literal.display();
    return node;
}

ExprPtr Expr::column_ref(std::uint32_t index, std::string name) {
    auto node = std::make_unique<Expr>();
    node->kind = Kind::Column;
    node->column = index;
    node->source = name;
    node->name = std::move(name);
    return node;
}

bool is_aggregate(std::string_view name) noexcept {
    static const std::unordered_set<std::string> names = {
        "count", "count_distinct", "sum", "avg", "mean", "min", "max",
    };
    return names.count(to_lower(name)) > 0;
}

std::string suggest_column(std::string_view name, const Dataset& dataset) {
    std::string best;
    const std::string needle = to_lower(name);
    int best_score = 0;
    for (std::uint32_t i = 0; i < dataset.column_count(); ++i) {
        const std::string candidate = to_lower(dataset.column_name(i));
        int score = 0;
        if (candidate.rfind(needle, 0) == 0) score = 100 - static_cast<int>(candidate.size());
        else if (candidate.find(needle) != std::string::npos) score = 50;
        else {
            // Common prefix length as a weak signal.
            std::size_t common = 0;
            while (common < candidate.size() && common < needle.size() && candidate[common] == needle[common]) {
                ++common;
            }
            score = static_cast<int>(common) * 5;
        }
        if (score > best_score) {
            best_score = score;
            best = dataset.column_name(i);
        }
    }
    if (best_score >= 10) return ". Did you mean '" + best + "'?";
    return {};
}

std::optional<Query> parse_query(std::string_view sql, const Dataset& dataset, std::string& error) {
    if (trim(sql).empty()) {
        error = "empty statement";
        return std::nullopt;
    }
    try {
        Parser parser(sql, dataset);
        return parser.parse_statement();
    } catch (const ParseFailure& failure) {
        std::string marker(failure.position < 200 ? failure.position : 0, ' ');
        error = failure.message + "\n  " + std::string(sql.substr(0, 160)) + "\n  " + marker + "^";
        return std::nullopt;
    }
}

ExprPtr parse_expression(std::string_view text, const Dataset& dataset, std::string& error) {
    if (trim(text).empty()) {
        error = "empty expression";
        return nullptr;
    }
    try {
        Parser parser(text, dataset);
        return parser.parse_standalone_expression();
    } catch (const ParseFailure& failure) {
        error = failure.message;
        return nullptr;
    }
}

} // namespace tess::query
