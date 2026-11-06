#include <Geode/Geode.hpp>
#include <Geode/modify/CCDirector.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/MenuLayer.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>

using namespace geode::prelude;

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

static std::atomic<int>  g_fps{240};
static std::atomic<bool> g_smoothing{true};
static std::atomic<bool> g_extrapolate{true};

static void applyFPS() {
	CCDirector::get()->setAnimationInterval(1.0 / g_fps.load());
}

static void loadSettings() {
	auto mod = Mod::get();
	g_fps = std::clamp<int>(static_cast<int>(mod->getSettingValue<int64_t>("fps")), 24, 1440);
	g_smoothing = mod->getSettingValue<bool>("smoothing");
	g_extrapolate = mod->getSettingValue<std::string>("mode") == "Extrapolation";
}

$on_mod(Loaded) {
	loadSettings();

	listenForSettingChanges<int64_t>("fps", [](int64_t) {
		loadSettings();
		applyFPS();
	});
	listenForSettingChanges<bool>("smoothing", [](bool) { loadSettings(); });
	listenForSettingChanges<std::string>("mode", [](std::string) { loadSettings(); });
}

// ---------------------------------------------------------------------------
// Frame rate (physics is not touched)
// ---------------------------------------------------------------------------

// The game calls setAnimationInterval itself when video settings change,
// so we override the value to keep our FPS from being reset.
class $modify(SmoothDirector, CCDirector) {
	void setAnimationInterval(double) {
		CCDirector::setAnimationInterval(1.0 / g_fps.load());
	}
};

class $modify(SmoothMenu, MenuLayer) {
	bool init() {
		if (!MenuLayer::init()) return false;
		static bool done = false;
		if (!done) {
			done = true;
			applyFPS();
		}
		return true;
	}
};

// ---------------------------------------------------------------------------
// Frame smoothing (visual only)
//
// GD runs physics in fixed steps of 1/240 s, but a frame can contain 0, 1 or
// several steps, so the raw picture stutters at high FPS.
//
// How this works:
//   update(): let the game run, then work out how far into the *next* step we
//             are (the "phase"). The game logic only ever sees real positions.
//   visit():  right before drawing, move the players and the camera to a
//             smoothed position, draw, then put the real positions back.
//
// Extrapolation: draw the state predicted for "now" (newest step + velocity
//                * phase). No added latency.
// Interpolation: draw between the last two steps (up to 1 step behind, but
//                never overshoots).
// ---------------------------------------------------------------------------

namespace {
	constexpr double kStep = 1.0 / 240.0; // one physics step, in game time

	struct Track {
		CCPoint prev;       // state one physics step before `curr`
		CCPoint curr;       // newest real state
		bool valid = false;
		bool flipY = false; // vertical direction just reversed (jump / landing)

		void reset(CCPoint const& p) {
			prev = curr = p;
			valid = true;
			flipY = false;
		}

		// p: newest real position, n: number of physics steps since last push
		void push(CCPoint const& p, int n) {
			if (!valid) {
				reset(p);
				return;
			}
			CCPoint delta = ccpSub(p, curr);

			// Teleport, portal, respawn: never smooth across a big jump
			if (ccpLength(delta) > std::max(30.f, 12.f * static_cast<float>(n))) {
				reset(p);
				return;
			}

			CCPoint perStep = ccpMult(delta, 1.f / static_cast<float>(n));
			CCPoint oldStep = ccpSub(curr, prev);
			flipY = oldStep.y * perStep.y < -0.001f;

			prev = ccpSub(p, perStep);
			curr = p;
		}

		CCPoint sample(float alpha, bool extrapolate) const {
			CCPoint step = ccpSub(curr, prev);
			if (extrapolate) {
				// Do not predict through a bounce, it would sink into the floor
				if (flipY) step.y = 0.f;
				return ccpAdd(curr, ccpMult(step, alpha));
			}
			return ccpAdd(prev, ccpMult(step, alpha));
		}
	};
}

class $modify(SmoothGameLayer, GJBaseGameLayer) {
	struct Fields {
		int8_t isPlayLayer = -1;
		bool active = false;       // tracks hold usable data
		bool applied = false;      // smoothed positions are currently set
		bool appliedP2 = false;    // player 2 was moved too
		double phase = 0.0;        // game seconds since the last physics step

		Track p1, p2, cam;
		CCPoint real1, real2, realCam;

		// Wave trails are separate nodes built from real positions. Only their
		// head point is moved to the smoothed position, the rest stays put.
		HardStreak* trail1 = nullptr;
		HardStreak* trail2 = nullptr;
		CCPoint head1, head2;
	};

	bool inPlayLayer() {
		auto f = m_fields.self();
		if (f->isPlayLayer < 0) {
			auto base = static_cast<GJBaseGameLayer*>(this);
			f->isPlayLayer = typeinfo_cast<PlayLayer*>(base) ? 1 : 0;
		}
		return f->isPlayLayer == 1;
	}

	void resetTracks() {
		auto f = m_fields.self();
		f->active = false;
		f->p1.valid = f->p2.valid = f->cam.valid = false;
		f->phase = 0.0;
	}

