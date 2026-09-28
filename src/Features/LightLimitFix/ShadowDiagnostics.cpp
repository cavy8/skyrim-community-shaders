#include "ShadowDiagnostics.h"

#include "Features/Effects11.h"
#include "Features/InverseSquareLighting.h"

#include <numbers>
#include <psapi.h>

namespace LocalShadowDiagnostics
{
	namespace
	{
		constexpr uint32_t LIGHT_LOG_INTERVAL = 120;
		constexpr uint32_t LOG_BUDGET_FRAMES = 60;
		constexpr uint32_t LOG_BUDGET_LINES = 30;
		constexpr uint32_t SUMMARY_INTERVAL = 600;
		constexpr uint32_t BLACKOUT_MAX_FRAMES = 30;
		constexpr uint32_t STUCK_FRAMES = 120;
		constexpr uint32_t CHURN_MAX_FRAMES = 300;
		constexpr uint32_t REMOVED_RETENTION = 600;
		constexpr uint32_t CELL_GRACE_FRAMES = 120;
		constexpr uint32_t CELL_GRACE_WINDOW = 30;
		constexpr uint32_t HOOK_CHECK_INTERVAL = 1800;
		constexpr uint32_t MISSED_SCHEDULE_FRAMES = 30;
		constexpr uint32_t OVER_CAP_LOG_INTERVAL = 600;
		constexpr uint32_t VANILLA_POINT_SLICES = 4;
		constexpr size_t EVENT_RING = 256;
		constexpr size_t FRAME_RING = 120;
		constexpr size_t FRAME_IDS = 4;
		constexpr size_t RENDERED_IDS = 8;
		constexpr float MAX_SANE_COLOR = 1.0e4f;
		constexpr float MAX_SANE_RADIUS = 1.0e5f;
		constexpr auto REPORT_NOTICE_DURATION = std::chrono::seconds(4);
		constexpr uint32_t REPORT_KEY = VK_F11;

		struct TrackedLight
		{
			uint32_t id = 0;
			RE::NiLight* niLight = nullptr;
			std::string identity;
			bool identityLogged = false;
			uint32_t firstFrame = 0;
			uint32_t seenFrame = 0;
			bool removed = false;
			uint32_t removedFrame = 0;
			uint32_t litFrame = 0;
			uint32_t shadowedFrame = 0;
			bool wasLit = false;
			bool wasShadowed = false;
			bool everShadowed = false;
			uint32_t unshadowedSince = 0;
			uint32_t unshadowedOnScreen = 0;
			bool stuckLogged = false;
			uint32_t darkSince = 0;
			std::string darkReason;
			Loss loss = Loss::None;
			uint32_t lossFrame = 0;
			uint32_t lossBy = 0;
			float lossDistance = 0.0f;
			uint32_t noSliceFrame = 0;
			uint32_t lastLogFrame = 0;
			uint32_t renders = 0;
			uint32_t losses = 0;
			uint32_t lodRestores = 0;
			bool zeroRadiusLogged = false;
		};

		struct Counters
		{
			uint32_t frames = 0;
			uint32_t schedules = 0;
			uint32_t multiSchedules = 0;
			uint32_t renders = 0;
			uint32_t lost = 0;
			uint32_t lostOnScreen = 0;
			uint32_t unshadowedWindows = 0;
			uint32_t unshadowedOnScreenWindows = 0;
			uint32_t longestOnScreen = 0;
			uint32_t evictions = 0;
			uint32_t preemptions = 0;
			uint32_t resets = 0;
			uint32_t noSlice = 0;
			uint32_t removals = 0;
			uint32_t churn = 0;
			uint32_t demotions = 0;
			uint32_t blackouts = 0;
			uint32_t collisions = 0;
			uint32_t foreignRenders = 0;
			uint32_t badSlices = 0;
			uint32_t overCapFrames = 0;
			uint32_t badData = 0;
			uint32_t lodRestores = 0;
			uint32_t suppressed = 0;
			uint32_t maxTracked = 0;

			bool Noteworthy() const
			{
				return lostOnScreen || unshadowedOnScreenWindows || churn || demotions || blackouts || collisions || foreignRenders || badSlices || overCapFrames || badData || multiSchedules;
			}
		};

		struct FrameRecord
		{
			uint32_t frame = 0;
			uint32_t schedulerFrame = 0;
			uint32_t schedules = 0;
			uint32_t tracked = 0;
			uint32_t lit = 0;
			uint32_t shadowed = 0;
			uint32_t unshadowedOnScreen = 0;
			uint32_t events = 0;
			uint32_t allowedCount = 0;
			uint32_t renderedCount = 0;
			uint32_t unshadowedCount = 0;
			std::array<uint32_t, FRAME_IDS> allowed{};
			std::array<uint32_t, RENDERED_IDS> rendered{};
			std::array<uint32_t, FRAME_IDS> unshadowed{};
		};

		struct View
		{
			bool valid = false;
			RE::NiPoint3 position{};
			RE::NiPoint3 forward{};
			float halfAngle = 0.0f;
		};

		struct HookStatus
		{
			std::string text;
			bool conflict = false;
		};

		struct DiagnosticsState
		{
			bool active = false;
			uint32_t frame = 1;
			uint32_t nextId = 0;
			std::unordered_map<RE::BSShadowLight*, TrackedLight> lights;
			std::unordered_map<RE::NiLight*, RE::BSShadowLight*> removedNiLights;
			std::unordered_map<RE::NiLight*, uint32_t> badDataLogged;
			Counters window{};
			uint32_t windowStart = 1;
			uint32_t schedulesThisFrame = 0;
			uint32_t eventsThisFrame = 0;
			uint32_t missedSchedules = 0;
			uint32_t overCapLogFrame = 0;
			uint32_t budgetFrame = 1;
			uint32_t budgetUsed = 0;
			uint32_t lastHookCheck = 0;
			std::array<std::string, 3> hookStatus;
			RE::TESObjectCELL* cell = nullptr;
			uint32_t cellChangeFrame = 0;
			std::string cellText;
			View view{};
			uint32_t viewFrame = 0;
			std::vector<RE::BSShadowLight*> renderedThisFrame;
			std::array<std::string, EVENT_RING> events;
			size_t eventHead = 0;
			size_t eventCount = 0;
			std::array<FrameRecord, FRAME_RING> frames;
			size_t frameHead = 0;
			size_t frameCount = 0;
			std::atomic<bool> reportRequested = false;
			uint32_t reportCount = 0;
			std::chrono::steady_clock::time_point reportTime{};
		};

		DiagnosticsState& State()
		{
			static DiagnosticsState state;
			return state;
		}

		LightLimitFix& Feature()
		{
			return globals::features::lightLimitFix;
		}

		bool IsTracking()
		{
			auto& llf = Feature();
			return llf.settings.EnableLocalShadows && llf.localShadowCache != nullptr;
		}

		std::string ToUtf8(const std::filesystem::path& a_path)
		{
			return stl::utf16_to_utf8(a_path.wstring()).value_or("<unprintable>");
		}

