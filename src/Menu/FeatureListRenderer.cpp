#include "FeatureListRenderer.h"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <format>
#include <imgui.h>
#include <imgui_internal.h>
#include <ranges>
#include <unordered_map>
#include <unordered_set>

#include "CSEditor/Browser/BrowserWidgets.h"
#include "CSEditor/EditorWindow.h"
#include "CSEditor/SceneManager/FeatureOverwritesPanel.h"
#include "CSEditor/SceneManager/SceneSettingsManager.h"
#include "CSEditor/SceneManager/SceneWidgetInterceptor.h"
#include "Feature.h"
#include "FeatureConstraints.h"
#include "FeatureIssues.h"
#include "Features/CSEditor.h"
#include "Fonts.h"
#include "Globals.h"
#include "I18n/I18n.h"
#include "IconsTabler.h"
#include "Menu.h"
#include "Menu/AdvancedSettingsRenderer.h"
#include "Menu/PresetsPageRenderer.h"
#include "Menu/ProfilingRenderer.h"
#include "Menu/SettingReveal.h"
#include "Menu/ThemeManager.h"
#include "State.h"
#include "Util.h"
#include "Utils/SettingsCatalog.h"
#include "Utils/UI.h"
#include "Utils/UIAnim.h"
#include "Utils/VectorIcons.h"

namespace
{
	constexpr size_t CORE_MENU_COUNT = 4;

	bool IsCoreMenu(std::string_view menuId)
	{
		return menuId == MenuPageIds::kPresets || menuId == MenuPageIds::kGeneral ||
		       menuId == MenuPageIds::kAdvanced || menuId == MenuPageIds::kCSEditor;
	}

	constexpr std::string_view kNoSettingsSectionKey = "No Settings";
	constexpr std::string_view kPinnedSectionKey = "__Pinned";

	void ExpandSectionOf(const std::string& shortName, std::map<std::string, bool>& categoryExpansionStates)
	{
		for (Feature* feat : Feature::GetFeatureList()) {
			if (feat->GetShortName() == shortName) {
				categoryExpansionStates[std::string(feat->HasSettings() ? feat->GetCategory() : kNoSettingsSectionKey)] = true;
				return;
			}
		}
	}

	/**
	 * @brief Clears a feature's edits through the menu's undo hook, so it shows what defaults and Baseline packs set.
	 * The scene layer's cached apply document takes the restored values too, or the next scene apply would replay the old ones.
	 */
	void RestoreFeatureDefaults(const FeatureListRenderer::Hooks& hooks, Feature* feat)
	{
		const auto restore = [feat] {
			if (feat->UsesMainSettings())
				globals::state->RestoreFeatureLayers(feat);
			else
				feat->RestoreDefaultSettings();
			globals::sceneSettingsManager->CaptureExternalFeatureChanges(feat, true);
		};
		if (hooks.runFeatureCommand)
			hooks.runFeatureCommand(feat, restore);
		else
			restore();
		globals::menu->MarkFeatureEdited(feat);
	}

	/// State of the auto-hide navigation (Menu::Settings::AutoHideFeatureList): whether it is meant to be shown,
	/// how far it has slid in, and the timers that keep it from flickering.
	struct AutoHideDrawer
	{
		bool open = false;
		float progress = 0.0f;     ///< 0 = hidden, 1 = fully shown
		double hoverSince = -1.0;  ///< When the cursor reached the edge zone; -1 while it is not there
		double leaveSince = -1.0;  ///< When the cursor left the shown list; -1 while it is there
		bool keepOpen = false;     ///< Last frame, something in the list was active or a popup was open
	};
	AutoHideDrawer g_drawer;

	const FeatureListRenderer::Hooks* g_activeHooks = nullptr;

	/** @brief The feature list's sort modes as menu items. */
	void DrawSortModeItems()
	{
		using enum Menu::FeatureSortMode;
		auto& sort = globals::menu->GetSettings().FeatureListSort;
		if (ImGui::MenuItem(T("menu.features.sort_by_category", "By Category"), nullptr, sort == Category))
			sort = Category;
		if (ImGui::MenuItem(T("menu.features.sort_alphabetically", "Alphabetical"), nullptr, sort == Alphabetical))
			sort = Alphabetical;
	}

	/// Set when a shortcut moved the selection, so the list scrolls the newly selected row into view.
	bool g_scrollToSelection = false;

	/// Where the category headers' labels start, measured in the heading font each frame the list draws.
	float g_headerLabelInset = 0.0f;

	/**
	 * @brief Left inset of a list row's text. Rows line up with the category labels above them, so the
	 * disclosure triangles alone sit in the gutter and the groups read as groups; never less than the
	 * room the selected row's accent bar needs.
	 */
	float ListRowTextInset()
	{
		return std::max({ g_headerLabelInset, ImGui::GetStyle().FramePadding.x, ThemeManager::Constants::ANIM_SELECTED_BAR_WIDTH * Util::GetUIScale() });
	}

