#pragma once
#ifdef __cplusplus
extern "C" {
#endif
// Once per presented frame while online: reads the local player's body and passes it to net_set_self (puppet_self.c).
void puppet_self_frame(void);
#ifdef __cplusplus
}
#endif
