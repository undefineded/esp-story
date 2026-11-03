#include <cstdio>
#include <cstring>

#include "app_context.h"
#include "app_page.h"
#include "app_services.h"
#include "app_theme.h"
#include "display.h"
#include "fonts.h"
#include "parental_binding.h"

namespace App {
namespace {

constexpr int kVisibleRows = 4;
constexpr int kRowY = 34;
constexpr int kRowHeight = 44;
constexpr int kParentalItems = 9;

enum class PinPurpose { Verify, SetPin };

void pinEntryReset(PinPurpose purpose);

const char* minutesText(uint16_t minutes) {
    if (!minutes) return "不限";
    char buffer[24];
    std::snprintf(buffer, sizeof(buffer), "%u 分钟", static_cast<unsigned>(minutes));
    // Callers only use this inside draw(), so a rotating static is safe.
    static char texts[4][24];
    static int slot = 0;
    slot = (slot + 1) % 4;
    std::memcpy(texts[slot], buffer, sizeof(buffer));
    return texts[slot];
}

const char* clockText(uint16_t minute) {
    static char texts[4][8];
    static int slot = 0;
    slot = (slot + 1) % 4;
    std::snprintf(texts[slot], sizeof(texts[slot]), "%02u:%02u",
                  static_cast<unsigned>(minute / 60), static_cast<unsigned>(minute % 60));
    return texts[slot];
}

uint16_t cycleDailyLimit(uint16_t current, int direction) {
    static const uint16_t kOptions[] = {0, 15, 30, 45, 60, 90, 120};
    constexpr int kCount = static_cast<int>(sizeof(kOptions) / sizeof(kOptions[0]));
    int index = 0;
    for (int i = 0; i < kCount; ++i) {
        if (kOptions[i] == current) index = i;
    }
    index = (index + (direction > 0 ? 1 : -1) + kCount) % kCount;
    return kOptions[index];
}

void drawRow(lgfx::LGFXBase& g, int y, const char* label, const char* value, bool selected) {
    const auto& c = Theme::colors();
    const uint16_t background = selected ? c.yellow : TFT_WHITE;
    if (selected) g.fillRoundRect(13, y + 3, 300, kRowHeight, 6, c.ink);
    g.fillRoundRect(10, y, 300, kRowHeight, 6, selected ? c.ink : TFT_WHITE);
    g.fillRoundRect(12, y + 2, 296, kRowHeight - 4, 5, background);
    if (!selected) g.drawRoundRect(10, y, 300, kRowHeight, 6, c.track_border);
    g.setFont(Fonts::body());
    g.setTextColor(c.text, background);
    g.drawString(label, 24, y + 11);
    g.drawRightString(value, 294, y + 11);
}

void drawTopBar(lgfx::LGFXBase& g, const char* title) {
    const auto& c = Theme::colors();
    Theme::setTopBarColor(c.coral);
    g.fillRect(0, 0, 320, 30, c.coral);
    g.fillRect(0, 30, 320, 2, c.coral_dark);
    Theme::drawSmileFace(g, 19, 15, 10);
    g.setFont(Fonts::body());
    g.setTextColor(TFT_WHITE, c.coral);
    g.drawString("小小故事盒", 36, 7);
    g.drawRightString(title, 310, 7);
}

// Four..six digit enter. The dial changes the current digit, OK appends it and
// advances, SET short deletes, OK long submits.
class PinEntryPage final : public Page {
public:
    const char* id() const override { return "pin"; }

    void begin(PinPurpose purpose) {
        purpose_ = purpose;
        digit_ = 0;
        length_ = 0;
        message_[0] = '\0';
        std::memset(digits_, 0, sizeof(digits_));
    }

    void enter() override { invalidate(); }

    void handle(const Event& event) override {
        if (event.delta) {
            digit_ = (digit_ + event.delta % 10 + 10) % 10;
            invalidate();
            return;
        }
        switch (event.key) {
            case Key::Ok:
                if (length_ < static_cast<int>(sizeof(digits_))) {
                    digits_[length_++] = static_cast<char>('0' + digit_);
                    digit_ = 0;
                    invalidate();
                }
                break;
            case Key::OkLong:
                submit();
                break;
            case Key::Set:
                if (length_ > 0) --length_;
                invalidate();
                break;
            case Key::SetLong:
            case Key::OkDouble:
                popPage();
                break;
            default:
                break;
        }
    }

