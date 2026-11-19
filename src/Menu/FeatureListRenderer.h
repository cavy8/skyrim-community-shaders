#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "Feature.h"
#include "Menu/Icons/helpers/IconFonts.h"

/**
 * @brief Canonical, untranslated IDs of the built-in menu pages.
 *
 * Page logic compares these rather than the translated labels, so it works in every locale. Pass one to
 * Menu::SelectFeatureMenu() to open that page.
 */
namespace MenuPageIds
{
	/// Alias: opens General, the page shown when nothing else is selected
	inline constexpr std::string_view kHome = "Home";
	inline constexpr std::string_view kPresets = "Presets";
	/// Alias: opens General
	inline constexpr std::string_view kSettings = "Settings";
	inline constexpr std::string_view kGeneral = "General";
	inline constexpr std::string_view kAdvanced = "Advanced";
	/// Alias: opens Advanced, which holds Profiling as a tab
	inline constexpr std::string_view kProfiling = "Profiling";
	inline constexpr std::string_view kCSEditor = "CS Editor";
	/// Feature issues; listed only while there are any
	inline constexpr std::string_view kIssues = "Issues";
	/// Alias of kIssues
	inline constexpr std::string_view kFeatureIssues = kIssues;
}

/**
 * @brief Filters of the main menu's feature list, stored as a bitmask in Menu::Settings::FeatureListFilter.
 *
 * Filters combine with AND. Without kUnloaded only loaded features are listed; with it, only the ones that are not.
 */
namespace FeatureListFilter
{
	inline constexpr std::uint32_t kFavorites = 1u << 0;   ///< Pinned features
	inline constexpr std::uint32_t kModified = 1u << 1;    ///< Settings differ from defaults and Baseline presets
	inline constexpr std::uint32_t kScene = 1u << 2;       ///< Has Scene Manager settings
	inline constexpr std::uint32_t kPreRelease = 1u << 3;  ///< Alpha or beta
	inline constexpr std::uint32_t kUnloaded = 1u << 4;    ///< Not loaded: missing, failed, or disabled at boot
	/// Every filter; a saved mask is cut to these bits.
	inline constexpr std::uint32_t kKnown = kFavorites | kModified | kScene | kPreRelease | kUnloaded;
}

/**
 * @brief Renders the feature navigation and the settings page of the main menu.
 *
 * Wide windows show a searchable, categorized navigation column beside the page.
 * Narrow windows (such as the side panel layout) collapse the navigation into a
 * popup opened from a location bar above the page.
 */
class FeatureListRenderer
{
public:
	/** @brief Describes a built-in (non-feature) menu page with an ID, a display name and a draw callback. */
	struct BuiltInMenu
	{
		std::string_view id;  ///< Canonical page ID from MenuPageIds, used for all page logic
		std::string name;     ///< Translated label shown in the list
		std::function<void()> func;
		std::string description;
	};

	/** @brief Represents a collapsible category header in the feature list. */
	struct CategoryHeader
	{
		std::string name;
		std::string label;
		int count = 0;
		bool expanded = true;
	};

	struct PinnedFeature
	{
		Feature* feature = nullptr;
	};

	/** @brief Variant type representing any entry in the menu list. */
	using MenuFuncInfo = std::variant<BuiltInMenu, std::string, CategoryHeader, Feature*, PinnedFeature>;

	/** @brief Main-menu state the list and settings panel read and report back, passed in by Menu each frame. */
	struct Hooks
	{
		/// In: moves the selection to the previous (< 0) or next (> 0) feature in the visible list, skipping
		/// category headers, labels and built-in pages. Reset to 0 once applied.
		int featureStep = 0;
		/// Called with the feature whose page is about to be drawn this frame, before its header.
		std::function<void(Feature*)> onFeatureShown;
		/// Runs a header command that rewrites a feature's settings (Restore Defaults, Use Baselines) so it can
		/// be undone. When empty the command just runs.
		std::function<void(Feature*, const std::function<void()>&)> runFeatureCommand;
	};

