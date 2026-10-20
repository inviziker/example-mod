#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/LevelEditorLayer.hpp>
#include <Geode/modify/GameObject.hpp>
#include <Geode/modify/EffectGameObject.hpp>
#include <Geode/modify/HardStreak.hpp>
#include <algorithm>
#include <chrono>
#include <string>

using namespace geode::prelude;

// ============================================================================
//  Predictive Input Buffer
//
//  Configured entirely from the mod's settings page in Geode (see mod.json).
//  No pause menu button, no hotkey.
//
//  Units: 1 tick = 1/240 s. 4 ticks = 1 frame @ 60 FPS.
// ============================================================================

namespace {

    constexpr int MAX_TICKS = 240;          // must match "max" in mod.json
    constexpr int FULL_SCAN_LIMIT = 16;     // delays 1..16 are always tested one by one
    constexpr int MAX_HORIZON_EXTRA = 64;   // cap on extra lookahead for long buffers

    using Clock = std::chrono::steady_clock;

    struct Config {
        bool enabled = true;
        int normalTicks = 2;
        int waveTicks = 2;

        bool spamGuard = true;
        int spamInterval = 8;
        int spamStreak = 3;

        int lookaheadMs = 5;
        int scanStride = 2;

        bool showHud = false;
        std::string hudCorner = "Bottom Left";
        bool debugLog = false;
    };

} // namespace

class TrajectoryBufferSim {
private:
    PlayerObject* m_simPlayer1 = nullptr;
    PlayerObject* m_simPlayer2 = nullptr;

    bool m_simulating = false;
    bool m_firingBuffered = false;
    bool m_simulationDead = false;

    bool m_physicalHolding = false;
    bool m_gameHolding = false;

    Config m_cfg;
    bool m_enabled = true;

    int  m_waitTicks = -1;          // -1 inactive, >=0 countdown, -2 queued opposite
    bool m_bufferedPush = true;
    bool m_queuedOpposite = false;
    float m_frameDt = 0.25f;

    int  m_ticksSinceLastInput = 999;
    int  m_rapidInputStreak = 0;

    CCLabelBMFont* m_hud = nullptr;
    std::string m_hudText;
    std::string m_hudCorner;
    int m_saves = 0;

    PlayerObject* createTrajectoryPlayer() {
        PlayLayer* pl = PlayLayer::get();
        if (!pl) return nullptr;

        PlayerObject* player = PlayerObject::create(1, 1, pl, pl, true);
        player->retain();
        player->setPosition({0, 105});
        player->setVisible(false);
        pl->m_objectLayer->addChild(player);
        return player;
    }

    void resetCollisionLog(PlayerObject* self) {
        self->m_collisionLogTop->removeAllObjects();
        self->m_collisionLogBottom->removeAllObjects();
        self->m_collisionLogLeft->removeAllObjects();
        self->m_collisionLogRight->removeAllObjects();
    }

    void syncSimPlayer(PlayerObject* sim, PlayerObject* real) {
        sim->setVisible(false);
        sim->copyAttributes(real);
        sim->setScale(real->getScale());
        sim->setContentSize(real->getContentSize());
        sim->m_width = real->m_width;
        sim->m_height = real->m_height;
        sim->m_gravityMod = real->m_gravityMod;
        sim->m_isOnGround = real->m_isOnGround;
        sim->m_isUpsideDown = real->m_isUpsideDown;
        sim->m_vehicleSize = real->m_vehicleSize;
        sim->m_playerSpeed = real->m_playerSpeed;
    }

    float getValidDt() const {
        return (m_frameDt > 0.001f && m_frameDt < 2.0f) ? m_frameDt : 0.25f;
    }

    // ------------------------------------------------------------------
    //  HUD
    // ------------------------------------------------------------------

    static std::string tickText(int ticks) {
        return ticks > 0 ? fmt::format("{}", ticks) : std::string("off");
    }

    std::string hudString() const {
        if (!m_enabled) return "Buffer: OFF";
        return fmt::format("Buffer {} / {} | Saves {}",
            tickText(m_cfg.normalTicks), tickText(m_cfg.waveTicks), m_saves);
    }

