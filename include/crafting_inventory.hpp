#pragma once
#include <algorithm>
#include <array>
#include <cstdlib>
#include <string>
#include <vector>
namespace itemization {
constexpr int salvage_equipment_bag=9; // EJackItem_Bag::Bag_Equipment.
inline bool salvage_bag_source(int bag,int count,int region) {
    return bag==salvage_equipment_bag && count==1 && region==0;
}
struct CraftInventorySlot {std::string id,name;int character=-1,region=0;bool craftable=false;std::string rarity="normal";};
struct CraftEquipmentAssignment {std::string id;int character=-1,region=0;};
// Equipped positions come from the character's equipment list. Bag entries
// supply ownership and presentation, but their cached region is not authoritative.
inline std::vector<CraftInventorySlot> crafting_inventory_slots(const std::vector<CraftInventorySlot>& owned,
    const std::vector<CraftEquipmentAssignment>& assignments) {
    std::vector<CraftInventorySlot> result;std::vector<bool> equipped(owned.size(),false);
    for(const auto& a:assignments) {
        if(a.character<0||a.character>=8||a.region<1||a.region>6||a.id.empty())continue;
        for(size_t i=0;i<owned.size();++i)if(owned[i].character==a.character&&owned[i].id==a.id) {
            auto slot=owned[i];slot.region=a.region;result.push_back(std::move(slot));equipped[i]=true;break;
        }
    }
    for(size_t i=0;i<owned.size();++i)if(!equipped[i]){auto slot=owned[i];slot.region=0;result.push_back(std::move(slot));}
    return result;
}
// Roster is supplied by the native current-party accessor, never by save-file
// character objects or a record of everyone who has joined at some earlier time.
class CraftInventoryGrid {
    std::vector<int> party_;std::vector<CraftInventorySlot> bag_;
    std::array<std::array<CraftInventorySlot,6>,8> equipped_{};
    size_t column_=0,row_=0,bag_row_=0,bag_first_=0;
    int nearest_row(size_t column,size_t row) const {
        if(column==party_.size())return bag_.empty()?-1:static_cast<int>(std::min(bag_first_+row,bag_.size()-1));
        int nearest=-1,distance=7;
        for(int r=0;r<6;++r)if(!equipped_[party_[column]][r].id.empty()) {
            const int d=std::abs(r-static_cast<int>(row));if(d<distance){distance=d;nearest=r;}
        }
        return nearest;
    }
    void select_at(size_t column,int row) {column_=column;if(in_bag())bag_row_=static_cast<size_t>(row);else row_=static_cast<size_t>(row);reveal();}
    void select_occupied() {
        const size_t anchor=visible_row();
        if(const int r=nearest_row(column_,anchor);r>=0){select_at(column_,r);return;}
        for(size_t c=0;c<=party_.size();++c)if(const int r=nearest_row(c,anchor);r>=0){select_at(c,r);return;}
    }
public:
    void refresh(std::vector<int> party,const std::vector<CraftInventorySlot>& slots,const std::string& preferred={}) {
        party_.clear();for(int c:party)if(c>=0&&c<8&&std::find(party_.begin(),party_.end(),c)==party_.end())party_.push_back(c);
        bag_.clear();equipped_={};
        for(int c:party_)for(int r=0;r<6;++r) {equipped_[c][r].character=c;equipped_[c][r].region=r+1;}
        for(const auto& s:slots) {
            if(s.character>=0&&std::find(party_.begin(),party_.end(),s.character)==party_.end())continue;
            if(s.region>=1&&s.region<=6&&s.character>=0)equipped_[s.character][s.region-1]=s;
            else if(!s.id.empty())bag_.push_back(s);
        }
        std::stable_sort(bag_.begin(),bag_.end(),[](const auto& a,const auto& b){return a.name==b.name?a.id<b.id:a.name<b.name;});
        column_=std::min(column_,party_.size());row_=std::min(row_,size_t(5));
        bag_row_=bag_.empty()?0:std::min(bag_row_,bag_.size()-1);
        if(!preferred.empty()) {
            for(size_t c=0;c<party_.size();++c)for(size_t r=0;r<6;++r)if(equipped_[party_[c]][r].id==preferred){column_=c;row_=r;}
            for(size_t r=0;r<bag_.size();++r)if(bag_[r].id==preferred){column_=party_.size();bag_row_=r;}
        }reveal();select_occupied();
    }
    void reveal() {if(bag_row_<bag_first_)bag_first_=bag_row_;if(bag_row_>=bag_first_+6)bag_first_=bag_row_-5;
        bag_first_=std::min(bag_first_,bag_.size()>6?bag_.size()-6:0);}
    void vertical(int direction,bool page=false) {
        if(in_bag()) {if(!bag_.empty())bag_row_=static_cast<size_t>(std::clamp(static_cast<int>(bag_row_)+direction*(page?6:1),0,static_cast<int>(bag_.size())-1));reveal();return;}
        if(!direction)return;
        if(page) {for(int r=direction>0?5:0;r>=0&&r<6;r+=direction>0?-1:1)
            if(!equipped_[party_[column_]][r].id.empty()&&((direction>0&&r>static_cast<int>(row_))||(direction<0&&r<static_cast<int>(row_)))){row_=r;return;}}
        else for(int r=std::clamp(static_cast<int>(row_)+direction,0,5);r>=0&&r<6;r+=direction>0?1:-1)
            if(!equipped_[party_[column_]][r].id.empty()){row_=r;return;}
    }
    // Horizontal movement stays on the same visible row. Skip empty cells,
    // including empty character columns; never move vertically as a fallback.
    bool horizontal(int direction) {if(direction>0&&column_==party_.size())return true;
        const size_t visible_row=in_bag()?bag_row_-bag_first_:row_;
        if(!direction)return false;
        for(int c=static_cast<int>(column_)+(direction>0?1:-1);c>=0&&c<=static_cast<int>(party_.size());c+=direction>0?1:-1)
            if(!at(static_cast<size_t>(c),visible_row).id.empty()) {
                select_at(static_cast<size_t>(c),static_cast<int>(c==static_cast<int>(party_.size())?bag_first_+visible_row:visible_row));return false;
            }
        return false;}
    bool in_bag() const {return column_==party_.size();}
    size_t column() const{return column_;}size_t visible_row() const{return in_bag()?bag_row_-bag_first_:row_;}
    size_t bag_first() const{return bag_first_;}size_t bag_cursor() const{return bag_row_;}
    const std::vector<int>& party() const{return party_;}const std::vector<CraftInventorySlot>& bag() const{return bag_;}
    const CraftInventorySlot& at(size_t column,size_t row) const {static const CraftInventorySlot empty{};
        if(column<party_.size())return equipped_[party_[column]].at(row);
        return bag_first_+row<bag_.size()?bag_[bag_first_+row]:empty;}
    const CraftInventorySlot& selected() const {return at(column_,visible_row());}
};
}
