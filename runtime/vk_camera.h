// The scene camera for overlays (vk_camera.c): the view-projection of the frame's scene, readable from any thread.
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct VkExec VkExec;
// vk_exec_draw.c, each draw beside vk_dlaa_draw: notes the scene camera block of a scene draw.
void vk_camera_draw(VkExec *ex);
// The newest scene view-projection, 16 floats row by row: clip = M * (x, y, z, 1) with x, y, z in the game's world
// units, the convention vk_dlaa.c mv_pass reprojects with. *age_ns: how long ago a scene draw last carried it. 0 when
// no scene was drawn yet.
int vk_camera_viewproj(float m[16], uint64_t *age_ns);
#ifdef __cplusplus
}
#endif
