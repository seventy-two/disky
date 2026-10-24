// Slash commands: the apps' commands Discord lists for a server or DM, and
// reading a command typed in the composer, as "/name option: value …".
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "store.h"

namespace disky {

// Option types, as Discord numbers them.
namespace opt {
constexpr int SubCommand = 1, SubCommandGroup = 2, String = 3, Integer = 4, Boolean = 5, User = 6, Channel = 7, Role = 8,
              Mentionable = 9, Number = 10, Attachment = 11;
}

struct CommandChoice {
    std::string name;
    json value;
};

struct CommandOption {
    int type = opt::String;
    std::string name, description;
    bool required = false, autocomplete = false;
    std::vector<CommandChoice> choices;
};

// Command is one thing that can be run: a command, or one of its
// subcommands, whose path is then "name group sub".
struct Command {
    Snowflake id = 0, applicationId = 0;
    std::string version, path, description;
    std::vector<std::string> parts;      // the path's words
    std::vector<CommandOption> options;  // the runnable part's
    json raw;                            // the whole command, sent back when it's run
};

struct CommandApp {
    Snowflake id = 0;
    std::string name, icon;
};

// CommandIndex is what's been read of an application-command-index.
struct CommandIndex {
    std::vector<Command> commands;
    std::unordered_map<Snowflake, CommandApp> apps;
};

// ReadCommandIndex adds an application-command-index response's slash
// commands (not context menu ones or activities) to an index.
void ReadCommandIndex(const json& j, CommandIndex& into);

// MatchCommand finds the command text starts with ("/path", then a space
// or the end), the longest path winning; prefer picks between commands of
// the same path. rest is set to where the options start.
const Command* MatchCommand(const std::vector<Command>& commands, const std::string& text, Snowflake prefer, size_t* rest);

// OptionAt finds the option being written at a cursor in the options'
// text: its name and where its value starts. False if there's none, e.g.
// before the first "name:".
bool OptionAt(const Command& c, const std::string& rest, size_t cursor, std::string* name, size_t* valueStart);

// SplitOptions reads "a: 1 b: two words" into option values, by name. With
// no "name:" at all, the whole text is the first required option's (or the
// only option's), so "/weather Bristol" works. Errors read as sentences.
bool SplitOptions(const Command& c, const std::string& rest, std::vector<std::pair<std::string, std::string>>* values,
                  std::string* error);

// OptionsJson builds an interaction's options from typed values: numbers,
// booleans, choices by name or value, and mentions (<@id>, <#id>, <@&id>, or
// a bare ID). picked gives values chosen from suggestions, by option name
// then the name shown. With focused set, that option's value is sent as
// typed and marked focused, for autocomplete, and nothing is required.
bool OptionsJson(const Command& c, const std::vector<std::pair<std::string, std::string>>& values,
                 const std::map<std::string, std::map<std::string, json>>& picked, const std::string& focused, json* out,
                 std::string* error);

}  // namespace disky
