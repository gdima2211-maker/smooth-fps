#include <Geode/Geode.hpp>
#include <Geode/modify/CCDirector.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/MenuLayer.hpp>

#include <algorithm>

using namespace geode::prelude;

// ---------------------------------------------------------------------------
// Настройки
// ---------------------------------------------------------------------------

static int targetFPS() {
	auto v = Mod::get()->getSettingValue<int64_t>("fps");
	return std::clamp<int>(static_cast<int>(v), 24, 1440);
}

static bool smoothEnabled() {
	return Mod::get()->getSettingValue<bool>("interpolation");
}

static void applyFPS() {
	// Вызов проходит через хук ниже, который подставит нужное значение.
	CCDirector::get()->setAnimationInterval(1.0 / targetFPS());
}

$on_mod(Loaded) {
	listenForSettingChanges<int64_t>("fps", [](int64_t) {
		applyFPS();
	});
}

// ---------------------------------------------------------------------------
// Частота кадров отрисовки (физика не затрагивается)
// ---------------------------------------------------------------------------

// Игра сама вызывает setAnimationInterval при смене настроек, поэтому
// подменяем значение, чтобы наш FPS не сбрасывался.
class $modify(SmoothDirector, CCDirector) {
	void setAnimationInterval(double) {
		CCDirector::setAnimationInterval(1.0 / targetFPS());
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
// Интерполяция: только визуал
//
// Схема на каждый кадр:
//   1. Если в прошлом кадре мы подставляли визуальные позиции, возвращаем
//      настоящие, ДО того как игра посчитает физику.
//   2. Игра считает кадр как обычно (физика целиком её).
//   3. Запоминаем настоящие позиции и на время отрисовки подставляем
//      промежуточные между предыдущим и текущим тиком.
// ---------------------------------------------------------------------------

class $modify(SmoothGameLayer, GJBaseGameLayer) {
	struct Fields {
		bool applied = false;
		bool hasData = false;
		float sinceStep = 0.f;

		CCPoint real1, real2, realCam;
		CCPoint prev1, curr1, prev2, curr2, prevCam, currCam;
	};

	void restoreReal() {
		auto f = m_fields.self();
		if (!f->applied) return;
		f->applied = false;
		if (m_player1) m_player1->setPosition(f->real1);
		if (m_player2) m_player2->setPosition(f->real2);
		if (m_objectLayer) m_objectLayer->setPosition(f->realCam);
	}

	static CCPoint lerp(CCPoint const& a, CCPoint const& b, float t) {
		return ccp(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t);
	}

	void update(float dt) {
		auto f = m_fields.self();

		this->restoreReal();
		GJBaseGameLayer::update(dt);

		// Работаем только в обычном уровне, не в редакторе
		auto self = static_cast<GJBaseGameLayer*>(this);
		if (!smoothEnabled() || !typeinfo_cast<PlayLayer*>(self)) {
			f->hasData = false;
			return;
		}
		if (!m_player1 || !m_objectLayer || m_player1->m_isDead) {
			f->hasData = false;
			return;
		}

		CCPoint n1 = m_player1->getPosition();
		CCPoint n2 = m_player2 ? m_player2->getPosition() : n1;
		CCPoint nc = m_objectLayer->getPosition();

		constexpr float stepTime = 1.f / 240.f;

		if (!f->hasData) {
			f->prev1 = f->curr1 = n1;
			f->prev2 = f->curr2 = n2;
			f->prevCam = f->currCam = nc;
			f->sinceStep = 0.f;
			f->hasData = true;
			return;
		}

		bool stepped = !n1.equals(f->curr1) || !n2.equals(f->curr2) || !nc.equals(f->currCam);
		if (stepped) {
			f->prev1 = f->curr1; f->curr1 = n1;
			f->prev2 = f->curr2; f->curr2 = n2;
			f->prevCam = f->currCam; f->currCam = nc;
			f->sinceStep = 0.f;

			// Телепорт, респавн, портал: не сглаживаем большой скачок
			if (ccpDistance(f->prev1, f->curr1) > 120.f || ccpDistance(f->prevCam, f->currCam) > 120.f) {
				f->prev1 = f->curr1;
				f->prev2 = f->curr2;
				f->prevCam = f->currCam;
			}
		} else {
			f->sinceStep += dt;
		}

		// Если кадр дольше тика, сглаживать нечего
		if (dt >= stepTime) return;

		float t = std::clamp(f->sinceStep / stepTime, 0.f, 1.f);

		f->real1 = n1;
		f->real2 = n2;
		f->realCam = nc;
		f->applied = true;

		m_player1->setPosition(lerp(f->prev1, f->curr1, t));
		if (m_player2) m_player2->setPosition(lerp(f->prev2, f->curr2, t));
		m_objectLayer->setPosition(lerp(f->prevCam, f->currCam, t));
	}
};
