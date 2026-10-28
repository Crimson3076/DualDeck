#include "app.h"

#include <algorithm>
#include <cmath>

namespace dualdeck::hostui {

namespace {

constexpr SDL_Color kInk{14, 19, 27, 255};
constexpr SDL_Color kPanel{22, 29, 40, 255};
constexpr SDL_Color kPanel2{30, 39, 53, 255};
constexpr SDL_Color kLine{42, 53, 70, 255};
constexpr SDL_Color kText{232, 237, 244, 255};
constexpr SDL_Color kMuted{141, 155, 176, 255};
constexpr SDL_Color kEmber{255, 148, 67, 255};
constexpr SDL_Color kOk{79, 209, 139, 255};
constexpr SDL_Color kWarn{242, 192, 77, 255};
constexpr SDL_Color kDanger{255, 107, 107, 255};
constexpr SDL_Color kDeviceIdle{95, 111, 134, 255};

constexpr float kMargin = 56.0f;
constexpr float kContentBottom = 724.0f;
constexpr float kHintsLine = 744.0f;
constexpr float kListTop = 206.0f;
constexpr float kListRowHeight = 76.0f;
constexpr float kListGap = 10.0f;

SDL_Color withAlpha(SDL_Color c, float a) {
    c.a = static_cast<Uint8>(std::clamp(a, 0.0f, 1.0f) * 255.0f);
    return c;
}

SDL_Color tagColor(TagStyle style) {
    switch (style) {
    case TagStyle::Accent: return kEmber;
    case TagStyle::Warn: return kWarn;
    case TagStyle::Ok: return kOk;
    case TagStyle::Danger: return kDanger;
    default: return kMuted;
    }
}

std::string upper(std::string s) {
    for (char& c : s) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return s;
}

float approach(float current, float target, float dt, float speed) {
    if (dt <= 0.0f) return target;
    const float t = 1.0f - std::exp(-speed * dt);
    return current + (target - current) * t;
}

struct HintSpec {
    const char* button;
    std::string label;
};

} // namespace

App::App(TextRenderer& text, ReplyFn reply) : text_(text), reply_(std::move(reply)) {}

void App::replyOnce(const std::string& text) {
    if (!awaitingReply_) return;
    awaitingReply_ = false;
    reply_(text);
}

bool App::apply(const Request& request) {
    switch (request.kind) {
    case Request::Kind::Hello:
        reply_("ready " + std::to_string(kProtocolVersion));
        return true;
    case Request::Kind::Header:
        version_ = request.header;
        return true;
    case Request::Kind::Quit:
        return false;
    case Request::Kind::Press:
        press(request.press);
        return true;
    case Request::Kind::ShowMenu: {
        const bool sameMenu = menu_ && menu_->title == request.menu.title &&
                              menu_->items.size() == request.menu.items.size();
        menu_ = request.menu;
        dialog_.reset();
        awaitingReply_ = true;
        if (!sameMenu) {
            focus_ = 0;
            homeListFocus_ = 1;
            listScroll_ = listScrollTarget_ = 0.0f;
            focusRectValid_ = false;
        }
        focus_ = std::clamp(focus_, 0, std::max(0, static_cast<int>(menu_->items.size()) - 1));
        return true;
    }
    case Request::Kind::ShowDialog:
        dialog_ = request.dialog;
        if (dialog_->kind == DialogKind::Message && dialog_->title.empty()) {
            splitHeadline(dialog_->body, dialog_->title, dialog_->body);
        }
        dialogFocus_ = dialog_->kind == DialogKind::Confirm && dialog_->danger ? 1 : 0;
        textScroll_ = 0.0f;
        dialogFade_ = 0.0f;
        awaitingReply_ = dialog_->kind != DialogKind::Busy;
        return true;
    }
    return true;
}

void App::press(Press p) {
    if (!awaitingReply_) return;
    if (dialog_) {
        switch (dialog_->kind) {
        case DialogKind::Busy:
            return;
        case DialogKind::Message:
            if (p == Press::Accept || p == Press::Back) replyOnce("ok");
            return;
        case DialogKind::TextView:
            if (p == Press::Up) textScroll_ = std::max(0.0f, textScroll_ - 120.0f);
            if (p == Press::Down) textScroll_ = std::min(textMaxScroll_, textScroll_ + 120.0f);
            if (p == Press::Accept || p == Press::Back) replyOnce("ok");
            return;
        case DialogKind::Confirm:
            if (p == Press::Left || p == Press::Up) dialogFocus_ = 0;
            if (p == Press::Right || p == Press::Down) dialogFocus_ = 1;
            if (p == Press::Accept) replyOnce(dialogFocus_ == 0 ? "yes" : "no");
            if (p == Press::Back) replyOnce("no");
            return;
        }
    }
    if (!menu_ || menu_->items.empty()) {
        if (p == Press::Back) replyOnce("cancel");
        return;
    }
    if (p == Press::Accept) {
        replyOnce(menu_->items[static_cast<size_t>(focus_)].key);
    } else if (p == Press::Back) {
        replyOnce("cancel");
    } else {
        moveFocus(p);
    }
}

void App::moveFocus(Press p) {
    const int n = static_cast<int>(menu_->items.size());
    switch (menu_->style) {
    case MenuStyle::Home:
        if (focus_ == 0) {
            if (p == Press::Right && n > 1) focus_ = std::clamp(homeListFocus_, 1, n - 1);
        } else {
            if (p == Press::Up && focus_ > 1) --focus_;
            if (p == Press::Down && focus_ < n - 1) ++focus_;
            if (p == Press::Left) {
                homeListFocus_ = focus_;
                focus_ = 0;
            }
        }
        break;
    case MenuStyle::Cards: {
        const int cols = std::min(n, 5);
        if (p == Press::Left && focus_ > 0) --focus_;
        if (p == Press::Right && focus_ < n - 1) ++focus_;
        if (p == Press::Up && focus_ - cols >= 0) focus_ -= cols;
        if (p == Press::Down && focus_ + cols < n) focus_ += cols;
        break;
    }
    case MenuStyle::List:
        if (p == Press::Up && focus_ > 0) --focus_;
        if (p == Press::Down && focus_ < n - 1) ++focus_;
        break;
    }
}

void App::pointerMoved(float x, float y) {
    if (!awaitingReply_) return;
    if (dialog_) {
        for (const Hit& h : buttonHits_) {
            if (h.rect.contains(x, y)) dialogFocus_ = h.index;
        }
        return;
    }
    for (const Hit& h : itemHits_) {
        if (h.rect.contains(x, y)) {
            focus_ = h.index;
            if (menu_ && menu_->style == MenuStyle::Home && h.index > 0) homeListFocus_ = h.index;
        }
    }
}

void App::pointerClicked(float x, float y) {
    if (!awaitingReply_) return;
    if (dialog_) {
        for (const Hit& h : buttonHits_) {
            if (h.rect.contains(x, y)) {
                dialogFocus_ = h.index;
                press(Press::Accept);
                return;
            }
        }
        return;
    }
    for (const Hit& h : itemHits_) {
        if (h.rect.contains(x, y)) {
            focus_ = h.index;
            press(Press::Accept);
            return;
        }
    }
}

void App::scroll(float amount) {
    if (dialog_ && dialog_->kind == DialogKind::TextView) {
        textScroll_ = std::clamp(textScroll_ - amount * 60.0f, 0.0f, textMaxScroll_);
    } else if (menu_ && menu_->style == MenuStyle::List && !dialog_) {
        const int n = static_cast<int>(menu_->items.size());
        focus_ = std::clamp(focus_ - static_cast<int>(amount), 0, std::max(0, n - 1));
    }
}

void App::closeRequested() {
    if (!awaitingReply_) return;
    if (dialog_) {
        replyOnce(dialog_->kind == DialogKind::Confirm ? "no" : "ok");
    } else {
        replyOnce("cancel");
    }
}

Rect App::focusTarget() const {
    if (focus_ >= 0 && static_cast<size_t>(focus_) < itemRects_.size()) return itemRects_[static_cast<size_t>(focus_)];
    return Rect{};
}

void App::update(float dt) {
    clock_ += dt;
    dialogFade_ = dialog_ ? approach(dialogFade_, 1.0f, dt, 18.0f) : 0.0f;

    // Lay out menu items so focus, hit tests and drawing agree.
    itemRects_.clear();
    if (menu_) {
        const int n = static_cast<int>(menu_->items.size());
        if (menu_->style == MenuStyle::Home && n > 0) {
            const int listCount = n - 1;
            float rowH = 0.0f, listH = 0.0f;
            if (listCount > 0) {
                rowH = std::clamp((kContentBottom - 176.0f - 10.0f * static_cast<float>(listCount - 1)) /
                                      static_cast<float>(listCount),
                                  56.0f, 76.0f);
                listH = rowH * static_cast<float>(listCount) + 10.0f * static_cast<float>(listCount - 1);
            }
            itemRects_.push_back(Rect{kMargin, 176.0f, 500.0f, std::max(listH, 400.0f)});
            for (int i = 0; i < listCount; ++i) {
                itemRects_.push_back(Rect{588.0f, 176.0f + static_cast<float>(i) * (rowH + 10.0f),
                                          kLayoutWidth - kMargin - 588.0f, rowH});
            }
        } else if (menu_->style == MenuStyle::Cards && n > 0) {
            const int cols = std::min(n, 5);
            const int rows = (n + cols - 1) / cols;
            const float gap = 18.0f;
            const float w = (kLayoutWidth - 2 * kMargin - gap * static_cast<float>(cols - 1)) / static_cast<float>(cols);
            const float h = rows == 1 ? 390.0f : (kContentBottom - 250.0f - gap * static_cast<float>(rows - 1)) /
                                                     static_cast<float>(rows);
            for (int i = 0; i < n; ++i) {
                const float lift = i == focus_ ? -8.0f : 0.0f;
                itemRects_.push_back(Rect{kMargin + static_cast<float>(i % cols) * (w + gap),
                                          250.0f + static_cast<float>(i / cols) * (h + gap) + lift, w, h});
            }
        } else if (menu_->style == MenuStyle::List) {
            // Keep the focused row inside the visible band.
            const float step = kListRowHeight + kListGap;
            const float visible = kContentBottom - kListTop;
            const float focusTop = static_cast<float>(focus_) * step;
            if (focusTop - listScrollTarget_ < 0.0f) listScrollTarget_ = focusTop;
            if (focusTop + kListRowHeight - listScrollTarget_ > visible) {
                listScrollTarget_ = focusTop + kListRowHeight - visible;
            }
            listScroll_ = approach(listScroll_, listScrollTarget_, dt, 16.0f);
            for (int i = 0; i < n; ++i) {
                itemRects_.push_back(Rect{kMargin, kListTop + static_cast<float>(i) * step - listScroll_,
                                          kLayoutWidth - 2 * kMargin, kListRowHeight});
            }
        }
    }

    itemHits_.clear();
    for (size_t i = 0; i < itemRects_.size(); ++i) {
        const Rect& rr = itemRects_[i];
        if (menu_ && menu_->style == MenuStyle::List && (rr.y < kListTop - 1.0f || rr.y + rr.h > kContentBottom + 1.0f)) {
            continue;
        }
        itemHits_.push_back(Hit{rr, static_cast<int>(i)});
    }

    const Rect target = focusTarget();
    focusRadius_ = menu_ && menu_->style != MenuStyle::List && focus_ == 0 && menu_->style == MenuStyle::Home ? 22.0f
                   : menu_ && menu_->style == MenuStyle::Cards                                               ? 20.0f
                                                                                                             : 14.0f;
    if (!focusRectValid_ || dt <= 0.0f) {
        focusRect_ = target;
        focusRectValid_ = true;
    } else {
        focusRect_.x = approach(focusRect_.x, target.x, dt, 22.0f);
        focusRect_.y = approach(focusRect_.y, target.y, dt, 22.0f);
        focusRect_.w = approach(focusRect_.w, target.w, dt, 22.0f);
        focusRect_.h = approach(focusRect_.h, target.h, dt, 22.0f);
    }
}

void App::render(SDL_Renderer* r) {
    renderBackground(r);
    renderHeader(r);
    if (menu_) {
        switch (menu_->style) {
        case MenuStyle::Home: renderHome(r); break;
        case MenuStyle::Cards: renderCards(r); break;
        case MenuStyle::List: renderList(r); break;
        }
        if (!dialog_) renderFocusRing(r);
    }
    renderHints(r);
    buttonHits_.clear();
    if (dialog_) renderDialog(r);
}

void App::renderBackground(SDL_Renderer* r) {
    SDL_SetRenderDrawColor(r, kInk.r, kInk.g, kInk.b, 255);
    SDL_RenderClear(r);
    radialGlow(r, 1100.0f, -60.0f, 900.0f, 420.0f, withAlpha(kEmber, 0.075f));
    radialGlow(r, 120.0f, 860.0f, 700.0f, 300.0f, withAlpha(SDL_Color{70, 110, 200, 255}, 0.05f));
}

void App::renderHeader(SDL_Renderer* r) {
    drawMark(r, kMargin, 40.0f, 40.0f, kEmber);
    const float wordX = kMargin + 52.0f;
    text_.draw(r, Face::Display, 26.0f, "DUALDECK", wordX, 44.0f, kText, 3.0f);
    const float w = text_.measure(Face::Display, 26.0f, "DUALDECK", 3.0f);
    text_.draw(r, Face::DisplaySemi, 26.0f, "HOST", wordX + w + 12.0f, 44.0f, kMuted, 3.0f);
    if (!version_.empty()) {
        const std::string v = version_;
        const float tw = text_.measure(Face::DisplaySemi, 17.0f, v, 1.0f);
        const Rect chip{kLayoutWidth - kMargin - tw - 26.0f, 42.0f, tw + 26.0f, 36.0f};
        strokeRoundRect(r, chip, 8.0f, 1.5f, kLine);
        text_.draw(r, Face::DisplaySemi, 17.0f, v, chip.x + 13.0f, chip.y + 7.0f, kMuted, 1.0f);
    }
}

void App::renderStatus(SDL_Renderer* r, float x, float y) {
    if (!menu_ || menu_->status.empty()) return;
    const SDL_Color c = menu_->statusOn ? kOk : kMuted;
    const std::string label = menu_->statusOn ? "Host Control on" : "Host Control off";
    const float tw = text_.measure(Face::BodyBold, 18.0f, label);
    const Rect pill{x, y, tw + 52.0f, 40.0f};
    fillRoundRect(r, pill, 20.0f, withAlpha(c, 0.12f));
    if (menu_->statusOn) fillCircle(r, x + 22.0f, y + 20.0f, 9.0f, withAlpha(c, 0.22f));
    fillCircle(r, x + 22.0f, y + 20.0f, 5.0f, c);
    text_.draw(r, Face::BodyBold, 18.0f, label, x + 36.0f, y + 9.0f, c);
    text_.draw(r, Face::Body, 17.0f, menu_->status, pill.x + pill.w + 16.0f, y + 10.0f, kMuted);
}

void App::renderHome(SDL_Renderer* r) {
    renderStatus(r, kMargin, 108.0f);
    const auto& items = menu_->items;
    if (items.empty()) return;

    // The hero: the menu's first entry, normally Play.
    const MenuItem& hero = items[0];
    const Rect h = itemRects_[0];
    const bool heroFocused = focus_ == 0;
    fillRoundRectGradient(r, h, 22.0f, heroFocused ? SDL_Color{48, 31, 20, 255} : SDL_Color{34, 27, 24, 255}, kPanel);
    text_.draw(r, Face::Display, 64.0f, upper(hero.title), h.x + 40.0f, h.y + 34.0f, kText, 4.0f);
    text_.drawWrapped(r, Face::Body, 21.0f, hero.detail, h.x + 40.0f, h.y + 130.0f, h.w - 80.0f,
                      SDL_Color{217, 199, 184, 255}, 1.4f, 4);

    // Chips sit just above the call to action; lay them out first so
    // they can grow upwards when they wrap.
    struct Placed {
        const Chip* chip;
        float x, w, nameW;
        int row;
    };
    std::vector<Placed> placed;
    float cx = 0.0f;
    int row = 0;
    const float chipsW = h.w - 80.0f;
    for (const Chip& chip : hero.chips) {
        const float nw = text_.measure(Face::DisplaySemi, 16.0f, chip.name, 1.0f);
        const float sw = chip.note.empty() ? 0.0f : text_.measure(Face::DisplaySemi, 16.0f, chip.note, 0.5f) + 8.0f;
        const float w = nw + sw + 28.0f;
        if (cx > 0.0f && cx + w > chipsW) {
            cx = 0.0f;
            ++row;
        }
        placed.push_back({&chip, cx, w, nw, row});
        cx += w + 10.0f;
    }
    const float goY = h.y + h.h - 70.0f;
    const float chipsTop = goY - 30.0f - static_cast<float>(row + 1) * 46.0f + 10.0f;
    // The two-screen mark, large and faint, in the card's open middle.
    const float markH = std::min(130.0f, chipsTop - 20.0f - (h.y + 240.0f));
    if (markH > 60.0f) {
        drawMark(r, h.x + h.w - 40.0f - markH * 0.85f, chipsTop - 20.0f - markH, markH,
                 withAlpha(kEmber, heroFocused ? 0.10f : 0.05f));
    }
    for (const Placed& p : placed) {
        const float x = h.x + 40.0f + p.x;
        const float y = chipsTop + static_cast<float>(p.row) * 46.0f;
        fillRoundRect(r, Rect{x, y, p.w, 36.0f}, 8.0f, withAlpha(SDL_Color{255, 255, 255, 255}, 0.06f));
        text_.draw(r, Face::DisplaySemi, 16.0f, p.chip->name, x + 14.0f, y + 7.0f, SDL_Color{243, 230, 219, 255}, 1.0f);
        if (!p.chip->note.empty()) {
            text_.draw(r, Face::DisplaySemi, 16.0f, p.chip->note, x + 14.0f + p.nameW + 8.0f, y + 7.0f, kMuted, 0.5f);
        }
    }
    fillCircle(r, h.x + 56.0f, goY + 16.0f, 16.0f, heroFocused ? kEmber : kPanel2);
    text_.draw(r, Face::Display, 16.0f, "A", h.x + 51.0f, goY + 5.0f, heroFocused ? kInk : kMuted);
    text_.draw(r, Face::Display, 20.0f, "CHOOSE A SYSTEM", h.x + 84.0f, goY + 3.0f, heroFocused ? kEmber : kMuted,
               2.0f);

    for (size_t i = 1; i < items.size(); ++i) {
        renderRow(r, items[i], itemRects_[i], focus_ == static_cast<int>(i));
    }
}

void App::renderRow(SDL_Renderer* r, const MenuItem& item, Rect rect, bool focused) {
    fillRoundRect(r, rect, 14.0f, focused ? kPanel2 : kPanel);
    const bool danger = item.tagStyle == TagStyle::Danger;
    const SDL_Color titleColor = danger ? kDanger : kText;
    float textX = rect.x + 22.0f;
    if (!item.icon.empty()) {
        const float box = std::min(44.0f, rect.h - 22.0f);
        const Rect ib{rect.x + 16.0f, rect.y + (rect.h - box) / 2.0f, box, box};
        fillRoundRect(r, ib, 11.0f, focused ? SDL_Color{40, 51, 68, 255} : kPanel2);
        const SDL_Color iconColor = danger ? kDanger : focused ? kEmber : kMuted;
        drawIcon(r, item.icon, ib.x + (box - 24.0f) / 2.0f, ib.y + (box - 24.0f) / 2.0f, 24.0f, iconColor);
        textX = ib.x + box + 18.0f;
    }
    float right = rect.x + rect.w - 22.0f;
    float tagWidth = 0.0f;
    if (!item.tag.empty() || item.tagStyle == TagStyle::ToggleOn || item.tagStyle == TagStyle::ToggleOff) {
        tagWidth = item.tagStyle == TagStyle::ToggleOn || item.tagStyle == TagStyle::ToggleOff
                       ? 58.0f
                       : text_.measure(Face::DisplaySemi, 14.0f, upper(item.tag), 1.2f) + 20.0f;
        renderTag(r, item, right, rect.y + rect.h / 2.0f);
        right -= tagWidth + 16.0f;
    }
    const float maxW = right - textX;
    const bool hasDetail = !item.detail.empty();
    const float titleY = hasDetail ? rect.y + rect.h / 2.0f - 25.0f : rect.y + rect.h / 2.0f - 14.0f;
    text_.drawWrapped(r, Face::DisplaySemi, 21.0f, item.title, textX, titleY, maxW, titleColor, 1.2f, 1);
    if (hasDetail) {
        text_.drawWrapped(r, Face::Body, 16.0f, item.detail, textX, titleY + 29.0f, maxW, kMuted, 1.2f, 1);
    }
}

void App::renderTag(SDL_Renderer* r, const MenuItem& item, float right, float centerY) {
    if (item.tagStyle == TagStyle::ToggleOn || item.tagStyle == TagStyle::ToggleOff) {
        const bool on = item.tagStyle == TagStyle::ToggleOn;
        const Rect track{right - 58.0f, centerY - 16.0f, 58.0f, 32.0f};
        fillRoundRect(r, track, 16.0f, on ? kOk : SDL_Color{52, 63, 80, 255});
        fillCircle(r, on ? track.x + track.w - 16.0f : track.x + 16.0f, centerY, 12.0f, on ? kInk : kMuted);
        return;
    }
    const std::string label = upper(item.tag);
    const SDL_Color c = tagColor(item.tagStyle);
    const float tw = text_.measure(Face::DisplaySemi, 14.0f, label, 1.2f);
    const Rect pill{right - tw - 20.0f, centerY - 14.0f, tw + 20.0f, 28.0f};
    fillRoundRect(r, pill, 6.0f, withAlpha(c, 0.13f));
    text_.draw(r, Face::DisplaySemi, 14.0f, label, pill.x + 10.0f, pill.y + 5.0f, c, 1.2f);
}

void App::renderCards(SDL_Renderer* r) {
    text_.draw(r, Face::Display, 40.0f, menu_->title, kMargin, 112.0f, kText, 1.5f);
    if (!menu_->subtitle.empty()) {
        text_.draw(r, Face::Body, 22.0f, menu_->subtitle, kMargin, 170.0f, kMuted);
    }
    const auto& items = menu_->items;
    for (size_t i = 0; i < items.size(); ++i) {
        const MenuItem& item = items[i];
        const Rect c = itemRects_[i];
        const bool focused = focus_ == static_cast<int>(i);
        fillRoundRectGradient(r, c, 20.0f, focused ? SDL_Color{48, 31, 20, 255} : kPanel, kPanel);
        const float artH = std::min(150.0f, c.h * 0.4f);
        const SDL_Color dev = focused ? kEmber : kDeviceIdle;
        drawDevice(r, item.icon, c.x + c.w / 2.0f, c.y + 26.0f + artH / 2.0f, dev, dev);
        const float inner = c.w - 44.0f;
        float y = c.y + 26.0f + artH + 14.0f;
        text_.drawWrapped(r, Face::Display, 25.0f, item.title, c.x + 22.0f, y, inner, kText, 1.15f, 1);
        y += 36.0f;
        text_.drawWrapped(r, Face::Body, 17.0f, item.detail, c.x + 22.0f, y, inner, kMuted, 1.35f, 3);
        if (!item.tag.empty()) {
            const std::string label = upper(item.tag);
            const SDL_Color tc = tagColor(item.tagStyle);
            const float tw = text_.measure(Face::DisplaySemi, 14.0f, label, 1.2f);
            const Rect pill{c.x + 22.0f, c.y + c.h - 50.0f, tw + 20.0f, 28.0f};
            fillRoundRect(r, pill, 6.0f, withAlpha(tc, 0.13f));
            text_.draw(r, Face::DisplaySemi, 14.0f, label, pill.x + 10.0f, pill.y + 5.0f, tc, 1.2f);
        }
    }
}

void App::renderList(SDL_Renderer* r) {
    text_.draw(r, Face::Display, 40.0f, menu_->title, kMargin, 104.0f, kText, 1.5f);
    if (!menu_->subtitle.empty()) {
        text_.drawWrapped(r, Face::Body, 19.0f, menu_->subtitle, kMargin, 158.0f, kLayoutWidth - 2 * kMargin, kMuted,
                          1.3f, 1);
    }
    const SDL_Rect clip{0, static_cast<int>(kListTop) - 8, static_cast<int>(kLayoutWidth),
                        static_cast<int>(kContentBottom - kListTop) + 16};
    SDL_SetRenderClipRect(r, &clip);
    for (size_t i = 0; i < menu_->items.size(); ++i) {
        const Rect& rr = itemRects_[i];
        if (rr.y + rr.h < kListTop - 8.0f || rr.y > kContentBottom + 8.0f) continue;
        renderRow(r, menu_->items[i], rr, focus_ == static_cast<int>(i));
    }
    SDL_SetRenderClipRect(r, nullptr);
    // Scroll cues.
    if (listScroll_ > 1.0f) {
        text_.draw(r, Face::DisplaySemi, 16.0f, "\xE2\x96\xB2  MORE", kLayoutWidth - kMargin - 80.0f, kListTop - 34.0f,
                   kMuted, 1.0f);
    }
    if (!itemRects_.empty() && itemRects_.back().y + itemRects_.back().h > kContentBottom + 1.0f) {
        text_.draw(r, Face::DisplaySemi, 16.0f, "\xE2\x96\xBC  MORE", kLayoutWidth - kMargin - 80.0f,
                   kContentBottom - 2.0f, kMuted, 1.0f);
    }
}

void App::renderFocusRing(SDL_Renderer* r) {
    if (!awaitingReply_ || itemRects_.empty()) return;
    const Rect f = focusRect_;
    if (menu_->style == MenuStyle::List && (f.y < kListTop - 10.0f || f.y + f.h > kContentBottom + 10.0f)) return;
    glow(r, f, focusRadius_, 22.0f, withAlpha(kEmber, 0.16f));
    strokeRoundRect(r, Rect{f.x - 1.5f, f.y - 1.5f, f.w + 3.0f, f.h + 3.0f}, focusRadius_ + 1.5f, 3.0f, kEmber);
}

void App::renderHints(SDL_Renderer* r) {
    std::vector<HintSpec> hints;
    bool showInputNote = false;
    if (dialog_) {
        switch (dialog_->kind) {
        case DialogKind::Busy: break;
        case DialogKind::Message: hints = {{"A", "OK"}}; break;
        case DialogKind::TextView: hints = {{"D-PAD", "Scroll"}, {"A", "Close"}}; break;
        case DialogKind::Confirm: hints = {{"A", "Select"}, {"B", dialog_->noLabel}, {"D-PAD", "Move"}}; break;
        }
    } else if (menu_) {
        const bool top = menu_->style == MenuStyle::Home;
        hints = {{"A", menu_->style == MenuStyle::Cards ? "Play" : "Select"}, {"B", top ? "Exit" : "Back"},
                 {"D-PAD", "Move"}};
        showInputNote = top;
    }
    SDL_SetRenderDrawColor(r, kLine.r, kLine.g, kLine.b, 255);
    const SDL_FRect rule{kMargin, kHintsLine, kLayoutWidth - 2 * kMargin, 1.0f};
    SDL_RenderFillRect(r, &rule);
    float x = kMargin;
    const float y = kHintsLine + 18.0f;
    for (const HintSpec& h : hints) {
        const std::string button = h.button;
        const bool wide = button.size() > 1;
        const float bw = wide ? text_.measure(Face::Display, 14.0f, button, 0.5f) + 20.0f : 32.0f;
        const Rect b{x, y, bw, 32.0f};
        fillRoundRect(r, b, wide ? 8.0f : 16.0f, kPanel2);
        strokeRoundRect(r, b, wide ? 8.0f : 16.0f, 1.5f, SDL_Color{58, 71, 91, 255});
        const float lw = text_.measure(Face::Display, wide ? 14.0f : 16.0f, button, wide ? 0.5f : 0.0f);
        text_.draw(r, Face::Display, wide ? 14.0f : 16.0f, button, x + (bw - lw) / 2.0f, y + (wide ? 7.0f : 5.0f), kText,
                   wide ? 0.5f : 0.0f);
        text_.draw(r, Face::Body, 18.0f, h.label, x + bw + 10.0f, y + 5.0f, kMuted);
        x += bw + 10.0f + text_.measure(Face::Body, 18.0f, h.label) + 30.0f;
    }
    if (showInputNote) {
        const char* note = "Mouse and keyboard work too";
        const float w = text_.measure(Face::Body, 16.0f, note);
        text_.draw(r, Face::Body, 16.0f, note, kLayoutWidth - kMargin - w, y + 6.0f, withAlpha(kMuted, 0.8f));
    }
}

void App::renderDialog(SDL_Renderer* r) {
    const Dialog& d = *dialog_;
    const float fade = dialogFade_;
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, 6, 9, 13, static_cast<Uint8>(184.0f * fade));
    SDL_RenderFillRect(r, nullptr);