    void layoutHud() {
        auto win = CCDirector::sharedDirector()->getWinSize();
        const float margin = 6.f;
        bool right = m_cfg.hudCorner.find("Right") != std::string::npos;
        bool top = m_cfg.hudCorner.find("Top") != std::string::npos;
        m_hud->setAnchorPoint({right ? 1.f : 0.f, top ? 1.f : 0.f});
        m_hud->setPosition({
            right ? win.width - margin : margin,
            top ? win.height - margin : margin
        });
    }

    void syncHud() {
        PlayLayer* pl = PlayLayer::get();
        if (!pl) return;

        if (!m_cfg.showHud) {
            if (m_hud) m_hud->setVisible(false);
            return;
        }

        if (!m_hud) {
            m_hud = CCLabelBMFont::create("", "chatFont.fnt");
            m_hud->retain();
            m_hud->setScale(0.55f);
            m_hud->setOpacity(170);
            pl->addChild(m_hud, 1000);
            m_hudText.clear();
            m_hudCorner.clear();
        }
        m_hud->setVisible(true);

        if (m_hudCorner != m_cfg.hudCorner) {
            layoutHud();
            m_hudCorner = m_cfg.hudCorner;
        }

        auto text = hudString();
        if (text != m_hudText) {
            m_hud->setString(text.c_str());
            m_hudText = text;
        }
    }

public:
    // ------------------------------------------------------------------
    //  Settings
    // ------------------------------------------------------------------

    void refreshConfig() {
        auto mod = Mod::get();
        auto readInt = [&](char const* key, int lo, int hi) {
            return std::clamp(static_cast<int>(mod->getSettingValue<int64_t>(key)), lo, hi);
        };

        Config c;
        c.enabled = mod->getSettingValue<bool>("enabled");
        c.normalTicks = readInt("normal-ticks", 0, MAX_TICKS);
        c.waveTicks = readInt("wave-ticks", 0, MAX_TICKS);
        c.spamGuard = mod->getSettingValue<bool>("spam-guard");
        c.spamInterval = readInt("spam-interval", 1, 64);
        c.spamStreak = readInt("spam-streak", 1, 20);
        c.lookaheadMs = readInt("lookahead-ms", 1, 100);
        c.scanStride = readInt("scan-stride", 1, 16);
        c.showHud = mod->getSettingValue<bool>("show-hud");
        c.hudCorner = mod->getSettingValue<std::string>("hud-corner");
        c.debugLog = mod->getSettingValue<bool>("debug-log");
        m_cfg = std::move(c);

        bool wasEnabled = m_enabled;
        m_enabled = m_cfg.enabled && (m_cfg.normalTicks > 0 || m_cfg.waveTicks > 0);
        if (wasEnabled && !m_enabled) {
            m_waitTicks = -1;
            m_queuedOpposite = false;
        }

        syncHud();
    }

    // ------------------------------------------------------------------
    //  Level lifecycle
    // ------------------------------------------------------------------

    void init() {
        PlayLayer* pl = PlayLayer::get();
        if (!pl) return;

        quit();

        m_simPlayer1 = createTrajectoryPlayer();
        m_simPlayer2 = createTrajectoryPlayer();
        m_waitTicks = -1;
        m_queuedOpposite = false;
        m_physicalHolding = false;
        m_gameHolding = false;
        m_simulating = false;
        m_firingBuffered = false;
        m_ticksSinceLastInput = 999;
        m_rapidInputStreak = 0;
        m_saves = 0;

        refreshConfig();
    }

    void quit() {
        if (m_simPlayer1) {
            m_simPlayer1->release();
            m_simPlayer1 = nullptr;
        }
        if (m_simPlayer2) {
            m_simPlayer2->release();
            m_simPlayer2 = nullptr;
        }
        if (m_hud) {
            m_hud->release();
            m_hud = nullptr;
        }
        m_hudText.clear();
        m_hudCorner.clear();
        m_simulating = false;
        m_firingBuffered = false;
        m_waitTicks = -1;
        m_queuedOpposite = false;
        m_rapidInputStreak = 0;
    }

    bool isSimulating() const { return m_simulating; }
    bool isFiringBuffered() const { return m_firingBuffered; }

    bool checkSimulationDeath(PlayerObject* player) {
        if (m_simulating && (player == m_simPlayer1 || player == m_simPlayer2)) {
            m_simulationDead = true;
            return true;
        }
        m_waitTicks = -1;
        m_queuedOpposite = false;
        m_gameHolding = false;
        m_rapidInputStreak = 0;
        return false;
    }

