// Checks CardsToStrands::ChainSimulator, the chain a hanging braid swings on, against a head
// that stays still, runs, starts and stops, shakes, bobs, and turns (smoothly, fast, and in one
// frame, as the game's snap turns do). Builds on any platform with a C++20 compiler:
//
//   g++ -std=c++20 -O2 -I src/Features/HairStrands/CardsToStrands
//       tools/hair_cards_to_strands/chain_check.cpp
//       src/Features/HairStrands/CardsToStrands/CardsToStrands.cpp -o build/chain_check
//   (one command)
//
// Checked: a braid at rest sags under gravity and holds still; running carries it along rather
// than blowing it up behind the head; it is heavy: no motion lifts it more than a twelfth of its
// length above its styled place or swings it more than a third of its length from it, and a
// back rocking against it on a run does not kick it off; segments keep their length; joints
// stay out of the head, neck and back; it comes to rest once the head stops; the state stays
// finite. Exit code 1 on failure.

#include "CardsToStrands.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <vector>

using namespace CardsToStrands;

namespace
{
	constexpr float kStep = 1.0f / 60.0f;
	constexpr int kJoints = 10;
	constexpr float kSegment = 4.0f;
	constexpr float kLength = kSegment * (kJoints - 1);
	const Vec3 kPivot(0.0f, 0.0f, 100.0f);  // the neck: the head turns about it

	struct Pose
	{
		Vec3 move;
		float yaw = 0.0f;
		float pitch = 0.0f;
	};

	// The head's skin-to-world transform: a turn about the neck (yaw about Z, then pitch about X), then a move.
	Affine HeadTransform(const Pose& a_pose)
	{
		const float cy = std::cos(a_pose.yaw), sy = std::sin(a_pose.yaw), cp = std::cos(a_pose.pitch), sp = std::sin(a_pose.pitch);
		const float yaw[3][3] = { { cy, -sy, 0.0f }, { sy, cy, 0.0f }, { 0.0f, 0.0f, 1.0f } };
		const float pitch[3][3] = { { 1.0f, 0.0f, 0.0f }, { 0.0f, cp, -sp }, { 0.0f, sp, cp } };
		Affine affine;
		for (int r = 0; r < 3; ++r) {
			for (int c = 0; c < 3; ++c) {
				float sum = 0.0f;
				for (int k = 0; k < 3; ++k)
					sum += yaw[r][k] * pitch[k][c];
				affine.rows[r][c] = sum;
			}
		}
		const Vec3 translate = kPivot - affine.ApplyLinear(kPivot) + a_pose.move;
		for (int r = 0; r < 3; ++r)
			affine.rows[r][3] = translate[r];
		return affine;
	}

	struct Scenario
	{
		const char* name;
		float seconds;
		std::function<Pose(float)> pose;  // the head at a time
		float rock = 0.0f;                // how far the back sways towards the braid and away, at a run's stride
	};

	struct Outcome
	{
		float rise = 0.0f;         // the most a free joint rose above its styled place
		float swing = 0.0f;        // the furthest a free joint strayed from its styled place
		float stretch = 0.0f;      // the worst segment length error, as a fraction
		float penetration = 0.0f;  // the deepest a joint went into a collider, less the margin allowed
		float finalSpeed = 0.0f;   // units per step, after two still seconds at the end
		bool finite = true;
	};