	/** @brief Room above a section heading so the groups of a long list read apart. Skipped above the list's first row. */
	void AddSectionGap()
	{
		if (ImGui::GetCursorPosY() > ImGui::GetCursorStartPos().y)
			ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ThemeManager::Constants::FEATURE_LIST_SECTION_GAP * Util::GetUIScale());
	}

	/**
	 * @brief Moves the selection to the nearest feature entry in a direction, skipping category headers,
	 * labels and built-in pages. Nothing changes when there is no feature that way.
	 * @param direction -1 for the previous feature, +1 for the next.
	 * @return True when the selection moved.
	 */
	bool StepFeatureSelection(const std::vector<FeatureListRenderer::MenuFuncInfo>& menuList, size_t& selectedMenu, int direction)
	{
		const auto count = static_cast<std::ptrdiff_t>(menuList.size());
		auto index = selectedMenu < menuList.size() ? static_cast<std::ptrdiff_t>(selectedMenu) : (direction > 0 ? -1 : count);
		for (index += direction; index >= 0 && index < count; index += direction) {
			if (std::holds_alternative<Feature*>(menuList[static_cast<size_t>(index)])) {
				selectedMenu = static_cast<size_t>(index);
				return true;
			}
		}
		return false;
	}

	/** @brief Badge label of a pre-release stage; null for release features. */
	const char* GetStageBadgeLabel(Feature::ReleaseStage stage)
	{
		switch (stage) {
		case Feature::ReleaseStage::Alpha:
			return T("menu.features.badge_alpha", "Alpha");
		case Feature::ReleaseStage::Beta:
			return T("menu.features.badge_beta", "Beta");
		default:
			return nullptr;
		}
	}

	struct TitleMetrics
	{
		float width = 0.0f;
		float height = 0.0f;
	};

	TitleMetrics DrawPageTitle(const std::string& title, const std::string& version, Feature::ReleaseStage stage = Feature::ReleaseStage::Release)
	{
		const ImVec4 textColor = globals::menu->GetTheme().Palette.Text;
		const float titleScale = globals::menu->GetFeatureTitleScale();

		// Size and draw text in a font role at the title scale
		const auto scaledSize = [titleScale](Menu::FontRole role, const char* text) {
			MenuFonts::FontRoleGuard guard(role);
			const ImVec2 size = ImGui::CalcTextSize(text);
			return ImVec2(size.x * titleScale, size.y * titleScale);
		};
		const auto drawScaled = [titleScale](Menu::FontRole role, const ImVec4& color, const char* text) {
			MenuFonts::FontRoleGuard guard(role);
			ImGui::SetWindowFontScale(titleScale);
			ImGui::TextColored(color, "%s", text);
			ImGui::SetWindowFontScale(1.0f);
		};

		const ImVec2 startPos = ImGui::GetCursorScreenPos();
		const float itemSpacingX = ImGui::GetStyle().ItemSpacing.x;
		const ImVec2 titleSize = scaledSize(Menu::FontRole::Title, title.c_str());
		drawScaled(Menu::FontRole::Title, ImGui::GetStyleColorVec4(ImGuiCol_Text), title.c_str());

		float annotationX = startPos.x + titleSize.x + itemSpacingX;
		float rightEdge = startPos.x + titleSize.x;

		const bool hasStageBadge = stage != Feature::ReleaseStage::Release;
		if (hasStageBadge) {
			// Centred on the version's line
			const float lineHeight = scaledSize(Menu::FontRole::Body, title.c_str()).y;
			ImGui::SetCursorScreenPos(ImVec2(annotationX, startPos.y + titleSize.y - lineHeight));
			ImGui::SetWindowFontScale(titleScale);
			FeatureListRenderer::DrawStageBadge(stage, lineHeight);
			ImGui::SetWindowFontScale(1.0f);
			rightEdge = annotationX + ImGui::GetItemRectSize().x;
			annotationX = rightEdge + itemSpacingX;
		}

		if (!version.empty()) {
			std::string formattedVersion = "v" + version;
			std::ranges::replace(formattedVersion, '-', '.');
			const ImVec2 versionSize = scaledSize(Menu::FontRole::Body, formattedVersion.c_str());
			ImGui::SetCursorScreenPos(ImVec2(annotationX, startPos.y + titleSize.y - versionSize.y));
			drawScaled(Menu::FontRole::Body, Util::Anim::ScaleAlpha(textColor, ThemeManager::Constants::VERSION_TEXT_OPACITY), formattedVersion.c_str());
			rightEdge = annotationX + versionSize.x;
		}

		if (hasStageBadge || !version.empty())
			ImGui::SetCursorScreenPos(ImVec2(startPos.x, startPos.y + titleSize.y + ImGui::GetStyle().ItemSpacing.y * 0.25f));

		return { rightEdge - startPos.x, titleSize.y };
	}

	void DrawPageDescription(const std::string& description)
	{
		if (description.empty())
			return;
		MenuFonts::FontRoleGuard subtextGuard(Menu::FontRole::Subtext);
		ImGui::PushStyleColor(ImGuiCol_Text, Util::Anim::ScaleAlpha(globals::menu->GetTheme().Palette.Text, ThemeManager::Constants::FEATURE_DESCRIPTION_OPACITY));
		ImGui::TextWrapped("%s", description.c_str());
		ImGui::PopStyleColor();
	}

	// ---------------------------------------------------------------------------
	// Persistent state for the reactive constraint warning popup.
	// DrawMenuVisitor is reconstructed every frame (it's a temporary passed to
	// std::visit), so member state is lost immediately.  These file-scope
	// variables survive across frames so the popup can actually render.
	// ---------------------------------------------------------------------------

	// Set of constraint keys we have already "seen" (and therefore warned about
	// or suppressed).  Keyed as "featureShortName|settingPath".
	std::unordered_set<std::string> g_knownConstraintKeys;
	bool g_knownConstraintKeysInitialised = false;

	// Pending popup state: non-empty when we have new constraints to show.
	bool g_reactiveWarningShow = false;
	std::vector<std::pair<FeatureConstraints::SettingId, FeatureConstraints::ConstraintResult>> g_reactiveWarningConstraints;

	// "Don't show again" checkbox state inside the modal (reset each time popup opens).
	bool g_dontShowAgainCheckbox = false;

	// ---------------------------------------------------------------------------
	// Selection is tracked by a stable key rather than by list index: the menu list
	// is rebuilt every frame, so collapsing a category above the selection removes
	// entries and shifts every later index, silently moving the right pane to a
	// different feature. The index is re-derived from this key each frame.
	// ---------------------------------------------------------------------------
	std::string g_selectedEntryKey;

	std::string FeatureEntryKey(const std::string& shortName)
	{
		return "F:" + shortName;
	}

	std::string BuiltInEntryKey(std::string_view pageId)
	{
		return "B:" + std::string(pageId);
	}

	/** @brief Stable identity for a menu entry; empty for headers and labels, which are not selectable. */
	std::string EntryKey(const FeatureListRenderer::MenuFuncInfo& entry)
	{
		if (const auto* menu = std::get_if<FeatureListRenderer::BuiltInMenu>(&entry))
			return BuiltInEntryKey(menu->id);
		if (const auto* feat = std::get_if<Feature*>(&entry))
			return FeatureEntryKey((*feat)->GetShortName());
		if (const auto* pinned = std::get_if<FeatureListRenderer::PinnedFeature>(&entry))
			return FeatureEntryKey(pinned->feature->GetShortName());
		return {};
	}

	std::string DescribeLocation(const FeatureListRenderer::MenuFuncInfo& entry)
	{
		Feature* feat = nullptr;
		if (const auto* menu = std::get_if<FeatureListRenderer::BuiltInMenu>(&entry))
			return menu->name;
		if (const auto* listed = std::get_if<Feature*>(&entry))
			feat = *listed;
		else if (const auto* pinned = std::get_if<FeatureListRenderer::PinnedFeature>(&entry))
			feat = pinned->feature;
		if (!feat)
			return {};
		return std::format("{} / {}", Feature::TranslateCategory(feat->GetCategory()), feat->GetDisplayName());
	}

	ImGuiSelectableFlags NavigationSelectableFlags(bool selectOnNav)
	{
		return ImGuiSelectableFlags_SpanAllColumns | (selectOnNav ? ImGuiSelectableFlags_SelectOnNav : ImGuiSelectableFlags_None);
	}

	struct SearchHit
	{
		bool nameMatch = false;
		std::string settingLabel;
		int extraSettings = 0;
	};

	struct SettingLabel
	{
		Feature* feature = nullptr;
		std::string text;
		std::string lowered;
	};

	constexpr std::pair<std::string_view, std::string_view> kTranslationPrefixAliases[] = {
		{ "expheightfog", "ExponentialHeightFog" },
		{ "ibl", "ImageBasedLighting" },
		{ "perfoverlay", "PerformanceOverlay" },
		{ "sss", "SubsurfaceScattering" },
	};

	std::string ToLowerAscii(std::string_view text)
	{
		std::string result(text);
		std::ranges::transform(result, result.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return result;
	}

	std::string NormalizePrefix(std::string_view text)
	{
		std::string result;
		result.reserve(text.size());
		for (const unsigned char c : text) {
			if (c != '_')
				result += static_cast<char>(std::tolower(c));
		}
		return result;
	}

	bool IsSearchableLabel(std::string_view key, std::string_view text)
	{
		if (text.empty() || text.size() > ThemeManager::Constants::MAX_SEARCHABLE_LABEL_LENGTH || text.find_first_of("\n%{") != std::string_view::npos)
			return false;
		const std::string_view leaf = key.substr(key.rfind('.') + 1);
		constexpr std::string_view kProseLeaves[] = { "tooltip", "_tip", "desc", "description", "_help", "_info", "_note", "warning", "summary" };
		return leaf != "tip" && std::ranges::none_of(kProseLeaves, [&](std::string_view suffix) { return leaf.ends_with(suffix); });
	}

	const std::vector<SettingLabel>& GetSettingLabelIndex(const std::string& locale)
	{
		static std::vector<SettingLabel> index;
		static std::string indexedLocale;
		static bool built = false;
		if (built && locale == indexedLocale)
			return index;
		built = true;
		indexedLocale = locale;
		index.clear();

		std::unordered_map<std::string, Feature*> featuresByPrefix;
		for (Feature* feat : Feature::GetFeatureList())
			featuresByPrefix.emplace(NormalizePrefix(feat->GetShortName()), feat);
		for (const auto& [alias, shortName] : kTranslationPrefixAliases) {
			for (Feature* feat : Feature::GetFeatureList()) {
				if (feat->GetShortName() == shortName)
					featuresByPrefix.emplace(std::string(alias), feat);
			}
		}

		constexpr std::string_view kFeaturePrefix = "feature.";
		std::unordered_set<std::string> seen;
		for (auto& [key, text] : I18n::GetSingleton()->CollectStrings(kFeaturePrefix)) {
			const std::string_view rest = std::string_view(key).substr(kFeaturePrefix.size());
			const size_t dot = rest.find('.');
			if (dot == std::string_view::npos || !IsSearchableLabel(key, text))
				continue;
			const auto it = featuresByPrefix.find(NormalizePrefix(rest.substr(0, dot)));
			if (it == featuresByPrefix.end())
				continue;
			std::string lowered = ToLowerAscii(text);
			if (!seen.insert(it->second->GetShortName() + '|' + lowered).second)
				continue;
			index.push_back({ it->second, std::move(text), std::move(lowered) });
		}

		std::ranges::sort(index, [](const SettingLabel& a, const SettingLabel& b) {
			return a.text.size() != b.text.size() ? a.text.size() < b.text.size() : a.text < b.text;
		});
		return index;
	}

	const std::unordered_map<Feature*, SearchHit>& GetSearchHits(const std::string& query)
	{
		static std::unordered_map<Feature*, SearchHit> hits;
		static std::string cachedQuery;
		static std::string cachedLocale;

		const std::string locale = I18n::GetSingleton()->GetCurrentLocale();
		if (query == cachedQuery && locale == cachedLocale)
			return hits;
		cachedQuery = query;
		cachedLocale = locale;
		hits.clear();
		if (query.empty())
			return hits;

		const std::string needle = ToLowerAscii(query);
		for (Feature* feat : Feature::GetFeatureList()) {
			if (Util::FeatureMatchesSearch(feat, query) ||
				ToLowerAscii(feat->GetDisplayName()).find(needle) != std::string::npos ||
				ToLowerAscii(Feature::TranslateCategory(feat->GetCategory())).find(needle) != std::string::npos)
				hits[feat].nameMatch = true;
		}

		if (needle.size() >= ThemeManager::Constants::MIN_SETTING_SEARCH_LENGTH) {
			for (const auto& label : GetSettingLabelIndex(locale)) {
				if (label.lowered.find(needle) == std::string::npos)
					continue;
				auto& hit = hits[label.feature];
				if (hit.settingLabel.empty())
					hit.settingLabel = label.text;
				else
					++hit.extraSettings;
			}
		}
		return hits;
	}

	bool WasItemEditedThisFrame()
	{
		return ImGui::GetCurrentContext()->ActiveIdHasBeenEditedThisFrame;
	}

	/**
	 * @brief Queues the warning popup for constraints that appeared since the last call; the first call only seeds.
	 * Diffing every frame also catches changes made outside DrawSettings (e.g. Upscaling's render-loop resolutionScale).
	 */
	void DetectReactiveConstraints()
	{
		if (g_reactiveWarningShow)  // don't overwrite a pending popup
			return;

		auto currentConstraints = FeatureConstraints::GetAllActiveConstraints();

		if (!g_knownConstraintKeysInitialised) {
			// First time: seed known set, no popup
			for (const auto& [settingId, result] : currentConstraints) {
				g_knownConstraintKeys.insert(settingId.featureShortName + "|" + settingId.settingPath);
			}
			g_knownConstraintKeysInitialised = true;
			return;
		}

		// Diff: find keys present now but not previously known
		std::vector<std::pair<FeatureConstraints::SettingId, FeatureConstraints::ConstraintResult>> newConstraints;
		std::unordered_set<std::string> currentKeys;
		for (const auto& [settingId, result] : currentConstraints) {
			std::string key = settingId.featureShortName + "|" + settingId.settingPath;
			currentKeys.insert(key);
			if (g_knownConstraintKeys.find(key) == g_knownConstraintKeys.end()) {
				newConstraints.emplace_back(settingId, result);
			}
		}
		// Update known set to current (removes keys for constraints that went away)
		g_knownConstraintKeys = std::move(currentKeys);

		if (!newConstraints.empty() && !globals::menu->GetSettings().SkipConstraintWarning) {
			logger::info("Reactive constraint detection: {} new constraints", newConstraints.size());
			for (const auto& [settingId, result] : newConstraints) {
				logger::info("  - {}.{} forced to {} by {}", settingId.featureShortName, settingId.settingPath, FeatureConstraints::FormatConstraintValue(result.forcedValue), result.sources.empty() ? "?" : result.sources[0].featureName);
			}
			g_reactiveWarningShow = true;
			g_reactiveWarningConstraints = std::move(newConstraints);
			g_dontShowAgainCheckbox = false;
		}
	}
}

std::string FeatureListRenderer::TranslateCategory(std::string_view category)
{
	return Feature::TranslateCategory(category);
}

float FeatureListRenderer::MeasureStageBadge(Feature::ReleaseStage stage)
{
	const char* label = GetStageBadgeLabel(stage);
	if (!label)
		return 0.0f;
	MenuFonts::FontRoleGuard badgeFont(Menu::FontRole::Subtext);
	return Util::MeasureBadgeWidth(label);
}

void FeatureListRenderer::DrawStageBadge(Feature::ReleaseStage stage, float rowHeight)
{
	const char* label = GetStageBadgeLabel(stage);
	if (!label)
		return;
	const auto& statusPalette = globals::menu->GetTheme().StatusPalette;
	MenuFonts::FontRoleGuard badgeFont(Menu::FontRole::Subtext);
	Util::Badge(label, stage == Feature::ReleaseStage::Alpha ? statusPalette.Error : statusPalette.Warning, { .rowHeight = rowHeight });
}