    if (d.kind == DialogKind::Busy) {
        const float w = std::max(420.0f, text_.measure(Face::BodyBold, 20.0f, d.body) + 140.0f);
        const Rect box{(kLayoutWidth - w) / 2.0f, 330.0f, std::min(w, kLayoutWidth - 2 * kMargin), 110.0f};
        fillRoundRect(r, box, 22.0f, kPanel);
        strokeRoundRect(r, box, 22.0f, 1.0f, kLine);
        const float a = clock_ * 5.0f;
        arc(r, box.x + 56.0f, box.y + 55.0f, 18.0f, 0.0f, 6.2832f, 4.0f, kPanel2);
        arc(r, box.x + 56.0f, box.y + 55.0f, 18.0f, a, a + 1.9f, 4.0f, kEmber);
        text_.drawWrapped(r, Face::BodyBold, 20.0f, d.body, box.x + 96.0f, box.y + 41.0f, box.w - 120.0f, kText, 1.3f, 1);
        return;
    }

    const bool textView = d.kind == DialogKind::TextView;
    const float w = textView ? 1040.0f : 720.0f;
    const float pad = 48.0f;
    const float innerW = w - 2 * pad;
    const std::vector<std::string> titleLines = text_.wrap(Face::Display, 34.0f, d.title, innerW);
    const float titleH = static_cast<float>(titleLines.size()) * 40.0f;

