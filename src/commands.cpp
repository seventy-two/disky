#include "commands.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace disky {

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

std::string text(const json& j, const char* key) {
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

std::vector<CommandOption> readOptions(const json& list) {
    std::vector<CommandOption> out;
    if (!list.is_array()) return out;
    for (const auto& o : list) {
        CommandOption co;
        co.type = o.value("type", opt::String);
        co.name = text(o, "name");
        co.description = text(o, "description");
        co.required = o.contains("required") && o["required"].is_boolean() && o["required"].get<bool>();
        co.autocomplete = o.contains("autocomplete") && o["autocomplete"].is_boolean() && o["autocomplete"].get<bool>();
        if (auto ch = o.find("choices"); ch != o.end() && ch->is_array())
            for (const auto& c : *ch) co.choices.push_back({text(c, "name"), c.value("value", json())});
        out.push_back(std::move(co));
    }
    // Required options come first, as Discord shows them.
    std::stable_sort(out.begin(), out.end(), [](const CommandOption& a, const CommandOption& b) { return a.required > b.required; });
    return out;
}

// A marker is a "name:" in the options' text, starting an option's value.
struct Marker {
    const CommandOption* option;
    size_t start, value;
};

std::vector<Marker> markers(const Command& c, const std::string& rest) {
    std::vector<Marker> out;
    for (size_t i = 0; i < rest.size(); i++) {
        if (i > 0 && !std::isspace((unsigned char)rest[i - 1])) continue;
        for (const auto& o : c.options) {
            size_t n = o.name.size();
            if (i + n < rest.size() && rest[i + n] == ':' && lower(rest.substr(i, n)) == lower(o.name)) {
                out.push_back({&o, i, i + n + 1});
                i += n;
                break;
            }
        }
    }
    return out;
}

// mentionId reads <@id>, <@!id>, <@&id>, <#id> or a bare ID.
std::string mentionId(std::string v) {
    if (v.size() > 2 && v.front() == '<' && v.back() == '>') {
        v = v.substr(1, v.size() - 2);
        size_t i = 0;
        while (i < v.size() && (v[i] == '@' || v[i] == '!' || v[i] == '&' || v[i] == '#')) i++;
        v = v.substr(i);
    }
    if (v.empty() || !std::all_of(v.begin(), v.end(), [](unsigned char c) { return std::isdigit(c); })) return {};
    return v;
}

}  // namespace

void ReadCommandIndex(const json& j, CommandIndex& into) {
    if (auto apps = j.find("applications"); apps != j.end() && apps->is_array())
        for (const auto& a : *apps) {
            CommandApp app{ParseId(a.value("id", json())), text(a, "name"), text(a, "icon")};
            into.apps[app.id] = app;
        }
    auto cmds = j.find("application_commands");
    if (cmds == j.end() || !cmds->is_array()) return;
    for (const auto& c : *cmds) {
        if (c.value("type", 1) != 1) continue;
        Command base;
        base.id = ParseId(c.value("id", json()));
        base.applicationId = ParseId(c.value("application_id", json()));
        base.version = text(c, "version");
        base.raw = c;
        std::string name = text(c, "name");
        auto add = [&](std::vector<std::string> parts, const std::string& desc, const json& options) {
            Command cmd = base;
            cmd.parts = std::move(parts);
            for (const auto& p : cmd.parts) cmd.path += (cmd.path.empty() ? "" : " ") + p;
            cmd.description = desc;
            cmd.options = readOptions(options);
            into.commands.push_back(std::move(cmd));
        };
        json options = c.value("options", json::array());
        bool nested = false;
        for (const auto& o : options) {
            int t = o.value("type", 0);
            if (t == opt::SubCommand) {
                nested = true;
                add({name, text(o, "name")}, text(o, "description"), o.value("options", json::array()));
            } else if (t == opt::SubCommandGroup) {
                nested = true;
                for (const auto& s : o.value("options", json::array()))
                    add({name, text(o, "name"), text(s, "name")}, text(s, "description"), s.value("options", json::array()));
            }
        }
        if (!nested) add({name}, text(c, "description"), options);
    }
}

const Command* MatchCommand(const std::vector<Command>& commands, const std::string& t, Snowflake prefer, size_t* rest) {
    const Command* best = nullptr;
    for (const auto& c : commands) {
        std::string p = "/" + c.path;
        if (t.compare(0, p.size(), p) != 0) continue;
        if (t.size() > p.size() && !std::isspace((unsigned char)t[p.size()])) continue;
        if (!best || p.size() > best->path.size() + 1 || (p.size() == best->path.size() + 1 && c.id == prefer)) best = &c;
    }
    if (best && rest) *rest = best->path.size() + 1;
    return best;
}

bool OptionAt(const Command& c, const std::string& rest, size_t cursor, std::string* name, size_t* valueStart) {
    const Marker* at = nullptr;
    auto ms = markers(c, rest);
    for (const auto& m : ms)
        if (m.value <= cursor) at = &m;
    if (!at) return false;
    *name = at->option->name;
    *valueStart = at->value;
    while (*valueStart < cursor && rest[*valueStart] == ' ') ++*valueStart;
    return true;
}