    void setFrameDelta(float dt) {
        PlayLayer* pl = PlayLayer::get();
        if (pl && pl->m_gameState.m_timeWarp > 0.f) {
            m_frameDt = dt / pl->m_gameState.m_timeWarp;
        } else {
            m_frameDt = dt;
        }
    }

    // ------------------------------------------------------------------
    //  Trajectory simulation
    // ------------------------------------------------------------------

    // Simulates "press/release after `delayFrames` ticks" and returns how many ticks
    // (up to maxTicks) the player survives afterwards.
    // If `diedBeforeInput` is given, it is set to true when the player already died
    // BEFORE the delayed input would have fired. In that case every larger delay is
    // guaranteed to die the same way, so the caller can stop scanning.
    int getSurvivalTicks(PlayLayer* pl, PlayerObject* realP1, PlayerObject* realP2, bool isDual,
                         int delayFrames, bool actionPush, int maxTicks, bool* diedBeforeInput = nullptr) {
        if (diedBeforeInput) *diedBeforeInput = false;
        if (!pl || !m_simPlayer1 || !realP1) return maxTicks;

        const bool dual2 = isDual && m_simPlayer2 && realP2;

        m_simulating = true;
        m_simulationDead = false;

        syncSimPlayer(m_simPlayer1, realP1);
        if (dual2) {
            syncSimPlayer(m_simPlayer2, realP2);
        }

        if (delayFrames > 0) {
            if (!actionPush) {
                m_simPlayer1->pushButton(PlayerButton::Jump);
                if (dual2) m_simPlayer2->pushButton(PlayerButton::Jump);
            } else {
                m_simPlayer1->releaseButton(PlayerButton::Jump);
                if (dual2) m_simPlayer2->releaseButton(PlayerButton::Jump);
            }
        }

        const float dt = getValidDt();
        int survived = 0;
        const bool isContinuousMode = realP1->m_isDart || realP1->m_isBird || realP1->m_isSwing ||
                                      (isDual && realP2 && (realP2->m_isDart || realP2->m_isBird || realP2->m_isSwing));
        int airborneTicks = 0;
        int safeLandedTicks = 0;

        const int totalSteps = delayFrames + maxTicks;
        for (int i = 0; i < totalSteps; i++) {
            if (i == delayFrames) {
                if (actionPush) {
                    m_simPlayer1->pushButton(PlayerButton::Jump);
                    if (dual2) m_simPlayer2->pushButton(PlayerButton::Jump);
                } else {
                    m_simPlayer1->releaseButton(PlayerButton::Jump);
                    if (dual2) m_simPlayer2->releaseButton(PlayerButton::Jump);
                }
            }

            m_simPlayer1->update(dt);
            m_simPlayer1->updateRotation(dt);
            if (dual2) {
                m_simPlayer2->update(dt);
                m_simPlayer2->updateRotation(dt);
            }
            if (m_simulationDead) break;

            resetCollisionLog(m_simPlayer1);
            pl->checkCollisions(m_simPlayer1, dt, false);
            if (m_simulationDead) break;

            if (dual2) {
                resetCollisionLog(m_simPlayer2);
                pl->checkCollisions(m_simPlayer2, dt, false);
                if (m_simulationDead) break;
            }

            if (i >= delayFrames) {
                survived++;
                if (!isContinuousMode) {
                    bool p1Grounded = m_simPlayer1->m_isOnGround;
                    bool p2Grounded = dual2 ? m_simPlayer2->m_isOnGround : true;
                    if (!p1Grounded || !p2Grounded) {
                        airborneTicks++;
                        safeLandedTicks = 0;
                    } else if (airborneTicks >= 15) {
                        safeLandedTicks++;
                        if (safeLandedTicks >= 10) {
                            survived = maxTicks;
                            break;
                        }
                    }
                }
            }
        }

        if (diedBeforeInput && m_simulationDead && survived == 0 && delayFrames > 0) {
            *diedBeforeInput = true;
        }

        m_simulating = false;
        return survived;
    }

    // ------------------------------------------------------------------
    //  Input handling
    // ------------------------------------------------------------------

