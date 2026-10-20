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
//  Everything is configured from the mod's settings page in Geode
//  (see the "settings" block in mod.json). There is no in-game button or hotkey.
//
//  Units: 1 tick = 1/240 s. 4 ticks = 1 frame @ 60 FPS. 240 ticks = 1 second.
// ============================================================================

namespace {

    // Must match "max" of normal-ticks / wave-ticks in mod.json.
    constexpr int MAX_TICKS = 240;

    // Delays 1..16 are always tested one by one (see "scan-stride" for above 16).
    constexpr int FULL_SCAN_LIMIT = 16;

    // Very long buffers need a longer lookahead, but it is capped so the cost of a
    // single simulation stays reasonable.
    constexpr int MAX_HORIZON_EXTRA = 64;

    using Clock = std::chrono::steady_clock;

    // Snapshot of all settings (re-read on every click, so changes apply immediately).
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
    // --- simulation players (invisible copies of the real players) ---
    PlayerObject* m_simPlayer1 = nullptr;
    PlayerObject* m_simPlayer2 = nullptr;

    bool m_simulating = false;
    bool m_firingBuffered = false;
    bool m_simulationDead = false;

    // Real physical button state vs. what the game currently has active
    bool m_physicalHolding = false;
    bool m_gameHolding = false;

    // --- settings ---
    Config m_cfg;
    bool m_enabled = true;

    // --- buffer state ---
    int  m_waitTicks = -1;          // -1 = inactive, >= 0 = countdown ticks, -2 = queued opposite tick
    bool m_bufferedPush = true;     // true = buffered PRESS, false = buffered RELEASE
    bool m_queuedOpposite = false;  // true if the user quickly released/re-pressed during the wait
    float m_frameDt = 0.25f;        // GD 240 TPS normalized dt is 0.25f

    // --- streak-based wave spam guard ---
    int  m_ticksSinceLastInput = 999;
    int  m_rapidInputStreak = 0;

    // --- optional on-screen status label ---
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

    // Matches Show Trajectory's exact attribute + hitbox size synchronization
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
        if (m_frameDt > 0.001f && m_frameDt < 2.0f) {
            return m_frameDt;
        }
        return 0.25f;
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

        // Only touch the label when its text actually changes.
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
        m_cfg = c;

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

        quit(); // drop anything left over from a previous level

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
        // Real player died -> clear any pending buffer so respawn is 100% clean
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
    int getSurvivalTicks(PlayLayer* pl, PlayerObject* realP1, PlayerObject* realP2, bool isDual, int delayFrames, bool actionPush, int maxTicks) {
        if (!pl || !m_simPlayer1 || !realP1) return maxTicks;

        // Second sim player is only used when the real player 2 exists in Dual mode.
        const bool dual2 = isDual && m_simPlayer2 && realP2;

        m_simulating = true;
        m_simulationDead = false;

        syncSimPlayer(m_simPlayer1, realP1);
        if (dual2) {
            syncSimPlayer(m_simPlayer2, realP2);
        }

        // Before delayFrames, maintain the pre-input button state
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

        m_simulating = false;
        return survived;
    }

    // ------------------------------------------------------------------
    //  Input handling
    // ------------------------------------------------------------------