std::uint32_t FeatureListRenderer::GetFilterBits(Feature* feat)
{
	auto* sceneManager = globals::sceneSettingsManager;
	const auto shortName = feat->GetShortName();
	std::uint32_t bits = 0;
	if (feat->loaded && globals::menu->IsFeaturePinned(shortName))
		bits |= FeatureListFilter::kFavorites;
	if (globals::state->HasUserEdits(feat))
		bits |= FeatureListFilter::kModified;
	if (sceneManager && sceneManager->HasAnySceneEntriesForFeature(shortName))
		bits |= FeatureListFilter::kScene;
	if (feat->GetReleaseStage() != Feature::ReleaseStage::Release)
		bits |= FeatureListFilter::kPreRelease;
	if (!feat->loaded)
		bits |= FeatureListFilter::kUnloaded;
	return bits;
}

bool FeatureListRenderer::PassesFilter(std::uint32_t bits, std::uint32_t filter)
{
	constexpr std::uint32_t kAttributes = FeatureListFilter::kKnown & ~FeatureListFilter::kUnloaded;
	const std::uint32_t required = filter & kAttributes;
	return (bits & required) == required;
}

void FeatureListRenderer::DrawFilterDot(const ImVec2& buttonMin, const ImVec2& buttonMax)
{
	if ((globals::menu->GetSettings().FeatureListFilter & FeatureListFilter::kKnown) == 0)
		return;
	const float radius = ImGui::GetFrameHeight() * ThemeManager::Constants::STATUS_DOT_RATIO;
	Util::DrawIconCircle(ImVec2(buttonMax.x - radius * 1.5f, buttonMin.y + radius * 1.5f), radius, ImGui::GetColorU32(Util::Colors::GetAccent()), true);
}

void FeatureListRenderer::RenderSortPopup(const char* popupId)
{
	if (!ImGui::BeginPopup(popupId))
		return;

	ImGui::SeparatorText(T("menu.settings.feature_list_order", "Feature List Order"));
	DrawSortModeItems();

	ImGui::SeparatorText(T("menu.features.filter_section", "Filter"));
	auto& filter = globals::menu->GetSettings().FeatureListFilter;
	struct Item
	{
		std::uint32_t bit;
		const char* label;
		const char* tooltip;
	};
	const Item items[] = {
		{ FeatureListFilter::kFavorites, T("menu.features.filter_label_favorites", "Pinned"), T("menu.features.filter_favorites", "Pinned features") },
		{ FeatureListFilter::kModified, T("menu.features.filter_label_modified", "Modified"), T("menu.features.filter_modified", "Modified: settings differ from defaults and your Baseline presets") },
		{ FeatureListFilter::kScene, T("menu.features.filter_label_scene", "Scene"), T("menu.features.filter_scene", "Scene: has Scene Manager settings") },
		{ FeatureListFilter::kPreRelease, T("menu.features.filter_label_pre_release", "Pre-release"), T("menu.features.filter_pre_release", "Pre-release: alpha and beta features") },
		{ FeatureListFilter::kUnloaded, T("menu.features.filter_label_unloaded", "Unloaded"), T("menu.features.filter_unloaded", "Unloaded: missing, failed, or disabled at boot. Only these are listed while it is on") },
	};
	// Filters combine, so toggling one leaves the popup open
	ImGui::PushItemFlag(ImGuiItemFlags_AutoClosePopups, false);
	for (const auto& item : items) {
		if (ImGui::MenuItem(item.label, nullptr, (filter & item.bit) != 0))
			filter ^= item.bit;
		Util::AddTooltip(item.tooltip);
	}
	ImGui::PopItemFlag();

	ImGui::Separator();
	if (ImGui::MenuItem(T("menu.features.clear_filter_chips", "Clear filters"), nullptr, false, (filter & FeatureListFilter::kKnown) != 0))
		filter = 0;
	ImGui::EndPopup();
}

void FeatureListRenderer::ToggleFavorite(Feature* feat)
{
	const auto featureName = feat->GetShortName();
	globals::menu->SetFeaturePinned(featureName, !globals::menu->IsFeaturePinned(featureName));
}

void FeatureListRenderer::DrawFeatureBody(Feature* feat)
{
	assert(feat && feat->loaded);

	const ImVec2 cursorPosBefore = ImGui::GetCursorPos();
	{
		// Sketches live only while their feature is on screen; the next Update drops any other.
		globals::sceneSettingsManager->RetainSketches(feat->GetShortName());
		SceneWidgetInterceptor::Scope baselineScope({ .feature = feat, .baseline = true });
		// ImGui raises this flag for any value edit; set only inside the call, it means this feature changed.
		// Buttons that rewrite settings (a reset, a preset) raise no flag, so a finished click is checked instead.
		const bool editedBefore = WasItemEditedThisFrame();
		SettingReveal::BeginFeatureBody(feat);
		feat->DrawSettings();
		if (feat->HasDebugSection())
			feat->DrawDebugSection();
		SettingReveal::EndFeatureBody();
		if (feat->UsesMainSettings()) {
			auto* state = globals::state;
			const bool edited = !editedBefore && WasItemEditedThisFrame();
			if (edited)
				state->MarkSettingsChanged();
			if (Util::InteractionEndedThisFrame()) {
				// Those buttons also bypass the interceptor, so the scene layer's cached apply document must
				// take their values before anything replays it.
				globals::sceneSettingsManager->CaptureExternalFeatureChanges(feat, true);
				if (!edited)
					state->CheckFeatureSettingsChanged(feat);
				state->InvalidateEditsUnderBaselines(feat);
			}
		}
	}
	const ImVec2 cursorPosAfter = ImGui::GetCursorPos();

	DetectReactiveConstraints();

	constexpr float cursorEpsilon = 0.1f;
	const bool cursorMoved = std::abs(cursorPosAfter.x - cursorPosBefore.x) > cursorEpsilon ||
	                         std::abs(cursorPosAfter.y - cursorPosBefore.y) > cursorEpsilon;
	if (!cursorMoved)
		BrowserUI::EmptyState(T("menu.features.no_settings_available", "There are no settings available for this feature."));
}