		HMODULE ModuleOf(std::uintptr_t a_address)
		{
			HMODULE module = nullptr;
			if (!a_address || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(a_address), &module))
				return nullptr;
			return module;
		}

		std::string ModuleName(HMODULE a_module)
		{
			wchar_t buffer[MAX_PATH]{};
			const DWORD size = a_module ? GetModuleFileNameW(a_module, buffer, MAX_PATH) : 0;
			if (size == 0 || size == MAX_PATH)
				return "<unknown module>";
			return ToUtf8(std::filesystem::path(std::wstring_view(buffer, size)).filename());
		}

		std::string DescribeAddress(std::uintptr_t a_address)
		{
			if (auto module = ModuleOf(a_address))
				return fmt::format("{}+0x{:X}", ModuleName(module), a_address - reinterpret_cast<std::uintptr_t>(module));
			return fmt::format("0x{:X} (no module)", a_address);
		}

		bool ReadMemory(std::uintptr_t a_address, void* a_out, size_t a_size)
		{
			MEMORY_BASIC_INFORMATION info{};
			if (!a_address || !VirtualQuery(reinterpret_cast<LPCVOID>(a_address), &info, sizeof(info)) || info.State != MEM_COMMIT)
				return false;
			constexpr DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
			if (!(info.Protect & readable) || (info.Protect & PAGE_GUARD))
				return false;
			if (a_address + a_size > reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize)
				return false;
			std::memcpy(a_out, reinterpret_cast<const void*>(a_address), a_size);
			return true;
		}

		std::uintptr_t FollowJump(std::uintptr_t a_address)
		{
			uint8_t code[14]{};
			if (!ReadMemory(a_address, code, sizeof(code)) && !ReadMemory(a_address, code, 6))
				return 0;
			if (code[0] == 0xE9) {
				int32_t rel = 0;
				std::memcpy(&rel, code + 1, sizeof(rel));
				return a_address + 5 + static_cast<std::intptr_t>(rel);
			}
			if (code[0] == 0xEB)
				return a_address + 2 + static_cast<int8_t>(code[1]);
			if (code[0] == 0xFF && code[1] == 0x25) {
				int32_t displacement = 0;
				std::memcpy(&displacement, code + 2, sizeof(displacement));
				std::uintptr_t target = 0;
				return ReadMemory(a_address + 6 + static_cast<std::intptr_t>(displacement), &target, sizeof(target)) ? target : 0;
			}
			if (code[0] == 0x48 && code[1] == 0xB8 && code[10] == 0xFF && code[11] == 0xE0) {
				std::uintptr_t target = 0;
				std::memcpy(&target, code + 2, sizeof(target));
				return target;
			}
			if (code[0] == 0x49 && code[1] == 0xBB && code[10] == 0x41 && code[11] == 0xFF && code[12] == 0xE3) {
				std::uintptr_t target = 0;
				std::memcpy(&target, code + 2, sizeof(target));
				return target;
			}
			if (code[0] == 0x68 && code[5] == 0xC7 && code[6] == 0x44 && code[7] == 0x24 && code[8] == 0x04 && code[13] == 0xC3) {
				uint32_t low = 0;
				uint32_t high = 0;
				std::memcpy(&low, code + 1, sizeof(low));
				std::memcpy(&high, code + 9, sizeof(high));
				return (static_cast<std::uintptr_t>(high) << 32) | low;
			}
			return 0;
		}

		std::uintptr_t ResolveJumps(std::uintptr_t a_address, std::string* a_path = nullptr)
		{
			std::uintptr_t current = a_address;
			for (uint32_t hop = 0; hop < 8; hop++) {
				const std::uintptr_t next = FollowJump(current);
				if (!next || next == current)
					break;
				current = next;
				if (a_path)
					*a_path += " -> " + DescribeAddress(current);
			}
			return current;
		}

		HookStatus CheckEntry(std::string_view a_name, std::uintptr_t a_entry, std::uintptr_t a_original)
		{
			const HMODULE self = ModuleOf(reinterpret_cast<std::uintptr_t>(&IsActive));
			const HMODULE game = GetModuleHandleW(nullptr);

			std::string entryPath;
			const HMODULE entryModule = ModuleOf(ResolveJumps(a_entry, &entryPath));
			std::string originalPath;
			const std::uintptr_t original = ResolveJumps(a_original, &originalPath);
			const HMODULE originalModule = ModuleOf(original);

			HookStatus status;
			std::string text = fmt::format("{}: entry {}{}", a_name, DescribeAddress(a_entry), entryPath);
			if (entryModule != self) {
				status.conflict = true;
				text += fmt::format(" -- TAKEN OVER by {}, which runs before Community Shaders", ModuleName(entryModule));
			}
			text += fmt::format("; original {}{}", DescribeAddress(a_original), originalPath);
			if (originalModule && originalModule != game && originalModule != self) {
				status.conflict = true;
				text += fmt::format(" -- ALSO HOOKED by {}, which runs after Community Shaders", ModuleName(originalModule));
			}
			status.text = std::move(text);
			return status;
		}

		std::uintptr_t ReadVTableSlot(REL::VariantID a_vtable, size_t a_index)
		{
			REL::Relocation<std::uintptr_t> vtable{ a_vtable };
			std::uintptr_t value = 0;
			ReadMemory(vtable.address() + a_index * sizeof(std::uintptr_t), &value, sizeof(value));
			return value;
		}

		void CheckHooks(bool a_logAll)
		{
			auto& state = State();
			using Hooks = LightLimitFix::Hooks;
			const HookStatus statuses[] = {
				CheckEntry("CalculateActiveShadowCasterLights", REL::RelocationID(100419, 107137).address(), Hooks::CalculateActiveShadowCasterLights::func.address()),
				CheckEntry("BSShadowParabolicLight::UpdateCamera", ReadVTableSlot(RE::VTABLE_BSShadowParabolicLight[0], 0x10), Hooks::BSShadowParabolicLight_UpdateCamera::func.address()),
				CheckEntry("BSShadowFrustumLight::UpdateCamera", ReadVTableSlot(RE::VTABLE_BSShadowFrustumLight[0], 0x10), Hooks::BSShadowFrustumLight_UpdateCamera::func.address()),
			};
			for (size_t i = 0; i < std::size(statuses); i++) {
				const auto& status = statuses[i];
				if (!a_logAll && status.text == state.hookStatus[i])
					continue;
				state.hookStatus[i] = status.text;
				if (status.conflict)
					logger::warn("[LLF][ShadowDiag] Hook conflict: {}", status.text);
				else
					logger::info("[LLF][ShadowDiag] Hook ok: {}", status.text);
			}
		}

