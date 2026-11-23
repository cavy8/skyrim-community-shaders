#pragma once

#include "Menu/SettingsSections.h"

#include <functional>
#include <span>
#include <string>

/**
 * @brief Renders the Advanced Settings page of the in-game menu.
 *
 * Provides tabbed sections for developer tools, shader debugging, logging
 * configuration, disable-at-boot toggles, profiling statistics, A/B testing
 * controls, and one sub-tab per Utility-category feature. The compiler controls
 * are drawn on the General page's Shaders tab.
 */
class AdvancedSettingsRenderer
{
public:
	/**
	 * @brief Renders the full Advanced Settings tab bar with all sub-sections.
	 *
	 * Draws a tab bar containing Developer, Disable at Boot, Logging, Shader
	 * Debug, Profiling and Testing tabs, followed by one tab per utility feature.
	 *
	 * @param drawDisableAtBootSettings Callback that renders the per-feature
	 *        disable-at-boot checkboxes (provided by the Menu class).
	 */
	static void RenderAdvancedSettings(
		const std::function<void()>& drawDisableAtBootSettings);

	/**
	 * @brief Queues a utility feature's sub-tab to be activated on the next Advanced render.
	 *
	 * Utility features have no entry in the feature list, so navigation requests aimed at
	 * them are routed here.
	 *
	 * @param featureShortName Short name of the utility feature to focus.
	 */
	static void RequestUtilityTab(const std::string& featureShortName);

	/** @brief Searchable setting labels of the sections drawn here. */
	static std::span<const SettingsSearchEntry> GetSearchIndex();

	/** @brief Compiler thread counts and Dump Shaders, drawn on the General page's Shaders tab. */
	static void RenderCompilerSection();
	/** @brief Log level, shader defines, and the log and ini dump buttons. */
	static void RenderLoggingSection();
	/** @brief Shader replacement toggles and the shader blocking switch. */
	static void RenderShaderDebugSection();
	/** @brief The shaders used in recent frames, with the blocked shader's details; a row click blocks that shader. */
	static void RenderActiveShadersSection();
	/** @brief Developer mode, file watcher, compiler flags and build statistics. */
	static void RenderDeveloperSection();
	/** @brief A/B testing controls. */
	static void RenderTestingSection();

	/**
	 * @brief Puts the player in a known test scene: fast, invulnerable, no collision, time paused at noon, in Whiterun
	 * under a forced weather. Does nothing unless a game is loaded and a menu is open (the console needs both).
	 */
	static void RunTestConditions();

	/** @brief Tooltip for the Test Conditions button, listing the console commands it runs. */
	static const char* GetTestConditionsTooltip();

	/**
	 * @brief Stills the world in place for development and agents: fast, no AI or collision, time paused at noon,
	 * forced weather, idle camera spin off. Unlike Test Conditions it does not teleport the player.
	 */
	static void RunDevelopmentConditions();

	/** @brief Tooltip for the Development Conditions button, listing what it sets. */
	static const char* GetDevelopmentConditionsTooltip();

	/** @brief Clears the once-per-game state, so the next Development Conditions run reapplies tai and tcl. Call on game load and reset. */
	static void ResetDevelopmentConditionsState();

private:
	static void RenderUtilityFeatureTabs();
	static void RenderDisableAtBootSection(const std::function<void()>& drawDisableAtBootSettings);
};
