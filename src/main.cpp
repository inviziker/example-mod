// Input Buffer - Geode mod for Geometry Dash
// A click made in the air shortly before landing is remembered and fires on
// the first possible step, as long as it happened within the buffer window.

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <algorithm>
#include <chrono>

using namespace geode::prelude;
using Clock = std::chrono::steady_clock;
using Ms = std::chrono::milliseconds;

struct Slot {
    bool pending = false;       // an early click is waiting for a landing
    bool physicalDown = false;  // the real button is currently held
    bool synthDown = false;     // we injected a press and still owe a release
    Clock::time_point pressedAt{};
    Clock::time_point releaseAt{};
    Ms heldFor{16};             // how long the player really held the early click
};

static Slot g_slots[2];         // 0 = player 1, 1 = player 2
static bool g_injecting = false;

// Ship, UFO, wave and swing don't "jump from the ground", so don't buffer them.
static bool isGroundMode(PlayerObject* p) {
    return p && !(p->m_isShip || p->m_isBird || p->m_isDart || p->m_isSwing);
}

static void inject(GJBaseGameLayer* layer, bool down, bool isPlayer1) {
    g_injecting = true;
    layer->handleButton(down, 1, isPlayer1);
    g_injecting = false;
}

class $modify(BufferLayer, GJBaseGameLayer) {
    // Watch real inputs.
    void handleButton(bool down, int button, bool isPlayer1) {
        auto mod = Mod::get();
        if (g_injecting || button != 1 || !PlayLayer::get() ||
            !mod->getSettingValue<bool>("enabled")) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        auto& s = g_slots[isPlayer1 ? 0 : 1];
        auto* p = isPlayer1 ? m_player1 : m_player2;
        auto now = Clock::now();

        if (down) {
            s.physicalDown = true;
            if (isGroundMode(p) && !p->m_isOnGround) {
                s.pending = true;       // early click: remember it
                s.pressedAt = now;
            }
        } else {
            s.physicalDown = false;
            if (s.pending) {
                s.heldFor = std::max(
                    Ms(1), std::chrono::duration_cast<Ms>(now - s.pressedAt)
                );
            }
        }

        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    // Runs every physics step: fire buffered clicks the moment we're grounded.
    void processCommands(float dt) {
        auto mod = Mod::get();
        if (PlayLayer::get() && mod->getSettingValue<bool>("enabled")) {
            auto now = Clock::now();
            auto window = Ms(mod->getSettingValue<int64_t>("window-ms"));

            for (int i = 0; i < 2; i++) {
                bool isP1 = (i == 0);
                auto& s = g_slots[i];
                auto* p = isP1 ? m_player1 : m_player2;

                // Release the click we injected earlier.
                if (s.synthDown && now >= s.releaseAt) {
                    s.synthDown = false;
                    inject(this, false, isP1);
                }

                if (!s.pending || !p) continue;

                // Click is too old, drop it.
                if (now - s.pressedAt > window) {
                    s.pending = false;
                    continue;
                }

                if (p->m_isOnGround) {
                    s.pending = false;
                    // If the button is still held, the game jumps by itself.
                    // Only inject when it was released before landing.
                    if (!s.physicalDown && !s.synthDown) {
                        s.synthDown = true;
                        s.releaseAt = now + s.heldFor;
                        inject(this, true, isP1);
                    }
                }
            }
        }
        GJBaseGameLayer::processCommands(dt);
    }
};

// Clear state on death / restart so nothing leaks into the next attempt.
class $modify(BufferPlayLayer, PlayLayer) {
    void resetLevel() {
        for (auto& s : g_slots) s = Slot{};
        PlayLayer::resetLevel();
    }
};
