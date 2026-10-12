#pragma once
#include <string_view>
namespace itemization {
enum class ForgeHomeVariant {None,Standard,RecentRecipes};
inline ForgeHomeVariant forge_home_variant(std::wstring_view script) {
    // Match the complete English forge choice block, never an ordinary camp
    // menu, story mention of the forge, or a recipe/quality confirmation.
    if(script.size()>4096 || script.find(L"It's the Fun-Size Forge!")==script.npos ||
       script.find(L"<select>")==script.npos || script.find(L"<select_end>")==script.npos ||
       script.find(L"Forge Weapons")==script.npos || script.find(L"Forge Armour")==script.npos ||
       script.find(L"Forge Accessories")==script.npos ||
       (script.find(L"Reforge Quality")==script.npos && script.find(L"Craft with Veyls")==script.npos && script.find(L"Rework an Item")==script.npos))return ForgeHomeVariant::None;
    return script.find(L"Forge Recently Learned Recipes")!=script.npos?ForgeHomeVariant::RecentRecipes:ForgeHomeVariant::Standard;
}
}
