#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <array>
#include <cstdint>
#include <string>

using namespace geode::prelude;

// Recreated from the supplied patch script. Values are mod-specific ticks;
// the original notes say 4 ticks = 1 frame at 60 FPS.
struct BufferStage {
    int allTicks;
    int waveTicks;
    const char* label;
};

static constexpr std::array<BufferStage, 10> kStages{{
    {2, 2, "2/2"},
    {4, 4, "4/4"},
    {8, 8, "8/8"},
    {16, 8, "16/8"},
    {24, 24, "24"},
    {32, 32, "32"},
    {48, 48, "48"},
    {54, 54, "54"},
    {60, 60, "60"},
    {0, 0, "OFF"},
}};

static size_t s_stageIndex = 0;
static bool s_enabled = true;

static const BufferStage& currentStage() {
    return kStages[s_stageIndex];
}

static void advanceStage() {
    s_stageIndex = (s_stageIndex + 1) % kStages.size();
    s_enabled = currentStage().allTicks != 0 || currentStage().waveTicks != 0;
    log::info("[PredictiveBuffer] Stage {} ({}) — all={} wave={}",
        s_stageIndex + 1, currentStage().label,
        currentStage().allTicks, currentStage().waveTicks);
}

// This is deliberately a prototype UI. It gives mobile users a touch target
// to cycle the same stage table revealed by patch_predictive_buffer.py.
// It does NOT claim to apply predictive input to gameplay yet: the supplied
// artifacts expose the Windows patch layout, but not enough source-level
// behavior to faithfully reproduce the original trajectory algorithm.
class $modify(PredictiveBufferPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects))
            return false;

        auto* menu = CCMenu::create();
        if (!menu) return true;
        menu->setID("predictive-buffer-menu"_spr);
        menu->setPosition(CCPointZero);
        menu->setZOrder(9999);

        auto* label = CCLabelBMFont::create("BUF 2/2", "bigFont.fnt");
        if (!label) return true;
        label->setScale(0.45f);
        auto* button = CCMenuItemLabel::create(label, this,
            menu_selector(PredictiveBufferPlayLayer::onCycleBuffer));
        if (!button) return true;
        button->setID("predictive-buffer-cycle"_spr);
        button->setPosition({ CCDirector::get()->getWinSize().width * 0.83f,
                              CCDirector::get()->getWinSize().height * 0.90f });
        menu->addChild(button);
        this->addChild(menu);
        return true;
    }

    void onCycleBuffer(CCObject*) {
        advanceStage();
        auto* menu = this->getChildByID("predictive-buffer-menu"_spr);
        if (!menu) return;
        auto* button = typeinfo_cast<CCMenuItemLabel*>(menu->getChildByID("predictive-buffer-cycle"_spr));
        if (!button) return;
        std::string text = "BUF ";
        text += currentStage().label;
        if (auto* label = typeinfo_cast<CCLabelBMFont*>(button->getLabel()))
            label->setString(text.c_str());
    }
};

$on_mod(Loaded) {
    log::info("Predictive Input Buffer recreation prototype loaded. Current stage: {}.",
        currentStage().label);
}
