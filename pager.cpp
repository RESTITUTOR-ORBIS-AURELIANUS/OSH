#include "include/adaptive.hpp"
#include <cctype>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <set>
#include <sstream>
#include <unordered_set>

struct Options {
    std::vector<std::string> algorithms = {"adaptive"};
    std::vector<std::size_t> frames = {5, 10, 20, 50, 100};
    std::vector<std::string> files;
    std::string format = "table";
    int base = 16;
    double window = 32, temperature = 4, half_life = 16, switch_cost = 0.5;
    bool audit = false;
};
const std::vector<std::string> all_algorithms = {
    "fifo", "lru", "clock", "lfu", "mru", "arc", "decay", "predict", "adaptive", "opt"
};
std::vector<std::string> split(const std::string& input) {
    std::vector<std::string> result;
    std::istringstream stream(input);
    std::string part;
    while (std::getline(stream, part, ',')) {
        if (part.empty()) throw std::invalid_argument("empty comma-separated item");
        result.push_back(part);
    }
    if (result.empty() || input.back() == ',') throw std::invalid_argument("empty option value");
    return result;
}
std::uint64_t parse_integer(const std::string& token, int base) {
    std::size_t start = 0;
    if (base == 16 && token.size() > 2 && token[0] == '0' && (token[1] == 'x' || token[1] == 'X')) start = 2;
    if (start == token.size()) throw std::invalid_argument("empty number");
    std::uint64_t value = 0;
    for (std::size_t i = start; i < token.size(); ++i) {
        unsigned char ch = static_cast<unsigned char>(token[i]);
        int digit = ch >= '0' && ch <= '9' ? ch - '0' :
                    ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 :
                    ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
        if (digit < 0 || digit >= base) throw std::invalid_argument("invalid digit");
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / base)
            throw std::invalid_argument("integer exceeds 64 bits");
        value = value * base + digit;
    }
    return value;
}
double positive_double(const std::string& s) {
    std::size_t pos;
    double d = std::stod(s, &pos);
    if (pos != s.size() || !std::isfinite(d) || d <= 0) throw std::invalid_argument("expected positive finite number");
    return d;
}
void usage() {
    std::cout << "Page replacement challenge (C++17)\n"
        "Usage: pager [options] TRACE [TRACE ...]\n"
        "  --algorithm adaptive|all|fifo,lru,...  (default: adaptive)\n"
        "  --frames 5,10,20,50,100              (default: these five)\n"
        "  --format table|csv                   (default: table)\n"
        "  --base 16|10                         (default: 16; input is PAGE IDs)\n"
        "  --window NUMBER                      (adaptive loss half-life / frames; default 32)\n"
        "  --temperature NUMBER                 (expert voting softness; default 4)\n"
        "  --switch-cost NUMBER                 (switch advantage / frames; default 0.5)\n"
        "  --half-life NUMBER                   (decay half-life / frames; default 16)\n"
        "  --audit                              (check demand-paging invariants)\n"
        "  --help\n"
        "Use '-' for stdin. Online policies consume each reference once, in order.\n"
        "OPT/MIN is an OFFLINE lower bound requiring future references.\n";
}
Options parse_options(int argc, char** argv) {
    Options o;
    bool positional = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto value = [&]() { if (++i >= argc) throw std::invalid_argument("missing value for " + a); return std::string(argv[i]); };
        if (a == "--" && !positional) { positional = true; continue; }
        if (positional || a == "-" || a.empty() || a[0] != '-') o.files.push_back(a);
        else if (a == "--algorithm") {
            std::string v = value(); o.algorithms = v == "all" ? all_algorithms : split(v);
            for (auto& name : o.algorithms) {
                if (name == "min") name = "opt";
                if (name == "second-chance") name = "clock";
                if (std::find(all_algorithms.begin(), all_algorithms.end(), name) == all_algorithms.end())
                    throw std::invalid_argument("unknown algorithm: " + name);
            }
            std::set<std::string> unique(o.algorithms.begin(), o.algorithms.end());
            if (unique.size() != o.algorithms.size()) throw std::invalid_argument("duplicate algorithm name");
        } else if (a == "--frames") {
            o.frames.clear();
            for (const auto& token : split(value())) {
                auto n = parse_integer(token, 10);
                if (!n || n > 1000000) throw std::invalid_argument("frames must be between 1 and 1000000");
                o.frames.push_back(static_cast<std::size_t>(n));
            }
        } else if (a == "--format") {
            o.format = value(); if (o.format != "csv" && o.format != "table") throw std::invalid_argument("format must be table or csv");
        } else if (a == "--base") {
            auto v = value(); if (v != "16" && v != "10") throw std::invalid_argument("base must be 16 or 10"); o.base = std::stoi(v);
        } else if (a == "--window") o.window = positive_double(value());
        else if (a == "--temperature") o.temperature = positive_double(value());
        else if (a == "--switch-cost") o.switch_cost = positive_double(value());
        else if (a == "--half-life") o.half_life = positive_double(value());
        else if (a == "--audit") o.audit = true;
        else throw std::invalid_argument("unknown option: " + a);
    }
    if (o.files.empty()) throw std::invalid_argument("no trace file supplied; see --help");
    if (std::count(o.files.begin(), o.files.end(), "-") > 1) throw std::invalid_argument("stdin may only be consumed once");
    return o;
}
std::unique_ptr<Policy> make_policy(const std::string& name, std::size_t c, const Options& o) {
    if (name == "fifo") return std::make_unique<FifoPolicy>(c);
    if (name == "lru") return std::make_unique<LruPolicy>(c);
    if (name == "clock") return std::make_unique<ClockPolicy>(c);
    if (name == "lfu") return std::make_unique<LfuPolicy>(c);
    if (name == "mru") return std::make_unique<MruPolicy>(c);
    if (name == "arc") return std::make_unique<ArcPolicy>(c);
    if (name == "decay") return std::make_unique<DecayPolicy>(c, o.half_life);
    if (name == "predict") return std::make_unique<PredictivePolicy>(c);
    if (name == "adaptive") return std::make_unique<AdaptivePolicy>(c, o.window, o.temperature, o.switch_cost);
    return nullptr;
}
struct Run {
    std::string name;
    std::size_t capacity;
    std::unique_ptr<Policy> policy;
    std::uint64_t faults = 0;
};
bool audited_access(Policy& p, Page page, std::size_t capacity) {
    const auto before = p.resident_pages();
    const bool expected_fault = std::find(before.begin(), before.end(), page) == before.end();
    const bool fault = p.access(page);
    const auto after = p.resident_pages();
    std::unordered_set<Page> state(after.begin(), after.end());
    if (fault != expected_fault || after.size() > capacity || state.size() != after.size() || !state.count(page))
        throw std::logic_error("resident/fault/capacity invariant failed");
    for (Page q : after)
        if (q != page && std::find(before.begin(), before.end(), q) == before.end())
            throw std::logic_error("prefetch invariant failed");
    if ((!fault && after.size() != before.size()) ||
        (fault && after.size() != std::min(capacity, before.size() + 1)))
        throw std::logic_error("demand-loading invariant failed");
    return fault;
}
std::string csv_quote(const std::string& s) {
    std::string result = "\"";
    for (char c : s) { if (c == '"') result += '"'; result += c; }
    return result + "\"";
}
int main(int argc, char** argv) {
    try {
        for (int i = 1; i < argc; ++i) if (std::string(argv[i]) == "--help") { usage(); return 0; }
        if (argc == 1) { usage(); return 0; }
        const Options o = parse_options(argc, argv);
        if (o.format == "csv") std::cout << "trace,algorithm,frames,references,faults,fault_rate\n";
        if (o.format == "table") std::cout << "Algorithm          Frames    References        Faults    Fault rate\n";
        std::unordered_map<std::string, long double> total_faults;
        std::size_t total_cases = 0;
        for (const auto& file : o.files) {
            std::ifstream input;
            if (file != "-") { input.open(file); if (!input) throw std::runtime_error("cannot open: " + file); }
            std::istream& stream = file == "-" ? std::cin : input;
            std::vector<Run> runs;
            bool needs_opt = false;
            for (const auto& a : o.algorithms) for (auto c : o.frames) {
                runs.push_back({a, c, make_policy(a, c, o), 0}); if (a == "opt") needs_opt = true;
            }
            std::vector<Page> offline_trace;
            std::uint64_t count = 0;
            std::string token;
            while (stream >> token) {
                if (count == 0 && token.compare(0, 3, "\xef\xbb\xbf") == 0) token.erase(0, 3);
                Page page;
                try { page = parse_integer(token, o.base); }
                catch (const std::exception& e) { throw std::runtime_error(file + ": token " + std::to_string(count + 1) + " (" + token + "): " + e.what()); }
                for (auto& run : runs) if (run.policy)
                    run.faults += o.audit ? audited_access(*run.policy, page, run.capacity) : run.policy->access(page);
                if (needs_opt) offline_trace.push_back(page);
                ++count;
            }
            if (stream.bad()) throw std::runtime_error("I/O error: " + file);
            if (!count) throw std::runtime_error("empty trace: " + file);
            if (o.format == "table") std::cout << "\n" << file << '\n';
            std::unordered_map<std::string, long double> averages;
            for (auto& run : runs) {
                if (!run.policy) run.faults = optimal_faults(offline_trace, run.capacity);
                const double rate = static_cast<double>(run.faults) / count;
                if (o.format == "csv") std::cout << csv_quote(file) << ',' << run.name << ',' << run.capacity << ',' << count << ',' << run.faults << ',' << std::setprecision(10) << rate << '\n';
                else std::cout << std::left << std::setw(17) << (run.name == "opt" ? "opt (OFFLINE)" : run.name) << std::right << std::setw(8) << run.capacity << std::setw(14) << count << std::setw(14) << run.faults << std::setw(13) << std::fixed << std::setprecision(4) << 100 * rate << "%\n";
                averages[run.name] += run.faults;
                total_faults[run.name] += run.faults;
            }
            if (o.format == "table") for (const auto& a : o.algorithms)
                std::cout << "Mean faults [" << a << "] over " << o.frames.size() << " frame counts: " << std::fixed << std::setprecision(2) << averages[a] / o.frames.size() << '\n';
            total_cases += o.frames.size();
        }
        if (o.format == "table" && o.files.size() > 1) for (const auto& a : o.algorithms)
            std::cout << "Overall mean faults [" << a << "] over " << total_cases << " (trace,frames) cases: " << std::fixed << std::setprecision(2) << total_faults[a] / total_cases << '\n';
        return 0;
    } catch (const std::exception& e) { std::cerr << "error: " << e.what() << '\n'; return 1; }
}
