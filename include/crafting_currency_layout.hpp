#pragma once
#include "crafting_screen.hpp"
namespace itemization {
struct CraftCurrencySection {bool visible=false,eligible=false;float y=0;};
struct CraftCurrencyLayout {
    size_t first=0;float row_height=68,icon_size=62,selected_y=236;
    std::array<float,craft_currency_rows> row_y{};
    std::array<CraftCurrencySection,2> sections{};
};
// Section labels occupy presentation space only. Logical choice indices and
// confirmation eligibility remain untouched, so a separator cannot take focus.
inline CraftCurrencyLayout craft_currency_layout(const std::vector<CraftScreenRow>& rows,size_t cursor,bool grouped) {
    CraftCurrencyLayout out;
    cursor=rows.empty()?0:std::min(cursor,rows.size()-1);
    out.first=(cursor/craft_currency_rows)*craft_currency_rows;
    float y=236;
    if(grouped&&!rows.empty()) {
        out.row_height=60;out.icon_size=54;
        out.sections[0]={true,rows[out.first].available,y};y+=32;
    }
    for(size_t i=0;i<craft_currency_rows;++i) {
        const size_t index=out.first+i;
        if(grouped&&i&&index<rows.size()&&rows[index-1].available&&!rows[index].available) {
            out.sections[1]={true,false,y};y+=32;
        }
        out.row_y[i]=y;y+=out.row_height;
    }
    out.selected_y=out.row_y[cursor%craft_currency_rows];
    return out;
}
}
