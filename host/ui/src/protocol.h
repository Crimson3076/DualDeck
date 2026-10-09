#pragma once

// The line protocol packaging/host/dualdeck-host.sh uses to drive
// dualdeck-host-ui. The script keeps all of the menu logic (what each
// entry does, which internal/ script it runs); this program only draws
// what it's told and answers with what the user picked. One request per
// line on stdin, fields separated by tabs; text fields may contain the
// escapes \n, \t and \\.
//
//   hello                                    -> "ready 1"
//   header <version>                         (no reply)
//   menu <style> <title> <subtitle>          starts a menu; style is
//                                            home, cards or list
//   status <on|off> <text>                   (optional) status pill
//   item <key> <title> <detail> <icon> <tag> <tag-style> <chips>
//                                            one entry; chips is a
//                                            comma list of NAME|NOTE
//   end                                      -> chosen key, or "cancel"
//   message <title> <body>                   -> "ok"
//   confirm <title> <body> <yes> <no> <danger 0|1>
//                                            -> "yes" or "no"
//   textview <title> <body>                  -> "ok"
//   busy <text>                              spinner until the next request
//   press <up|down|left|right|a|b>           (tests) simulated input
//   quit                                     exits
//
// Kept free of SDL so it can be unit tested on its own.

#include <optional>
#include <string>
#include <vector>

namespace dualdeck::hostui {

inline constexpr int kProtocolVersion = 1;

enum class MenuStyle { Home, Cards, List };

enum class TagStyle { None, Accent, Warn, Ok, Danger, ToggleOn, ToggleOff };

struct Chip {
    std::string name;
    std::string note;
};

struct MenuItem {
    std::string key;
    std::string title;
    std::string detail;
    std::string icon;
    std::string tag;
    TagStyle tagStyle = TagStyle::None;
    std::vector<Chip> chips;
};

struct Menu {
    MenuStyle style = MenuStyle::List;
    std::string title;
    std::string subtitle;
    // Empty when the menu has no status pill.
    std::string status;
    bool statusOn = false;
    std::vector<MenuItem> items;
};

enum class DialogKind { Message, Confirm, TextView, Busy };

struct Dialog {
    DialogKind kind = DialogKind::Message;
    std::string title;
    std::string body;
    std::string yesLabel = "OK";
    std::string noLabel;
    bool danger = false;
};

enum class Press { Up, Down, Left, Right, Accept, Back };

// What one complete request asks for once parsed.
struct Request {
    enum class Kind { Hello, Header, ShowMenu, ShowDialog, Press, Quit } kind = Kind::Hello;
    std::string header;
    Menu menu;
    Dialog dialog;
    Press press = Press::Accept;
};

// Undoes the \n, \t and \\ escapes.
std::string unescapeField(const std::string& field);
std::vector<std::string> splitFields(const std::string& line);
TagStyle parseTagStyle(const std::string& name);
// Splits a message the script sent as plain sentences into a short
// title (its first sentence or line) and the rest. text may alias body.
void splitHeadline(const std::string& text, std::string& title, std::string& body);

// Feeds lines one at a time; a menu spans several lines (menu, status,
// item..., end), everything else is one line. Unknown or malformed lines
// are ignored so an older UI can run a newer script.
class RequestParser {
public:
    std::optional<Request> feed(const std::string& line);

private:
    std::optional<Menu> pending_;
};

} // namespace dualdeck::hostui