		void LogLoadedModules()
		{
			std::vector<HMODULE> modules(1024);
			DWORD needed = 0;
			if (!EnumProcessModules(GetCurrentProcess(), modules.data(), static_cast<DWORD>(modules.size() * sizeof(HMODULE)), &needed))
				return;
			modules.resize(std::min<size_t>(needed / sizeof(HMODULE), modules.size()));

			wchar_t exeBuffer[MAX_PATH]{};
			const DWORD exeSize = GetModuleFileNameW(nullptr, exeBuffer, MAX_PATH);
			const std::filesystem::path exePath(std::wstring_view(exeBuffer, exeSize));
			auto lower = [](std::wstring a_text) {
				std::transform(a_text.begin(), a_text.end(), a_text.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
				return a_text;
			};
			const std::wstring gameDirectory = lower(exePath.parent_path().wstring());

			std::vector<std::string> sksePlugins;
			std::vector<std::string> gameFolder;
			for (auto module : modules) {
				wchar_t buffer[MAX_PATH]{};
				const DWORD size = GetModuleFileNameW(module, buffer, MAX_PATH);
				if (size == 0 || size == MAX_PATH)
					continue;
				const std::filesystem::path path(std::wstring_view(buffer, size));
				const std::wstring lowerPath = lower(path.wstring());
				if (lowerPath.find(L"\\skse\\plugins\\") != std::wstring::npos)
					sksePlugins.push_back(ToUtf8(path.filename()));
				else if (lower(path.parent_path().wstring()) == gameDirectory && lowerPath != lower(exePath.wstring()))
					gameFolder.push_back(ToUtf8(path.filename()));
			}
			auto join = [](std::vector<std::string>& a_names) {
				std::sort(a_names.begin(), a_names.end());
				std::string text;
				for (const auto& name : a_names)
					text += (text.empty() ? "" : ", ") + name;
				return text;
			};
			logger::info("[LLF][ShadowDiag] SKSE plugins loaded ({}): {}", sksePlugins.size(), join(sksePlugins));
			logger::info("[LLF][ShadowDiag] DLLs loaded from the game folder ({}): {}", gameFolder.size(), join(gameFolder));
		}

		float ReadIniFloat(const char* a_name, float a_fallback)
		{
			RE::Setting* setting = globals::game::iniSettingCollection ? globals::game::iniSettingCollection->GetSetting(a_name) : nullptr;
			if (!setting && globals::game::iniPrefSettingCollection)
				setting = globals::game::iniPrefSettingCollection->GetSetting(a_name);
			if (!setting)
				return a_fallback;
			return a_name[0] == 'i' ? static_cast<float>(setting->data.i) : setting->data.f;
		}

		void LogSettings(const LightLimitFix& a_llf)
		{
			const auto& settings = a_llf.settings;
			logger::info("[LLF][ShadowDiag] Settings: local shadows {}, cache slots {}, cache resolution {}, filter samples {}, filter scale {:.2f}, contact shadows {} (steps {}, length {:.1f}, thickness {:.1f}, strength {:.2f}), particle lights {}, ISL {}, Effects11 {}",
				settings.EnableLocalShadows, settings.LocalShadowSlots, settings.LocalShadowResolution == 0 ? "match game"s : std::to_string(settings.LocalShadowResolution), settings.LocalShadowSamples, settings.LocalShadowFilterScale,
				settings.EnableContactShadows, settings.ContactShadowMaxSteps, settings.ContactShadowLength, settings.ContactShadowDepthThickness, settings.ContactShadowStrength, settings.EnableParticleLights,
				globals::features::inverseSquareLighting.loaded, globals::features::effects11.enableEffect);
			const float cullSquared = *reinterpret_cast<float*>(REL::RelocationID(528316, 415264).address());
			const float lightFadeEndSquared = *reinterpret_cast<float*>(REL::RelocationID(527669, 414583).address());
			logger::info("[LLF][ShadowDiag] Game INI: fShadowDistance {:.0f}, fInteriorShadowDistance {:.0f}, iShadowMapResolution {:.0f}, fShadowBiasScale {:.3f}, fPoissonRadiusScale {:.2f}; engine point/spot casters per frame {}, point/spot shadow cull {:.0f}, light fade end {:.0f}",
				ReadIniFloat("fShadowDistance:Display", -1.0f), ReadIniFloat("fInteriorShadowDistance:Display", -1.0f), ReadIniFloat("iShadowMapResolution:Display", -1.0f),
				ReadIniFloat("fShadowBiasScale:Display", -1.0f), ReadIniFloat("fPoissonRadiusScale:Display", -1.0f), LightLimitFix::ENGINE_LOCAL_SHADOW_CASTERS,
				std::sqrt(std::max(cullSquared, 0.0f)), std::sqrt(std::max(lightFadeEndSquared, 0.0f)));
			if (a_llf.localShadowCache)
				logger::info("[LLF][ShadowDiag] Shadow cache: {} slots at {}x{}, engine shadow maps {}x{} with {} slices, {}", a_llf.localShadowCacheSlots, a_llf.localShadowCacheResolution, a_llf.localShadowCacheResolution,
					a_llf.localShadowEngineResolution, a_llf.localShadowEngineResolution, a_llf.localShadowEngineSlices, a_llf.localShadowDirectCopy ? "direct copy" : "compute copy");
			else
				logger::info("[LLF][ShadowDiag] Shadow cache: not allocated");
		}

		std::string SafeName(const RE::NiObjectNET* a_object)
		{
			const char* name = a_object ? a_object->name.c_str() : nullptr;
			return name && *name ? name : "<unnamed>";
		}

		std::string DescribeForm(const RE::TESForm* a_form)
		{
			std::string text = fmt::format("{:08X}", a_form->GetFormID());
			const auto editorID = clib_util::editorID::get_editorID(a_form);
			if (!editorID.empty())
				text += fmt::format(" '{}'", editorID);
			const auto* first = a_form->GetFile(0);
			const auto* last = a_form->GetFile(-1);
			text += fmt::format(" [{}", first ? first->GetFilename() : "no plugin"sv);
			if (last && last != first)
				text += fmt::format(", last edited by {}", last->GetFilename());
			text += "]";
			return text;
		}

		const char* ShadowType(RE::BSShadowLight* a_light)
		{
			if (a_light->GetIsParabolicLight())
				return a_light->shadowMapCount == 2 ? "omni" : "hemisphere";
			return a_light->GetIsFrustumLight() ? "spot" : "shadow";
		}

		std::string DescribeLight(RE::BSLight* a_light)
		{
			auto* niLight = a_light ? a_light->light.get() : nullptr;
			if (!niLight)
				return "<no NiLight>";

			std::string text = fmt::format("{} light '{}'", a_light->IsShadowLight() ? ShadowType(static_cast<RE::BSShadowLight*>(a_light)) : "point", SafeName(niLight));
			if (niLight->parent)
				text += fmt::format(" under '{}'", SafeName(niLight->parent));

			RE::TESObjectREFR* reference = nullptr;
			const RE::NiAVObject* node = niLight;
			for (uint32_t depth = 0; node && !reference && depth < 64; depth++, node = node->parent)
				reference = node->GetUserData();
			if (!reference)
				return text + " (no owning reference)";

			text += " ref " + DescribeForm(reference);
			if (const char* name = reference->GetName(); name && *name)
				text += fmt::format(" \"{}\"", name);
			if (auto* base = reference->GetBaseObject()) {
				text += fmt::format(" base {} {}", RE::FormTypeToString(base->GetFormType()), DescribeForm(base));
				if (auto* model = base->As<RE::TESModel>()) {
					if (const char* path = model->GetModel(); path && *path)
						text += fmt::format(" model '{}'", path);
				}
			}
			return text;
		}

		std::string DescribeCell(RE::TESObjectCELL* a_cell)
		{
			if (!a_cell)
				return "no cell";
			return fmt::format("{} cell {}", a_cell->IsInteriorCell() ? "interior" : "exterior", DescribeForm(a_cell));
		}

		const View& CurrentView()
		{
			auto& state = State();
			if (state.viewFrame == state.frame)
				return state.view;
			state.viewFrame = state.frame;
			state.view = {};
			auto* camera = RE::Main::WorldRootCamera();
			if (!camera)
				return state.view;
			const auto& rotate = camera->world.rotate;
			const auto& frustum = camera->GetRuntimeData2().viewFrustum;
			const float horizontal = std::max(std::abs(frustum.fLeft), std::abs(frustum.fRight));
			const float vertical = std::max(std::abs(frustum.fTop), std::abs(frustum.fBottom));
			state.view.position = camera->world.translate;
			state.view.forward = { rotate.entry[0][0], rotate.entry[1][0], rotate.entry[2][0] };
			state.view.halfAngle = frustum.bOrtho ? std::numbers::pi_v<float> : std::atan(std::sqrt(horizontal * horizontal + vertical * vertical));
			state.view.valid = state.view.forward.SqrLength() > 0.5f && std::isfinite(state.view.halfAngle);
			return state.view;
		}

		float BearingDegrees(const View& a_view, const RE::NiPoint3& a_point)
		{
			const RE::NiPoint3 toPoint = a_point - a_view.position;
			const float distance = toPoint.Length();
			if (distance < 1e-3f)
				return 0.0f;
			return std::acos(std::clamp(toPoint.Dot(a_view.forward) / distance, -1.0f, 1.0f)) * 180.0f / std::numbers::pi_v<float>;
		}

		bool TouchesView(const View& a_view, const RE::NiPoint3& a_center, float a_radius)
		{
			if (!a_view.valid)
				return true;
			const float distance = a_view.position.GetDistance(a_center);
			if (distance <= a_radius)
				return true;
			const float angularRadius = std::asin(std::min(a_radius / distance, 1.0f));
			return BearingDegrees(a_view, a_center) * std::numbers::pi_v<float> / 180.0f - angularRadius <= a_view.halfAngle;
		}

		std::string DescribeCamera(const View& a_view)
		{
			if (!a_view.valid)
				return "camera unknown";
			const float yaw = std::atan2(a_view.forward.x, a_view.forward.y) * 180.0f / std::numbers::pi_v<float>;
			const float pitch = std::asin(std::clamp(a_view.forward.z, -1.0f, 1.0f)) * 180.0f / std::numbers::pi_v<float>;
			return fmt::format("camera ({:.0f}, {:.0f}, {:.0f}) yaw {:.0f} pitch {:.0f}", a_view.position.x, a_view.position.y, a_view.position.z, yaw < 0.0f ? yaw + 360.0f : yaw, pitch);
		}

		TrackedLight& Track(RE::BSShadowLight* a_light)
		{
			auto& state = State();
			auto* niLight = a_light->light.get();
			auto [it, inserted] = state.lights.try_emplace(a_light);
			auto& tracked = it->second;
			if (!inserted && tracked.niLight != niLight) {
				tracked = TrackedLight{};
				inserted = true;
			}
			if (inserted) {
				tracked.id = ++state.nextId;
				tracked.niLight = niLight;
				tracked.identity = DescribeLight(a_light);
				tracked.firstFrame = state.frame;
			}
			return tracked;
		}

		TrackedLight* FindTracked(RE::BSShadowLight* a_light)
		{
			auto& lights = State().lights;
			auto it = lights.find(a_light);
			return it != lights.end() ? &it->second : nullptr;
		}

		void PushEvent(std::string a_text)
		{
			auto& state = State();
			state.events[state.eventHead] = std::move(a_text);
			state.eventHead = (state.eventHead + 1) % EVENT_RING;
			state.eventCount = std::min(state.eventCount + 1, EVENT_RING);
			state.eventsThisFrame++;
		}

		void Emit(TrackedLight* a_light, bool a_log, const std::string& a_text)
		{
			auto& state = State();
			PushEvent(fmt::format("f{} {}", state.frame, a_text));
			if (!a_log)
				return;
			if (a_light && a_light->lastLogFrame && state.frame - a_light->lastLogFrame < LIGHT_LOG_INTERVAL) {
				state.window.suppressed++;
				return;
			}
			if (state.budgetUsed >= LOG_BUDGET_LINES) {
				state.window.suppressed++;
				return;
			}
			state.budgetUsed++;
			logger::info("[LLF][ShadowDiag] f{} {}", state.frame, a_text);
			if (a_light) {
				a_light->lastLogFrame = state.frame;
				if (!a_light->identityLogged) {
					a_light->identityLogged = true;
					logger::info("[LLF][ShadowDiag]     #{} = {}", a_light->id, a_light->identity);
				}
			}
		}

		std::string Age(uint32_t a_now, uint32_t a_frame)
		{
			return a_frame ? fmt::format("{} frames ago", a_now - a_frame) : "never"s;
		}

		std::string LossText(const TrackedLight& a_light)
		{
			const uint32_t frame = State().frame;
			std::string text;
			switch (a_light.loss) {
			case Loss::Evicted:
				text = fmt::format("its cache slot was reclaimed by #{}", a_light.lossBy);
				break;
			case Loss::Preempted:
				text = fmt::format("its cache slot was preempted by actor-lit caster #{}", a_light.lossBy);
				break;
			case Loss::NewNiLight:
				text = "its BSShadowLight now wraps a different NiLight";
				break;
			case Loss::Teleported:
				text = fmt::format("it moved {:.0f} units since its last shadow render", a_light.lossDistance);
				break;
			case Loss::CacheReleased:
				text = "the shadow cache was released";
				break;
			default:
				return a_light.everShadowed ? "no recorded reason"s : "new caster, never cached"s;
			}
			return text + fmt::format(" ({} frames ago)", frame - a_light.lossFrame);
		}

		bool IsAllowed(const LightLimitFix& a_llf, RE::BSShadowLight* a_light)
		{
			return std::find(a_llf.localShadowAllowed.begin(), a_llf.localShadowAllowed.end(), a_light) != a_llf.localShadowAllowed.end();
		}

		std::string WaitReason(const LightLimitFix& a_llf, const LightLimitFix::LocalShadowCaster& a_caster, const TrackedLight& a_light)
		{
			const uint32_t now = a_llf.localShadowFrame;
			if (a_caster.hidden)
				return "hidden";
			if (a_caster.lastEligibleFrame == 0 || now - a_caster.lastEligibleFrame > LightLimitFix::LOCAL_SHADOW_CAMERA_HOLD_FRAMES)
				return fmt::format("the engine's shadow camera test has not passed it (last passed {}; re-tested every {} frames)", Age(now, a_caster.lastEligibleFrame), LightLimitFix::LOCAL_SHADOW_SWEEP_INTERVAL);
			if (now < a_caster.rejectUntilFrame)
				return fmt::format("the engine skipped it after its portal visibility test; retry in {} frames (streak {})", a_caster.rejectUntilFrame - now, a_caster.rejectStreak);
			if (a_light.noSliceFrame && State().frame - a_light.noSliceFrame < 5)
				return fmt::format("rendered, but none of the {} cache slots could be reclaimed (all owned by on-screen casters)", a_llf.localShadowCacheSlots);
			if (IsAllowed(a_llf, a_caster.light))
				return "scheduled for this frame";
			return fmt::format("waiting for a render turn (score {:.1f}, {} engine renders per frame)", a_caster.score, LightLimitFix::ENGINE_LOCAL_SHADOW_CASTERS);
		}

		std::string CasterState(const LightLimitFix& a_llf, const LightLimitFix::LocalShadowCaster& a_caster, const View& a_view)
		{
			const uint32_t now = a_llf.localShadowFrame;
			std::string text = fmt::format("{} r {:.0f} dist {:.0f} bearing {:.0f} | slice {} rendered {} | engine camera test passed {}", ShadowType(a_caster.light), a_caster.radius,
				a_view.valid ? a_view.position.GetDistance(a_caster.position) : -1.0f, a_view.valid ? BearingDegrees(a_view, a_caster.position) : -1.0f,
				a_caster.slice, Age(now, a_caster.lastRenderedFrame), Age(now, a_caster.lastEligibleFrame));
			if (now < a_caster.rejectUntilFrame)
				text += fmt::format(" | portal-rejected for {} more frames", a_caster.rejectUntilFrame - now);
			text += fmt::format(" | score {:.1f}", a_caster.score);
			if (a_caster.dynamic)
				text += " actor-lit";
			if (a_caster.hidden)
				text += " hidden";
			return text;
		}

		void EvaluateCasters(LightLimitFix& a_llf, const View& a_view, FrameRecord& a_record)
		{
			auto& state = State();
			const uint32_t frame = state.frame;
			const bool cellGrace = frame - state.cellChangeFrame < CELL_GRACE_FRAMES;

			for (auto& caster : a_llf.localShadowCasters) {
				if (!caster.light || !caster.niLight || caster.lastSeenFrame != a_llf.localShadowFrame)
					continue;
				auto& tracked = Track(caster.light);
				tracked.seenFrame = frame;
				const bool onScreen = TouchesView(a_view, caster.position, caster.radius);

				if (tracked.removed) {
					tracked.removed = false;
					state.removedNiLights.erase(tracked.niLight);
					const uint32_t gone = frame - tracked.removedFrame;
					if (gone <= CHURN_MAX_FRAMES) {
						state.window.churn++;
						Emit(&tracked, onScreen && !cellGrace, fmt::format("#{} came back to the shadow light list {} frames after it left it", tracked.id, gone));
					}
				}

				const bool lit = tracked.litFrame == frame;
				const bool shadowed = lit && tracked.shadowedFrame == frame;

				if (lit && !shadowed) {
					if (tracked.wasLit && tracked.wasShadowed) {
						tracked.losses++;
						state.window.lost++;
						if (onScreen)
							state.window.lostOnScreen++;
						Emit(&tracked, onScreen, fmt::format("#{} LOST its cached shadow while lit{}: {} | {}", tracked.id, onScreen ? " ON SCREEN" : "", LossText(tracked), CasterState(a_llf, caster, a_view)));
					}
					if (!tracked.unshadowedSince) {
						tracked.unshadowedSince = frame;
						tracked.unshadowedOnScreen = 0;
						tracked.stuckLogged = false;
					}
					if (onScreen) {
						tracked.unshadowedOnScreen++;
						a_record.unshadowedOnScreen++;
						if (a_record.unshadowedCount < FRAME_IDS)
							a_record.unshadowed[a_record.unshadowedCount++] = tracked.id;
						if (!tracked.stuckLogged && frame - tracked.unshadowedSince >= STUCK_FRAMES) {
							tracked.stuckLogged = true;
							Emit(&tracked, true, fmt::format("#{} has been lit on screen WITHOUT a shadow for {} frames: {} | {} | {}", tracked.id, frame - tracked.unshadowedSince, LossText(tracked), WaitReason(a_llf, caster, tracked), CasterState(a_llf, caster, a_view)));
						}
					}
				} else if (shadowed && tracked.unshadowedSince) {
					const uint32_t duration = frame - tracked.unshadowedSince;
					state.window.unshadowedWindows++;
					const bool wasOnScreen = tracked.unshadowedOnScreen > 0;
					if (wasOnScreen) {
						state.window.unshadowedOnScreenWindows++;
						state.window.longestOnScreen = std::max(state.window.longestOnScreen, tracked.unshadowedOnScreen);
					}
					const bool expectedAfterLoad = cellGrace && duration < CELL_GRACE_WINDOW;
					Emit(&tracked, wasOnScreen && !expectedAfterLoad, fmt::format("#{} was lit WITHOUT a shadow for {} frames ({} on screen) before its shadow {}: {} | {}", tracked.id, duration, tracked.unshadowedOnScreen,
																		   tracked.everShadowed ? "came back" : "first appeared", LossText(tracked), CasterState(a_llf, caster, a_view)));
					tracked.unshadowedSince = 0;
					tracked.loss = Loss::None;
				}

				if (!lit && tracked.wasLit) {
					tracked.darkSince = frame;
					if (caster.hidden)
						tracked.darkReason = "hidden flag set";
				} else if (lit && tracked.darkSince) {
					const uint32_t dark = frame - tracked.darkSince;
					if (dark <= BLACKOUT_MAX_FRAMES) {
						state.window.blackouts++;
						Emit(&tracked, onScreen, fmt::format("#{} went dark for {} frames and came back ({}) | {}", tracked.id, dark, tracked.darkReason, CasterState(a_llf, caster, a_view)));
					}
					tracked.darkSince = 0;
				}

				if (shadowed && caster.shadowParams.y <= 0.0f && !tracked.zeroRadiusLogged) {
					tracked.zeroRadiusLogged = true;
					Emit(&tracked, true, fmt::format("#{} has a cached shadow recorded with radius {:.1f}; the shader treats it as unshadowed", tracked.id, caster.shadowParams.y));
				}

				tracked.everShadowed = tracked.everShadowed || shadowed;
				tracked.wasLit = lit;
				tracked.wasShadowed = shadowed;
				a_record.tracked++;
				a_record.lit += lit ? 1 : 0;
				a_record.shadowed += shadowed ? 1 : 0;
			}

			for (auto it = state.lights.begin(); it != state.lights.end();) {
				auto& tracked = it->second;
				if (tracked.seenFrame != frame) {
					if (!tracked.removed) {
						tracked.removed = true;
						tracked.removedFrame = frame;
						state.window.removals++;
						state.removedNiLights[tracked.niLight] = it->first;
						PushEvent(fmt::format("f{} #{} left the shadow light list{}", frame, tracked.id, tracked.wasLit ? " while lit" : ""));
						tracked.wasLit = false;
						tracked.wasShadowed = false;
						tracked.unshadowedSince = 0;
						tracked.darkSince = 0;
					} else if (frame - tracked.removedFrame > REMOVED_RETENTION) {
						if (auto removed = state.removedNiLights.find(tracked.niLight); removed != state.removedNiLights.end() && removed->second == it->first)
							state.removedNiLights.erase(removed);
						it = state.lights.erase(it);
						continue;
					}
				}
				++it;
			}
		}

		void UpdateCell(const View& a_view)
		{
			auto& state = State();
			auto* player = RE::PlayerCharacter::GetSingleton();
			auto* cell = player ? player->GetParentCell() : nullptr;
			if (cell == state.cell)
				return;
			state.cell = cell;
			state.cellChangeFrame = state.frame;
			state.cellText = DescribeCell(cell);
			logger::info("[LLF][ShadowDiag] f{} Entered {} ({})", state.frame, state.cellText, DescribeCamera(a_view));
		}

		void LogSummary(const LightLimitFix& a_llf)
		{
			auto& state = State();
			const auto& c = state.window;
			logger::info("[LLF][ShadowDiag] Summary f{}-f{} in {}: casters up to {} tracked / {} cache slots, {} shadow renders; shadow lost while lit {} ({} on screen); lit without shadow {} times ({} on screen, longest {} frames on screen); "
						 "evictions {}, preemptions {}, resets {}, no free slot {}; left list {}, came back {}, turned into non-shadow lights {}; went dark briefly {}; slice collisions {}, renders not scheduled by CS {}, engine slices above 3 {}, frames over the engine cap {}; "
						 "bad light data {}, lodDimmer restores {}; scheduler runs {} ({} frames with more than one); log lines suppressed {}",
				state.windowStart, state.frame, state.cellText, c.maxTracked, a_llf.localShadowCacheSlots, c.renders, c.lost, c.lostOnScreen, c.unshadowedWindows, c.unshadowedOnScreenWindows, c.longestOnScreen,
				c.evictions, c.preemptions, c.resets, c.noSlice, c.removals, c.churn, c.demotions, c.blackouts, c.collisions, c.foreignRenders, c.badSlices, c.overCapFrames,
				c.badData, c.lodRestores, c.schedules, c.multiSchedules, c.suppressed);
		}

		std::string JoinIds(const uint32_t* a_ids, uint32_t a_count)
		{
			std::string text;
			for (uint32_t i = 0; i < a_count; i++)
				text += fmt::format(" #{}", a_ids[i]);
			return text.empty() ? " -" : text;
		}

		void WriteReport(const LightLimitFix& a_llf, const View& a_view)
		{
			auto& state = State();
			state.reportCount++;
			const uint32_t now = a_llf.localShadowFrame;

			logger::info("[LLF][ShadowDiag] ===== Shadow report {} (f{}, scheduler frame {}) =====", state.reportCount, state.frame, now);
			logger::info("[LLF][ShadowDiag] {} | {}", state.cellText, DescribeCamera(a_view));
			LogSettings(a_llf);

			std::vector<const LightLimitFix::LocalShadowCaster*> casters;
			for (const auto& caster : a_llf.localShadowCasters) {
				if (caster.light && caster.niLight && state.schedulesThisFrame > 0 && caster.lastSeenFrame == now)
					casters.push_back(&caster);
			}
			std::sort(casters.begin(), casters.end(), [&](auto* a_lhs, auto* a_rhs) {
				return a_view.position.GetSquaredDistance(a_lhs->position) < a_view.position.GetSquaredDistance(a_rhs->position);
			});

			uint32_t ownedSlots = 0;
			for (auto owner : a_llf.localShadowSliceOwner)
				ownedSlots += owner ? 1 : 0;
			std::string allowed;
			for (auto* light : a_llf.localShadowAllowed) {
				auto* tracked = FindTracked(light);
				allowed += tracked ? fmt::format(" #{}", tracked->id) : " ?";
			}
			logger::info("[LLF][ShadowDiag] {} casters tracked, {}/{} cache slots owned, scheduled this frame:{}", casters.size(), ownedSlots, a_llf.localShadowCacheSlots, allowed.empty() ? " -" : allowed);

			for (auto* caster : casters) {
				auto* tracked = FindTracked(caster->light);
				if (!tracked)
					continue;
				const bool onScreen = TouchesView(a_view, caster->position, caster->radius);
				const bool lit = tracked->litFrame + 1 >= state.frame && tracked->wasLit;
				std::string status = fmt::format("{}{}{}", onScreen ? "on-screen" : "off-screen", lit ? " lit" : " dark", tracked->wasShadowed ? " shadowed" : (lit ? " UNSHADOWED" : ""));
				if (tracked->unshadowedSince)
					status += fmt::format(" for {} frames ({} on screen)", state.frame - tracked->unshadowedSince, tracked->unshadowedOnScreen);
				logger::info("[LLF][ShadowDiag]   #{} {} | {} | {} | renders {} losses {} lodDimmer restores {} | first seen f{} | {}", tracked->id, status, CasterState(a_llf, *caster, a_view),
					tracked->unshadowedSince ? WaitReason(a_llf, *caster, *tracked) : LossText(*tracked), tracked->renders, tracked->losses, tracked->lodRestores, tracked->firstFrame, tracked->identity);
				tracked->identityLogged = true;
			}

			logger::info("[LLF][ShadowDiag] Recent events (oldest first, {}):", state.eventCount);
			for (size_t i = 0; i < state.eventCount; i++)
				logger::info("[LLF][ShadowDiag]   {}", state.events[(state.eventHead + EVENT_RING - state.eventCount + i) % EVENT_RING]);

			logger::info("[LLF][ShadowDiag] Recent frames (oldest first, {}):", state.frameCount);
			for (size_t i = 0; i < state.frameCount; i++) {
				const auto& record = state.frames[(state.frameHead + FRAME_RING - state.frameCount + i) % FRAME_RING];
				logger::info("[LLF][ShadowDiag]   f{} sched {} runs {} | scheduled{} | rendered{} | casters {} lit {} shadowed {} | unshadowed on screen {}{} | events {}",
					record.frame, record.schedulerFrame, record.schedules, JoinIds(record.allowed.data(), record.allowedCount), JoinIds(record.rendered.data(), record.renderedCount),
					record.tracked, record.lit, record.shadowed, record.unshadowedOnScreen, record.unshadowedCount ? JoinIds(record.unshadowed.data(), record.unshadowedCount) : "", record.events);
			}
			logger::info("[LLF][ShadowDiag] ===== End of shadow report {} =====", state.reportCount);
			spdlog::default_logger()->flush();
			state.reportTime = std::chrono::steady_clock::now();
		}

		void Activate(const LightLimitFix& a_llf)
		{
			auto& state = State();
			state.active = true;
			state.windowStart = state.frame;
			state.window = {};
			logger::info("[LLF][ShadowDiag] Local shadow diagnostics enabled. Press F11 right after a flash to write a full report to this log.");
			LogSettings(a_llf);
			LogLoadedModules();
			CheckHooks(true);
			state.lastHookCheck = state.frame;
		}

		void Deactivate()
		{
			auto& state = State();
			state.active = false;
			state.lights.clear();
			state.removedNiLights.clear();
			state.badDataLogged.clear();
			state.cell = nullptr;
			state.eventCount = 0;
			state.frameCount = 0;
			state.hookStatus = {};
			logger::info("[LLF][ShadowDiag] Local shadow diagnostics disabled");
		}

		void ValidateLightData(RE::BSLight* a_light, const LightLimitFix::LightData& a_data)
		{
			const auto& p = a_data.positionWS.data;
			const bool finite = std::isfinite(a_data.color.x) && std::isfinite(a_data.color.y) && std::isfinite(a_data.color.z) && std::isfinite(a_data.fade) && std::isfinite(a_data.radius) &&
			                    std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
			const float peak = std::max({ std::abs(a_data.color.x), std::abs(a_data.color.y), std::abs(a_data.color.z) }) * std::abs(a_data.fade);
			if (finite && peak <= MAX_SANE_COLOR && a_data.radius <= MAX_SANE_RADIUS)
				return;

			auto& state = State();
			state.window.badData++;
			auto* niLight = a_light->light.get();
			auto [it, inserted] = state.badDataLogged.try_emplace(niLight, 0);
			if (!inserted && state.frame - it->second < LIGHT_LOG_INTERVAL * 5)
				return;
			it->second = state.frame;
			if (state.badDataLogged.size() > 256)
				state.badDataLogged.clear();
			logger::warn("[LLF][ShadowDiag] f{} {} light data uploaded: color ({}, {}, {}) fade {} radius {} position ({}, {}, {}) | {}", state.frame, finite ? "Extreme" : "NON-FINITE",
				a_data.color.x, a_data.color.y, a_data.color.z, a_data.fade, a_data.radius, p.x, p.y, p.z, DescribeLight(a_light));
		}
	}

