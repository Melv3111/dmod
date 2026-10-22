#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <algorithm>
#include <deque>
#include <legowiifun.cheat_api/include/cheatAPI.hpp>

using namespace geode::prelude;

// ---------------------------------------------------------------------------
// D mod: predictive input buffer.
//
// When you press jump, the click is NOT applied immediately. The mod saves
// the game state (a checkpoint), then trial-runs the next few ticks for each
// possible delay (0, 1, 2 ... max-buffer). It picks the EARLIEST delay where
// you don't die, restores the state, and applies the click at that tick.
// If the click is already safe, delay is 0 and nothing changes.
//
// Limits: it can only hold a click LATER (fix early clicks). It cannot fix a
// click you never made or one that is too late.
// ---------------------------------------------------------------------------

struct QueuedInput {
    bool down;
    int button;
    bool isPlayer1;
    int dueTick;
    bool decided;  // has the look-ahead chosen a delay for this click yet?
};

// Shared between the two hooks below.
static bool g_simulating = false;  // true while trial-running ticks
static bool g_simDied = false;     // set if the trial run killed the player

static int64_t setting(char const* key) {
    return Mod::get()->getSettingValue<int64_t>(key);
}
static bool flag(char const* key) {
    return Mod::get()->getSettingValue<bool>(key);
}
static bool affects(bool isPlayer1) {
    return isPlayer1 ? flag("affect-player1") : flag("affect-player2");
}
static bool bufferActive() {
    return flag("enabled") && setting("max-buffer") > 0;
}

// Tell the Cheat API (and through it, cheat indicators / safe mode mods)
// whether D mod is currently cheating. Only calls it when the state changes.
static void syncCheat() {
    static bool flagged = false;
    bool want = bufferActive();
    if (want && !flagged) {
        cheatAPIEvents::setCheatingAll();
        flagged = true;
    } else if (!want && flagged) {
        cheatAPIEvents::endCheatingAll();
        flagged = false;
    }
}

// Master hotkey: flips "enabled" on or off. The key is a normal setting, so
// you can rebind it in the mod's settings.
$on_mod(Loaded) {
    syncCheat();
    listenForKeybindSettingPresses(
        "toggle-key",
        [](Keybind const&, bool down, bool repeat, double) {
            if (!down || repeat) return;
            auto mod = Mod::get();
            bool now = !mod->getSettingValue<bool>("enabled");
            mod->setSettingValue<bool>("enabled", now);
            syncCheat();
            // Off by default, so the toggle stays quiet during a prank.
            if (mod->getSettingValue<bool>("toggle-popup")) {
                Notification::create(now ? "D mod: on" : "D mod: off",
                                     NotificationIcon::None, 0.8f)->show();
            }
        },
        Mod::get()
    );
}

class $modify(DGameLayer, GJBaseGameLayer) {
    struct Fields {
        std::deque<QueuedInput> queue;
        int tick = 0;
        bool flushing = false;
        int shift[2] = {0, 0};  // delay chosen for the currently held click, per player
    };

    bool isLiveRun() {
        auto pl = PlayLayer::get();
        return pl && static_cast<GJBaseGameLayer*>(pl) == this;
    }

    void clearBuffer() {
        auto f = m_fields.self();
        f->queue.clear();
        f->tick = 0;
        f->shift[0] = f->shift[1] = 0;
    }

    // Trial-run: apply the click after `delay` ticks, run `window` ticks past
    // that, and report whether the player survived.
    bool survives(QueuedInput const& q, int delay, int window) {
        g_simulating = true;
        g_simDied = false;
        float const dt = 1.f / 240.f;
        for (int i = 0; i < delay + window && !g_simDied; ++i) {
            if (i == delay) GJBaseGameLayer::handleButton(q.down, q.button, q.isPlayer1);
            GJBaseGameLayer::processCommands(dt, false, false);
        }
        g_simulating = false;
        return !g_simDied;
    }

