#include "protocol.h"
#include "test_framework.h"

using namespace dualdeck::hostui;

MDR_TEST(host_ui_unescapes_newlines_tabs_and_backslashes) {
    MDR_CHECK_EQ(unescapeField("a\\nb\\tc\\\\d"), std::string("a\nb\tc\\d"));
    // Unknown escapes and a trailing backslash pass through.
    MDR_CHECK_EQ(unescapeField("C:\\x\\"), std::string("C:\\x\\"));
}

MDR_TEST(host_ui_builds_a_menu_across_lines) {
    RequestParser parser;
    MDR_CHECK(!parser.feed("menu\thome\tDualDeck Host\t").has_value());
    MDR_CHECK(!parser.feed("status\ton\tA Deck can connect any time.").has_value());
    MDR_CHECK(!parser.feed("item\tlaunch\tPlay\tLaunch an emulator\t\t\t\tDS|melonDS,DESKTOP").has_value());
    MDR_CHECK(!parser.feed("item\tsteam-remove\tRemove\tUninstall\ttrash\t\tdanger\t").has_value());
    const auto request = parser.feed("end");
    MDR_CHECK(request.has_value());
    if (!request) return;
    MDR_CHECK(request->kind == Request::Kind::ShowMenu);
    const Menu& menu = request->menu;
    MDR_CHECK(menu.style == MenuStyle::Home);
    MDR_CHECK(menu.statusOn);
    MDR_CHECK_EQ(menu.status, std::string("A Deck can connect any time."));
    MDR_CHECK_EQ(menu.items.size(), static_cast<size_t>(2));
    if (menu.items.size() != 2) return;
    MDR_CHECK_EQ(menu.items[0].key, std::string("launch"));
    MDR_CHECK_EQ(menu.items[0].chips.size(), static_cast<size_t>(2));
    MDR_CHECK_EQ(menu.items[0].chips[0].note, std::string("melonDS"));
    MDR_CHECK_EQ(menu.items[0].chips[1].name, std::string("DESKTOP"));
    MDR_CHECK(menu.items[1].tagStyle == TagStyle::Danger);
    MDR_CHECK_EQ(menu.items[1].icon, std::string("trash"));
}

MDR_TEST(host_ui_end_without_menu_is_ignored) {
    RequestParser parser;
    MDR_CHECK(!parser.feed("end").has_value());
    MDR_CHECK(!parser.feed("item\tx\tX").has_value());
}

MDR_TEST(host_ui_other_request_abandons_half_built_menu) {
    RequestParser parser;
    parser.feed("menu\tlist\tAdvanced\t");
    const auto busy = parser.feed("busy\tWorking");
    MDR_CHECK(busy.has_value() && busy->dialog.kind == DialogKind::Busy);
    MDR_CHECK(!parser.feed("end").has_value());
}

MDR_TEST(host_ui_parses_dialogs) {
    RequestParser parser;
    const auto confirm = parser.feed("confirm\tRemove?\tLine one\\nLine two\tRemove\tKeep\t1");
    MDR_CHECK(confirm.has_value());
    if (!confirm) return;
    MDR_CHECK(confirm->dialog.kind == DialogKind::Confirm);
    MDR_CHECK_EQ(confirm->dialog.body, std::string("Line one\nLine two"));
    MDR_CHECK_EQ(confirm->dialog.yesLabel, std::string("Remove"));
    MDR_CHECK_EQ(confirm->dialog.noLabel, std::string("Keep"));
    MDR_CHECK(confirm->dialog.danger);

    const auto plain = parser.feed("confirm\tSure?\tBody");
    MDR_CHECK(plain.has_value() && plain->dialog.yesLabel == "Yes" && plain->dialog.noLabel == "No" &&
              !plain->dialog.danger);

    const auto message = parser.feed("message\tDone\tUpdated.");
    MDR_CHECK(message.has_value() && message->dialog.kind == DialogKind::Message);
}

MDR_TEST(host_ui_parses_control_requests) {
    RequestParser parser;
    MDR_CHECK(parser.feed("hello")->kind == Request::Kind::Hello);
    MDR_CHECK(parser.feed("quit")->kind == Request::Kind::Quit);
    const auto header = parser.feed("header\tv0.1.183");
    MDR_CHECK(header.has_value() && header->header == "v0.1.183");
    const auto press = parser.feed("press\tdown");
    MDR_CHECK(press.has_value() && press->press == Press::Down);
    MDR_CHECK(!parser.feed("press\tsideways").has_value());
    MDR_CHECK(!parser.feed("something-new\tfrom a newer script").has_value());
}

MDR_TEST(host_ui_splits_message_headlines) {
    std::string title, body;
    splitHeadline("Added DualDeck Host to Steam. Restart Steam to see it.", title, body);
    MDR_CHECK_EQ(title, std::string("Added DualDeck Host to Steam"));
    MDR_CHECK_EQ(body, std::string("Restart Steam to see it."));

    splitHeadline("Couldn't turn on always-on Host Control:\n\nsystemd said no", title, body);
    MDR_CHECK_EQ(title, std::string("Couldn't turn on always-on Host Control"));
    MDR_CHECK_EQ(body, std::string("systemd said no"));

    splitHeadline("Removed.", title, body);
    MDR_CHECK_EQ(title, std::string("Removed"));
    MDR_CHECK(body.empty());

    // Version numbers aren't sentence ends.
    splitHeadline("Updated to v0.1.184.", title, body);
    MDR_CHECK_EQ(title, std::string("Updated to v0.1.184"));

    std::string text(120, 'x');
    splitHeadline(text, title, body);
    MDR_CHECK_EQ(title, std::string("DualDeck Host"));
    MDR_CHECK_EQ(body, text);

    // body may be the same string as text.
    std::string same = "First. Second.";
    splitHeadline(same, title, same);
    MDR_CHECK_EQ(title, std::string("First"));
    MDR_CHECK_EQ(same, std::string("Second."));
}
