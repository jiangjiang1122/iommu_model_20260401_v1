// Minimal JSON parser for configuration loading
// Self-contained, no external dependencies
#ifndef IOMMU_MINI_JSON_H
#define IOMMU_MINI_JSON_H

#include <string>
#include <map>
#include <vector>
#include <variant>
#include <stdexcept>
#include <sstream>
#include <cctype>

namespace mini_json {

class Value;
using Object = std::map<std::string, Value>;
using Array  = std::vector<Value>;

class Value {
public:
    enum Type { T_NULL, T_BOOL, T_INT, T_DOUBLE, T_STRING, T_ARRAY, T_OBJECT };

    Value() : type_(T_NULL) {}
    Value(bool b)              : type_(T_BOOL),   bool_val_(b) {}
    Value(int64_t i)           : type_(T_INT),    int_val_(i) {}
    Value(double d)            : type_(T_DOUBLE), double_val_(d) {}
    Value(const std::string& s): type_(T_STRING), str_val_(s) {}
    Value(const char* s)       : type_(T_STRING), str_val_(s) {}
    Value(const Object& o)     : type_(T_OBJECT), obj_val_(o) {}
    Value(const Array& a)      : type_(T_ARRAY),  arr_val_(a) {}

    Type type() const { return type_; }

    bool contains(const std::string& key) const {
        return type_ == T_OBJECT && obj_val_.count(key) > 0;
    }
    bool contains(const char* key) const {
        return contains(std::string(key));
    }

    const Value& operator[](const std::string& key) const {
        if (type_ != T_OBJECT) throw std::runtime_error("Not an object");
        auto it = obj_val_.find(key);
        if (it == obj_val_.end()) throw std::runtime_error("Key not found: " + key);
        return it->second;
    }
    const Value& operator[](const char* key) const {
        return operator[](std::string(key));
    }

    const Value& operator[](size_t idx) const {
        if (type_ != T_ARRAY) throw std::runtime_error("Not an array");
        return arr_val_.at(idx);
    }

    operator bool() const {
        if (type_ == T_BOOL) return bool_val_;
        if (type_ == T_INT) return int_val_ != 0;
        throw std::runtime_error("Not a bool");
    }

    operator int() const { return static_cast<int>(to_int()); }
    operator uint32_t() const { return static_cast<uint32_t>(to_int()); }
    operator int64_t() const { return to_int(); }
    operator uint64_t() const { return static_cast<uint64_t>(to_int()); }

    operator double() const {
        if (type_ == T_DOUBLE) return double_val_;
        if (type_ == T_INT) return static_cast<double>(int_val_);
        throw std::runtime_error("Not a number");
    }

    operator std::string() const {
        if (type_ == T_STRING) return str_val_;
        throw std::runtime_error("Not a string");
    }

    const Object& as_object() const { return obj_val_; }
    const Array& as_array() const { return arr_val_; }

private:
    Type type_;
    bool bool_val_ = false;
    int64_t int_val_ = 0;
    double double_val_ = 0.0;
    std::string str_val_;
    Object obj_val_;
    Array arr_val_;

    int64_t to_int() const {
        if (type_ == T_INT) return int_val_;
        if (type_ == T_DOUBLE) return static_cast<int64_t>(double_val_);
        throw std::runtime_error("Not a number");
    }
};

class Parser {
public:
    static Value parse(const std::string& json) {
        Parser p(json);
        Value v = p.parse_value();
        return v;
    }

private:
    explicit Parser(const std::string& s) : src_(s), pos_(0) {}
    const std::string& src_;
    size_t pos_;

    char peek() { skip_ws(); return pos_ < src_.size() ? src_[pos_] : '\0'; }
    char next() { skip_ws(); return pos_ < src_.size() ? src_[pos_++] : '\0'; }

    void skip_ws() {
        while (pos_ < src_.size()) {
            char c = src_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { ++pos_; continue; }
            // Skip // line comments
            if (c == '/' && pos_ + 1 < src_.size() && src_[pos_+1] == '/') {
                pos_ += 2;
                while (pos_ < src_.size() && src_[pos_] != '\n') ++pos_;
                continue;
            }
            // Skip /* block comments */
            if (c == '/' && pos_ + 1 < src_.size() && src_[pos_+1] == '*') {
                pos_ += 2;
                while (pos_ + 1 < src_.size() && !(src_[pos_] == '*' && src_[pos_+1] == '/')) ++pos_;
                pos_ += 2;
                continue;
            }
            break;
        }
    }