    void draw() override {
        auto& g = Display::canvas();
        const auto& c = Theme::colors();
        g.fillScreen(c.paper);
        drawTopBar(g, purpose_ == PinPurpose::Verify ? "家长密码" : "设置密码");
        g.setFont(Fonts::body());
        g.setTextColor(c.text, c.paper);
        g.drawCenterString(purpose_ == PinPurpose::Verify ? "输入家长密码" : "设置 4~6 位数字", 160, 60);

        const int box = 38, gap = 8;
        int x = (320 - (6 * box + 5 * gap)) / 2;
        for (int i = 0; i < 6; ++i) {
            g.fillRoundRect(x, 92, box, 54, 8, i < length_ ? c.yellow : TFT_WHITE);
            g.drawRoundRect(x, 92, box, 54, 8, c.ink);
            char text[4] = {};
            if (i < length_) std::snprintf(text, sizeof(text), "%c", digits_[i]);
            else if (i == length_) std::snprintf(text, sizeof(text), "%d", digit_);
            else std::snprintf(text, sizeof(text), "_");
            g.setFont(Fonts::title());
            g.setTextColor(i < length_ ? c.ink : c.track_border, i < length_ ? c.yellow : TFT_WHITE);
            g.drawCenterString(text, x + box / 2, 110);
            x += box + gap;
        }

        g.setFont(Fonts::small());
        g.setTextColor(c.text, c.paper);
        if (message_[0]) {
            g.drawCenterString(message_, 160, 168);
        } else {
            g.drawCenterString("旋钮改数字  OK 输入一位  SET 删除", 160, 168);
            g.drawCenterString("长按 OK 确认  长按 SET 取消", 160, 188);
        }
    }

private:
    void submit() {
        if (length_ < 4) {
            std::snprintf(message_, sizeof(message_), "至少 4 位");
            invalidate();
            return;
        }
        char pin[8] = {};
        std::memcpy(pin, digits_, static_cast<std::size_t>(length_));
        if (purpose_ == PinPurpose::Verify) {
            if (!unlockParental(pin)) {
                std::snprintf(message_, sizeof(message_), "密码不正确");
                length_ = 0;
                invalidate();
                return;
            }
            popPage();
            pushPage(createParentalPage());
            return;
        }
        if (!parentalSetPin(pin)) {
            std::snprintf(message_, sizeof(message_), "密码保存失败");
            invalidate();
            return;
        }
        notify("家长密码已更新");
        popPage();
    }

    PinPurpose purpose_ = PinPurpose::Verify;
    char digits_[6] = {};
    int length_ = 0;
    int digit_ = 0;
    char message_[32] = {};
};

class ParentalPage final : public Page {
public:
    const char* id() const override { return "parental"; }

    void enter() override {
        record_ = parentalRecord();
        selected_ = 0;
        invalidate();
    }

    void handle(const Event& event) override {
        if (event.delta) {
            adjust(event.delta > 0 ? 1 : -1);
            return;
        }
        switch (event.key) {
            case Key::Next: selected_ = (selected_ + 1) % kParentalItems; break;
            case Key::Prev: selected_ = (selected_ + kParentalItems - 1) % kParentalItems; break;
            case Key::Ok: activate(); break;
            case Key::OkDouble:
            case Key::SetLong:
            case Key::OkLong:
                popPage();
                break;
            default:
                break;
        }
        invalidate();
    }

