#pragma once

namespace hai {

void OverlayInit();   // call once from InitThread after hooks are installed
void OverlayTick();   // call from the 250ms loop; checks menu_key, renders nothing itself

}  // namespace hai