	bool IsActive()
	{
		auto& llf = Feature();
		return llf.loaded && llf.settings.LogShadowDiagnostics && !REL::Module::IsVR();
	}

	void NoteSchedule()
	{
		if (!IsActive())
			return;
		auto& state = State();
		state.schedulesThisFrame++;
		state.window.schedules++;
	}

	void NoteLoss(RE::BSShadowLight* a_light, Loss a_reason, RE::BSShadowLight* a_by, float a_distance)
	{
		if (!IsActive() || !a_light)
			return;
		auto& state = State();
		const uint32_t byId = a_by ? Track(a_by).id : 0;
		const bool live = a_reason == Loss::NewNiLight || a_reason == Loss::Teleported;
		auto* found = live ? &Track(a_light) : FindTracked(a_light);
		if (!found)
			return;
		auto& tracked = *found;
		tracked.loss = a_reason;
		tracked.lossFrame = state.frame;
		tracked.lossBy = byId;
		tracked.lossDistance = a_distance;
		switch (a_reason) {
		case Loss::Evicted:
			state.window.evictions++;
			break;
		case Loss::Preempted:
			state.window.preemptions++;
			break;
		case Loss::NewNiLight:
		case Loss::Teleported:
			state.window.resets++;
			break;
		default:
			break;
		}
		PushEvent(fmt::format("f{} #{} dropped its cached shadow: {}", state.frame, tracked.id, LossText(tracked)));
	}