    // Earliest delay (0..max-buffer) that survives, or 0 if none do.
    int pickDelay(QueuedInput const& q) {
        auto pl = PlayLayer::get();
        if (!pl) return 0;
        int cap = static_cast<int>(std::clamp<int64_t>(setting("max-buffer"), 0, 20));
        int window = static_cast<int>(std::max<int64_t>(setting("look-ahead"), 1));

        auto snap = pl->createCheckpoint();
        if (!snap) return 0;
        snap->retain();

        int chosen = 0;
        bool found = false;
        for (int d = 0; d <= cap && !found; ++d) {
            if (survives(q, d, window)) {
                chosen = d;
                found = true;
            }
            pl->loadFromCheckpoint(snap);  // rewind to the real state
        }
        snap->release();
        return found ? chosen : 0;
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        auto f = m_fields.self();
        // Only buffer the jump button, only in a live run, never mid-trial.
        if (f->flushing || g_simulating || button != 1 || !bufferActive() ||
            !affects(isPlayer1) || !isLiveRun()) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }
        int idx = isPlayer1 ? 0 : 1;
        if (down) {
            // Delay is decided at the start of the next tick.
            f->queue.push_back({true, button, isPlayer1, f->tick, false});
        } else {
            // A release follows the same shift as its click, so hold time is kept.
            int due = f->tick + f->shift[idx];
            if (!f->queue.empty()) due = std::max(due, f->queue.back().dueTick);
            f->queue.push_back({false, button, isPlayer1, due, true});
        }
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        if (g_simulating) {
            GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
            return;
        }
        auto f = m_fields.self();
        f->tick++;

        // Run the look-ahead for any click that hasn't been decided yet.
        for (size_t i = 0; i < f->queue.size(); ++i) {
            auto& q = f->queue[i];
            if (q.decided || !q.down) continue;
            int d = pickDelay(q);
            int due = f->tick + d;
            if (i > 0) due = std::max(due, f->queue[i - 1].dueTick);
            q.dueTick = due;
            q.decided = true;
            f->shift[q.isPlayer1 ? 0 : 1] = d;
        }

        // Apply everything that is due.
        f->flushing = true;
        while (!f->queue.empty() && f->queue.front().decided &&
               f->queue.front().dueTick <= f->tick) {
            auto q = f->queue.front();
            f->queue.pop_front();
            GJBaseGameLayer::handleButton(q.down, q.button, q.isPlayer1);
        }
        f->flushing = false;

        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
    }
};

class $modify(DPlayLayer, PlayLayer) {
    // Safe mode: while the buffer is on, mark the run as test mode so the
    // game won't save progress or submit a completion.
    void forceSafe() {
        syncCheat();
        if (bufferActive()) m_isTestMode = true;
    }

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        forceSafe();
        return true;
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        static_cast<DGameLayer*>(static_cast<GJBaseGameLayer*>(this))->clearBuffer();
        forceSafe();
    }

    void levelComplete() {
        bool old = m_isTestMode;
        forceSafe();
        PlayLayer::levelComplete();
        m_isTestMode = old;
    }

    void doNot() {
        if (!Mod::get()->getSettingValue<bool>("do-not")) return;
        if (!m_uiLayer) return;

        static char const* lines[] = {
            "I SAID DO NOT.",
            "Told you.",
            "Bold of you to keep going.",
            "The spike sends its regards.",
            "Was that on purpose?",
            "You pressed it. Own it.",
            "Nothing happened. Probably.",
            "We are not mad. Just disappointed.",
        };
        static int count = 0;
        auto text = lines[(count++) % (sizeof(lines) / sizeof(lines[0]))];

        auto win = CCDirector::get()->getWinSize();
        auto label = CCLabelBMFont::create(text, "bigFont.fnt");
        label->setPosition({win.width / 2.f, win.height * 0.7f});
        label->setScale(0.f);
        label->setColor({255, 80, 80});
        m_uiLayer->addChild(label, 1000);
        label->runAction(CCSequence::create(
            CCEaseBackOut::create(CCScaleTo::create(0.25f, 0.8f)),
            CCDelayTime::create(1.2f),
            CCFadeOut::create(0.4f),
            CCRemoveSelf::create(),
            nullptr
        ));
    }

    void destroyPlayer(PlayerObject* player, GameObject* obj) {
        // During a trial run, just record that this timeline dies. Do not
        // actually kill anything.
        if (g_simulating) {
            g_simDied = true;
            return;
        }
        bool old = m_isTestMode;
        forceSafe();
        PlayLayer::destroyPlayer(player, obj);
        m_isTestMode = old;
        if (player == m_player1) doNot();
    }
};