void FeatureListRenderer::RenderFeatureList(
	size_t& selectedMenu,
	std::string& featureSearch,
	bool& focusSearch,
	std::string& pendingFeatureSelection,
	std::map<std::string, bool>& categoryExpansionStates,
	const std::function<void()>& drawGeneralSettings,
	const std::function<void()>& drawAdvancedSettings,
	float reservedBottomHeight,
	Hooks& hooks)
{
	g_activeHooks = &hooks;
	ImGui::BeginChild("Menus Table", ImVec2(0, -reservedBottomHeight), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

	if (!pendingFeatureSelection.empty()) {
		ExpandSectionOf(pendingFeatureSelection, categoryExpansionStates);
		featureSearch.clear();
	}

	auto menuList = BuildMenuList(featureSearch, categoryExpansionStates, drawGeneralSettings, drawAdvancedSettings);

	ResolveSelectionFromKey(menuList, selectedMenu);

	HandlePendingFeatureSelection(pendingFeatureSelection, menuList, selectedMenu);

	if (hooks.featureStep != 0) {
		g_scrollToSelection = StepFeatureSelection(menuList, selectedMenu, hooks.featureStep > 0 ? 1 : -1);
		if (g_scrollToSelection)
			g_selectedEntryKey = EntryKey(menuList[selectedMenu]);
		hooks.featureStep = 0;
	}

	const bool narrow = ImGui::GetContentRegionAvail().x < ImGui::GetFontSize() * ThemeManager::Constants::NARROW_LAYOUT_WIDTH_EM;
	if (narrow) {
		RenderNavigatorBar(menuList, selectedMenu, featureSearch, focusSearch, categoryExpansionStates, hooks);
		RenderRightColumn(menuList, selectedMenu, pendingFeatureSelection, hooks);
	} else if (globals::menu->GetSettings().AutoHideFeatureList) {
		RenderAutoHidePane(menuList, selectedMenu, featureSearch, focusSearch, pendingFeatureSelection, categoryExpansionStates, hooks);
	} else if (ImGui::BeginTable("Menus Table", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable)) {
		ImGui::TableSetupColumn("##ListOfMenus", 0, ThemeManager::Constants::FEATURE_LIST_WIDTH_WEIGHT);
		ImGui::TableSetupColumn("##MenuConfig", 0, ThemeManager::Constants::FEATURE_PAGE_WIDTH_WEIGHT);
		ImGui::TableNextColumn();
		RenderNavigation(menuList, selectedMenu, featureSearch, focusSearch, categoryExpansionStates, false, hooks);
		ImGui::TableNextColumn();
		RenderRightColumn(menuList, selectedMenu, pendingFeatureSelection, hooks);
		ImGui::EndTable();
	}
	g_scrollToSelection = false;  // a hidden list drops the request rather than scrolling later

	ImGui::EndChild();
	g_activeHooks = nullptr;
}

std::vector<FeatureListRenderer::MenuFuncInfo> FeatureListRenderer::BuildMenuList(
	const std::string& featureSearch,
	std::map<std::string, bool>& categoryExpansionStates,
	const std::function<void()>& drawGeneralSettings,
	const std::function<void()>& drawAdvancedSettings)
{
	// Build the menu list
	auto& featureList = Feature::GetFeatureList();
	auto sortedFeatureList{ featureList };  // need a copy so the load order is not lost
	std::ranges::sort(sortedFeatureList, [](Feature* a, Feature* b) {
		return a->GetDisplayName() < b->GetDisplayName();
	});

	const bool searching = !featureSearch.empty();
	if (searching) {
		const auto& hits = GetSearchHits(featureSearch);
		std::erase_if(sortedFeatureList, [&hits](Feature* feat) { return !hits.contains(feat); });
	}

	auto menuList = std::vector<MenuFuncInfo>{
		BuiltInMenu{ MenuPageIds::kPresets, T("menu.features.presets", "Presets"), []() { PresetsPageRenderer::Render(); } },
		BuiltInMenu{ MenuPageIds::kGeneral, T("menu.features.general", "General"), drawGeneralSettings, T("menu.features.general_description", "Shader compilation, keybindings, language and the look of this menu.") },
		BuiltInMenu{ MenuPageIds::kAdvanced, T("menu.features.advanced", "Advanced"), drawAdvancedSettings, T("menu.features.advanced_description", "Developer tools, logging, profiling and utility features.") },
		BuiltInMenu{ MenuPageIds::kCSEditor, T("menu.features.cs_editor", "CS Editor"), []() {} }
	};  // NOTE: The menu list is rebuilt every frame, so category expansion states
	// persist correctly. This is acceptable since the list is small and built
	// infrequently, but could be optimized if performance becomes an issue.

	if (!searching) {
		std::vector<Feature*> pinnedFeatures;
		for (const auto& shortName : globals::menu->GetPinnedFeatures()) {
			const auto it = std::ranges::find_if(featureList, [&shortName](Feature* feat) { return feat->GetShortName() == shortName; });
			if (it != featureList.end() && (*it)->IsInMenu() && (*it)->loaded && (*it)->GetCategory() != FeatureCategories::kUtility)
				pinnedFeatures.push_back(*it);
		}
		if (!pinnedFeatures.empty()) {
			const std::string key(kPinnedSectionKey);
			const bool expanded = categoryExpansionStates.try_emplace(key, true).first->second;
			menuList.push_back(CategoryHeader{ key, T("menu.features.pinned", "Pinned"), static_cast<int>(pinnedFeatures.size()), expanded });
			if (expanded) {
				for (Feature* feat : pinnedFeatures)
					menuList.push_back(PinnedFeature{ feat });
			}
		}
	}

	// Group features by category. Utility features are deliberately absent: they are
	// rendered as sub-tabs of the Advanced page instead of as their own list category.
	std::map<std::string, std::vector<Feature*>, std::less<>> categorizedFeatures;
	std::vector<Feature*> settingsFreeFeatures;
	const bool alphabetical = globals::menu->GetSettings().FeatureListSort == Menu::FeatureSortMode::Alphabetical;
	for (Feature* feat : sortedFeatureList) {
		if (!feat->IsInMenu() || !feat->loaded || feat->GetCategory() == FeatureCategories::kUtility)
			continue;
		if (!feat->HasSettings())
			settingsFreeFeatures.push_back(feat);
		else if (alphabetical)  // already in display-name order
			menuList.push_back(feat);
		else
			categorizedFeatures[std::string(feat->GetCategory())].push_back(feat);
	}

	auto appendSection = [&](const std::string& key, std::string label, const std::vector<Feature*>& features, bool expandedByDefault) {
		if (features.empty())
			return;
		const bool expanded = searching || categoryExpansionStates.try_emplace(key, expandedByDefault).first->second;
		menuList.push_back(CategoryHeader{ key, std::move(label), static_cast<int>(features.size()), expanded });
		if (expanded)
			std::ranges::copy(features, std::back_inserter(menuList));
	};

	for (const auto category : FeatureCategories::kOrdered) {
		if (auto it = categorizedFeatures.find(category); it != categorizedFeatures.end()) {
			appendSection(it->first, Feature::TranslateCategory(category), it->second, true);
			categorizedFeatures.erase(it);
		}
	}

	for (const auto& [category, features] : categorizedFeatures)
		appendSection(category, Feature::TranslateCategory(category), features, true);

	if (searching) {
		auto utilityMatches = GetUtilityFeatures();
		const auto& hits = GetSearchHits(featureSearch);
		std::erase_if(utilityMatches, [&hits](Feature* feat) { return !hits.contains(feat); });
		appendSection(std::string(FeatureCategories::kUtility), Feature::TranslateCategory(FeatureCategories::kUtility), utilityMatches, true);
	}

	appendSection(std::string(kNoSettingsSectionKey), T("menu.features.no_settings_section", "No Settings"), settingsFreeFeatures, false);

	auto unloadedFeatures = sortedFeatureList | std::ranges::views::filter([](Feature* feat) {
		return !feat->loaded && feat->IsInMenu() && !feat->IsHiddenUnreleased() && feat->GetCategory() != FeatureCategories::kUtility && (!FeatureIssues::IsObsoleteFeature(feat->GetShortName()) || globals::state->IsDeveloperMode());
	});
	if (std::ranges::distance(unloadedFeatures) != 0) {
		menuList.push_back(T("menu.features.unloaded_features", "Unloaded Features"));
		std::ranges::copy(unloadedFeatures, std::back_inserter(menuList));
	}
	// Add top section for feature issues (rejected features, obsolete info, etc.)
	if (FeatureIssues::HasFeatureIssues()) {
		menuList.insert(menuList.begin(), BuiltInMenu{ MenuPageIds::kIssues, T("menu.features.feature_issues", "Feature Issues"), []() {
														  FeatureIssues::DrawFeatureIssuesUI();
													  } });
	}

	return menuList;
}

void FeatureListRenderer::ResolveSelectionFromKey(
	const std::vector<MenuFuncInfo>& menuList,
	size_t& selectedMenu)
{
	// Seed the key from whatever index the caller starts with (first frame, or a restored layout).
	if (g_selectedEntryKey.empty()) {
		if (selectedMenu < menuList.size())
			g_selectedEntryKey = EntryKey(menuList[selectedMenu]);
		return;
	}

	for (size_t i = 0; i < menuList.size(); ++i) {
		if (EntryKey(menuList[i]) == g_selectedEntryKey) {
			selectedMenu = i;
			return;
		}
	}

	// The selected entry is gone: its category was collapsed, or the search filtered it out.
	// Falling back to General is deterministic; keeping the stale index would render whichever
	// unrelated entry has shifted into that slot.
	const std::string generalKey = BuiltInEntryKey(MenuPageIds::kGeneral);
	for (size_t i = 0; i < menuList.size(); ++i) {
		if (EntryKey(menuList[i]) == generalKey) {
			selectedMenu = i;
			return;
		}
	}

	if (selectedMenu >= menuList.size())
		selectedMenu = menuList.empty() ? 0 : menuList.size() - 1;
}

void FeatureListRenderer::HandlePendingFeatureSelection(
	std::string& pendingFeatureSelection,
	const std::vector<MenuFuncInfo>& menuList,
	size_t& selectedMenu)
{
	if (pendingFeatureSelection.empty())
		return;

	if (pendingFeatureSelection == MenuPageIds::kHome || pendingFeatureSelection == MenuPageIds::kSettings)
		pendingFeatureSelection = std::string(MenuPageIds::kGeneral);
	else if (pendingFeatureSelection == MenuPageIds::kProfiling)
		pendingFeatureSelection = std::string(MenuPageIds::kAdvanced);

	for (size_t i = 0; i < menuList.size(); ++i) {
		if (std::holds_alternative<Feature*>(menuList[i])) {
			Feature* feature = std::get<Feature*>(menuList[i]);
			if (feature->GetShortName() == pendingFeatureSelection) {
				selectedMenu = i;
				g_selectedEntryKey = FeatureEntryKey(pendingFeatureSelection);
				logger::info("Navigated to {} feature menu", pendingFeatureSelection);
				pendingFeatureSelection.clear();
				return;
			}
		}
	}

	// Built-in pages are addressed by their canonical ID, or by their display name for callers that pass a label.
	for (size_t i = 0; i < menuList.size(); ++i) {
		if (const auto* page = std::get_if<BuiltInMenu>(&menuList[i]); page && page->id != MenuPageIds::kCSEditor && (page->id == pendingFeatureSelection || page->name == pendingFeatureSelection)) {
			selectedMenu = i;
			g_selectedEntryKey = EntryKey(menuList[i]);
			logger::info("Navigated to {} page", pendingFeatureSelection);
			pendingFeatureSelection.clear();
			return;
		}
	}

	// Utility features are not list entries; they live as sub-tabs of the Advanced page.
	if (IsUtilityFeature(pendingFeatureSelection)) {
		for (size_t i = 0; i < menuList.size(); ++i) {
			if (std::holds_alternative<BuiltInMenu>(menuList[i]) && std::get<BuiltInMenu>(menuList[i]).id == MenuPageIds::kAdvanced) {
				selectedMenu = i;
				g_selectedEntryKey = EntryKey(menuList[i]);
				AdvancedSettingsRenderer::RequestUtilityTab(pendingFeatureSelection);
				logger::info("Navigated to {} utility tab under Advanced", pendingFeatureSelection);
				break;
			}
		}
	}

	pendingFeatureSelection.clear();  // Clear after processing
}

bool FeatureListRenderer::IsUtilityFeature(const std::string& shortName)
{
	const auto& featureList = Feature::GetFeatureList();
	return std::ranges::any_of(featureList, [&shortName](Feature* feat) {
		return feat->GetShortName() == shortName && feat->GetCategory() == FeatureCategories::kUtility;
	});
}

std::vector<Feature*> FeatureListRenderer::GetUtilityFeatures()
{
	std::vector<Feature*> utilityFeatures;
	for (Feature* feat : Feature::GetFeatureList()) {
		if (!feat->IsInMenu() || feat->GetCategory() != FeatureCategories::kUtility)
			continue;
		if (feat->IsHiddenUnreleased())
			continue;
		if (!feat->loaded && FeatureIssues::IsObsoleteFeature(feat->GetShortName()) && !globals::state->IsDeveloperMode())
			continue;
		utilityFeatures.push_back(feat);
	}

	std::ranges::sort(utilityFeatures, [](Feature* a, Feature* b) {
		return a->GetDisplayName() < b->GetDisplayName();
	});
	return utilityFeatures;
}

void FeatureListRenderer::RenderFeaturePage(Feature* feat)
{
	// The Advanced page owns no pending-selection state, so navigation requests raised from a
	// utility page are routed back through the Menu, which re-enters HandlePendingFeatureSelection.
	static std::string pendingSelection;
	static const Hooks noHooks;
	DrawMenuVisitor visitor{ pendingSelection, g_activeHooks ? *g_activeHooks : noHooks };
	visitor(feat);
	if (!pendingSelection.empty()) {
		globals::menu->SelectFeatureMenu(pendingSelection);
		pendingSelection.clear();
	}
}

bool FeatureListRenderer::RenderNavigation(
	const std::vector<MenuFuncInfo>& menuList,
	size_t& selectedMenu,
	std::string& featureSearch,
	bool& focusSearch,
	std::map<std::string, bool>& categoryExpansionStates,
	bool inPopup,
	const Hooks& hooks)
{
	bool selectionMade = false;

	if (Util::DrawFeatureSearchBar(featureSearch, 0.0f, focusSearch) && !featureSearch.empty()) {
		const auto first = std::ranges::find_if(menuList, [](const MenuFuncInfo& entry) { return std::holds_alternative<Feature*>(entry); });
		if (first != menuList.end()) {
			Feature* feat = std::get<Feature*>(*first);
			if (feat->GetCategory() == FeatureCategories::kUtility) {
				globals::menu->SelectFeatureMenu(feat->GetShortName());
			} else {
				selectedMenu = static_cast<size_t>(std::distance(menuList.begin(), first));
				g_selectedEntryKey = EntryKey(*first);
			}
			selectionMade = true;
		}
	}
	focusSearch = false;

	const std::string currentKey = selectedMenu < menuList.size() ? EntryKey(menuList[selectedMenu]) : std::string();
	const bool selectOnNav = !inPopup;
	const auto& style = ImGui::GetStyle();
	{
		MenuFonts::FontRoleGuard fontGuard(Menu::FontRole::Heading);
		g_headerLabelInset = Util::CategoryLabelInset();
	}

	float footerHeight = 0.0f;
	{
		MenuFonts::FontRoleGuard fontGuard(Menu::FontRole::Subheading);
		footerHeight = style.ItemSpacing.y * 2.0f + 1.0f + ImGui::GetTextLineHeightWithSpacing() * static_cast<float>(CORE_MENU_COUNT);
	}

	auto isCoreEntry = [](const MenuFuncInfo& entry) {
		const auto* menu = std::get_if<BuiltInMenu>(&entry);
		return menu && IsCoreMenu(menu->id);
	};

	if (ImGui::BeginChild("##FeatureNavList", ImVec2(0.0f, -footerHeight), ImGuiChildFlags_NavFlattened)) {
		bool anyFeature = false;
		for (size_t i = 0; i < menuList.size(); i++) {
			if (isCoreEntry(menuList[i]))
				continue;
			anyFeature = anyFeature || std::holds_alternative<Feature*>(menuList[i]);
			std::visit(ListMenuVisitor{ i, selectedMenu, categoryExpansionStates, currentKey, selectOnNav, selectionMade, hooks }, menuList[i]);
		}

		if (!featureSearch.empty() && !anyFeature) {
			ImGui::Spacing();
			ImGui::PushTextWrapPos(0.0f);
			const auto noResults = I18n::GetSingleton()->Format("menu.features.no_search_results", { { "search", featureSearch } }, "Nothing matches \"{search}\".");
			Util::TextUnformattedDisabled(noResults.c_str());
			ImGui::PopTextWrapPos();
		}
	}
	ImGui::EndChild();

	ImGui::Separator();
	for (size_t i = 0; i < menuList.size(); i++) {
		if (isCoreEntry(menuList[i]))
			std::visit(ListMenuVisitor{ i, selectedMenu, categoryExpansionStates, currentKey, selectOnNav, selectionMade, hooks }, menuList[i]);
	}

	return selectionMade;
}

void FeatureListRenderer::RenderNavigatorBar(
	const std::vector<MenuFuncInfo>& menuList,
	size_t& selectedMenu,
	std::string& featureSearch,
	bool& focusSearch,
	std::map<std::string, bool>& categoryExpansionStates,
	const Hooks& hooks)
{
	constexpr const char* kPopupId = "##FeatureNavigator";
	static bool navigatorOpenLastFrame = false;
	static bool pressedWhileOpen = false;
	const ImVec2 barStart = ImGui::GetCursorScreenPos();
	const float barWidth = ImGui::GetContentRegionAvail().x;

	bool open = focusSearch;
	{
		MenuFonts::FontRoleGuard fontGuard(Menu::FontRole::Subheading);
		const std::string location = selectedMenu < menuList.size() ? DescribeLocation(menuList[selectedMenu]) : std::string();
		ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
		const bool clicked = Util::Icons::Button(std::format("{}###OpenFeatureNavigator", location).c_str(), Util::Icons::Kind::Menu, ImVec2(-FLT_MIN, 0.0f));
		ImGui::PopStyleVar();
		if (ImGui::IsItemActivated())
			pressedWhileOpen = navigatorOpenLastFrame;
		if (clicked && !pressedWhileOpen)
			open = true;
	}
	const float barBottom = ImGui::GetItemRectMax().y;
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T("menu.features.browse_tooltip", "Browse and search features (Ctrl+F)"));

	const ImGuiID popupId = ImGui::GetID(kPopupId);
	if (open && !ImGui::IsPopupOpen(popupId, ImGuiPopupFlags_None))
		ImGui::OpenPopup(popupId);

	ImGui::SetNextWindowPos(ImVec2(barStart.x, barBottom + ImGui::GetStyle().ItemSpacing.y));
	ImGui::SetNextWindowSize(ImVec2(barWidth, ImGui::GetWindowHeight() * ThemeManager::Constants::NAVIGATOR_POPUP_HEIGHT_RATIO));
	if (ImGui::BeginPopupEx(popupId, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize)) {
		if (RenderNavigation(menuList, selectedMenu, featureSearch, focusSearch, categoryExpansionStates, true, hooks))
			ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}
	navigatorOpenLastFrame = ImGui::IsPopupOpen(popupId, ImGuiPopupFlags_None);
	focusSearch = false;

	ImGui::Spacing();
}