	Outcome Run(const Scenario& a_scenario)
	{
		ChainCurve chain;
		for (int j = 0; j < kJoints; ++j)
			chain.joints.push_back({ 0.0f, -9.0f - 0.3f * j, 120.0f - kSegment * j });  // down the back of the head
		chain.pinnedJoints = 2;
		chain.radius = 1.0f;
		const ChainCollider head{ { 0.0f, 0.0f, 120.0f }, { 0.0f, 0.0f, 120.0f }, 8.0f };
		const ChainCollider neck{ { 0.0f, -1.0f, 110.0f }, { 0.0f, -1.0f, 100.0f }, 4.0f };
		const ChainCollider back{ { 0.0f, -6.0f, 100.0f }, { 0.0f, -6.0f, 70.0f }, 6.5f };  // the braid lies against it

		ChainSimulator sim;
		const ChainSettings settings;
		Outcome outcome;
		const int moving = static_cast<int>(a_scenario.seconds / kStep);
		const int still = 120;
		Pose last;
		std::vector<Vec3> before;
		for (int step = 0; step < moving + still; ++step) {
			const Pose pose = step < moving ? a_scenario.pose(step * kStep) : last;
			if (step < moving)
				last = pose;
			const Affine parent = HeadTransform(pose);
			ChainCollider colliders[3] = { head, neck, back };
			const Vec3 sway(0.0f, -a_scenario.rock * std::sin(2.0f * 3.14159265f * 2.5f * std::min(step, moving) * kStep), 0.0f);
			colliders[2].a = colliders[2].a + sway;
			colliders[2].b = colliders[2].b + sway;
			for (auto& collider : colliders) {
				collider.a = parent.Apply(collider.a);
				collider.b = parent.Apply(collider.b);
			}
			if (step == 0)
				sim.Reset(chain, parent);
			before = sim.Joints();
			sim.Step(chain, parent, kStep, settings, colliders, a_scenario.rock > 0.0f ? 3 : 2);
			const auto& joints = sim.Joints();
			for (int j = 0; j < kJoints; ++j) {
				const Vec3 styled = parent.Apply(chain.joints[j]);
				outcome.finite = outcome.finite && std::isfinite(joints[j].x) && std::isfinite(joints[j].y) && std::isfinite(joints[j].z);
				if (j >= static_cast<int>(chain.pinnedJoints)) {
					outcome.rise = std::max(outcome.rise, joints[j].z - styled.z);
					outcome.swing = std::max(outcome.swing, (joints[j] - styled).Length());
				}
				if (j + 1 < kJoints)
					outcome.stretch = std::max(outcome.stretch, std::abs((joints[j + 1] - joints[j]).Length() / kSegment - 1.0f));
				for (int k = 0; k < (a_scenario.rock > 0.0f ? 3 : 2); ++k) {
					const auto& collider = colliders[k];
					const Vec3 d = collider.b - collider.a;
					const float s = d.LengthSquared() > 0.0f ? std::clamp((joints[j] - collider.a).Dot(d) / d.LengthSquared(), 0.0f, 1.0f) : 0.0f;
					const float distance = (joints[j] - (collider.a + d * s)).Length();
					outcome.penetration = std::max(outcome.penetration, 0.5f * (collider.radius + chain.radius) - distance);
				}
				if (step == moving + still - 1)
					outcome.finalSpeed = std::max(outcome.finalSpeed, (joints[j] - before[j]).Length());
			}
		}
		return outcome;
	}
}

int main()
{
	const Scenario scenarios[] = {
		{ "rest", 4.0f, [](float) { return Pose{}; } },
		{ "run", 4.0f, [](float t) { return Pose{ { 0.0f, 300.0f * t, 0.0f } }; } },
		{ "start and stop", 4.0f, [](float t) { return Pose{ { 0.0f, 300.0f * std::clamp(t - 0.5f, 0.0f, 2.0f), 0.0f } }; } },
		{ "spin", 4.0f, [](float t) { return Pose{ {}, 3.0f * t }; } },
		{ "head shake", 4.0f, [](float t) { return Pose{ {}, 0.8f * std::sin(6.0f * t), 0.3f * std::sin(4.0f * t) }; } },
		{ "run and bob", 4.0f, [](float t) { return Pose{ { 0.0f, 300.0f * t, 20.0f * std::sin(10.0f * t) }, 0.5f * std::sin(3.0f * t) }; } },
		{ "snap turns", 4.0f, [](float t) { return Pose{ {}, static_cast<int>(t / 1.5f) % 2 ? 3.14159f : 0.0f }; } },
		{ "fast turns", 4.0f, [](float t) { return Pose{ {}, 3.14159f * std::min(1.0f, std::fmod(t, 1.5f) / 0.15f) }; } },
		{ "back rocking", 4.0f, [](float t) { return Pose{ { 0.0f, 300.0f * t, 0.0f } }; }, 3.0f },
	};

	bool ok = true;
	for (const auto& scenario : scenarios) {
		const Outcome o = Run(scenario);
		const bool pass = o.finite && o.rise <= kLength / 12.0f && o.swing <= kLength / 3.0f && o.stretch <= 0.02f && o.penetration <= 0.01f && o.finalSpeed <= 0.01f;
		std::printf("%-15s %s  rise %5.2f  swing %5.2f  stretch %5.3f  penetration %5.2f  moving %6.4f/step at the end\n", scenario.name, pass ? "ok  " : "FAIL", o.rise, o.swing, o.stretch,
			o.penetration, o.finalSpeed);
		ok = ok && pass;
	}
	return ok ? 0 : 1;
}