	void NoteCacheReleased(const LightLimitFix& a_llf)
	{
		if (!IsActive())
			return;
		auto& state = State();
		uint32_t cached = 0;
		for (const auto& caster : a_llf.localShadowCasters) {
			if (!caster.light || caster.slice < 0 || caster.lastRenderedFrame == 0)
				continue;
			cached++;
			if (auto* tracked = FindTracked(caster.light)) {
				tracked->loss = Loss::CacheReleased;
				tracked->lossFrame = state.frame;
				tracked->lossBy = 0;
			}
		}
		if (!a_llf.localShadowCache && cached == 0)
			return;
		logger::info("[LLF][ShadowDiag] f{} Shadow cache released ({} slots at {}x{}, engine maps {}x{}); {} cached shadows dropped", State().frame, a_llf.localShadowCacheSlots, a_llf.localShadowCacheResolution,
			a_llf.localShadowCacheResolution, a_llf.localShadowEngineResolution, a_llf.localShadowEngineResolution, cached);
	}

	void NoteNoSlice(RE::BSShadowLight* a_light)
	{
		if (!IsActive() || !a_light)
			return;
		auto& state = State();
		auto& tracked = Track(a_light);
		state.window.noSlice++;
		if (state.frame - tracked.noSliceFrame > LIGHT_LOG_INTERVAL)
			PushEvent(fmt::format("f{} #{} was rendered but no cache slot could be reclaimed", state.frame, tracked.id));
		tracked.noSliceFrame = state.frame;
	}