    bool interceptInput(GJBaseGameLayer* layer, bool push, bool isPlayer1) {
        PlayLayer* pl = PlayLayer::get();
        if (!pl) return true;

        bool isDual = pl->m_gameState.m_isDualMode && pl->m_player2 != nullptr;
        bool isTwoPlayerControl = pl->m_level && pl->m_level->m_twoPlayerMode;

        if (!isPlayer1) {
            if (isDual && !isTwoPlayerControl && (m_waitTicks >= 0 || m_waitTicks == -2)) {
                return false;
            }
            return true;
        }

        refreshConfig();

        m_physicalHolding = push;

        if (!m_enabled) {
            m_gameHolding = push;
            return true;
        }

        PlayerObject* p1 = layer->m_player1;
        PlayerObject* p2 = isDual ? pl->m_player2 : nullptr;
        if (!p1 || p1->m_isDead) {
            m_waitTicks = -1;
            m_queuedOpposite = false;
            m_gameHolding = push;
            return true;
        }

        // Never touch Ship mode (or Dual with Ship)
        if (p1->m_isShip || (isDual && p2 && p2->m_isShip)) {
            m_waitTicks = -1;
            m_queuedOpposite = false;
            m_gameHolding = push;
            return true;
        }

        // An input is waiting in the buffer: queue quick taps so they are never eaten
        if (m_waitTicks >= 0) {
            m_queuedOpposite = (push != m_bufferedPush);
            m_ticksSinceLastInput = 0;
            return false;
        }

        if (m_waitTicks == -2) {
            m_waitTicks = -1;
            m_queuedOpposite = false;
            if (m_gameHolding != !m_bufferedPush) {
                m_SimFireButton(pl, !m_bufferedPush);
            }
        }

        bool isWave = p1->m_isDart || (isDual && p2 && p2->m_isDart);
        bool isMiniWave = (p1->m_isDart && p1->m_vehicleSize < 1.0f) ||
                          (isDual && p2 && p2->m_isDart && p2->m_vehicleSize < 1.0f);

        // Streak-based wave spam guard
        if (m_cfg.spamGuard && m_ticksSinceLastInput < m_cfg.spamInterval) {
            m_rapidInputStreak++;
        } else {
            m_rapidInputStreak = 0;
        }
        m_ticksSinceLastInput = 0;

        if (isWave && m_cfg.spamGuard && m_rapidInputStreak >= m_cfg.spamStreak) {
            m_gameHolding = push;
            return true;
        }

        bool releaseAffectsPhysics = p1->m_isDart || p1->m_isRobot ||
                                     (isDual && p2 && (p2->m_isDart || p2->m_isRobot));
        if (!push && !releaseAffectsPhysics) {
            m_gameHolding = false;
            return true;
        }

        m_queuedOpposite = false;

        int maxDelayFrames = isWave ? m_cfg.waveTicks : m_cfg.normalTicks;
        if (maxDelayFrames <= 0) {
            m_gameHolding = push;
            return true;
        }

        int targetHorizon = isMiniWave ? 50 : (isWave ? 65 : 160);

        if (maxDelayFrames > FULL_SCAN_LIMIT) {
            targetHorizon += std::min(maxDelayFrames - FULL_SCAN_LIMIT, MAX_HORIZON_EXTRA);
        }

        // Start the time budget before the first simulation so it covers everything.
        const auto deadline = Clock::now() + std::chrono::milliseconds(m_cfg.lookaheadMs);

        // Frame 0 (immediate input)
        int ticksFrame0 = getSurvivalTicks(pl, p1, p2, isDual, 0, push, targetHorizon);
        if (ticksFrame0 >= targetHorizon) {
            m_gameHolding = push;
            return true;
        }

        int scanLimit = maxDelayFrames;
        if (isWave) {
            // Exact shortcut: larger delays can never be accepted (see gain rules below).
            scanLimit = std::min(scanLimit, (targetHorizon - ticksFrame0 - 6) / 2);
        }

        // Delays 1..15 one by one; above that the step grows with the delay
        // (about delay/8, never below "scan-stride"), so huge buffers stay cheap.
        auto nextStep = [&](int delay) {
            if (delay < FULL_SCAN_LIMIT) return 1;
            return std::max(m_cfg.scanStride, delay / 8);
        };

        int bestDelay = 0;
        int bestTicks = ticksFrame0;

        for (int delay = 1; delay <= scanLimit; delay += nextStep(delay)) {
            bool diedEarly = false;
            int ticks = getSurvivalTicks(pl, p1, p2, isDual, delay, push, targetHorizon, &diedEarly);

            // Died before the input could even fire: every longer delay dies here too.
            if (diedEarly) break;

            if (isWave) {
                int minClearedGain = (delay * 2) + 10;
                if (ticks >= targetHorizon && (ticks - ticksFrame0) >= (delay * 2) + 6) {
                    bestDelay = delay;
                    bestTicks = ticks;
                    break;
                }
                if (ticks >= ticksFrame0 + minClearedGain && ticks > bestTicks + 2) {
                    bestDelay = delay;
                    bestTicks = ticks;
                    if (ticks >= targetHorizon) break;
                }
            } else {
                if (ticks >= targetHorizon) {
                    bestDelay = delay;
                    bestTicks = ticks;
                    break;
                }
                if (ticks > bestTicks + 12) {
                    bestDelay = delay;
                    bestTicks = ticks;
                }
            }

            if (Clock::now() >= deadline) break;
        }

        if (bestDelay > 0) {
            m_saves++;
            if (m_cfg.debugLog) {
                log::debug("[InputBuffer] SAVED +{} ticks ({:.2f} frames @ 60fps) {} (Frame0: {} -> Delayed: {}) [Limit: {}]",
                    bestDelay, bestDelay / 4.0f, push ? "PRESS" : "RELEASE", ticksFrame0, bestTicks, maxDelayFrames);
            }
            syncHud();
            m_bufferedPush = push;
            m_waitTicks = bestDelay;
            return false;
        }

        m_gameHolding = push;
        return true;
    }

