// SPDX-License-Identifier: GPL-3.0-or-later
#include "font_resolve.hpp"

#include <fontconfig/fontconfig.h>

#include <strings.h>

namespace lterm {

std::vector<std::string> splitFamilies(const std::string& list) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i <= list.size()) {
        size_t j = list.find(',', i);
        if (j == std::string::npos) j = list.size();
        std::string s = list.substr(i, j - i);
        auto b = s.find_first_not_of(" \t"), e = s.find_last_not_of(" \t");
        if (b != std::string::npos) out.push_back(s.substr(b, e - b + 1));
        i = j + 1;
    }
    return out;
}

namespace {

bool isGeneric(const std::string& f) {
    static const char* g[] = {"monospace", "mono", "sans", "sans-serif", "serif", "system-ui"};
    for (const char* x : g)
        if (strcasecmp(f.c_str(), x) == 0) return true;
    return false;
}

// Matches exactly like Pango would and inspects what fontconfig actually returned.
bool matchFamily(const std::string& family, bool* mono) {
    FcPattern* pat = FcPatternCreate();
    FcPatternAddString(pat, FC_FAMILY, reinterpret_cast<const FcChar8*>(family.c_str()));
    FcConfigSubstitute(nullptr, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);
    FcResult r;
    FcPattern* m = FcFontMatch(nullptr, pat, &r);
    bool found = false;
    if (m) {
        FcChar8* name = nullptr;
        for (int i = 0; FcPatternGetString(m, FC_FAMILY, i, &name) == FcResultMatch; ++i)
            if (strcasecmp(reinterpret_cast<const char*>(name), family.c_str()) == 0) found = true;
        if (mono) {
            // fontconfig sets spacing only for fixed-pitch fonts; no value = proportional.
            int sp = FC_PROPORTIONAL;
            *mono = FcPatternGetInteger(m, FC_SPACING, 0, &sp) == FcResultMatch && sp >= FC_DUAL;
        }
        FcPatternDestroy(m);
    }
    FcPatternDestroy(pat);
    return found;
}

}  // namespace

bool familyInstalled(const std::string& family) {
    if (family.empty()) return false;
    if (isGeneric(family)) return true;
    return matchFamily(family, nullptr);
}

FontPick pickFont(const std::vector<FontChoice>& choices) {
    FontPick p;
    bool first = true;
    for (const auto& c : choices) {
        bool mono = true;
        const bool hit = matchFamily(c.family, &mono);
        if (hit || isGeneric(c.family)) {
            p.family = c.family;
            p.px = c.px;
            p.fallback = !first;
            p.monospace = mono;
            return p;
        }
        first = false;
    }
    p.family = "Monospace";
    p.fallback = true;
    matchFamily(p.family, &p.monospace);
    return p;
}

FontPick pickFont(const std::string& familyList, int px) {
    std::vector<FontChoice> v;
    for (auto& f : splitFamilies(familyList)) v.push_back(FontChoice{f, px});
    return pickFont(v);
}

}  // namespace lterm