	/**
	 * @brief Renders the navigation and the selected page.
	 *
	 * @param selectedMenu Index of the currently selected menu item (updated on selection change).
	 * @param featureSearch Current search filter string (updated by the search input).
	 * @param focusSearch Request to move keyboard focus into the search box; cleared once handled.
	 * @param pendingFeatureSelection Feature short name or page ID (MenuPageIds) to auto-select; a page display
	 *        name is accepted as a fallback. Cleared after processing.
	 * @param categoryExpansionStates Map of category name to expanded/collapsed state.
	 * @param drawGeneralSettings Callback that renders the General settings page content.
	 * @param drawAdvancedSettings Callback that renders the Advanced settings page content.
	 * @param reservedBottomHeight Height left free below the list for the status bar.
	 * @param hooks Requests from the menu and callbacks into it; requests are reset once applied.
	 */
	static void RenderFeatureList(
		size_t& selectedMenu,
		std::string& featureSearch,
		bool& focusSearch,
		std::string& pendingFeatureSelection,
		std::map<std::string, bool>& categoryExpansionStates,
		const std::function<void()>& drawGeneralSettings,
		const std::function<void()>& drawAdvancedSettings,
		float reservedBottomHeight,
		Hooks& hooks);

	/**
	 * @brief Renders a single feature's settings page (header, action buttons and content).
	 *
	 * Used by the Advanced page to draw utility features as sub-tabs rather than as
	 * entries in the left-hand feature list.
	 *
	 * @param feat The feature whose page should be rendered.
	 */
	static void RenderFeaturePage(Feature* feat);

	/**
	 * @brief Returns the in-menu utility features, sorted by display name.
	 *
	 * These are excluded from the feature list and rendered as Advanced sub-tabs.
	 */
	static std::vector<Feature*> GetUtilityFeatures();

	/**
	 * @brief Pins a loaded feature to the top of the list, or unpins it. This is what Ctrl+P and the feature page's
	 * pin button do.
	 * @param feat A loaded feature.
	 */
	static void ToggleFavorite(Feature* feat);

	/**
	 * @brief Draws a loaded feature's settings under the baseline interceptor scope and queues new constraint warnings.
	 * @param feat A loaded feature.
	 */
	static void DrawFeatureBody(Feature* feat);

	/**
	 * @brief Draws a feature's full page (header with its action buttons, then its settings) inside a built-in page.
	 *
	 * The same content a feature list entry shows, without the surrounding frame. Navigation from the
	 * constraint warning dialog is queued through Menu::SelectFeatureMenu.
	 * @param feat Any feature, loaded or not: an unloaded one shows its boot toggle and unloaded notice.
	 */
	static void DrawEmbeddedFeaturePage(Feature* feat);

	/**
	 * @brief Runs a command that rewrites a feature's settings through the menu's undo hook (Hooks::runFeatureCommand),
	 * as the feature header's own commands do. Outside RenderFeatureList the command just runs.
	 * @param feat The feature whose settings the command changes.
	 * @param command Changes the settings.
	 */
	static void RunFeatureCommand(Feature* feat, const std::function<void()>& command);

	/**
	 * @brief Draws the reactive constraint warning modal while one is pending; call once per frame, outside child windows.
	 * @param pendingFeatureSelection Receives the short name of a feature the user navigates to from the dialog.
	 */
	static void DrawConstraintWarningDialog(std::string& pendingFeatureSelection);

	/** @brief Translated display label for a FeatureCategories value. */
	static std::string TranslateCategory(std::string_view category);

	/** @brief Width of a pre-release stage's badge; 0 for release features. */
	static float MeasureStageBadge(Feature::ReleaseStage stage);

	/** @brief Draws a pre-release stage's badge at the cursor, centred in rowHeight; nothing for release features. */
	static void DrawStageBadge(Feature::ReleaseStage stage, float rowHeight);

	/**
	 * @brief Which filters a feature matches, as FeatureListFilter bits (kUnloaded set when it is not loaded).
	 * Call State::PrimeUserEdits() first when asking for many features.
	 */
	static std::uint32_t GetFilterBits(Feature* feat);

	/**
	 * @brief Whether a feature's filter bits satisfy the active filters. Only the attribute filters (Pinned, Modified,
	 * Scene, Pre-release) are checked; the Unloaded filter picks which features are candidates and is up to the caller.
	 */
	static bool PassesFilter(std::uint32_t bits, std::uint32_t filter);

	/** @brief Accent dot in the top-right corner of a sort button while a filter is on. */
	static void DrawFilterDot(const ImVec2& buttonMin, const ImVec2& buttonMax);

	/**
	 * @brief Draws the sort and filter popup (sort order, filters, Clear filters) if `popupId` is open.
	 * @param popupId ID passed to ImGui::OpenPopup.
	 */
	static void RenderSortPopup(const char* popupId);

private:
	struct ListMenuVisitor
	{
		size_t listId;
		size_t& selectedMenuRef;
		std::map<std::string, bool>& categoryExpansionStates;
		const std::string& currentKey;
		bool selectOnNav;
		bool& selectionMade;
		const Hooks& hooks;

