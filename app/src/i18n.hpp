#pragma once

// The UI's language. Texts are written in Turkish in the code and wrapped: tr("Cihazlar") gives
// "Devices" when the language is English (from the table in i18n.cpp), the Turkish itself
// otherwise or when the table has no entry. trf fills a text's {} with values in order:
// trf("{} cihazına gönder", name). The language is a Kompoz state: a composable reading a text
// is drawn again when it changes.

#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace airlink {

// "system" follows the system's language (Turkish, else English); "tr" and "en" pin one.
std::string languageSetting();
void setLanguageSetting(const std::string& setting);
// Where the setting is kept (AirLink's data folder; empty: not kept); reads it now.
void loadLanguageSetting(const std::string& path);
bool english();  // the language in effect

std::string tr(std::string_view turkish);

namespace detail {
template <class T>
std::string text(const T& v) {
    if constexpr (std::is_same_v<T, bool>) return v ? "1" : "0";
    else if constexpr (std::is_arithmetic_v<T>) return std::to_string(v);
    else return std::string(v);
}
std::string fill(const std::string& pattern, const std::vector<std::string>& values);
}  // namespace detail

template <class... A>
std::string trf(std::string_view turkish, const A&... values) {
    return detail::fill(tr(turkish), {detail::text(values)...});
}

}  // namespace airlink