    void onPlayerUpdate(PlayerObject* player) {
        PlayLayer* pl = PlayLayer::get();
        if (!pl || player != pl->m_player1) return;

        if (!m_enabled) return;

        if (m_ticksSinceLastInput < 999) {
            m_ticksSinceLastInput++;
        }

        if (player->m_isShip || (pl->m_gameState.m_isDualMode && pl->m_player2 && pl->m_player2->m_isShip)) {
            m_waitTicks = -1;
            m_queuedOpposite = false;
            if (m_gameHolding != m_physicalHolding) {
                m_SimFireButton(pl, m_physicalHolding);
            }
            return;
        }

        if (m_waitTicks == 0) {
            m_SimFireButton(pl, m_bufferedPush);
            if (m_queuedOpposite) {
                m_waitTicks = -2;
                m_queuedOpposite = false;
            } else {
                m_waitTicks = -1;
            }
        } else if (m_waitTicks > 0) {
            m_waitTicks--;
        } else if (m_waitTicks == -2) {
            m_SimFireButton(pl, !m_bufferedPush);
            m_queuedOpposite = false;
            m_waitTicks = -1;
        } else if (m_waitTicks == -1) {
            // Failsafe: game hold state must always match the physical button.
            if (m_gameHolding != m_physicalHolding) {
                m_SimFireButton(pl, m_physicalHolding);
            }
        }
    }

    void m_SimFireButton(PlayLayer* pl, bool push) {
        m_firingBuffered = true;
        m_gameHolding = push;
        pl->handleButton(push, 1, true);
        if (pl->m_gameState.m_isDualMode && (!pl->m_level || !pl->m_level->m_twoPlayerMode)) {
            pl->handleButton(push, 1, false);
        }
        m_firingBuffered = false;
    }
};

static TrajectoryBufferSim s_sim;

$on_mod(Loaded) {
    log::info("[InputBuffer] v1.2.0 loaded - configure it in the mod's settings");
}

// ============================================================================
//  Hooks
// ============================================================================

class $modify(BufPlayLayer, PlayLayer) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("PlayLayer::destroyPlayer", -1000000);
        (void)self.setHookPriority("PlayLayer::playEndAnimationToPos", -1000000);
        (void)self.setHookPriority("PlayLayer::flipGravity", -1000000);
    }

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        s_sim.init();
        return true;
    }

    void destroyPlayer(PlayerObject* player, GameObject* gameObject) {
        if (s_sim.checkSimulationDeath(player)) return;
        PlayLayer::destroyPlayer(player, gameObject);
    }

    void playEndAnimationToPos(cocos2d::CCPoint p0) {
        if (s_sim.isSimulating()) return;
        PlayLayer::playEndAnimationToPos(p0);
    }

    void flipGravity(PlayerObject* p0, bool p1, bool p2) {
        if (s_sim.isSimulating()) return;
        PlayLayer::flipGravity(p0, p1, p2);
    }

    void onQuit() {
        PlayLayer::onQuit();
        s_sim.quit();
    }
};