void FeatureListRenderer::RenderAutoHidePane(
	const std::vector<MenuFuncInfo>& menuList,
	size_t& selectedMenu,
	std::string& featureSearch,
	bool& focusSearch,
	std::string& pendingFeatureSelection,
	std::map<std::string, bool>& categoryExpansionStates,
	const Hooks& hooks)
{
	using Constants = ThemeManager::Constants;
	const float scale = Util::GetUIScale();
	const ImVec2 areaMin = ImGui::GetCursorScreenPos();
	const ImVec2 avail = ImGui::GetContentRegionAvail();
	const float drawerWidth = std::max(avail.x * Constants::AUTOHIDE_PANEL_WIDTH_RATIO, Constants::AUTOHIDE_PANEL_MIN_WIDTH * scale);

	// The page takes the full width, so it never reflows while the list slides over its left edge. It keeps the
	// gutter a table cell would give it, which is also where the grip sits.
	const float gutter = ImGui::GetStyle().CellPadding.x;
	ImGui::Indent(gutter);
	RenderRightColumn(menuList, selectedMenu, pendingFeatureSelection, hooks);
	ImGui::Unindent(gutter);

	auto& drawer = g_drawer;
	const double now = ImGui::GetTime();
	const ImVec2 mouse = ImGui::GetIO().MousePos;
	const bool inY = ImGui::IsMousePosValid(&mouse) && mouse.y >= areaMin.y && mouse.y <= areaMin.y + avail.y;
	const bool inEdge = inY && mouse.x >= areaMin.x - Constants::AUTOHIDE_EDGE_SLOP * scale && mouse.x < areaMin.x + Constants::AUTOHIDE_ACTIVATION_ZONE_WIDTH * scale;
	const bool inDrawer = inY && mouse.x >= areaMin.x && mouse.x < areaMin.x + drawerWidth;

	if (focusSearch)
		drawer.open = true;

	if (!drawer.open) {
		// Opens after the cursor has rested on the edge for a moment, so passing by does not flash the list
		if (!inEdge)
			drawer.hoverSince = -1.0;
		else if (drawer.hoverSince < 0.0)
			drawer.hoverSince = now;
		else if (now - drawer.hoverSince >= Constants::AUTOHIDE_OPEN_DELAY)
			drawer.open = true;
	} else if (inDrawer || drawer.keepOpen) {
		drawer.leaveSince = -1.0;
	} else if (drawer.leaveSince < 0.0) {
		drawer.leaveSince = now;
	} else if (now - drawer.leaveSince >= Constants::AUTOHIDE_CLOSE_DELAY) {
		drawer.open = false;
		drawer.leaveSince = drawer.hoverSince = -1.0;
	}

	// Exponential smoothing: frame-rate independent, fast at first and settling softly
	const float target = drawer.open ? 1.0f : 0.0f;
	drawer.progress = Util::Anim::Smooth(drawer.progress, target, Constants::AUTOHIDE_ANIM_SPEED);

	if (drawer.progress <= 0.0f) {
		// A thin grip on the edge hints that the list is there
		const float grip = Constants::AUTOHIDE_GRIP_WIDTH * scale;
		ImVec4 gripColor = Util::Colors::GetSecondary();
		gripColor.w *= Constants::AUTOHIDE_GRIP_ALPHA;
		ImGui::GetWindowDrawList()->AddRectFilled(areaMin, ImVec2(areaMin.x + grip, areaMin.y + avail.y), ImGui::GetColorU32(gripColor));
		drawer.keepOpen = false;
		if (!drawer.open)
			focusSearch = false;
		return;
	}

	ImGui::SetCursorScreenPos(ImVec2(areaMin.x - drawerWidth * (1.0f - drawer.progress), areaMin.y));
	ImVec4 background = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
	background.w = 1.0f;
	ImGui::PushStyleColor(ImGuiCol_ChildBg, background);
	if (ImGui::BeginChild("##FeatureListDrawer", ImVec2(drawerWidth, avail.y), ImGuiChildFlags_Borders)) {
		RenderNavigation(menuList, selectedMenu, featureSearch, focusSearch, categoryExpansionStates, false, hooks);
		// An active field or an open popup holds the list open even with the cursor elsewhere
		drawer.keepOpen = ImGui::IsAnyItemActive() || ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
	}
	ImGui::EndChild();
	ImGui::PopStyleColor();
}

void FeatureListRenderer::RenderRightColumn(
	const std::vector<MenuFuncInfo>& menuList,
	size_t selectedMenu,
	std::string& pendingFeatureSelection,
	const Hooks& hooks)
{
	if (selectedMenu < menuList.size()) {
		// A newly opened page fades in; the key is the page itself, so the rebuilt list does not restart it
		Util::Anim::PageFade fade(ImGui::GetID(EntryKey(menuList[selectedMenu]).c_str()));
		std::visit(DrawMenuVisitor{ pendingFeatureSelection, hooks }, menuList[selectedMenu]);
	} else {
		BrowserUI::EmptyState(T("menu.features.select_item_left", "Please select an item on the left."));
	}
}

void FeatureListRenderer::ListMenuVisitor::Select(const std::string& entryKey)
{
	selectedMenuRef = listId;
	g_selectedEntryKey = entryKey;
	selectionMade = true;
}

void FeatureListRenderer::ListMenuVisitor::operator()(const BuiltInMenu& menu)
{
	MenuFonts::FontRoleGuard fontGuard(Menu::FontRole::Subheading);

	const std::string key = BuiltInEntryKey(menu.id);
	// Use error color for Feature Issues menu item
	const bool isFeatureIssues = menu.id == MenuPageIds::kIssues;
	if (isFeatureIssues)
		ImGui::PushStyleColor(ImGuiCol_Text, globals::menu->GetSettings().Theme.StatusPalette.Error);

	// The row's text lines up with the category labels; the highlight still spans the whole row
	ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ListRowTextInset());
	const std::string label = std::format("{} ###{}", menu.name, menu.id);
	if (menu.id == MenuPageIds::kCSEditor) {
		// Launcher entry: opens the editor without leaving the current page.
		const bool canOpenEditor = EditorWindow::CanBeOpen();
		if (Util::Selectable(label.c_str(), false, NavigationSelectableFlags(false))) {
			if (canOpenEditor)
				CSEditor::OpenEditorWindow();
			else  // Without a save, says why
				EditorWindow::GetSingleton()->WarnOpeningBlocked();
		}
		Util::HoverTip(canOpenEditor ? T("menu.features.cs_editor_tooltip", "Opens the CS Editor window") :
		                               T("menu.features.cs_editor_needs_save", "Load a save to open the CS Editor"));
	} else if (Util::Selectable(label.c_str(), key == currentKey, NavigationSelectableFlags(selectOnNav))) {
		Select(key);
	}

	if (isFeatureIssues)
		ImGui::PopStyleColor();
}