    float bodyH;
    std::vector<std::string> bodyLines;
    if (textView) {
        bodyH = 470.0f - titleH;
        bodyLines = text_.wrap(Face::Body, 17.0f, d.body, innerW - 40.0f);
    } else {
        bodyLines = text_.wrap(Face::Body, 19.0f, d.body, innerW);
        // Very long output (a script's error text) gets cut with a cue
        // rather than running off the screen.
        const size_t maxLines = titleLines.size() > 1 ? 11 : 13;
        if (bodyLines.size() > maxLines) {
            bodyLines.resize(maxLines);
            bodyLines.back() += " \xE2\x80\xA6";
        }
        bodyH = static_cast<float>(bodyLines.size()) * 28.0f;
    }
    const float buttonsH = 64.0f;
    const float eyebrowH = d.kind == DialogKind::Confirm ? 28.0f : 0.0f;
    const float h = pad + eyebrowH + titleH + 16.0f + bodyH + 32.0f + buttonsH + pad - 8.0f;
    const Rect box{(kLayoutWidth - w) / 2.0f, std::max(96.0f, (kLayoutHeight - 60.0f - h) / 2.0f), w, h};
    const float slide = (1.0f - fade) * 16.0f;
    const Rect b{box.x, box.y + slide, box.w, box.h};
    fillRoundRect(r, Rect{b.x, b.y + 18.0f, b.w, b.h}, 24.0f, withAlpha(SDL_Color{0, 0, 0, 255}, 0.35f * fade));
    fillRoundRect(r, b, 24.0f, kPanel);
    strokeRoundRect(r, b, 24.0f, 1.0f, kLine);

