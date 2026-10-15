#include "state_json.hpp"

#include <cstdlib>
#include <iostream>

int main() {
    using hymission::dumpStateJson;
    const std::string replacement = "\xef\xbf\xbd";
    const std::string valid = "Terminal 中文 🎵";
    const std::string invalidTitle = "bad \xff title";
    const std::string invalidClass = "class \xc3(";
    const std::string truncatedName = "workspace \xe2\x82";
    const nlohmann::json overview{{"version", 1}, {"active", true}, {"phase", "active"},
        {"windows", {{{"title", invalidTitle}, {"class", invalidClass}, {"address", "0x123"}}, {{"title", valid}}}}};
    const nlohmann::json stage{{"screens", {{{"monitor", "DP-1"},
        {"cards", {{{"workspace", 1}, {"name", truncatedName}}, {{"workspace", 2}, {"name", valid}}}}}}}};
    try {
        const auto overviewText = dumpStateJson(overview);
        const auto parsedOverview = nlohmann::json::parse(overviewText);
        const auto parsedStage = nlohmann::json::parse(dumpStateJson(stage));
        if (parsedOverview["windows"].size() != 2 || parsedOverview["windows"][0]["title"] != "bad " + replacement + " title" ||
            parsedOverview["windows"][0]["class"] != "class " + replacement + "(" || parsedOverview["windows"][1]["title"] != valid ||
            parsedOverview["version"] != 1 || parsedOverview["windows"][0]["address"] != "0x123" ||
            parsedStage["screens"][0]["cards"][0]["name"] != "workspace " + replacement ||
            parsedStage["screens"][0]["cards"][1]["name"] != valid || overviewText.find(valid) == std::string::npos)
            return EXIT_FAILURE;
        // Inactive responses and valid escaped/control bytes retain their schema.
        const nlohmann::json inactive{{"active", false}, {"windows", nlohmann::json::array()}};
        const nlohmann::json escaped{{"title", std::string("x\0\n\"", 4)}};
        if (nlohmann::json::parse(dumpStateJson(inactive)) != inactive || nlohmann::json::parse(dumpStateJson(escaped)) != escaped)
            return EXIT_FAILURE;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "PASS: invalid UTF-8 state fields are replaced while schema and valid Unicode survive\n";
}
