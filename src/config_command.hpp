#pragma once

#include <string>
#include <vector>

namespace hymission {

inline std::string luaStringLiteral(const std::string& value) {
    std::string out = "\"";
    for (const char ch : value) {
        if (ch == '\\' || ch == '"')
            out += '\\';
        out += ch;
    }
    out += '"';
    return out;
}

inline std::string luaConfigExpression(const std::string& name, const std::string& value) {
    std::vector<std::string> parts;
    std::string              current;
    for (const char ch : name) {
        if (ch == ':' || ch == '.') {
            if (!current.empty()) {
                parts.push_back(current);
                current.clear();
            }
            continue;
        }

        current += ch;
    }

    if (!current.empty())
        parts.push_back(current);

    if (parts.empty())
        return {};

    std::string expression = "hl.config({";
    for (std::size_t i = 0; i + 1 < parts.size(); ++i)
        expression += "[" + luaStringLiteral(parts[i]) + "] = {";

    expression += "[" + luaStringLiteral(parts.back()) + "] = " + value;

    for (std::size_t i = 0; i < parts.size(); ++i)
        expression += "}";

    expression += ")";
    return expression;
}

// Hyprland 0.56 supports keyword (legacy) and eval (Lua); newer versions
// remove keyword entirely. Cache only a successful fallback, never a failure.
class ConfigCommandAdapter {
  public:
    void reset() { m_preferEval = false; }

    // invoke(command, arguments) returns Hyprland's response. Empty return from
    // set means success; even an empty IPC response is a failure otherwise.
    template <typename Invoke>
    std::string set(const std::string& name, const std::string& value, Invoke&& invoke) {
        const auto expression = luaConfigExpression(name, value);
        const bool evalFirst = m_preferEval && !expression.empty();
        std::string evalResult;
        if (evalFirst) {
            evalResult = invoke("eval", expression);
            if (evalResult == "ok")
                return {};
            // Recover even if the provider changed without a reload event.
            reset();
        }

        const auto keywordResult = invoke("keyword", name + " " + value);
        if (keywordResult == "ok")
            return {};

        if (expression.empty())
            return "keyword: " + response(keywordResult) + "; eval: no Lua expression";

        if (!evalFirst)
            evalResult = invoke("eval", expression);
        if (evalResult == "ok") {
            m_preferEval = true;
            return {};
        }
        return "keyword: " + response(keywordResult) + "; eval: " + response(evalResult);
    }

  private:
    static std::string response(const std::string& result) { return result.empty() ? "<empty response>" : result; }
    bool m_preferEval = false;
};

} // namespace hymission
