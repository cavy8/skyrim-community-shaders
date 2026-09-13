// Adapted from NativeSystemMenuFramework (https://github.com/RoseEden30/NativeSystemMenuFramework)
// License: GPL-3.0-or-later

#include "NativeMenu/Vendor/SystemMenuHook.h"

#include "NativeMenu/Vendor/VanillaSettingsEngine.h"

#include "Globals.h"

#include <atomic>
#include <vector>

namespace NativeMenu::Vendor::SystemMenuHook
{
	namespace
	{
		constexpr const char* kMenuRootPath = "_root.QuestJournalFader.Menu_mc";
		constexpr const char* kSystemPageMember = "__cs_systemPage";
		constexpr int kInjectionRetryTicks = 150;
		// A freshly-opened menu's display list can still be under construction on the
		// first tick or two; letting it settle before the BFS walks it structurally
		// avoids reading a movie clip mid-attach.
		constexpr int kInjectionSettleTicks = 2;

		std::atomic<bool> g_injected{ false };
		std::atomic<int>  g_injectTicks{ 0 };
		// Set once a walk of the menu's Flash tree raises a hardware exception (a stale
		// GFxValue reached through a modded or half-built object graph). This hook is a
		// nice-to-have layered on top of vanilla, not something worth crashing the game
		// over, so the whole feature is switched off for the rest of the session rather
		// than risking a repeat on the very next tick.
		std::atomic<bool> g_disabled{ false };

		bool IsSystemPage(const RE::GFxValue& a_value)
		{
			return a_value.IsObject() && a_value.HasMember("CategoryList") && a_value.HasMember("SettingsList") &&
				a_value.HasMember("MappingList");
		}

		// BFS for SystemPage by structural signature.
		bool FindSystemPage(const RE::GFxValue& a_root, RE::GFxValue& a_out, int a_maxDepth)
		{
			std::vector<RE::GFxValue> current{ a_root };
			int                       budget = 2500;
			for (int depth = 0; depth <= a_maxDepth && !current.empty(); ++depth) {
				std::vector<RE::GFxValue> next;
				for (auto& node : current) {
					if (--budget <= 0)
						return false;
					if (IsSystemPage(node)) {
						a_out = node;
						return true;
					}
					if (depth == a_maxDepth)
						continue;

					node.VisitMembers([&next](const char* a_name, const RE::GFxValue& a_val) {
						static constexpr std::string_view skip[] = { "_parent", "_root", "_global", "_level0",
							"__proto__", "prototype", "constructor", "_listeners", "stage" };
						for (auto s : skip)
							if (s == a_name)
								return;
						if (a_val.IsObject() || a_val.IsArray() || a_val.IsDisplayObject())
							next.push_back(a_val);
					});
				}
				current.swap(next);
			}
			return false;
		}

		void Tick(RE::JournalMenu* a_this)
		{
			if (!a_this || !a_this->uiMovie)
				return;
			auto* view = a_this->uiMovie.get();

			if (!g_injected.load()) {
				const auto ticksLeft = g_injectTicks.load();
				if (ticksLeft <= 0)
					return;
				g_injectTicks.fetch_sub(1);
				if (ticksLeft > kInjectionRetryTicks)
					return;  // still settling; see kInjectionSettleTicks.

				RE::GFxValue root, page;
				if (view->GetVariable(&root, kMenuRootPath) && FindSystemPage(root, page, 10)) {
					root.SetMember(kSystemPageMember, page);
					g_injected.store(true);
					logger::info("NativeMenu: found the System menu's SettingsPage");
				}
				return;
			}

			RE::GFxValue root, systemPage;
			if (!view->GetVariable(&root, kMenuRootPath) ||
				!root.GetMember(kSystemPageMember, &systemPage) || !systemPage.IsObject())
				return;

			VanillaSettingsEngine::Tick(a_this, view, systemPage);
		}

		// Tick() walks Flash objects it doesn't own - vanilla's own menu, possibly
		// reshaped by another mod's replacer or hook - so a stale or half-built GFxValue
		// reaching into it can raise a hardware exception rather than fail cleanly.
		// __except performs a normal stack unwind on x64 (unlike x86), so Tick()'s own
		// std::lock_guard still unlocks correctly if this fires mid-tick.
		void TickGuarded(RE::JournalMenu* a_this) noexcept
		{
			__try {
				Tick(a_this);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				g_disabled.store(true);
				logger::critical(
					"NativeMenu: caught exception {:#x} walking the System menu's Flash tree - "
					"disabling the System menu injection for the rest of this session",
					static_cast<unsigned long>(GetExceptionCode()));
			}
		}

		struct JournalMenu_AdvanceMovie
		{
			static void thunk(RE::JournalMenu* a_this, float a_interval, std::uint32_t a_currentTime)
			{
				func(a_this, a_interval, a_currentTime);
				if (!g_disabled.load())
					TickGuarded(a_this);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// Reset injection state each time the System menu opens.
		class JournalSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
		public:
			static JournalSink* GetSingleton()
			{
				static JournalSink singleton;
				return &singleton;
			}

			RE::BSEventNotifyControl ProcessEvent(
				const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				if (a_event && a_event->menuName == RE::JournalMenu::MENU_NAME) {
					g_injected.store(false);
					g_injectTicks.store(a_event->opening ? kInjectionRetryTicks + kInjectionSettleTicks : 0);
					VanillaSettingsEngine::Reset();
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
	}

	void Install()
	{
		static bool installed = false;
		if (installed)
			return;
		installed = true;

		stl::write_vfunc<0x5, JournalMenu_AdvanceMovie>(RE::JournalMenu::VTABLE[0]);
		if (auto* ui = globals::game::ui)
			ui->GetEventSource<RE::MenuOpenCloseEvent>()->AddEventSink(JournalSink::GetSingleton());
		logger::info("NativeMenu: System menu hooks installed");
	}
}