	void NoteLodDimmerRestored(RE::BSShadowLight* a_light)
	{
		if (!IsActive() || !a_light)
			return;
		State().window.lodRestores++;
		Track(a_light).lodRestores++;
	}

	void NoteRenderedCaster(const LightLimitFix& a_llf, RE::BSShadowLight* a_light)
	{
		if (!IsActive() || !a_light)
			return;
		auto& state = State();
		state.window.renders++;
		state.renderedThisFrame.push_back(a_light);
		auto& tracked = Track(a_light);
		tracked.renders++;

		if (!IsAllowed(a_llf, a_light)) {
			state.window.foreignRenders++;
			Emit(&tracked, true, fmt::format("#{} was rendered by the engine although Community Shaders did not schedule it this frame (another plugin may be selecting shadow casters)", tracked.id));
		}

		auto& runtimeData = a_light->GetRuntimeData();
		if (runtimeData.shadowmapDescriptors.empty())
			return;
		const auto& descriptor = runtimeData.shadowmapDescriptors[0];
		if (static_cast<int32_t>(descriptor.renderTarget) == static_cast<int32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kSHADOWMAPS) && descriptor.shadowmapIndex >= VANILLA_POINT_SLICES) {
			state.window.badSlices++;
			Emit(&tracked, true, fmt::format("#{} rendered into engine shadow slice {}; vanilla point/spot casters only use slices 0-3 (4+ hold sun cascades and spot focus maps)", tracked.id, descriptor.shadowmapIndex));
		}

