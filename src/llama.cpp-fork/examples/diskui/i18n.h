// i18n: tiny table driven localisation for the diskinfer TUI / engine logs
//
// supported: en (default fallback), zh-Hans (simplified), zh-Hant (traditional)
//
// language selection order (contract v5 §1.1):
//   CLI --lang <auto|en|zh|zh-Hans|zh-Hant|zh-CN|zh-TW>
//   > environment DISKINFER_LANG
//   > platform system locale (Android persist.sys.locale/ro.product.locale,
//     Linux LC_ALL/LC_MESSAGES/LANG + /etc/default/locale, Windows
//     GetUserDefaultLocaleName)
//
// pure const char* tables + std::string, no gettext and no extra dependencies.
#pragma once

#include <string>

namespace i18n {

enum class Lang { En, ZhHans, ZhHant };

// "zh-CN", "zh_TW.UTF-8", "en_US", "auto" -> Lang (unknown -> En)
Lang from_tag(const char * tag);

// environment (DISKINFER_LANG) + platform system locale, no CLI
Lang detect();

// parse --lang from argv (and remove it from the argv vector, trailing entries
// are set to nullptr), env next, system locale last. call before any UI text.
void init(int argc, char ** argv);

Lang current();
const char * lang_id();            // "en" / "zh-Hans" / "zh-Hant"

const char * tr(const char * key); // missing key -> key itself (en fallback)
std::string tr(const std::string & key);

// printf style convenience: i18n::trf("set.ctx_max_fmt", 16, 24, "1.0 GiB")
// the format string is the translated entry, so placeholders move with it
std::string trf(const char * key, ...);

} // namespace i18n