    // Returns true if the input should reach the game now, false if it was
    // captured by the buffer (and will be fired later).
    bool interceptInput(GJBaseGameLayer* layer, bool push, bool isPlayer1) {
        PlayLayer* pl = PlayLayer::get();
        if (!pl) return true;

        bool isDual = pl->m_gameState.m_isDualMode && pl->m_player2 != nullptr;
        bool isTwoPlayerControl = pl->m_level && pl->m_level->m_twoPlayerMode;

        // Keep Player 2 synced with Player 1 in 1-player Dual mode
        if (!isPlayer1) {
            if (isDual && !isTwoPlayerControl && (m_waitTicks >= 0 || m_waitTicks == -2)) {
                return false;
            }
            return true;
        }

        // Pick up any settings changes (cheap).
        refreshConfig();

        // Always record the user's true physical button state first!
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

        // 1. NEVER touch Ship mode (or Dual with Ship)
        if (p1->m_isShip || (isDual && p2 && p2->m_isShip)) {
            m_waitTicks = -1;
            m_queuedOpposite = false;
            m_gameHolding = push;
            return true;
        }

        // 2. If an input is currently waiting in the buffer and the user quickly releases
        // (or re-presses) a moment later, queue it for the next tick so fast taps are never eaten
        if (m_waitTicks >= 0) {
            m_queuedOpposite = (push != m_bufferedPush);
            m_ticksSinceLastInput = 0;
            return false;
        }

        // If a queued opposite action is due to fire on the next tick (m_waitTicks == -2)
        // and the user clicks again, fire the pending opposite action first
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

        // 3. STREAK-BASED WAVE SPAM GUARD:
        // Only bypasses if you perform several rapid inputs in a row (closer together than
        // "spam-interval" ticks). Single micro-clicks and normal Wave turns around
        // spikes/sawblades stay 100% protected!
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

        // Only Wave (m_isDart) and Robot (m_isRobot) change trajectory on RELEASE
        bool releaseAffectsPhysics = p1->m_isDart || p1->m_isRobot ||
                                     (isDual && p2 && (p2->m_isDart || p2->m_isRobot));
        if (!push && !releaseAffectsPhysics) {
            m_gameHolding = false;
            return true;
        }

        m_queuedOpposite = false;

        // Buffer strength for the current mode. 0 = off for this mode.
        int maxDelayFrames = isWave ? m_cfg.waveTicks : m_cfg.normalTicks;
        if (maxDelayFrames <= 0) {
            m_gameHolding = push;
            return true;
        }

        // Full Show-Trajectory lookahead for Wave (65 ticks / 50 ticks Mini Wave)
        // so it sees spikes and sawblades 1-3 blocks ahead along the diagonal path!
        int targetHorizon = isMiniWave ? 50 : (isWave ? 65 : 160);

        // Long buffers (above the original 16-tick maximum) need a longer lookahead so the
        // delayed trajectory is still checked far enough ahead.
        if (maxDelayFrames > FULL_SCAN_LIMIT) {
            targetHorizon += std::min(maxDelayFrames - FULL_SCAN_LIMIT, MAX_HORIZON_EXTRA);
        }

        // 4. Check Frame 0 (immediate input) across the full trajectory horizon
        int ticksFrame0 = getSurvivalTicks(pl, p1, p2, isDual, 0, push, targetHorizon);
        if (ticksFrame0 >= targetHorizon) {
            m_gameHolding = push;
            return true;
        }

        // Last delay worth testing.
        int scanLimit = maxDelayFrames;
        if (isWave) {
            // EXACT shortcut (does not change results): a Wave delay is only accepted if it gains at
            // least (2 * delay + 6) ticks over Frame 0, and a simulation can never survive more than
            // targetHorizon ticks. So any larger delay can never be accepted -- skip simulating them.
            scanLimit = std::min(scanLimit, (targetHorizon - ticksFrame0 - 6) / 2);
        }

        // Delays 1..16 are tested one by one; above that every "scan-stride"-th delay is tested
        // (every 2x stride beyond 64 ticks).
        auto nextStep = [&](int delay) {
            if (delay < FULL_SCAN_LIMIT) return 1;
            return delay < 64 ? m_cfg.scanStride : m_cfg.scanStride * 2;
        };

        // Time limit for the delay search: keeps the frame rate steady. The first candidate
        // is always tested, then the search stops once the time is up.
        const auto deadline = Clock::now() + std::chrono::milliseconds(m_cfg.lookaheadMs);

        // 5. Frame 0 hits a spike, sawblade, or slope within the horizon!
        // Scan upwards for the earliest delay that genuinely clears the obstacle.
        int bestDelay = 0;
        int bestTicks = ticksFrame0;

        for (int delay = 1; delay <= scanLimit; delay += nextStep(delay)) {
            int ticks = getSurvivalTicks(pl, p1, p2, isDual, delay, push, targetHorizon);

            if (isWave) {
                // In Wave mode, delaying a turn inside a flat corridor naturally adds ~(2 * delay)
                // ticks against the opposite wall. Require at least (2 * delay + 10) extra ticks
                // so we ONLY buffer when the delayed trajectory actually clears past a spike,
                // sawblade, or corner!
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

    // Called on every player update tick.
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
            // FAILSAFE: When no buffer is active, guarantee the game's hold state
            // matches your actual physical button so Wave/Mini Wave can NEVER stay stuck holding!
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
    log::info("[InputBuffer] v5.0 loaded - configure it in the mod's settings");
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
            // This runs once per simulated tick (thousands of times per click), so the filtered
            // list is kept in one reusable buffer instead of allocating a new vector every call.
            static gd::vector<GameObject*> s_filtered;
            s_filtered.clear();
            s_filtered.reserve(objectsCount);
            for (int i = 0; i < objectsCount; i++) {
                GameObject* obj = vec->at(i);
                if (!obj) continue;
                if (obj->m_objectType == GameObjectType::Solid ||
                    obj->m_objectType == GameObjectType::Hazard ||
                    obj->m_objectType == GameObjectType::AnimatedHazard ||
                    obj->m_objectType == GameObjectType::CollisionObject ||
                    obj->m_objectType == GameObjectType::Slope) {
                    s_filtered.push_back(obj);
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