	static bool streakUsable(HardStreak* t) {
		return t && t->isVisible() && t->m_pointArray && t->m_pointArray->count() > 0;
	}

	void update(float dt) {
		auto f = m_fields.self();

		double timeBefore = m_gameState.m_levelTime;
		GJBaseGameLayer::update(dt);

		if (!g_smoothing.load() || !this->inPlayLayer() ||
		    !m_player1 || !m_objectLayer || m_player1->m_isDead) {
			this->resetTracks();
			return;
		}

		// Bad frame time (first frame, alt-tab, NaN): skip smoothing for it
		if (!std::isfinite(dt) || dt <= 0.f || dt > 0.25f) {
			this->resetTracks();
			return;
		}

		// Convert real time to game time (time warp triggers slow the game down)
		double warp = (std::isfinite(m_gameState.m_timeWarp) && m_gameState.m_timeWarp > 0.f)
			? m_gameState.m_timeWarp : 1.0;
		double total = f->phase + static_cast<double>(dt) * warp;
		int predicted = static_cast<int>(std::min(std::floor(total / kStep), 64.0));

		CCPoint n1 = m_player1->getPosition();
		CCPoint n2 = m_player2 ? m_player2->getPosition() : n1;
		CCPoint nc = m_objectLayer->getPosition();

		double levelDelta = m_gameState.m_levelTime - timeBefore;
		bool stepped = levelDelta > 0.0 ||
			!f->p1.valid ||
			!n1.equals(f->p1.curr) || !n2.equals(f->p2.curr) || !nc.equals(f->cam.curr);

		if (!stepped) {
			// No physics step this frame: just move further into the current one
			f->phase = std::min(total, kStep * 0.999);
			return;
		}

		// How many physics steps happened this frame. Level time is exact when
		// it moves in whole steps, otherwise fall back to our own estimate.
		int n = std::max(1, predicted);
		double exact = levelDelta / kStep;
		if (exact > 64.0) {
			// Checkpoint restore, start pos, etc: time jumped, start over
			this->resetTracks();
			return;
		}
		if (exact >= 0.95 && std::abs(exact - std::round(exact)) < 0.05) {
			n = static_cast<int>(std::round(exact));
		}

		f->phase = std::clamp(total - n * kStep, 0.0, kStep * 0.999);

		f->p1.push(n1, n);
		f->p2.push(n2, n);
		f->cam.push(nc, n);
		f->active = true;
	}

	bool applySmoothed() {
		auto f = m_fields.self();
		if (!f->active || f->applied) return false;
		if (!m_player1 || !m_objectLayer || !m_player1->getParent()) return false;

		float alpha = static_cast<float>(f->phase / kStep);
		bool extra = g_extrapolate.load();

		// Player 1 + camera
		CCPoint s1 = f->p1.sample(alpha, extra);
		f->real1 = m_player1->getPosition();
		f->realCam = m_objectLayer->getPosition();
		m_player1->setPosition(s1);
		m_objectLayer->setPosition(f->cam.sample(alpha, extra));

		f->trail1 = nullptr;
		if (streakUsable(m_player1->m_waveTrail)) {
			f->trail1 = m_player1->m_waveTrail;
			f->head1 = f->trail1->m_currentPoint;
			f->trail1->m_currentPoint = ccpAdd(f->head1, ccpSub(s1, f->real1));
			f->trail1->updateStroke(0.f);
		}

		// Player 2 (dual mode only)
		f->appliedP2 = false;
		f->trail2 = nullptr;
		if (m_player2 && m_gameState.m_isDualMode && m_player2->getParent()) {
			CCPoint s2 = f->p2.sample(alpha, extra);
			f->real2 = m_player2->getPosition();
			m_player2->setPosition(s2);
			f->appliedP2 = true;

			if (streakUsable(m_player2->m_waveTrail)) {
				f->trail2 = m_player2->m_waveTrail;
				f->head2 = f->trail2->m_currentPoint;
				f->trail2->m_currentPoint = ccpAdd(f->head2, ccpSub(s2, f->real2));
				f->trail2->updateStroke(0.f);
			}
		}

		f->applied = true;
		return true;
	}

	void restoreReal() {
		auto f = m_fields.self();
		if (!f->applied) return;
		f->applied = false;

		if (m_player1) {
			m_player1->setPosition(f->real1);
			// only touch the trail if it is still the same object
			if (f->trail1 && m_player1->m_waveTrail == f->trail1) {
				f->trail1->m_currentPoint = f->head1;
			}
		}
		if (m_objectLayer) m_objectLayer->setPosition(f->realCam);

		if (f->appliedP2 && m_player2) {
			m_player2->setPosition(f->real2);
			if (f->trail2 && m_player2->m_waveTrail == f->trail2) {
				f->trail2->m_currentPoint = f->head2;
			}
		}
		f->appliedP2 = false;
		f->trail1 = f->trail2 = nullptr;
	}

	// Smoothed positions exist only while the frame is being drawn, so game
	// logic (collisions, triggers, progress) never sees them.
	void visit() {
		bool applied = this->applySmoothed();
		GJBaseGameLayer::visit();
		if (applied) this->restoreReal();
	}
};