void FeatureListRenderer::ListMenuVisitor::operator()(const std::string& label)
{
	// Static list headings share the category header's font, metrics and alignment.
	AddSectionGap();
	MenuFonts::FontRoleGuard fontGuard(Menu::FontRole::Heading);
	Util::DrawCategoryLabel(label.c_str());
}

void FeatureListRenderer::ListMenuVisitor::operator()(const CategoryHeader& header)
{
	AddSectionGap();
	MenuFonts::FontRoleGuard fontGuard(Menu::FontRole::Heading);
	bool isExpanded = header.expanded;
	const ImVec2 rowMin = ImGui::GetCursorScreenPos();
	const float rowWidth = ImGui::GetContentRegionAvail().x;
	if (Util::DrawCategoryHeader(header.name.c_str(), header.label.c_str(), isExpanded, header.count))
		globals::menu->SetFeatureSectionExpanded(header.name, isExpanded);
	const ImVec2 rowMax(rowMin.x + rowWidth, ImGui::GetCursorScreenPos().y);

	// Right-click: fold the whole list down to its headings, or open all of it
	ImGui::PushID(header.name.c_str());
	if (ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(rowMin, rowMax) && ImGui::IsMouseReleased(ImGuiMouseButton_Right))
		ImGui::OpenPopup("##CategoryHeaderMenu");
	if (ImGui::BeginPopup("##CategoryHeaderMenu")) {
		{
			// Scoped so the font pops before EndPopup closes the popup
			MenuFonts::FontRoleGuard menuFont(Menu::FontRole::Body);
			if (ImGui::MenuItem(T("menu.features.collapse_all", "Collapse All")))
				globals::menu->SetAllFeatureSectionsExpanded(false);
			if (ImGui::MenuItem(T("menu.features.expand_all", "Expand All")))
				globals::menu->SetAllFeatureSectionsExpanded(true);
		}
		ImGui::EndPopup();
	}
	ImGui::PopID();
}

void FeatureListRenderer::ListMenuVisitor::operator()(Feature* feat)
{
	DrawFeatureRow(feat, false);
}

void FeatureListRenderer::ListMenuVisitor::operator()(const PinnedFeature& pinned)
{
	DrawFeatureRow(pinned.feature, true);
}

void FeatureListRenderer::ListMenuVisitor::DrawFeatureRow(Feature* feat, bool inPinnedSection)
{
	MenuFonts::FontRoleGuard fontGuard(Menu::FontRole::Subheading);

	const auto featureName = feat->GetShortName();
	bool isDisabled = globals::state->IsFeatureDisabled(featureName);
	bool isLoaded = feat->loaded;
	bool hasFailedMessage = !feat->failedLoadedMessage.empty();
	auto& themeSettings = globals::menu->GetSettings().Theme;

	ImVec4 textColor;

	// Determine the text color based on the state
	if (isDisabled) {
		textColor = themeSettings.StatusPalette.Disable;
	} else if (isLoaded) {
		textColor = ImGui::GetStyleColorVec4(ImGuiCol_Text);
	} else if (hasFailedMessage) {
		textColor = feat->version.empty() ? themeSettings.StatusPalette.Disable : themeSettings.StatusPalette.Error;
	} else {
		// Installed but not loaded means the feature is only pending a restart (green),
		// otherwise it is simply missing (grey).
		textColor = feat->installed ? themeSettings.StatusPalette.RestartNeeded : themeSettings.StatusPalette.Disable;
	}

	const std::string key = FeatureEntryKey(featureName);
	ImGui::PushID(inPinnedSection ? "Pinned" : "Listed");
	ImGui::PushID(featureName.c_str());

	const bool isUtility = feat->GetCategory() == FeatureCategories::kUtility;
	// The row's text lines up with the category label above it; the highlight still spans the whole row
	const float rowTextInset = ListRowTextInset();
	ImGui::SetCursorPosX(ImGui::GetCursorPosX() + rowTextInset);
	ImGui::PushStyleColor(ImGuiCol_Text, textColor);
	// The ID comes from the short name, so it survives a language change.
	if (Util::Selectable(std::format("{} ###Row", feat->GetDisplayName()).c_str(), key == currentKey, NavigationSelectableFlags(selectOnNav && !isUtility))) {
		if (isUtility) {
			globals::menu->SelectFeatureMenu(featureName);
			selectionMade = true;
		} else {
			Select(key);
		}
	}
	ImGui::PopStyleColor();
	if (g_scrollToSelection && key == currentKey && !ImGui::IsItemVisible())
		ImGui::SetScrollHereY();
	const float rowHeight = ImGui::GetItemRectSize().y;
	RenderRowContextMenu(feat);

	// The stage badge sits right after the name, regardless of loaded state
	if (const auto stage = feat->GetReleaseStage(); stage != Feature::ReleaseStage::Release) {
		ImGui::SameLine();
		DrawStageBadge(stage, rowHeight);
	}

	// A feature only authored for other scenes still gets a hollow dot, so its settings stay discoverable.
	if (auto* sceneManager = globals::sceneSettingsManager; sceneManager->HasAnySceneEntriesForFeature(featureName)) {
		const bool applying = sceneManager->IsFeatureOverridingBase(featureName);
		ImGui::SameLine();
		Util::DrawInlineIndicatorDot(ImGui::GetColorU32(Util::Colors::GetInfo()), applying);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(applying ?
									   T("menu.features.scene_indicator_overriding",
										   "Scene Manager is overriding settings here. Blue settings show its values.") :
									   T("menu.features.scene_indicator_configured",
										   "Has Scene Manager settings for other times, weathers or locations."));

		if (sceneManager->HasSketches(featureName)) {
			ImGui::SameLine();
			Util::DrawInlineIndicatorDot(ImGui::GetColorU32(Util::Colors::GetSuccess()), true);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(T("menu.features.scene_indicator_sketched",
					"Has settings sketched over the Scene Manager's values. They revert once you leave this feature."));
		}
	}

	const auto& search = globals::menu->featureSearch;
	if (!inPinnedSection && !search.empty()) {
		const auto& hits = GetSearchHits(search);
		if (const auto it = hits.find(feat); it != hits.end() && !it->second.nameMatch && !it->second.settingLabel.empty()) {
			MenuFonts::FontRoleGuard subtextGuard(Menu::FontRole::Subtext);
			const std::string hint = it->second.extraSettings > 0 ? std::format("{} (+{})", it->second.settingLabel, it->second.extraSettings) : it->second.settingLabel;
			const float indent = rowTextInset + ImGui::GetFontSize() * ThemeManager::Constants::FEATURE_LIST_HINT_INDENT_EM;
			ImGui::Indent(indent);
			Util::TextUnformattedDisabled(hint.c_str());
			ImGui::Unindent(indent);
		}
	}

	ImGui::PopID();
	ImGui::PopID();
}

void FeatureListRenderer::ListMenuVisitor::RenderRowContextMenu(Feature* feat) const
{
	if (!ImGui::BeginPopupContextItem("##FeatureRowMenu"))
		return;

	{
		// Scoped so the font pops before EndPopup closes the popup
		MenuFonts::FontRoleGuard menuFont(Menu::FontRole::Body);
		auto* state = globals::state;
		const auto featureName = feat->GetShortName();
		const bool isLoaded = feat->loaded;
		const bool isDisabled = state->IsFeatureDisabled(featureName);

		const bool canPin = isLoaded && feat->GetCategory() != FeatureCategories::kUtility;
		if (ImGui::MenuItem(globals::menu->IsFeaturePinned(featureName) ? T("menu.features.unpin", "Unpin") : T("menu.features.pin", "Pin to Top"), nullptr, false, canPin))
			ToggleFavorite(feat);
		if (!canPin)
			Util::AddTooltip(T("menu.features.pin_needs_loaded", "Only loaded features can be pinned"), Util::kTooltipWhenDisabled);

		const bool canRestore = isLoaded && !isDisabled && feat->HasSettings();
		if (ImGui::MenuItem(T("menu.features.restore_defaults", "Restore Defaults"), nullptr, false, canRestore && (!feat->UsesMainSettings() || state->HasUserEdits(feat))))
			RestoreFeatureDefaults(hooks, feat);
		Util::AddTooltip(canRestore ? T("menu.features.restore_defaults_tooltip", "Restore defaults: clears your edits to this feature, back to what defaults and your Baseline presets set.\nRevert undoes this until you save.") :
		                              T("menu.features.restore_unavailable", "Only loaded features with settings can be restored"),
			Util::kTooltipWhenDisabled);

		if (ImGui::MenuItem(T("menu.features.load_at_boot", "Load at Boot"), nullptr, !isDisabled)) {
			const bool disabled = feat->ToggleAtBootSetting();
			globals::menu->MarkFeatureEdited(feat);
			logger::info("{}: {} at boot.", featureName, disabled ? "Disabled" : "Enabled");
		}
		Util::AddTooltip(T("menu.features.load_at_boot_tooltip", "Whether the game loads this feature at startup. Takes effect after a restart."));

		ImGui::Separator();
		if (ImGui::MenuItem(T("menu.features.copy_name", "Copy Name")))
			ImGui::SetClipboardText(feat->GetDisplayName().c_str());
	}
	ImGui::EndPopup();
}

void FeatureListRenderer::DrawMenuVisitor::operator()(const BuiltInMenu& menu)
{
	const bool editedBefore = WasItemEditedThisFrame();
	if (ImGui::BeginChild("##FeatureConfigFrame", { 0, 0 }, true)) {
		if (!menu.description.empty()) {
			DrawPageTitle(menu.name, "");
			DrawPageDescription(menu.description);
			ImGui::Separator();
		}
		menu.func();
	}
	ImGui::EndChild();
	if (!editedBefore && WasItemEditedThisFrame() && !globals::menu->AreSettingEditsIgnored())
		globals::menu->MarkUnsavedChanges();
}

void FeatureListRenderer::DrawMenuVisitor::operator()(const std::string&)
{
	// std::unreachable() from c++23
	// you are not supposed to have selected a label!
}

void FeatureListRenderer::DrawMenuVisitor::operator()(const CategoryHeader&)
{
	// Category headers are not selectable in the right panel
	BrowserUI::EmptyState(T("menu.features.select_feature_left", "Please select a feature from the left."));
}

