// A passive wallet beside the native equipment list. Native widgets retain
// keyboard/controller focus; the wallet reuses the game's font and updates only
// on a balance/mode change, at most ten times a second.
struct CampWalletUI {FeedbackWeak owner,card,text;std::string previous;};
std::map<std::pair<int,int>,CampWalletUI> camp_wallet_uis;
uintptr_t camp_find_widget(uintptr_t node,const char* wanted,int& budget) {
    if(!node || --budget<0) return 0;
    if(feedback_is_a(node,wanted)) return node;
    if(feedback_is_a(node,"UserWidget")) {const auto t=read<uintptr_t>(node+0x168);return t?camp_find_widget(read<uintptr_t>(t+0x38),wanted,budget):0;}
    if(!feedback_is_a(node,"PanelWidget")) return 0;
    const auto slots=read<Array>(node+0xC8);if(slots.count<0 || slots.count>256 || slots.capacity<slots.count) return 0;
    for(int i=0;i<slots.count;++i) {const auto s=read<uintptr_t>(slots.data+uintptr_t(i)*8);if(s) if(const auto r=camp_find_widget(read<uintptr_t>(s+0x40),wanted,budget)) return r;}
    return 0;
}
void camp_wallet_update(uintptr_t accessor) noexcept {
    // The wallet belongs to the independent crafting screen now.
    (void)accessor;return;
    if(!camp_policy || !runtime_enabled.load()) return;
    try {
        std::lock_guard<std::recursive_mutex> lock(runtime_mutex);
        static uint64_t last=0;const auto now=GetTickCount64();if(last && now-last<100) return;last=now;
        static std::set<uintptr_t> verified;
        const auto cls=read<uintptr_t>(accessor+0x10);
        if(!verified.count(cls)) {feedback_require_property(cls,"FusigiKaziParts",0x90);verified.insert(cls);}
        // The accessor owns exactly 22 forge parts. Resolve the list directly,
        // so the wallet is present while browsing, before any prompt opens.
        uintptr_t owner=0;
        for(int i=0;i<22;++i) {const auto part=read<uintptr_t>(accessor+0x90+uintptr_t(i)*8);
            if(part && feedback_is_a(part,"WBP_Menu_FusigiKazi_Entrance_List_010_modern_C")) {owner=part;break;}}
        if(!owner) return;const auto previous=FeedbackWeak::from(owner);
        const auto key=std::make_pair(previous.index,previous.serial);
        auto existing=camp_wallet_uis.find(key);
        if(existing==camp_wallet_uis.end()) {
            const auto tree=read<uintptr_t>(owner+0x168);if(!tree) return;const auto root=read<uintptr_t>(tree+0x38);
            int budget=1024;const auto font=feedback_find_text(root,budget);budget=1024;
            const auto canvas=camp_find_widget(root,"CanvasPanel",budget);budget=1024;
            const auto image=camp_find_widget(root,"Image",budget);
            if(!font || !canvas || !image) return;
            if(feedback_functions.empty()) feedback_bind_umg(font,image,canvas);
            const auto card=feedback_widget(tree,feedback_canvas_class,canvas,"ARPG_CraftingWallet");
            const auto background=feedback_image(tree,card,"ARPG_CraftingWalletBackground",{0.06f,0.035f,0.025f,0.70f});
            const auto text=feedback_text(tree,card,"ARPG_CraftingWalletText",font,21);uint8_t left=0;
            feedback_event(text,"TextBlock.SetJustification",&left);
            // Below the character selector, including a full eight-person party;
            // stays left of the native bottom prompt and equipment rows.
            feedback_rect(card,42,670,335,302);feedback_rect(background,0,0,335,302);feedback_rect(text,14,10,307,282);
            existing=camp_wallet_uis.emplace(key,CampWalletUI{previous,FeedbackWeak::from(card),FeedbackWeak::from(text),{}}).first;
            for(auto it=camp_wallet_uis.begin();it!=camp_wallet_uis.end();) {if(!it->second.owner.get()) it=camp_wallet_uis.erase(it);else ++it;}
            log({{"event","campsite_wallet_created"},{"owner",previous.index},{"native_font",true}});
        }
        auto& ui=existing->second;const auto card=ui.card.get(),text=ui.text.get();if(!card || !text) return;
        feedback_visibility(card,camp_mode==itemization::CampMode::Quality?1:3);
        if(camp_mode==itemization::CampMode::Quality) return;
        const auto manager=find_manager();const auto wallet=veyl_wallet(manager),shards=shard_wallet(manager);
        std::string display="Veyls owned\n";
        for(const auto& c:veyl_policy->currencies()) {
            if(!veyl_policy->visible(c.at("id"),wallet))continue;
            const auto name=itemization::veyl_short_name(c.at("name"));
            display+=name+": "+std::to_string(wallet.at(c.at("id").get<std::string>()).get<int>())+"\n";
        }
        int count=0,types=0;for(const auto& s:shards) {const int n=s.get<int>();count+=n;if(n)++types;}
        display+="Shards: "+std::to_string(count)+" ("+std::to_string(types)+" types)";
        if(ui.previous!=display) {feedback_set_text(text,display);ui.previous=display;}
    } catch(const std::exception& error) {
        static std::set<std::string> reported;if(reported.insert(error.what()).second) log({{"event","campsite_wallet_unavailable"},{"reason",error.what()}});
    }
}