bool SplitOptions(const Command& c, const std::string& rest, std::vector<std::pair<std::string, std::string>>* values,
                  std::string* error) {
    values->clear();
    auto ms = markers(c, rest);
    std::string lead = trim(rest.substr(0, ms.empty() ? rest.size() : ms[0].start));
    if (!lead.empty()) {
        // Text before any "name:" is the main option's.
        if (c.options.empty()) {
            *error = "/" + c.path + " doesn't take anything after it.";
            return false;
        }
        const CommandOption* main = &c.options[0];  // required ones are first
        for (const auto& m : ms)
            if (m.option == main) {
                *error = "Write " + main->name + ": before its value.";
                return false;
            }
        values->push_back({main->name, lead});
    }
    for (size_t i = 0; i < ms.size(); i++) {
        size_t end = i + 1 < ms.size() ? ms[i + 1].start : rest.size();
        values->push_back({ms[i].option->name, trim(rest.substr(ms[i].value, end - ms[i].value))});
    }
    return true;
}

bool OptionsJson(const Command& c, const std::vector<std::pair<std::string, std::string>>& values,
                 const std::map<std::string, std::map<std::string, json>>& picked, const std::string& focused, json* out,
                 std::string* error) {
    json list = json::array();
    std::set<std::string> given;
    for (const auto& [name, v] : values) {
        const CommandOption* o = nullptr;
        for (const auto& x : c.options)
            if (x.name == name) o = &x;
        if (!o) continue;
        if (name == focused) {
            list.push_back({{"type", o->type}, {"name", name}, {"value", v}, {"focused", true}});
            given.insert(name);
            continue;
        }
        if (v.empty()) continue;
        json value;
        if (auto p = picked.find(name); p != picked.end() && p->second.count(v)) {
            value = p->second.at(v);
        } else if (!o->choices.empty()) {
            for (const auto& ch : o->choices)
                if (lower(ch.name) == lower(v) ||
                    lower(ch.value.is_string() ? ch.value.get<std::string>() : ch.value.dump()) == lower(v))
                    value = ch.value;
            if (value.is_null()) {
                if (!focused.empty()) continue;
                std::string names;
                for (const auto& ch : o->choices) names += (names.empty() ? "" : ", ") + ch.name;
                *error = name + " has to be one of: " + names + ".";
                return false;
            }
        } else if (o->autocomplete && !focused.empty()) {
            value = v;  // other options, sent as typed while one's completed
        } else {
            switch (o->type) {
            case opt::Integer: {
                char* end = nullptr;
                long long n = std::strtoll(v.c_str(), &end, 10);
                if (*end) {
                    if (!focused.empty()) continue;
                    *error = name + " needs a whole number.";
                    return false;
                }
                value = n;
                break;
            }
            case opt::Number: {
                char* end = nullptr;
                double n = std::strtod(v.c_str(), &end);
                if (*end) {
                    if (!focused.empty()) continue;
                    *error = name + " needs a number.";
                    return false;
                }
                value = n;
                break;
            }
            case opt::Boolean: {
                std::string b = lower(v);
                if (b == "true" || b == "yes" || b == "y" || b == "1" || b == "on") {
                    value = true;
                } else if (b == "false" || b == "no" || b == "n" || b == "0" || b == "off") {
                    value = false;
                } else {
                    if (!focused.empty()) continue;
                    *error = name + " needs true or false.";
                    return false;
                }
                break;
            }
            case opt::User:
            case opt::Channel:
            case opt::Role:
            case opt::Mentionable: {
                std::string id = mentionId(v);
                if (id.empty()) {
                    if (!focused.empty()) continue;
                    *error = name + (o->type == opt::Channel ? " needs a #channel." : o->type == opt::Role ? " needs a @role." : " needs an @mention.");
                    return false;
                }
                value = id;
                break;
            }
            case opt::Attachment:
                *error = "disky can't attach files to commands.";
                return false;
            default: value = v;
            }
        }
        list.push_back({{"type", o->type}, {"name", name}, {"value", value}});
        given.insert(name);
    }
    if (focused.empty())
        for (const auto& o : c.options)
            if (o.required && !given.count(o.name)) {
                *error = "/" + c.path + " needs " + o.name + ": " + (o.description.empty() ? "" : "(" + o.description + ")");
                if (error->back() == ' ') error->pop_back();
                return false;
            }
    // Subcommands wrap the options in their own.
    if (c.parts.size() == 3)
        list = json::array({{{"type", opt::SubCommandGroup}, {"name", c.parts[1]},
                             {"options", json::array({{{"type", opt::SubCommand}, {"name", c.parts[2]}, {"options", list}}})}}});
    else if (c.parts.size() == 2)
        list = json::array({{{"type", opt::SubCommand}, {"name", c.parts[1]}, {"options", list}}});
    *out = list;
    return true;
}

}  // namespace disky