class $modify(BufEditorLayer, LevelEditorLayer) {
    bool init(GJGameLevel* level, bool unk) {
        bool res = LevelEditorLayer::init(level, unk);
        s_sim.quit();
        return res;
    }
};

class $modify(BufHardStreak, HardStreak) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("HardStreak::addPoint", -1000000);
    }

    void addPoint(cocos2d::CCPoint p0) {
        if (s_sim.isSimulating()) return;
        HardStreak::addPoint(p0);
    }
};

class $modify(BufEffectGameObject, EffectGameObject) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("EffectGameObject::triggerObject", -1000000);
    }

    void triggerObject(GJBaseGameLayer* p0, int p1, const gd::vector<int>* p2) {
        if (s_sim.isSimulating()) return;
        EffectGameObject::triggerObject(p0, p1, p2);
    }
};

class $modify(BufGameObject, GameObject) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("GameObject::playShineEffect", -1000000);
    }

    void playShineEffect() {
        if (s_sim.isSimulating()) return;
        GameObject::playShineEffect();
    }
};

class $modify(BufPlayerObject, PlayerObject) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("PlayerObject::playSpiderDashEffect", -1000000);
        (void)self.setHookPriority("PlayerObject::incrementJumps", -1000000);
        (void)self.setHookPriority("PlayerObject::ringJump", -1000000);
    }

    void playSpiderDashEffect(cocos2d::CCPoint from, cocos2d::CCPoint to) {
        if (s_sim.isSimulating()) return;
        PlayerObject::playSpiderDashEffect(from, to);
    }

    void incrementJumps() {
        if (s_sim.isSimulating()) return;
        PlayerObject::incrementJumps();
    }

    void update(float dt) {
        if (!s_sim.isSimulating()) {
            s_sim.onPlayerUpdate(this);
        }
        PlayerObject::update(dt);
        if (PlayLayer::get() && !s_sim.isSimulating()) {
            s_sim.setFrameDelta(dt);
        }
    }

    void ringJump(RingObject* p0, bool p1) {
        if (s_sim.isSimulating()) return;
        PlayerObject::ringJump(p0, p1);
    }
};

class $modify(BufBaseGameLayer, GJBaseGameLayer) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("GJBaseGameLayer::canBeActivatedByPlayer", -1000000);
        (void)self.setHookPriority("GJBaseGameLayer::handleButton", -1000000);
        (void)self.setHookPriority("GJBaseGameLayer::collisionCheckObjects", -1000000);
        (void)self.setHookPriority("GJBaseGameLayer::playerTouchedRing", -1000000);
    }

    bool canBeActivatedByPlayer(PlayerObject* p0, EffectGameObject* p1) {
        if (s_sim.isSimulating()) return false;
        return GJBaseGameLayer::canBeActivatedByPlayer(p0, p1);
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        if (!s_sim.isSimulating() && !s_sim.isFiringBuffered() && button == 1 && PlayLayer::get()) {
            if (!s_sim.interceptInput(this, down, isPlayer1)) {
                return;
            }
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    void collisionCheckObjects(PlayerObject* player, gd::vector<GameObject*>* vec, int objectsCount, float dt) {
        if (s_sim.isSimulating()) {
            // Runs thousands of times per click: reuse one buffer, no per-call allocation.
            static gd::vector<GameObject*> s_filtered;
            s_filtered.clear();
            if (s_filtered.capacity() < static_cast<size_t>(objectsCount)) {
                s_filtered.reserve(objectsCount);
            }
            for (int i = 0; i < objectsCount; i++) {
                GameObject* obj = vec->at(i);
                if (!obj) continue;
                switch (obj->m_objectType) {
                    case GameObjectType::Solid:
                    case GameObjectType::Hazard:
                    case GameObjectType::AnimatedHazard:
                    case GameObjectType::CollisionObject:
                    case GameObjectType::Slope:
                        s_filtered.push_back(obj);
                        break;
                    default:
                        break;
                }
            }
            GJBaseGameLayer::collisionCheckObjects(player, &s_filtered, static_cast<int>(s_filtered.size()), dt);
            return;
        }
        GJBaseGameLayer::collisionCheckObjects(player, vec, objectsCount, dt);
    }

    void playerTouchedRing(PlayerObject* player, RingObject* ring) {
        if (s_sim.isSimulating()) return;
        GJBaseGameLayer::playerTouchedRing(player, ring);
    }
};