		void operator()(const BuiltInMenu& menu);
		void operator()(const std::string& label);
		void operator()(const CategoryHeader& header);
		void operator()(Feature* feat);
		void operator()(const PinnedFeature& pinned);

	private:
		/** @brief Marks this entry as selected, recording both its index and its stable key. */
		void Select(const std::string& entryKey);
		void DrawFeatureRow(Feature* feat, bool inPinnedSection);
		/** @brief Right-click menu of a feature row: pin, restore defaults, load at boot, copy name. */
		void RenderRowContextMenu(Feature* feat) const;
	};

	struct DrawMenuVisitor
	{
		DrawMenuVisitor(std::string& pendingFeatureSelectionRef, const Hooks& hooksRef) :
			pendingFeatureSelection(pendingFeatureSelectionRef), hooks(hooksRef) {}

		void operator()(const BuiltInMenu& menu);
		void operator()(const std::string&);
		void operator()(const CategoryHeader&);
		void operator()(Feature* feat);
		void operator()(const PinnedFeature& pinned);

		/** @brief Draws a feature's header and settings, without the frame operator() wraps them in. */
		void DrawFeatureContent(Feature* feat);

	private:
		std::string& pendingFeatureSelection;
		const Hooks& hooks;
		bool featureChanged = false;

		// Helper methods for Feature rendering
		void RenderFeatureHeader(Feature* feat, bool isDisabled, bool isLoaded);
		void RenderFeatureSettings(Feature* feat, bool isDisabled, bool isLoaded, bool hasFailedMessage);
		/** @brief Centred "disabled at boot" notice with a button that turns Load at Boot back on. */
		static void RenderDisabledAtBootNotice(Feature* feat);
		/** @brief Runs a command that rewrites the feature's settings through Hooks::runFeatureCommand. */
		void RunFeatureCommand(Feature* feat, const std::function<void()>& command) const;
	};

	static std::vector<MenuFuncInfo> BuildMenuList(
		const std::string& featureSearch,
		std::map<std::string, bool>& categoryExpansionStates,
		const std::function<void()>& drawGeneralSettings,
		const std::function<void()>& drawAdvancedSettings);

	/**
	 * @brief Re-derives the selected index from the stable selection key.
	 *
	 * The menu list is rebuilt every frame, so collapsing a category above the
	 * selection would otherwise shift the index onto a different entry.
	 */
	static void ResolveSelectionFromKey(
		const std::vector<MenuFuncInfo>& menuList,
		size_t& selectedMenu);

	static void HandlePendingFeatureSelection(
		std::string& pendingFeatureSelection,
		const std::vector<MenuFuncInfo>& menuList,
		size_t& selectedMenu);

	/** @brief True when the named feature belongs to the Utility category. */
	static bool IsUtilityFeature(const std::string& shortName);

	static bool RenderNavigation(
		const std::vector<MenuFuncInfo>& menuList,
		size_t& selectedMenu,
		std::string& featureSearch,
		bool& focusSearch,
		std::map<std::string, bool>& categoryExpansionStates,
		bool inPopup,
		const Hooks& hooks);

	static void RenderNavigatorBar(
		const std::vector<MenuFuncInfo>& menuList,
		size_t& selectedMenu,
		std::string& featureSearch,
		bool& focusSearch,
		std::map<std::string, bool>& categoryExpansionStates,
		const Hooks& hooks);

	/**
	 * @brief Draws the page at full width with the navigation as a drawer sliding over its left edge (Auto-hide Feature List).
	 *
	 * The drawer opens after the cursor rests on the left edge, stays while the cursor, a focused field or a popup is
	 * in it, and closes shortly after they leave. Its slide is time-based, so it looks the same at any frame rate.
	 */
	static void RenderAutoHidePane(
		const std::vector<MenuFuncInfo>& menuList,
		size_t& selectedMenu,
		std::string& featureSearch,
		bool& focusSearch,
		std::string& pendingFeatureSelection,
		std::map<std::string, bool>& categoryExpansionStates,
		const Hooks& hooks);

	static void RenderRightColumn(
		const std::vector<MenuFuncInfo>& menuList,
		size_t selectedMenu,
		std::string& pendingFeatureSelection,
		const Hooks& hooks);
};
