#include <CloverNT/Core/Modules/PluginRuntime/Bridge/AlkaBindings.hpp>

#include <fstream>
#include <print>
#include <string>
#include <string_view>

#include <Alka/Ts/Generator.hpp>

[[nodiscard]] static auto stripModuleWrapper(const std::string& dts) -> std::string {
    const auto open = dts.find("declare module");
    if (open == std::string::npos) {
        return dts;
    }
    const auto braceOpen  = dts.find('{', open);
    const auto braceClose = dts.rfind('}');
    if (braceOpen == std::string::npos || braceClose == std::string::npos || braceClose <= braceOpen) {
        return dts;
    }
    std::string       out  = dts.substr(0, open);
    const std::string body = dts.substr(braceOpen + 1, braceClose - braceOpen - 1);
    for (std::size_t start = 0; start < body.size();) {
        const auto        nl    = body.find('\n', start);
        const std::string line  = body.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        std::size_t       strip = 0;
        while (strip < 4 && strip < line.size() && line[strip] == ' ') {
            ++strip;
        }
        out += line.substr(strip);
        out += '\n';
        if (nl == std::string::npos) {
            break;
        }
        start = nl + 1;
    }
    return out;
}

int main(const int argc, char** argv) {
    if (argc < 2) {
        std::println("usage: {} <output.d.ts>", argv[0]);
        return 2;
    }
    const Alka::TsGenOptions options{.mStyle = Alka::TsStyle::EsModule};
    const std::string        dts =
            stripModuleWrapper(Alka::generateDts<CloverNT::Core::Modules::bridge::CloverntModule>(options));

    std::ofstream out(argv[1], std::ios::binary);
    if (!out) {
        std::println("GenCloverntDts: cannot open '{}' for writing\n", argv[1]);
        return 1;
    }
    out << dts;
    return out ? 0 : 1;
}