    float y = b.y + pad;
    if (d.kind == DialogKind::Confirm) {
        text_.draw(r, Face::DisplaySemi, 16.0f, d.danger ? "ARE YOU SURE?" : "CONFIRM", b.x + pad, y,
                   d.danger ? kDanger : kEmber, 2.2f);
        y += 28.0f;
    }
    for (const std::string& line : titleLines) {
        text_.draw(r, Face::Display, 34.0f, line, b.x + pad, y, kText, 0.5f);
        y += 40.0f;
    }
    y += 16.0f;
    if (textView) {
        const Rect well{b.x + pad, y, innerW, bodyH};
        fillRoundRect(r, well, 14.0f, kInk);
        const float lineStep = 24.0f;
        textMaxScroll_ = std::max(0.0f, static_cast<float>(bodyLines.size()) * lineStep - (bodyH - 40.0f));
        textScroll_ = std::min(textScroll_, textMaxScroll_);
        const SDL_Rect clip{static_cast<int>(well.x), static_cast<int>(well.y + 12.0f), static_cast<int>(well.w),
                            static_cast<int>(well.h - 24.0f)};
        SDL_SetRenderClipRect(r, &clip);
        for (size_t i = 0; i < bodyLines.size(); ++i) {
            const float ly = well.y + 20.0f + static_cast<float>(i) * lineStep - textScroll_;
            if (ly < well.y - lineStep || ly > well.y + well.h) continue;
            text_.draw(r, Face::Body, 17.0f, bodyLines[i], well.x + 20.0f, ly, SDL_Color{200, 209, 222, 255});
        }
        SDL_SetRenderClipRect(r, nullptr);
    } else {
        for (const std::string& line : bodyLines) {
            text_.draw(r, Face::Body, 19.0f, line, b.x + pad, y, kMuted);
            y += 28.0f;
        }
    }

