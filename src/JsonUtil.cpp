#include "JsonUtil.hpp"

namespace Exchange {

std::string get_json_string(const std::string& json, const std::string& key) {
    std::string key_quoted = "\"" + key + "\"";
    size_t key_pos = json.find(key_quoted);
    if (key_pos == std::string::npos) {
        return "";
    }
    
    size_t colon_pos = json.find(':', key_pos + key_quoted.length());
    if (colon_pos == std::string::npos) {
        return "";
    }
    
    size_t start_quote = json.find('"', colon_pos + 1);
    if (start_quote == std::string::npos) {
        return "";
    }
    
    size_t end_quote = json.find('"', start_quote + 1);
    if (end_quote == std::string::npos) {
        return "";
    }
    
    return json.substr(start_quote + 1, end_quote - start_quote - 1);
}

} // namespace Exchange