    void expect(char c) {
        char got = next();
        if (got != c) {
            throw std::runtime_error(std::string("Expected '") + c + "' got '" + got + "' at pos " + std::to_string(pos_));
        }
    }

    Value parse_value() {
        char c = peek();
        if (c == '{') return parse_object();
        if (c == '[') return parse_array();
        if (c == '"') return parse_string_value();
        if (c == 't' || c == 'f') return parse_bool();
        if (c == 'n') return parse_null();
        if (c == '-' || std::isdigit(c)) return parse_number();
        throw std::runtime_error(std::string("Unexpected char '") + c + "' at pos " + std::to_string(pos_));
    }

    Value parse_object() {
        expect('{');
        Object obj;
        if (peek() == '}') { next(); return Value(obj); }
        while (true) {
            std::string key = parse_string();
            expect(':');
            Value val = parse_value();
            obj[key] = val;
            char c = peek();
            if (c == ',') { next(); continue; }
            if (c == '}') { next(); break; }
            throw std::runtime_error("Expected ',' or '}' in object");
        }
        return Value(obj);
    }

    Value parse_array() {
        expect('[');
        Array arr;
        if (peek() == ']') { next(); return Value(arr); }
        while (true) {
            arr.push_back(parse_value());
            char c = peek();
            if (c == ',') { next(); continue; }
            if (c == ']') { next(); break; }
            throw std::runtime_error("Expected ',' or ']' in array");
        }
        return Value(arr);
    }

    std::string parse_string() {
        expect('"');
        std::string s;
        while (pos_ < src_.size() && src_[pos_] != '"') {
            if (src_[pos_] == '\\') {
                ++pos_;
                if (pos_ < src_.size()) {
                    switch (src_[pos_]) {
                        case '"': s += '"'; break;
                        case '\\': s += '\\'; break;
                        case '/': s += '/'; break;
                        case 'n': s += '\n'; break;
                        case 't': s += '\t'; break;
                        case 'r': s += '\r'; break;
                        default: s += src_[pos_]; break;
                    }
                }
            } else {
                s += src_[pos_];
            }
            ++pos_;
        }
        if (pos_ < src_.size()) ++pos_; // skip closing "
        return s;
    }

    Value parse_string_value() {
        return Value(parse_string());
    }

    Value parse_number() {
        skip_ws();
        size_t start = pos_;
        bool is_float = false;
        if (src_[pos_] == '-') ++pos_;
        while (pos_ < src_.size() && std::isdigit(src_[pos_])) ++pos_;
        if (pos_ < src_.size() && src_[pos_] == '.') { is_float = true; ++pos_; }
        while (pos_ < src_.size() && std::isdigit(src_[pos_])) ++pos_;
        if (pos_ < src_.size() && (src_[pos_] == 'e' || src_[pos_] == 'E')) {
            is_float = true; ++pos_;
            if (pos_ < src_.size() && (src_[pos_] == '+' || src_[pos_] == '-')) ++pos_;
            while (pos_ < src_.size() && std::isdigit(src_[pos_])) ++pos_;
        }
        std::string num_str = src_.substr(start, pos_ - start);
        if (is_float) return Value(std::stod(num_str));
        return Value(static_cast<int64_t>(std::stoll(num_str)));
    }

    Value parse_bool() {
        skip_ws();
        if (src_.compare(pos_, 4, "true") == 0) { pos_ += 4; return Value(true); }
        if (src_.compare(pos_, 5, "false") == 0) { pos_ += 5; return Value(false); }
        throw std::runtime_error("Expected 'true' or 'false'");
    }

    Value parse_null() {
        skip_ws();
        if (src_.compare(pos_, 4, "null") == 0) { pos_ += 4; return Value(); }
        throw std::runtime_error("Expected 'null'");
    }
};

} // namespace mini_json

#endif // IOMMU_MINI_JSON_H