    // Buttons.
    const float by = b.y + b.h - pad - buttonsH + 8.0f;
    std::vector<std::pair<std::string, std::string>> buttons; // label, glyph
    if (d.kind == DialogKind::Confirm) {
        buttons = {{d.yesLabel, "A"}, {d.noLabel, "B"}};
    } else {
        buttons = {{d.kind == DialogKind::TextView ? "Close" : d.yesLabel, "A"}};
    }
    const float gap = 16.0f;
    const float bw = d.kind == DialogKind::Confirm ? (innerW - gap) / 2.0f : 240.0f;
    float bx = d.kind == DialogKind::Confirm ? b.x + pad : b.x + b.w - pad - bw;
    for (size_t i = 0; i < buttons.size(); ++i) {
        const Rect br{bx, by, bw, buttonsH};
        const bool focused = static_cast<int>(i) == dialogFocus_;
        SDL_Color fill = kPanel2;
        SDL_Color label = kText;
        if (focused) {
            fill = d.danger && i == 0 ? kDanger : kEmber;
            label = SDL_Color{26, 15, 6, 255};
        }
        if (focused) glow(r, br, 14.0f, 16.0f, withAlpha(fill, 0.25f));
        fillRoundRect(r, br, 14.0f, fill);
        const std::string text = upper(buttons[i].first);
        const float tw = text_.measure(Face::Display, 22.0f, text, 1.6f);
        const float total = 32.0f + 12.0f + tw;
        const float sx = br.x + (br.w - total) / 2.0f;
        fillCircle(r, sx + 16.0f, br.y + br.h / 2.0f, 16.0f,
                   focused ? withAlpha(SDL_Color{0, 0, 0, 255}, 0.18f) : SDL_Color{40, 51, 68, 255});
        text_.draw(r, Face::Display, 16.0f, buttons[i].second, sx + 11.0f, br.y + br.h / 2.0f - 11.0f, label);
        text_.draw(r, Face::Display, 22.0f, text, sx + 44.0f, br.y + br.h / 2.0f - 15.0f, label, 1.6f);
        buttonHits_.push_back(Hit{br, static_cast<int>(i)});
        bx += bw + gap;
    }
}

} // namespace dualdeck::hostui
