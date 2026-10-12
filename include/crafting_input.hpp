#pragma once
namespace itemization {
// Jack delivers physical buttons and their resolved menu commands separately.
// Only commands own navigation; shoulders have no logical menu equivalent.
inline bool crafting_input_action(int key,bool pressed,bool repeated) {
    const bool command=key>=23&&key<=29;
    const bool shoulder=key==8||key==9||key==11||key==12;
    const bool repeatable=(key>=23&&key<=26)||key==9||key==12;
    return (command||shoulder)&&(pressed||(repeated&&repeatable));
}
}