void FeatureListRenderer::DrawMenuVisitor::operator()(const PinnedFeature& pinned)
{
	(*this)(pinned.feature);
}

void FeatureListRenderer::DrawMenuVisitor::operator()(Feature* feat)
{
	const auto featureName = feat->GetShortName();
	bool isDisabled = globals::state->IsFeatureDisabled(featureName);
	bool isLoaded = feat->loaded;
	bool hasFailedMessage = !feat->failedLoadedMessage.empty();

	if (hooks.onFeatureShown)
		hooks.onFeatureShown(feat);

	globals::menu->BeginFeatureEditTracking(feat);
	const bool editedBefore = WasItemEditedThisFrame();
	featureChanged = false;

	if (ImGui::BeginChild("##FeatureConfigFrame", { 0, 0 }, true)) {
		// Render feature header with integrated action buttons
		RenderFeatureHeader(feat, isDisabled, isLoaded);

		// Render feature settings content
		RenderFeatureSettings(feat, isDisabled, isLoaded, hasFailedMessage);
	}
	ImGui::EndChild();

	globals::menu->EndFeatureEditTracking(feat, featureChanged || (!editedBefore && WasItemEditedThisFrame()));
	globals::menu->IgnoreSettingEditsThisFrame();
	// Render reactive constraint warning outside the child window so it can appear as a top-level popup
	DrawConstraintWarningDialog(pendingFeatureSelection);
}

void FeatureListRenderer::DrawMenuVisitor::DrawFeatureContent(Feature* feat)
{
	const bool isDisabled = globals::state->IsFeatureDisabled(feat->GetShortName());
	const bool isLoaded = feat->loaded;
	const bool hasFailedMessage = !feat->failedLoadedMessage.empty();

	if (hooks.onFeatureShown)
		hooks.onFeatureShown(feat);

	RenderFeatureHeader(feat, isDisabled, isLoaded);
	RenderFeatureSettings(feat, isDisabled, isLoaded, hasFailedMessage);
}

void FeatureListRenderer::DrawEmbeddedFeaturePage(Feature* feat)
{
	static const Hooks noHooks{};
	std::string pendingFeatureSelection;
	DrawMenuVisitor{ pendingFeatureSelection, g_activeHooks ? *g_activeHooks : noHooks }.DrawFeatureContent(feat);
	DrawConstraintWarningDialog(pendingFeatureSelection);
	if (!pendingFeatureSelection.empty())
		globals::menu->SelectFeatureMenu(pendingFeatureSelection);
}

void FeatureListRenderer::RunFeatureCommand(Feature* feat, const std::function<void()>& command)
{
	if (g_activeHooks && g_activeHooks->runFeatureCommand)
		g_activeHooks->runFeatureCommand(feat, command);
	else
		command();
}

namespace
{
	/** @brief Table of a feature's edited settings with their current and base values, for the Modified chip's tooltip. */
	void RenderUserEditsList(Feature* feat)
	{
		const auto edits = globals::state->GetUserEdits(feat);
		const auto* base = globals::state->GetFeatureBase(feat->GetName());
		const auto rows = Util::Settings::GetExportSettings(feat->GetShortName(), edits);
		if (rows.empty() || !ImGui::BeginTable("##UserEdits", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg))
			return;
		ImGui::TableSetupColumn(T("menu.features.modified_list_setting", "Setting"));
		ImGui::TableSetupColumn(T("menu.features.modified_list_value", "Value"));
		ImGui::TableSetupColumn(T("menu.features.modified_list_default", "Default"));
		ImGui::TableHeadersRow();
		for (const auto& row : rows) {
			const nlohmann::json::json_pointer pointer(row.path);
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(row.label.c_str());
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(Util::Settings::FormatValue(edits.at(pointer)).c_str());
			ImGui::TableNextColumn();
			if (base && base->contains(pointer))
				ImGui::TextDisabled("%s", Util::Settings::FormatValue(base->at(pointer)).c_str());
		}
		ImGui::EndTable();
	}
}

void FeatureListRenderer::DrawMenuVisitor::RunFeatureCommand(Feature* feat, const std::function<void()>& command) const
{
	if (hooks.runFeatureCommand)
		hooks.runFeatureCommand(feat, command);
	else
		command();
}

void FeatureListRenderer::DrawMenuVisitor::RenderDisabledAtBootNotice(Feature* feat)
{
	BrowserUI::EmptyState(T("menu.features.settings_hidden_disabled", "Feature settings are hidden because this feature is disabled at boot."),
		T("menu.features.enable_at_boot_hint", "Load it at boot to use it the next time the game starts."));
	ImGui::Spacing();
	const char* label = T("menu.features.enable_at_boot", "Enable at Boot");
	const float width = ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
	ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (ImGui::GetContentRegionAvail().x - width) * 0.5f));
	if (Util::Button(label)) {
		const bool disabled = feat->ToggleAtBootSetting();
		globals::menu->MarkFeatureEdited(feat);
		logger::info("{}: {} at boot.", feat->GetShortName(), disabled ? "Disabled" : "Enabled");
	}
}

void FeatureListRenderer::DrawMenuVisitor::RenderFeatureHeader(Feature* feat, bool isDisabled, bool isLoaded)
{
	auto& themeSettings = globals::menu->GetSettings().Theme;
	const auto featureName = feat->GetShortName();
	const auto& style = ImGui::GetStyle();

	const float buttonPadding = ThemeManager::Constants::BUTTON_PADDING;
	const float frameHeight = ImGui::GetFrameHeight();

	const char* baselinesButtonText = T("menu.features.use_baselines", "Use Baselines");
	const char* restoreButtonText = T("menu.features.restore_defaults", "Restore Defaults");
	const char* bootLabel = T("menu.features.load_at_boot", "Load at Boot");
	const float bootToggleWidth = frameHeight * 1.6f;
	const float baselinesButtonWidth = ImGui::CalcTextSize(baselinesButtonText).x + buttonPadding;
	const float restoreButtonWidth = ImGui::CalcTextSize(restoreButtonText).x + ImGui::GetFontSize() + style.ItemInnerSpacing.x + buttonPadding;

	const bool canExport = !isDisabled && isLoaded && FeatureOverwritesPanel::HasExportableSettings(feat);
	const bool canRestoreDefaults = !isDisabled && isLoaded && feat->HasSettings();
	// Offered while the user's edits hide values one of their Baseline packs sets.
	const bool canUseBaselines = canRestoreDefaults && feat->UsesMainSettings() && globals::state->HasEditsUnderBaselines(feat);
	const bool canPin = isLoaded && feat->IsInMenu() && feat->GetCategory() != FeatureCategories::kUtility;

	float actionsWidth = ImGui::CalcTextSize(bootLabel).x + style.ItemInnerSpacing.x + bootToggleWidth;
	if (canPin)
		actionsWidth += frameHeight + style.ItemSpacing.x;
	if (canExport)
		actionsWidth += frameHeight + style.ItemSpacing.x;
	if (canUseBaselines)
		actionsWidth += baselinesButtonWidth + style.ItemSpacing.x;
	if (canRestoreDefaults)
		actionsWidth += restoreButtonWidth + style.ItemSpacing.x;

	auto drawActions = [&]() {
		bool first = true;
		auto nextItem = [&first](float width) {
			if (!first) {
				ImGui::SameLine();
				if (ImGui::GetContentRegionAvail().x < width)
					ImGui::NewLine();
			}
			first = false;
		};

		if (canPin) {
			nextItem(frameHeight);
			const bool pinned = globals::menu->IsFeaturePinned(featureName);
			const ImU32 starColor = ImGui::GetColorU32(pinned ? ImGuiCol_CheckMark : ImGuiCol_TextDisabled);
			if (Util::Icons::IconButton("##PinFeature", pinned ? Util::Icons::Kind::StarFilled : Util::Icons::Kind::Star, starColor))
				globals::menu->SetFeaturePinned(featureName, !pinned);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s", pinned ? T("menu.features.unpin_tooltip", "Remove from the Pinned section.") :
				                           T("menu.features.pin_tooltip", "Pin to the top of the feature list."));
			}
		}

		if (canExport) {
			nextItem(frameHeight);
			FeatureOverwritesPanel::DrawExportButton(feat, "##ExportOverwrite");
		}

		if (canUseBaselines) {
			nextItem(baselinesButtonWidth);
			if (Util::Button(baselinesButtonText, { baselinesButtonWidth, 0 })) {
				RunFeatureCommand(feat, [feat] { globals::state->ClearFeatureEditsUnderBaselines(feat); });
				featureChanged = true;
			}
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s", T("menu.features.use_baselines_tooltip",
									  "Use Baseline preset values: your changes override settings your Baseline presets set. "
									  "Click to drop those changes and use the presets' values. Your other changes stay."));
			}
		}

		if (canRestoreDefaults) {
			nextItem(restoreButtonWidth);
			if (Util::Icons::Button(restoreButtonText, Util::Icons::Kind::Reload, { restoreButtonWidth, 0 })) {
				RestoreFeatureDefaults(hooks, feat);
				featureChanged = true;
			}
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s", T("menu.features.restore_defaults_tooltip", "Restore defaults: clears your edits to this feature, back to what defaults and your Baseline presets set.\nRevert undoes this until you save."));
			}
		}

		// Enable/Disable at boot toggle
		nextItem(ImGui::CalcTextSize(bootLabel).x + style.ItemInnerSpacing.x + bootToggleWidth);
		bool bootEnabled = !isDisabled;
		auto bootTooltip = [isDisabled]() {
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text(
					T("menu.features.boot_toggle_tooltip",
						"Toggle feature loading at boot.\n"
						"Current state: %s\n"
						"Restart required for changes to take effect.\n"
						"Disabling removes performance impact."),
					!isDisabled ? T("menu.features.enabled", "Enabled") : T("menu.features.disabled", "Disabled"));
			}
		};

		const ImVec2 labelPos = ImGui::GetCursorScreenPos();
		const ImVec2 labelSize = ImGui::CalcTextSize(bootLabel);
		bool toggled = ImGui::InvisibleButton("##BootLabel", ImVec2(labelSize.x, frameHeight));
		ImGui::GetWindowDrawList()->AddText(ImVec2(labelPos.x, labelPos.y + (frameHeight - labelSize.y) * 0.5f), ImGui::GetColorU32(ImGuiCol_Text), bootLabel);
		bootTooltip();

		ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
		ImGui::SetCursorPosY(ImGui::GetCursorPosY() + frameHeight * 0.1f);
		{
			const bool failed = !feat->failedLoadedMessage.empty();
			if (failed)
				ImGui::PushStyleColor(ImGuiCol_Text, themeSettings.StatusPalette.Error);
			toggled = Util::FeatureToggle("##BootToggle", &bootEnabled) || toggled;
			if (failed)
				ImGui::PopStyleColor();
		}
		bootTooltip();

		if (toggled) {
			bool newState = feat->ToggleAtBootSetting();
			featureChanged = true;
			logger::info("{}: {} at boot.", featureName, newState ? "Disabled" : "Enabled");
		}
	};

	const float availableWidth = ImGui::GetContentRegionAvail().x;
	const ImVec2 titleStartPos = ImGui::GetCursorScreenPos();

	auto [description, keyFeatures] = feat->GetFeatureSummary();
	(void)keyFeatures;  // Not used for subtitle display

	const auto title = DrawPageTitle(feat->GetDisplayName(), isLoaded ? feat->version : "", feat->GetReleaseStage());
	const ImVec2 cursorAfterTitle = ImGui::GetCursorScreenPos();

	// Quiet marker while the feature differs from its defaults; clicking restores them
	float titleWidth = title.width;
	if (canRestoreDefaults && feat->UsesMainSettings() && globals::state->HasUserEdits(feat)) {
		const char* modifiedLabel = T("menu.features.modified_chip", "Modified");
		const ImVec4 secondary = Util::Colors::GetSecondary();
		float chipWidth = 0.0f;
		bool restore = false;
		{
			MenuFonts::FontRoleGuard subtext(Menu::FontRole::Subtext);
			chipWidth = BrowserUI::MeasureChip(modifiedLabel, true);
			ImGui::SetCursorScreenPos(ImVec2(titleStartPos.x + title.width + style.ItemSpacing.x, titleStartPos.y + title.height - frameHeight));
			ImGui::PushStyleColor(ImGuiCol_Text, secondary);
			restore = BrowserUI::Chip("##FeatureModified", modifiedLabel, Icons::TI(ICON_TI_PENCIL), &secondary);
			ImGui::PopStyleColor();
		}
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted(T("menu.features.modified_chip_tooltip", "Differs from the defaults and your Baseline presets. Click to restore defaults."));
			if (ImGui::GetIO().KeyShift)
				RenderUserEditsList(feat);
			else
				ImGui::TextDisabled("%s", T("menu.features.modified_chip_shift_hint", "Hold Shift to list the edited settings."));
		}
		if (restore) {
			RestoreFeatureDefaults(hooks, feat);
			featureChanged = true;
		}
		titleWidth += style.ItemSpacing.x + chipWidth;
		ImGui::SetCursorScreenPos(cursorAfterTitle);
	}

	if (!isDisabled && isLoaded) {
		const bool inlineWithoutChip = titleWidth + style.ItemSpacing.x * 2.0f + actionsWidth <= availableWidth;
		const float chipRightLimit = titleStartPos.x + availableWidth - (inlineWithoutChip ? actionsWidth + style.ItemSpacing.x * 2.0f : 0.0f);
		ImGui::SetCursorScreenPos(ImVec2(titleStartPos.x + titleWidth + style.ItemSpacing.x, titleStartPos.y + title.height - frameHeight));
		if (ProfilingRenderer::RenderFeatureCostChip(featureName, chipRightLimit))
			titleWidth = std::max(titleWidth, ImGui::GetItemRectMax().x - titleStartPos.x);
		ImGui::SetCursorScreenPos(cursorAfterTitle);
	}

	const bool actionsInline = titleWidth + style.ItemSpacing.x * 2.0f + actionsWidth <= availableWidth;
	if (actionsInline) {
		ImGui::SetCursorScreenPos(ImVec2(titleStartPos.x + availableWidth - actionsWidth, titleStartPos.y + (title.height - frameHeight) * 0.5f));
		drawActions();
		ImGui::SetCursorScreenPos(cursorAfterTitle);
	}

	DrawPageDescription(description);

	if (!actionsInline) {
		ImGui::Spacing();
		drawActions();
	}

	// Drawn after all header items so the modal cannot clobber their hover state.
	if (canExport)
		FeatureOverwritesPanel::DrawExport();

	ImGui::Separator();
}

