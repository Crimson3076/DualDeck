#include "protocol.h"

namespace dualdeck::hostui {

std::string unescapeField(const std::string& field) {
    std::string out;
    out.reserve(field.size());
    for (size_t i = 0; i < field.size(); ++i) {
        const char c = field[i];
        if (c != '\\' || i + 1 == field.size()) {
            out += c;
            continue;
        }
        const char next = field[++i];
        if (next == 'n') {
            out += '\n';
        } else if (next == 't') {
            out += '\t';
        } else if (next == '\\') {
            out += '\\';
        } else {
            out += '\\';
            out += next;
        }
    }
    return out;
}

std::vector<std::string> splitFields(const std::string& line) {
    std::vector<std::string> fields;
    std::string current;
    for (const char c : line) {
        if (c == '\t') {
            fields.push_back(unescapeField(current));
            current.clear();
        } else if (c != '\r' && c != '\n') {
            current += c;
        }
    }
    fields.push_back(unescapeField(current));
    return fields;
}

TagStyle parseTagStyle(const std::string& name) {
    if (name == "accent") return TagStyle::Accent;
    if (name == "warn") return TagStyle::Warn;
    if (name == "ok") return TagStyle::Ok;
    if (name == "danger") return TagStyle::Danger;
    if (name == "on") return TagStyle::ToggleOn;
    if (name == "off") return TagStyle::ToggleOff;
    return TagStyle::None;
}

void splitHeadline(const std::string& text, std::string& title, std::string& body) {
    // The script's messages are plain sentences ("Added DualDeck Host to
    // Steam. Restart Steam to see it."); the first one becomes the title.
    constexpr size_t kMaxTitle = 90;
    size_t cut = std::string::npos;
    size_t skip = 0;
    for (size_t i = 0; i < text.size() && i < kMaxTitle; ++i) {
        const char c = text[i];
        const char next = i + 1 < text.size() ? text[i + 1] : '\0';
        if (c == '\n') {
            cut = i;
            skip = 1;
            break;
        }
        if ((c == '.' || c == ':' || c == '!') && (next == ' ' || next == '\n' || next == '\0')) {
            cut = i;
            skip = 1;
            break;
        }
    }
    std::string head;
    std::string rest;
    if (cut == std::string::npos) {
        if (text.size() > kMaxTitle) {
            title = "DualDeck Host";
            body = text;
            return;
        }
        head = text;
    } else {
        head = text.substr(0, cut);
        rest = text.substr(std::min(text.size(), cut + skip));
    }
    const size_t start = rest.find_first_not_of(" \n");
    rest = start == std::string::npos ? std::string() : rest.substr(start);
    title = head;
    body = rest;
}

namespace {

std::string field(const std::vector<std::string>& fields, size_t index) {
    return index < fields.size() ? fields[index] : std::string();
}

std::vector<Chip> parseChips(const std::string& text) {
    std::vector<Chip> chips;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find(',', start);
        if (end == std::string::npos) end = text.size();
        const std::string entry = text.substr(start, end - start);
        if (!entry.empty()) {
            const size_t bar = entry.find('|');
            if (bar == std::string::npos) {
                chips.push_back({entry, ""});
            } else {
                chips.push_back({entry.substr(0, bar), entry.substr(bar + 1)});
            }
        }
        start = end + 1;
    }
    return chips;
}

std::optional<Press> parsePress(const std::string& name) {
    if (name == "up") return Press::Up;
    if (name == "down") return Press::Down;
    if (name == "left") return Press::Left;
    if (name == "right") return Press::Right;
    if (name == "a") return Press::Accept;
    if (name == "b") return Press::Back;
    return std::nullopt;
}

} // namespace

std::optional<Request> RequestParser::feed(const std::string& line) {
    const std::vector<std::string> fields = splitFields(line);
    const std::string& cmd = fields[0];
    Request request;

    if (cmd == "menu") {
        Menu menu;
        const std::string style = field(fields, 1);
        menu.style = style == "home" ? MenuStyle::Home : style == "cards" ? MenuStyle::Cards : MenuStyle::List;
        menu.title = field(fields, 2);
        menu.subtitle = field(fields, 3);
        pending_ = std::move(menu);
        return std::nullopt;
    }
    if (cmd == "status") {
        if (pending_) {
            pending_->statusOn = field(fields, 1) == "on";
            pending_->status = field(fields, 2);
        }
        return std::nullopt;
    }
    if (cmd == "item") {
        if (pending_ && !field(fields, 1).empty()) {
            MenuItem item;
            item.key = field(fields, 1);
            item.title = field(fields, 2);
            item.detail = field(fields, 3);
            item.icon = field(fields, 4);
            item.tag = field(fields, 5);
            item.tagStyle = parseTagStyle(field(fields, 6));
            item.chips = parseChips(field(fields, 7));
            pending_->items.push_back(std::move(item));
        }
        return std::nullopt;
    }
    if (cmd == "end") {
        if (!pending_) return std::nullopt;
        request.kind = Request::Kind::ShowMenu;
        request.menu = std::move(*pending_);
        pending_.reset();
        return request;
    }

    // Anything else abandons a half-built menu.
    pending_.reset();
    if (cmd == "hello") {
        request.kind = Request::Kind::Hello;
        return request;
    }
    if (cmd == "header") {
        request.kind = Request::Kind::Header;
        request.header = field(fields, 1);
        return request;
    }
    if (cmd == "quit") {
        request.kind = Request::Kind::Quit;
        return request;
    }
    if (cmd == "press") {
        const auto press = parsePress(field(fields, 1));
        if (!press) return std::nullopt;
        request.kind = Request::Kind::Press;
        request.press = *press;
        return request;
    }
    if (cmd == "message" || cmd == "confirm" || cmd == "textview" || cmd == "busy") {
        request.kind = Request::Kind::ShowDialog;
        Dialog& dialog = request.dialog;
        if (cmd == "busy") {
            dialog.kind = DialogKind::Busy;
            dialog.body = field(fields, 1);
            return request;
        }
        dialog.title = field(fields, 1);
        dialog.body = field(fields, 2);
        if (cmd == "message") {
            dialog.kind = DialogKind::Message;
        } else if (cmd == "textview") {
            dialog.kind = DialogKind::TextView;
        } else {
            dialog.kind = DialogKind::Confirm;
            dialog.yesLabel = field(fields, 3).empty() ? "Yes" : field(fields, 3);
            dialog.noLabel = field(fields, 4).empty() ? "No" : field(fields, 4);
            dialog.danger = field(fields, 5) == "1";
        }
        return request;
    }
    return std::nullopt;
}

} // namespace dualdeck::hostui
