#pragma once

#include <string>
#include <unordered_set>

/** @brief Parses plain-text allow/deny list files used by features to match objects by name or form ID. */
struct FormIdParser
{
	/** @brief Trims leading/trailing whitespace from a string. */
	static std::string trim(const std::string& str);
	/** @brief Parses a text file of numbers in hexadecimal format into a set. One number per line. A # symbol can be used for one-line comments. */
	static std::unordered_set<std::uint32_t> parseHexFile(const std::filesystem::path&);
	/** @brief Parses a text file of any line of text without surrounding whitespace into a set. Used to parse Trishape node names. A # symbol can be used for one-line comments. */
	static std::unordered_set<std::uint64_t> parseTriNameFile(const std::filesystem::path&);
	/** @brief Computes the FNV-1a hash of a null-terminated string. */
	static std::uint64_t fnv_hash(const char* key);
};