void FeatureListRenderer::DrawMenuVisitor::RenderFeatureSettings(Feature* feat, bool isDisabled, bool isLoaded, bool hasFailedMessage)
{
	auto& themeSettings = globals::menu->GetSettings().Theme;

	if (isDisabled) {
		RenderDisabledAtBootNotice(feat);
	} else {
		if (isLoaded) {
			DrawFeatureBody(feat);
		} else {
			if (FeatureIssues::IsObsoleteFeature(feat->GetShortName())) {
				feat->DrawUnloadedUI();
			} else if (feat->installed) {
				ImGui::Text("%s", T("menu.features.available_after_restart", "This feature will be available after restart."));
			} else {
				feat->DrawUnloadedUI();
				if (!feat->GetFeatureModLink().empty()) {
					ImGui::Spacing();
					auto featureModLink = feat->GetFeatureModLink();
					const auto downloadText = std::vformat(
						T("menu.features.download_link", "Click here to download this feature ({})"), std::make_format_args(featureModLink));
					if (Util::Selectable(downloadText.c_str()))
						ShellExecuteA(nullptr, "open", featureModLink.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
					Util::HoverTip(T("menu.features.download_tooltip", "Download the feature from the mod page."));
				}
			}
		}
	}

	if (hasFailedMessage && feat->DrawFailLoadMessage() && !FeatureIssues::IsObsoleteFeature(feat->GetShortName())) {
		ImGui::Spacing();
		Util::SeparatorTextWithFont(T("menu.features.error_header", "Error"), Menu::FontRole::Subheading);
		ImGui::TextColored(themeSettings.StatusPalette.Error, "%s", feat->failedLoadedMessage.c_str());
	}
}

void FeatureListRenderer::DrawConstraintWarningDialog(std::string& pendingFeatureSelection)
{
	if (!g_reactiveWarningShow)
		return;

	constexpr const char* popupId = "###SettingChangeWarning";
	const std::string popupTitle = fmt::format("{}{}", T("menu.features.setting_change_warning_title", "Setting Change Warning"), popupId);

	// Idempotent while the popup is open, so it is safe to call every frame while the flag is set
	ImGui::OpenPopup(popupId);
	ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));

	if (Util::BeginPopupModalWithRoundedClose(popupTitle.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		ImGui::TextWrapped("%s", T("menu.features.settings_adjusted_warning", "Some of your settings have been automatically adjusted due to feature incompatibilities."));
		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();

		std::string navigationTarget;  // Set on a link click so the table and popup still end cleanly.

		// Table columns: Impacted Feature | Setting | Constrained By | Forced To
		if (ImGui::BeginTable("##ReactiveConstraintTable", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
			ImGui::TableSetupColumn(T("menu.features.col_impacted_feature", "Impacted Feature"), ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn(T("menu.features.col_setting", "Setting"), ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn(T("menu.features.col_constrained_by", "Constrained By"), ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn(T("menu.features.col_forced_to", "Forced To"), ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableHeadersRow();

			size_t rowIndex = 0;
			for (const auto& [settingId, result] : g_reactiveWarningConstraints) {
				ImGui::TableNextRow();

				// Impacted feature: click to navigate to it
				ImGui::TableSetColumnIndex(0);
				auto* target = Feature::FindFeatureByShortName(settingId.featureShortName);
				const std::string targetDisplayName = target ? target->GetDisplayName() : settingId.featureShortName;
				if (Util::Selectable(fmt::format("{}##imp{}", targetDisplayName, rowIndex).c_str())) {
					navigationTarget = settingId.featureShortName;
					break;
				}
				if (auto _tt = Util::HoverTooltipWrapper())
					ImGui::Text(T("menu.features.click_to_navigate", "Click to navigate to %s"), targetDisplayName.c_str());

				ImGui::TableSetColumnIndex(1);
				ImGui::Text("%s", settingId.settingPath.c_str());

				// Constraining features: click the first to navigate to it
				ImGui::TableSetColumnIndex(2);
				if (!result.sources.empty()) {
					if (Util::Selectable(fmt::format("{}##src{}", result.sources[0].featureName, rowIndex).c_str())) {
						navigationTarget = result.sources[0].featureShortName;
						break;
					}
					if (auto _tt = Util::HoverTooltipWrapper()) {
						ImGui::Text(T("menu.features.click_to_navigate", "Click to navigate to %s"), result.sources[0].featureName.c_str());
						if (result.sources.size() > 1) {
							ImGui::Separator();
							for (size_t i = 1; i < result.sources.size(); ++i)
								ImGui::Text(T("menu.features.also_feature", "Also: %s"), result.sources[i].featureName.c_str());
						}
						ImGui::Separator();
						ImGui::Text("%s", result.sources[0].reason.c_str());
					}
				}

				ImGui::TableSetColumnIndex(3);
				ImGui::Text("%s", FeatureConstraints::FormatConstraintValue(result.forcedValue).c_str());

				rowIndex++;
			}

			ImGui::EndTable();
		}

		const bool escapePressed = !ImGui::GetIO().WantTextInput && ImGui::IsWindowFocused() && ImGui::IsKeyPressed(ImGuiKey_Escape, false);
		if (escapePressed)
			Util::NotifyEscapeHandledByPopup();
		if (!navigationTarget.empty() || escapePressed) {
			if (!navigationTarget.empty())
				pendingFeatureSelection = navigationTarget;
			g_reactiveWarningShow = false;
			g_reactiveWarningConstraints.clear();
			ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
			return;
		}

		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();

		ImGui::TextWrapped(
			"%s",
			T("menu.features.constraints_explanation",
				"These settings are disabled in their respective feature menus while the constraints are active. "
				"Adjust the constraining features to remove them."));

		ImGui::Spacing();

		ImGui::Checkbox(T("menu.features.dont_show_warning", "Don't show this warning again"), &g_dontShowAgainCheckbox);

		ImGui::Spacing();

		constexpr float buttonWidth = ThemeManager::Constants::POPUP_BUTTON_WIDTH;
		const float offset = (ImGui::GetWindowWidth() - buttonWidth) * 0.5f;
		if (offset > 0)
			ImGui::SetCursorPosX(offset);

		if (Util::Button(T("menu.features.ok_button", "OK"), ImVec2(buttonWidth, 0))) {
			if (g_dontShowAgainCheckbox)
				globals::menu->GetSettings().SkipConstraintWarning = true;
			g_reactiveWarningShow = false;
			g_reactiveWarningConstraints.clear();
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndPopup();
	} else {
		// Closed from outside (e.g. a click away): drop the pending warning
		g_reactiveWarningShow = false;
		g_reactiveWarningConstraints.clear();
	}
}
