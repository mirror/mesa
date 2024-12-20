#include "../src/nouveau/vulkan/video/video_bindings.h"

// Static wrappers

void * __vk_find_struct_nvk_video(void *start, VkStructureType sType) { return __vk_find_struct(start, sType); }
struct nvk_device_memory * nvk_device_memory_from_handle_nvk_video(VkDeviceMemory _handle) { return nvk_device_memory_from_handle(_handle); }
VkDeviceMemory nvk_device_memory_to_handle_nvk_video(struct nvk_device_memory *_obj) { return nvk_device_memory_to_handle(_obj); }
struct nvk_buffer * nvk_buffer_from_handle_nvk_video(VkBuffer _handle) { return nvk_buffer_from_handle(_handle); }
VkBuffer nvk_buffer_to_handle_nvk_video(struct nvk_buffer *_obj) { return nvk_buffer_to_handle(_obj); }
uint64_t nvk_buffer_address_nvk_video(const struct nvk_buffer *buffer, uint64_t offset) { return nvk_buffer_address(buffer, offset); }
struct nvk_addr_range nvk_buffer_addr_range_nvk_video(const struct nvk_buffer *buffer, uint64_t offset, uint64_t range) { return nvk_buffer_addr_range(buffer, offset, range); }
struct nvk_image_view * nvk_image_view_from_handle_nvk_video(VkImageView _handle) { return nvk_image_view_from_handle(_handle); }
VkImageView nvk_image_view_to_handle_nvk_video(struct nvk_image_view *_obj) { return nvk_image_view_to_handle(_obj); }
struct nvk_image * nvk_image_from_handle_nvk_video(VkImage _handle) { return nvk_image_from_handle(_handle); }
VkImage nvk_image_to_handle_nvk_video(struct nvk_image *_obj) { return nvk_image_to_handle(_obj); }
uint64_t nvk_image_plane_base_address_nvk_video(const struct nvk_image_plane *plane) { return nvk_image_plane_base_address(plane); }
uint64_t nvk_image_base_address_nvk_video(const struct nvk_image *image, uint8_t plane) { return nvk_image_base_address(image, plane); }
uint8_t nvk_image_aspects_to_plane_nvk_video(const struct nvk_image *image, VkImageAspectFlags aspectMask) { return nvk_image_aspects_to_plane(image, aspectMask); }
uint8_t nvk_image_memory_aspects_to_plane_nvk_video(const struct nvk_image *image, VkImageAspectFlags aspectMask) { return nvk_image_memory_aspects_to_plane(image, aspectMask); }
struct nvk_cmd_pool * nvk_cmd_pool_from_handle_nvk_video(VkCommandPool _handle) { return nvk_cmd_pool_from_handle(_handle); }
VkCommandPool nvk_cmd_pool_to_handle_nvk_video(struct nvk_cmd_pool *_obj) { return nvk_cmd_pool_to_handle(_obj); }
struct nvk_device * nvk_cmd_pool_device_nvk_video(struct nvk_cmd_pool *pool) { return nvk_cmd_pool_device(pool); }
uint32_t nvk_min_cbuf_alignment_nvk_video(const struct nv_device_info *info) { return nvk_min_cbuf_alignment(info); }
struct nvk_physical_device * nvk_physical_device_from_handle_nvk_video(VkPhysicalDevice _handle) { return nvk_physical_device_from_handle(_handle); }
VkPhysicalDevice nvk_physical_device_to_handle_nvk_video(struct nvk_physical_device *_obj) { return nvk_physical_device_to_handle(_obj); }
uint32_t nvk_use_edb_buffer_views_nvk_video(const struct nvk_physical_device *pdev) { return nvk_use_edb_buffer_views(pdev); }
struct nvk_instance * nvk_physical_device_instance_nvk_video(struct nvk_physical_device *pdev) { return nvk_physical_device_instance(pdev); }
bool nvk_use_bindless_cbuf_nvk_video(const struct nv_device_info *info) { return nvk_use_bindless_cbuf(info); }
struct nvk_buffer_address nvk_ubo_descriptor_addr_nvk_video(const struct nvk_physical_device *pdev, union nvk_buffer_descriptor desc) { return nvk_ubo_descriptor_addr(pdev, desc); }
void nvkmd_pdev_destroy_nvk_video(struct nvkmd_pdev *pdev) { nvkmd_pdev_destroy(pdev); }
uint64_t nvkmd_pdev_get_vram_used_nvk_video(struct nvkmd_pdev *pdev) { return nvkmd_pdev_get_vram_used(pdev); }
int nvkmd_pdev_get_drm_primary_fd_nvk_video(struct nvkmd_pdev *pdev) { return nvkmd_pdev_get_drm_primary_fd(pdev); }
VkResult nvkmd_pdev_create_dev_nvk_video(struct nvkmd_pdev *pdev, struct vk_object_base *log_obj, struct nvkmd_dev **dev_out) { return nvkmd_pdev_create_dev(pdev, log_obj, dev_out); }
void nvkmd_dev_destroy_nvk_video(struct nvkmd_dev *dev) { nvkmd_dev_destroy(dev); }
uint64_t nvkmd_dev_get_gpu_timestamp_nvk_video(struct nvkmd_dev *dev) { return nvkmd_dev_get_gpu_timestamp(dev); }
int nvkmd_dev_get_drm_fd_nvk_video(struct nvkmd_dev *dev) { return nvkmd_dev_get_drm_fd(dev); }
VkResult nvkmd_dev_alloc_mem_nvk_video(struct nvkmd_dev *dev, struct vk_object_base *log_obj, uint64_t size_B, uint64_t align_B, enum nvkmd_mem_flags flags, struct nvkmd_mem **mem_out) { return nvkmd_dev_alloc_mem(dev, log_obj, size_B, align_B, flags, mem_out); }
VkResult nvkmd_dev_alloc_tiled_mem_nvk_video(struct nvkmd_dev *dev, struct vk_object_base *log_obj, uint64_t size_B, uint64_t align_B, uint8_t pte_kind, uint16_t tile_mode, enum nvkmd_mem_flags flags, struct nvkmd_mem **mem_out) { return nvkmd_dev_alloc_tiled_mem(dev, log_obj, size_B, align_B, pte_kind, tile_mode, flags, mem_out); }
VkResult nvkmd_dev_import_dma_buf_nvk_video(struct nvkmd_dev *dev, struct vk_object_base *log_obj, int fd, struct nvkmd_mem **mem_out) { return nvkmd_dev_import_dma_buf(dev, log_obj, fd, mem_out); }
VkResult nvkmd_dev_create_ctx_nvk_video(struct nvkmd_dev *dev, struct vk_object_base *log_obj, enum nvkmd_engines engines, struct nvkmd_ctx **ctx_out) { return nvkmd_dev_create_ctx(dev, log_obj, engines, ctx_out); }
struct nvkmd_mem * nvkmd_mem_ref_nvk_video(struct nvkmd_mem *mem) { return nvkmd_mem_ref(mem); }
VkResult nvkmd_mem_overmap_nvk_video(struct nvkmd_mem *mem, struct vk_object_base *log_obj, enum nvkmd_mem_map_flags flags) { return nvkmd_mem_overmap(mem, log_obj, flags); }
VkResult nvkmd_mem_export_dma_buf_nvk_video(struct nvkmd_mem *mem, struct vk_object_base *log_obj, int *fd_out) { return nvkmd_mem_export_dma_buf(mem, log_obj, fd_out); }
void nvkmd_ctx_destroy_nvk_video(struct nvkmd_ctx *ctx) { nvkmd_ctx_destroy(ctx); }
VkResult nvkmd_ctx_wait_nvk_video(struct nvkmd_ctx *ctx, struct vk_object_base *log_obj, uint32_t wait_count, const struct vk_sync_wait *waits) { return nvkmd_ctx_wait(ctx, log_obj, wait_count, waits); }
VkResult nvkmd_ctx_exec_nvk_video(struct nvkmd_ctx *ctx, struct vk_object_base *log_obj, uint32_t exec_count, const struct nvkmd_ctx_exec *execs) { return nvkmd_ctx_exec(ctx, log_obj, exec_count, execs); }
VkResult nvkmd_ctx_signal_nvk_video(struct nvkmd_ctx *ctx, struct vk_object_base *log_obj, uint32_t signal_count, const struct vk_sync_signal *signals) { return nvkmd_ctx_signal(ctx, log_obj, signal_count, signals); }
VkResult nvkmd_ctx_flush_nvk_video(struct nvkmd_ctx *ctx, struct vk_object_base *log_obj) { return nvkmd_ctx_flush(ctx, log_obj); }
VkResult nvkmd_ctx_sync_nvk_video(struct nvkmd_ctx *ctx, struct vk_object_base *log_obj) { return nvkmd_ctx_sync(ctx, log_obj); }
struct nvkmd_mem * nvk_descriptor_table_get_mem_ref_nvk_video(struct nvk_descriptor_table *table, uint32_t *alloc_count_out) { return nvk_descriptor_table_get_mem_ref(table, alloc_count_out); }
uint64_t nvk_heap_contiguous_base_address_nvk_video(struct nvk_heap *heap) { return nvk_heap_contiguous_base_address(heap); }
struct nvk_device * nvk_queue_device_nvk_video(struct nvk_queue *queue) { return nvk_queue_device(queue); }
struct nvk_device * nvk_device_from_handle_nvk_video(VkDevice _handle) { return nvk_device_from_handle(_handle); }
VkDevice nvk_device_to_handle_nvk_video(struct nvk_device *_obj) { return nvk_device_to_handle(_obj); }
struct nvk_physical_device * nvk_device_physical_nvk_video(struct nvk_device *dev) { return nvk_device_physical(dev); }
struct nvk_descriptor_pool * nvk_descriptor_pool_from_handle_nvk_video(VkDescriptorPool _handle) { return nvk_descriptor_pool_from_handle(_handle); }
VkDescriptorPool nvk_descriptor_pool_to_handle_nvk_video(struct nvk_descriptor_pool *_obj) { return nvk_descriptor_pool_to_handle(_obj); }
struct nvk_descriptor_set * nvk_descriptor_set_from_handle_nvk_video(VkDescriptorSet _handle) { return nvk_descriptor_set_from_handle(_handle); }
VkDescriptorSet nvk_descriptor_set_to_handle_nvk_video(struct nvk_descriptor_set *_obj) { return nvk_descriptor_set_to_handle(_obj); }
struct nvk_buffer_address nvk_descriptor_set_addr_nvk_video(const struct nvk_descriptor_set *set) { return nvk_descriptor_set_addr(set); }
gl_shader_stage nvk_last_vtgm_shader_stage_nvk_video(VkShaderStageFlags stages) { return nvk_last_vtgm_shader_stage(stages); }
uint32_t nvk_cbuf_binding_for_stage_nvk_video(gl_shader_stage stage) { return nvk_cbuf_binding_for_stage(stage); }
struct nvk_shader * nvk_shader_from_handle_nvk_video(VkShaderEXT _handle) { return nvk_shader_from_handle(_handle); }
VkShaderEXT nvk_shader_to_handle_nvk_video(struct nvk_shader *_obj) { return nvk_shader_to_handle(_obj); }
struct nvk_cmd_buffer * nvk_cmd_buffer_from_handle_nvk_video(VkCommandBuffer _handle) { return nvk_cmd_buffer_from_handle(_handle); }
VkCommandBuffer nvk_cmd_buffer_to_handle_nvk_video(struct nvk_cmd_buffer *_obj) { return nvk_cmd_buffer_to_handle(_obj); }
struct nvk_device * nvk_cmd_buffer_device_nvk_video(struct nvk_cmd_buffer *cmd) { return nvk_cmd_buffer_device(cmd); }
struct nvk_cmd_pool * nvk_cmd_buffer_pool_nvk_video(struct nvk_cmd_buffer *cmd) { return nvk_cmd_buffer_pool(cmd); }
struct nv_push * nvk_cmd_buffer_push_nvk_video(struct nvk_cmd_buffer *cmd, uint32_t dw_count) { return nvk_cmd_buffer_push(cmd, dw_count); }
struct nvk_descriptor_state * nvk_get_descriptors_state_nvk_video(struct nvk_cmd_buffer *cmd, VkPipelineBindPoint bind_point) { return nvk_get_descriptors_state(cmd, bind_point); }
struct nvk_descriptor_state * nvk_get_descriptor_state_for_stages_nvk_video(struct nvk_cmd_buffer *cmd, VkShaderStageFlags stages) { return nvk_get_descriptor_state_for_stages(cmd, stages); }
struct nvk_video_session * nvk_video_session_from_handle_nvk_video(VkVideoSessionKHR _handle) { return nvk_video_session_from_handle(_handle); }
VkVideoSessionKHR nvk_video_session_to_handle_nvk_video(struct nvk_video_session *_obj) { return nvk_video_session_to_handle(_obj); }
struct nvk_video_session_params * nvk_video_session_params_from_handle_nvk_video(VkVideoSessionParametersKHR _handle) { return nvk_video_session_params_from_handle(_handle); }
VkVideoSessionParametersKHR nvk_video_session_params_to_handle_nvk_video(struct nvk_video_session_params *_obj) { return nvk_video_session_params_to_handle(_obj); }