    void draw() override {
        auto& g = Display::canvas();
        const auto& c = Theme::colors();
        g.fillScreen(c.paper);
        drawTopBar(g, "家长设置");

        int top = selected_ - (kVisibleRows - 1);
        if (top > kParentalItems - kVisibleRows) top = kParentalItems - kVisibleRows;
        if (top < 0) top = 0;
        g.fillRect(8, kRowY, 310, 183, c.paper);

        for (int row = 0; row < kVisibleRows; ++row) {
            const int item = top + row;
            if (item >= kParentalItems) break;
            const int y = kRowY + row * kRowHeight;
            const bool selected = item == selected_;
            switch (item) {
                case 0: drawRow(g, y, "每日时长", minutesText(record_.daily_limit), selected); break;
                case 1: {
                    char value[16];
                    std::snprintf(value, sizeof(value), "%u", static_cast<unsigned>(record_.volume_max));
                    drawRow(g, y, "音量上限", value, selected);
                    break;
                }
                case 2:
                    drawRow(g, y, "就寝时段", (record_.flags & Persistence::kParentalBedtime) ? "开" : "关", selected);
                    break;
                case 3: drawRow(g, y, "就寝开始", clockText(record_.bedtime_start), selected); break;
                case 4: drawRow(g, y, "就寝结束", clockText(record_.bedtime_end), selected); break;
                case 5:
                    drawRow(g, y, "远程锁", (record_.flags & Persistence::kParentalRemote) ? "开" : "关", selected);
                    break;
                case 6: drawRow(g, y, "修改密码", "单击设置", selected); break;
                case 7: drawRow(g, y, "解绑", "单击解除", selected); break;
                default: drawRow(g, y, "恢复默认", "单击恢复", selected); break;
            }
        }

        constexpr int sx = 314, sy = kRowY, sh = kRowHeight * kVisibleRows;
        const int thumb = sh * kVisibleRows / kParentalItems;
        const int thumb_y = sy + (sh - thumb) * top / (kParentalItems - kVisibleRows);
        g.fillRoundRect(sx, sy, 4, sh, 2, c.track);
        g.fillRoundRect(sx, thumb_y, 4, thumb, 2, c.coral);

        g.setFont(Fonts::body());
        g.setTextColor(c.text, c.paper);
        g.drawCenterString("旋钮调整  双击 OK 返回", 160, 220);
    }

private:
    void adjust(int direction) {
        switch (selected_) {
            case 0:
                record_.daily_limit = cycleDailyLimit(record_.daily_limit, direction);
                break;
            case 1: {
                int value = static_cast<int>(record_.volume_max) + direction;
                if (value < 3) value = 3;
                if (value > 21) value = 21;
                record_.volume_max = static_cast<uint8_t>(value);
                if (record_.volume_min > record_.volume_max) record_.volume_min = record_.volume_max;
                break;
            }
            case 2:
                Persistence::parentalSetFlag(record_, Persistence::kParentalBedtime,
                                             !(record_.flags & Persistence::kParentalBedtime));
                break;
            case 3: {
                int value = static_cast<int>(record_.bedtime_start) + direction * 15;
                value = (value % 1440 + 1440) % 1440;
                record_.bedtime_start = static_cast<uint16_t>(value);
                break;
            }
            case 4: {
                int value = static_cast<int>(record_.bedtime_end) + direction * 15;
                value = (value % 1440 + 1440) % 1440;
                record_.bedtime_end = static_cast<uint16_t>(value);
                break;
            }
            case 5:
                Persistence::parentalSetFlag(record_, Persistence::kParentalRemote,
                                             !(record_.flags & Persistence::kParentalRemote));
                break;
            default:
                return;
        }
        if (!parentalApply(record_)) notify("家长设置保存失败");
        record_ = parentalRecord();
        invalidate();
    }

    void activate() {
        switch (selected_) {
            case 6:
                pinEntryReset(PinPurpose::SetPin);
                pushPage(createPinEntryPage());
                break;
            case 7:
                Persistence::parentalUnbind(record_);
                if (parentalApply(record_)) notify("已解除绑定");
                else notify("解绑保存失败");
                record_ = parentalRecord();
                break;
            case 8:
                Persistence::parentalFactoryDefaults(record_, nullptr);
                if (parentalApply(record_)) notify("已恢复默认");
                else notify("恢复失败");
                record_ = parentalRecord();
                break;
            default:
                break;
        }
        invalidate();
    }

    Persistence::ParentalRecord record_{};
    int selected_ = 0;
};

PinEntryPage& pinInstance() {
    static PinEntryPage instance;
    return instance;
}
ParentalPage& parentalInstance() {
    static ParentalPage instance;
    return instance;
}

void pinEntryReset(PinPurpose purpose) { pinInstance().begin(purpose); }

}  // namespace

void openParentalSettings() {
    if (!parentalReady()) {
        notify("家长记录不可用");
        return;
    }
    if (parentalUnlocked()) {
        pushPage(createParentalPage());
        return;
    }
    pinEntryReset(PinPurpose::Verify);
    pushPage(createPinEntryPage());
}

Page* createParentalPage() { return &parentalInstance(); }
Page* createPinEntryPage() { return &pinInstance(); }

}  // namespace App