		float transform[16]{};
		static_assert(sizeof(transform) == sizeof(descriptor.lightTransform));
		std::memcpy(transform, &descriptor.lightTransform, sizeof(transform));
		const bool finite = std::all_of(std::begin(transform), std::end(transform), [](float a_value) { return std::isfinite(a_value); }) && std::isfinite(runtimeData.shadowBiasScale);
		if (!finite)
			Emit(&tracked, true, fmt::format("#{} has a NON-FINITE shadow projection or bias (bias scale {})", tracked.id, runtimeData.shadowBiasScale));
	}

	void NoteSliceCollision(RE::BSShadowLight* a_light, uint32_t a_engineSlice, uint32_t a_claims)
	{
		if (!IsActive() || !a_light)
			return;
		auto& state = State();
		state.window.collisions++;
		auto& tracked = Track(a_light);
		Emit(&tracked, true, fmt::format("#{} shares engine shadow slice {} with {} other caster(s) this frame; its copy was skipped", tracked.id, a_engineSlice, a_claims - 1));
	}

	void NoteUploadedLight(RE::BSLight* a_light, const LightLimitFix::LightData& a_data, bool a_lit, bool a_withheld)
	{
		auto& state = State();
		if (!state.active || !a_light)
			return;
		if (a_lit)
			ValidateLightData(a_light, a_data);

		if (!a_light->IsShadowLight()) {
			if (a_lit && !state.removedNiLights.empty()) {
				auto* niLight = a_light->light.get();
				if (auto it = state.removedNiLights.find(niLight); it != state.removedNiLights.end()) {
					auto* tracked = FindTracked(it->second);
					state.removedNiLights.erase(it);
					if (tracked) {
						state.window.demotions++;
						const auto& view = CurrentView();
						const bool onScreen = TouchesView(view, niLight->world.translate, a_data.radius);
						Emit(tracked, onScreen, fmt::format("#{} is lit again as a regular light WITHOUT a shadow, {} frames after it left the shadow light list (another plugin turned its shadow off)", tracked->id, state.frame - tracked->removedFrame));
					}
				}
			}
			return;
		}

		if (!IsTracking())
			return;
		auto& tracked = Track(static_cast<RE::BSShadowLight*>(a_light));
		if (a_lit) {
			tracked.litFrame = state.frame;
			if (a_data.lightFlags.any(LightLimitFix::LightFlags::LocalShadow))
				tracked.shadowedFrame = state.frame;
		} else if (a_withheld) {
			tracked.darkReason = "withheld until its first cached shadow";
		} else {
			tracked.darkReason = fmt::format("fade {:.3f} lodDimmer {:.2f} radius {:.1f} color {:.3f}", a_data.fade, a_light->lodDimmer, a_data.radius, a_data.color.x + a_data.color.y + a_data.color.z);
		}
	}

	void EndFrame(LightLimitFix& a_llf)
	{
		auto& state = State();
		if (!IsActive()) {
			if (state.active)
				Deactivate();
			return;
		}
		if (!state.active)
			Activate(a_llf);

		const View& view = CurrentView();
		UpdateCell(view);

		FrameRecord record{};
		record.frame = state.frame;
		record.schedulerFrame = a_llf.localShadowFrame;
		record.schedules = state.schedulesThisFrame;

		if (IsTracking()) {
			if (state.schedulesThisFrame > 0)
				EvaluateCasters(a_llf, view, record);
			state.window.maxTracked = std::max(state.window.maxTracked, record.tracked);

			if (state.schedulesThisFrame == 0 && !a_llf.localShadowCasters.empty()) {
				if (++state.missedSchedules == MISSED_SCHEDULE_FRAMES) {
					logger::warn("[LLF][ShadowDiag] f{} The shadow caster scheduler has not run for {} frames while shadow casters exist; checking hooks", state.frame, MISSED_SCHEDULE_FRAMES);
					CheckHooks(true);
				}
			} else {
				state.missedSchedules = 0;
			}
			if (state.schedulesThisFrame > 1)
				state.window.multiSchedules++;

			if (state.renderedThisFrame.size() > LightLimitFix::ENGINE_LOCAL_SHADOW_CASTERS) {
				state.window.overCapFrames++;
				if (!state.overCapLogFrame || state.frame - state.overCapLogFrame >= OVER_CAP_LOG_INTERVAL) {
					state.overCapLogFrame = state.frame;
					logger::warn("[LLF][ShadowDiag] f{} The engine rendered {} point/spot shadow maps this frame; vanilla renders at most {} (another plugin raised the shadow caster limit)", state.frame, state.renderedThisFrame.size(), LightLimitFix::ENGINE_LOCAL_SHADOW_CASTERS);
				}
			}

			for (auto* light : a_llf.localShadowAllowed) {
				if (record.allowedCount >= FRAME_IDS)
					break;
				auto* tracked = FindTracked(light);
				record.allowed[record.allowedCount++] = tracked ? tracked->id : 0;
			}
			for (auto* light : state.renderedThisFrame) {
				if (record.renderedCount >= RENDERED_IDS)
					break;
				auto* tracked = FindTracked(light);
				record.renderedCount++;
				record.rendered[record.renderedCount - 1] = tracked ? tracked->id : 0;
			}
		}

		if (state.frame - state.lastHookCheck >= HOOK_CHECK_INTERVAL) {
			state.lastHookCheck = state.frame;
			CheckHooks(false);
		}

		state.window.frames++;
		if (state.frame - state.windowStart >= SUMMARY_INTERVAL) {
			if (state.window.Noteworthy())
				LogSummary(a_llf);
			state.window = {};
			state.windowStart = state.frame + 1;
		}

		if (state.reportRequested.exchange(false))
			WriteReport(a_llf, view);

		record.events = state.eventsThisFrame;
		state.frames[state.frameHead] = record;
		state.frameHead = (state.frameHead + 1) % FRAME_RING;
		state.frameCount = std::min(state.frameCount + 1, FRAME_RING);

		state.schedulesThisFrame = 0;
		state.eventsThisFrame = 0;
		state.renderedThisFrame.clear();
		if (state.frame - state.budgetFrame >= LOG_BUDGET_FRAMES) {
			state.budgetFrame = state.frame;
			state.budgetUsed = 0;
		}
		state.frame++;
	}

	void RequestReport()
	{
		State().reportRequested = true;
	}

	bool HandleHotkey(uint32_t a_vkKey)
	{
		if (a_vkKey != REPORT_KEY || !IsActive())
			return false;
		constexpr int keyPressedMask = 0x8000;
		if ((GetAsyncKeyState(VK_CONTROL) & keyPressedMask) || (GetAsyncKeyState(VK_SHIFT) & keyPressedMask) || (GetAsyncKeyState(VK_MENU) & keyPressedMask))
			return false;
		logger::info("[LLF][ShadowDiag] Report hotkey pressed");
		RequestReport();
		return true;
	}

	bool IsReportNoticeVisible()
	{
		const auto& state = State();
		return state.reportCount > 0 && std::chrono::steady_clock::now() - state.reportTime < REPORT_NOTICE_DURATION;
	}
}
