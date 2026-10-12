#pragma once
#include <string_view>
#include <algorithm>
namespace itemization {
inline bool equipment_lock_menu_scope(std::string_view type,int bag,bool owned_generated_gear) {
    return type=="WBP_Menu_Dougu_Command_000_modern_C"&&bag==9&&owned_generated_gear;
}
inline const char* equipment_lock_menu_label(bool locked){return locked?"Unlock":"Lock";}
inline float equipment_lock_required_height(float panel_y,float row_y,float row_height,float original_height,float visible_bottom) {
    return panel_y+row_y+row_height+std::max(0.0f,original_height-panel_y-visible_bottom);
}
inline bool equipment_lock_row_ready(bool attached,bool native_visible,bool slate_visible,bool selectable,bool registered,bool active,bool parent_fits) {
    return attached&&native_visible&&slate_visible&&selectable&&registered&&active&&parent_fits;
}
}
