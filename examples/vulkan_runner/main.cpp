// Headless Vulkan test harness: compiles a .mim shader (via the `mim` CLI +
// `spirv-as`) and renders it offscreen into a PPM image, in one of two modes:
//   --mode=triangle  no vertex buffers, just a hardcoded 3-vertex draw call
//                    (matches lit/spirv/triangle.mim's shape).
//   --mode=mesh      real vertex/index buffers (loaded from an OBJ file) +
//                    a push-constant camera matrix (matches
//                    lit/spirv/mesh_vertex.mim's shape).
// Not linked against libmim: it only shells out to the already-built `mim`
// binary and `spirv-as`, then talks to the Vulkan API directly.

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "mat4.h"
#include "obj_loader.h"
#include "shader_compile.h"

using namespace vulkan_runner;
namespace fs = std::filesystem;

#define VK_CHECK(expr)                                                                       \
    do {                                                                                     \
        VkResult vk_check_result_ = (expr);                                                  \
        if (vk_check_result_ != VK_SUCCESS)                                                  \
            throw std::runtime_error(std::string(#expr) + " failed with VkResult " +         \
                                      std::to_string(vk_check_result_));                      \
    } while (0)

namespace {

struct Args {
    std::string mode        = "triangle";
    std::string shader_path;
    std::string output_path = "out.ppm";
    std::string obj_path    = "utah_teapot.obj";
    uint32_t width          = 256;
    uint32_t height         = 256;
};

Args parse_args(int argc, char** argv) {
    Args args;
    std::vector<std::string> positional;
    for (int i = 1; i != argc; ++i) {
        std::string a = argv[i];
        auto starts_with = [&](const char* p) { return a.rfind(p, 0) == 0; };
        if (starts_with("--mode=")) args.mode = a.substr(7);
        else if (starts_with("--output=")) args.output_path = a.substr(9);
        else if (starts_with("--obj=")) args.obj_path = a.substr(6);
        else if (starts_with("--width=")) args.width = static_cast<uint32_t>(std::stoul(a.substr(8)));
        else if (starts_with("--height=")) args.height = static_cast<uint32_t>(std::stoul(a.substr(9)));
        else positional.push_back(a);
    }
    if (positional.empty())
        throw std::runtime_error("usage: vulkan_runner [--mode=triangle|mesh] [--output=out.ppm] "
                                  "[--obj=path.obj] [--width=N] [--height=N] <shader.mim>");
    args.shader_path = positional[0];
    return args;
}

std::string exe_dir(const char* argv0) {
    std::error_code ec;
    auto p = fs::canonical(fs::path(argv0), ec);
    if (ec) throw std::runtime_error("could not resolve executable path");
    return p.parent_path().string();
}

uint32_t find_memory_type(VkPhysicalDevice phys, uint32_t type_bits, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(phys, &mem_props);
    for (uint32_t i = 0; i != mem_props.memoryTypeCount; ++i)
        if ((type_bits & (1u << i)) && (mem_props.memoryTypes[i].propertyFlags & props) == props) return i;
    throw std::runtime_error("no suitable memory type found");
}

struct Buffer {
    VkBuffer buffer           = VK_NULL_HANDLE;
    VkDeviceMemory memory     = VK_NULL_HANDLE;
    void* mapped              = nullptr;
};

Buffer create_host_buffer(VkDevice device, VkPhysicalDevice phys, VkDeviceSize size, VkBufferUsageFlags usage) {
    Buffer buf;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size        = size;
    bci.usage       = usage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateBuffer(device, &bci, nullptr, &buf.buffer));

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, buf.buffer, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize  = req.size;
    mai.memoryTypeIndex = find_memory_type(
        phys, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VK_CHECK(vkAllocateMemory(device, &mai, nullptr, &buf.memory));
    VK_CHECK(vkBindBufferMemory(device, buf.buffer, buf.memory, 0));
    VK_CHECK(vkMapMemory(device, buf.memory, 0, size, 0, &buf.mapped));
    return buf;
}

struct PushConstants {
    float mvp[16];
    float time;
};

} // namespace

int main(int argc, char** argv) {
    try {
        Args args = parse_args(argc, argv);
        bool mesh_mode = args.mode == "mesh";

        std::string dir     = exe_dir(argv[0]);
        std::string mim_bin = dir + "/mim";
        std::string plugin_dir = dir + "/../lib64/mim";

        // --- Instance ---
        VkApplicationInfo app_info{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app_info.pApplicationName = "vulkan_runner";
        app_info.apiVersion       = VK_API_VERSION_1_0;
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ici.pApplicationInfo = &app_info;
        VkInstance instance;
        VK_CHECK(vkCreateInstance(&ici, nullptr, &instance));

        uint32_t phys_count = 0;
        VK_CHECK(vkEnumeratePhysicalDevices(instance, &phys_count, nullptr));
        if (phys_count == 0) throw std::runtime_error("no Vulkan physical devices found");
        std::vector<VkPhysicalDevice> phys_devices(phys_count);
        VK_CHECK(vkEnumeratePhysicalDevices(instance, &phys_count, phys_devices.data()));
        VkPhysicalDevice phys = phys_devices[0];

        VkPhysicalDeviceProperties phys_props;
        vkGetPhysicalDeviceProperties(phys, &phys_props);
        std::cerr << "using physical device: " << phys_props.deviceName << "\n";

        // --- Queue family + logical device ---
        uint32_t queue_family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(phys, &queue_family_count, nullptr);
        std::vector<VkQueueFamilyProperties> queue_families(queue_family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(phys, &queue_family_count, queue_families.data());
        std::optional<uint32_t> graphics_family;
        for (uint32_t i = 0; i != queue_family_count; ++i)
            if (queue_families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { graphics_family = i; break; }
        if (!graphics_family) throw std::runtime_error("no graphics-capable queue family found");

        float queue_priority = 1.f;
        VkDeviceQueueCreateInfo dqci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        dqci.queueFamilyIndex = *graphics_family;
        dqci.queueCount       = 1;
        dqci.pQueuePriorities = &queue_priority;

        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos    = &dqci;
        VkDevice device;
        VK_CHECK(vkCreateDevice(phys, &dci, nullptr, &device));

        VkQueue queue;
        vkGetDeviceQueue(device, *graphics_family, 0, &queue);

        // --- Color attachment image (linear tiling + host-visible memory so
        // we can map it directly after rendering, no staging-buffer copy
        // needed) ---
        VkFormat color_format = VK_FORMAT_R8G8B8A8_UNORM;
        VkImageCreateInfo ici_color{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici_color.imageType     = VK_IMAGE_TYPE_2D;
        ici_color.format        = color_format;
        ici_color.extent        = {args.width, args.height, 1};
        ici_color.mipLevels     = 1;
        ici_color.arrayLayers   = 1;
        ici_color.samples       = VK_SAMPLE_COUNT_1_BIT;
        ici_color.tiling        = VK_IMAGE_TILING_LINEAR;
        ici_color.usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        ici_color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImage color_image;
        VK_CHECK(vkCreateImage(device, &ici_color, nullptr, &color_image));

        VkMemoryRequirements color_req;
        vkGetImageMemoryRequirements(device, color_image, &color_req);
        VkMemoryAllocateInfo color_mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        color_mai.allocationSize  = color_req.size;
        color_mai.memoryTypeIndex = find_memory_type(
            phys, color_req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkDeviceMemory color_memory;
        VK_CHECK(vkAllocateMemory(device, &color_mai, nullptr, &color_memory));
        VK_CHECK(vkBindImageMemory(device, color_image, color_memory, 0));

        VkImageViewCreateInfo color_view_ci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        color_view_ci.image                       = color_image;
        color_view_ci.viewType                    = VK_IMAGE_VIEW_TYPE_2D;
        color_view_ci.format                      = color_format;
        color_view_ci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        color_view_ci.subresourceRange.levelCount = 1;
        color_view_ci.subresourceRange.layerCount = 1;
        VkImageView color_view;
        VK_CHECK(vkCreateImageView(device, &color_view_ci, nullptr, &color_view));

        // --- Depth attachment (mesh mode only), device-local, never read back ---
        VkFormat depth_format = VK_FORMAT_D32_SFLOAT;
        VkImage depth_image        = VK_NULL_HANDLE;
        VkDeviceMemory depth_memory = VK_NULL_HANDLE;
        VkImageView depth_view      = VK_NULL_HANDLE;
        if (mesh_mode) {
            VkImageCreateInfo ici_depth{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            ici_depth.imageType     = VK_IMAGE_TYPE_2D;
            ici_depth.format        = depth_format;
            ici_depth.extent        = {args.width, args.height, 1};
            ici_depth.mipLevels     = 1;
            ici_depth.arrayLayers   = 1;
            ici_depth.samples       = VK_SAMPLE_COUNT_1_BIT;
            ici_depth.tiling        = VK_IMAGE_TILING_OPTIMAL;
            ici_depth.usage         = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
            ici_depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            VK_CHECK(vkCreateImage(device, &ici_depth, nullptr, &depth_image));

            VkMemoryRequirements depth_req;
            vkGetImageMemoryRequirements(device, depth_image, &depth_req);
            VkMemoryAllocateInfo depth_mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            depth_mai.allocationSize  = depth_req.size;
            depth_mai.memoryTypeIndex = find_memory_type(phys, depth_req.memoryTypeBits,
                                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            VK_CHECK(vkAllocateMemory(device, &depth_mai, nullptr, &depth_memory));
            VK_CHECK(vkBindImageMemory(device, depth_image, depth_memory, 0));

            VkImageViewCreateInfo depth_view_ci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            depth_view_ci.image                       = depth_image;
            depth_view_ci.viewType                    = VK_IMAGE_VIEW_TYPE_2D;
            depth_view_ci.format                      = depth_format;
            depth_view_ci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            depth_view_ci.subresourceRange.levelCount = 1;
            depth_view_ci.subresourceRange.layerCount = 1;
            VK_CHECK(vkCreateImageView(device, &depth_view_ci, nullptr, &depth_view));
        }

        // --- Render pass ---
        std::vector<VkAttachmentDescription> attachments;
        VkAttachmentDescription color_attachment{};
        color_attachment.format         = color_format;
        color_attachment.samples        = VK_SAMPLE_COUNT_1_BIT;
        color_attachment.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color_attachment.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        color_attachment.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        color_attachment.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        color_attachment.finalLayout    = VK_IMAGE_LAYOUT_GENERAL;
        attachments.push_back(color_attachment);

        VkAttachmentReference color_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};

        VkAttachmentReference depth_ref{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        if (mesh_mode) {
            VkAttachmentDescription depth_attachment{};
            depth_attachment.format         = depth_format;
            depth_attachment.samples        = VK_SAMPLE_COUNT_1_BIT;
            depth_attachment.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
            depth_attachment.storeOp        = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            depth_attachment.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            depth_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            depth_attachment.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
            depth_attachment.finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            attachments.push_back(depth_attachment);
        }

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount    = 1;
        subpass.pColorAttachments       = &color_ref;
        subpass.pDepthStencilAttachment = mesh_mode ? &depth_ref : nullptr;

        VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rpci.attachmentCount = static_cast<uint32_t>(attachments.size());
        rpci.pAttachments    = attachments.data();
        rpci.subpassCount    = 1;
        rpci.pSubpasses      = &subpass;
        VkRenderPass render_pass;
        VK_CHECK(vkCreateRenderPass(device, &rpci, nullptr, &render_pass));

        std::vector<VkImageView> fb_views{color_view};
        if (mesh_mode) fb_views.push_back(depth_view);
        VkFramebufferCreateInfo fbci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fbci.renderPass      = render_pass;
        fbci.attachmentCount = static_cast<uint32_t>(fb_views.size());
        fbci.pAttachments    = fb_views.data();
        fbci.width           = args.width;
        fbci.height          = args.height;
        fbci.layers          = 1;
        VkFramebuffer framebuffer;
        VK_CHECK(vkCreateFramebuffer(device, &fbci, nullptr, &framebuffer));

        // --- Shader module (compiled from the .mim source; both "vertex"
        // and "fragment" entry points live in the same module) ---
        std::vector<uint32_t> spirv = compile_shader(mim_bin, plugin_dir, args.shader_path);
        VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        smci.codeSize = spirv.size() * sizeof(uint32_t);
        smci.pCode    = spirv.data();
        VkShaderModule shader_module;
        VK_CHECK(vkCreateShaderModule(device, &smci, nullptr, &shader_module));

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = shader_module;
        stages[0].pName  = "vertex";
        stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = shader_module;
        stages[1].pName  = "fragment";

        // --- Pipeline layout: mesh mode needs a push-constant range for the
        // camera matrix; triangle mode needs neither push constants nor
        // vertex input state. ---
        VkPushConstantRange push_range{};
        push_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        push_range.offset     = 0;
        push_range.size       = sizeof(PushConstants);

        VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        if (mesh_mode) {
            plci.pushConstantRangeCount = 1;
            plci.pPushConstantRanges    = &push_range;
        }
        VkPipelineLayout pipeline_layout;
        VK_CHECK(vkCreatePipelineLayout(device, &plci, nullptr, &pipeline_layout));

        VkVertexInputBindingDescription binding{};
        binding.binding   = 0;
        binding.stride    = 6 * sizeof(float); // interleaved position(vec3) + normal(vec3)
        binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        VkVertexInputAttributeDescription mesh_attrs[2]{};
        mesh_attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};
        mesh_attrs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 3 * sizeof(float)};

        VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        if (mesh_mode) {
            vertex_input.vertexBindingDescriptionCount   = 1;
            vertex_input.pVertexBindingDescriptions      = &binding;
            vertex_input.vertexAttributeDescriptionCount = 2;
            vertex_input.pVertexAttributeDescriptions    = mesh_attrs;
        }

        VkPipelineInputAssemblyStateCreateInfo input_assembly{
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkViewport viewport{0.f, 0.f, static_cast<float>(args.width), static_cast<float>(args.height), 0.f, 1.f};
        VkRect2D scissor{{0, 0}, {args.width, args.height}};
        VkPipelineViewportStateCreateInfo viewport_state{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewport_state.viewportCount = 1;
        viewport_state.pViewports    = &viewport;
        viewport_state.scissorCount  = 1;
        viewport_state.pScissors     = &scissor;

        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode    = VK_CULL_MODE_NONE;
        raster.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth   = 1.f;

        VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo depth_stencil{
            VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        depth_stencil.depthTestEnable  = mesh_mode ? VK_TRUE : VK_FALSE;
        depth_stencil.depthWriteEnable = mesh_mode ? VK_TRUE : VK_FALSE;
        depth_stencil.depthCompareOp   = VK_COMPARE_OP_LESS;

        VkPipelineColorBlendAttachmentState blend_attachment{};
        blend_attachment.colorWriteMask =
            VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blend.attachmentCount = 1;
        blend.pAttachments    = &blend_attachment;

        VkGraphicsPipelineCreateInfo pipeline_ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        pipeline_ci.stageCount          = 2;
        pipeline_ci.pStages             = stages;
        pipeline_ci.pVertexInputState   = &vertex_input;
        pipeline_ci.pInputAssemblyState = &input_assembly;
        pipeline_ci.pViewportState      = &viewport_state;
        pipeline_ci.pRasterizationState = &raster;
        pipeline_ci.pMultisampleState   = &multisample;
        pipeline_ci.pDepthStencilState  = &depth_stencil;
        pipeline_ci.pColorBlendState    = &blend;
        pipeline_ci.layout              = pipeline_layout;
        pipeline_ci.renderPass          = render_pass;
        pipeline_ci.subpass             = 0;
        VkPipeline pipeline;
        VK_CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_ci, nullptr, &pipeline));

        // --- Mesh data (mesh mode only) ---
        Buffer vertex_buffer{}, index_buffer{};
        uint32_t index_count = 0;
        if (mesh_mode) {
            Mesh mesh = load_obj(args.obj_path);
            index_count = static_cast<uint32_t>(mesh.indices.size());
            vertex_buffer =
                create_host_buffer(device, phys, mesh.vertices.size() * sizeof(float), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
            std::memcpy(vertex_buffer.mapped, mesh.vertices.data(), mesh.vertices.size() * sizeof(float));
            index_buffer =
                create_host_buffer(device, phys, mesh.indices.size() * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
            std::memcpy(index_buffer.mapped, mesh.indices.data(), mesh.indices.size() * sizeof(uint32_t));
            std::cerr << "loaded mesh: " << mesh.vertices.size() / 6 << " vertices, " << index_count / 3
                      << " triangles\n";
        }

        // --- Command buffer ---
        VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        cpci.queueFamilyIndex = *graphics_family;
        VkCommandPool cmd_pool;
        VK_CHECK(vkCreateCommandPool(device, &cpci, nullptr, &cmd_pool));

        VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cbai.commandPool        = cmd_pool;
        cbai.level               = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 1;
        VkCommandBuffer cmd;
        VK_CHECK(vkAllocateCommandBuffers(device, &cbai, &cmd));

        VkCommandBufferBeginInfo cbbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        VK_CHECK(vkBeginCommandBuffer(cmd, &cbbi));

        std::vector<VkClearValue> clear_values(mesh_mode ? 2 : 1);
        clear_values[0].color = {{0.05f, 0.05f, 0.08f, 1.f}};
        if (mesh_mode) clear_values[1].depthStencil = {1.f, 0};

        VkRenderPassBeginInfo rpbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rpbi.renderPass               = render_pass;
        rpbi.framebuffer              = framebuffer;
        rpbi.renderArea               = {{0, 0}, {args.width, args.height}};
        rpbi.clearValueCount          = static_cast<uint32_t>(clear_values.size());
        rpbi.pClearValues             = clear_values.data();
        vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

        if (mesh_mode) {
            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &vertex_buffer.buffer, &offset);
            vkCmdBindIndexBuffer(cmd, index_buffer.buffer, 0, VK_INDEX_TYPE_UINT32);

            Mat4 model = Mat4::identity();
            // utah_teapot.obj is modeled Z-up (x: spout<->handle axis
            // -3..3.44, y: side-to-side -2..2, z: height 0..3.15) rather than
            // the more common Y-up, and is a larger scale than the
            // traditional unit-sized teapot -- both the up vector and the
            // camera distance/position are tuned for that.
            float eye[3]    = {5.f, -8.f, 3.5f};
            float center[3] = {0.f, 0.f, 1.2f};
            float up[3]     = {0.f, 0.f, 1.f};
            Mat4 view = Mat4::look_at(eye, center, up);
            Mat4 proj = Mat4::perspective(45.f * 3.14159265f / 180.f,
                                          static_cast<float>(args.width) / static_cast<float>(args.height), 0.1f,
                                          100.f);
            Mat4 vp  = Mat4::mul(proj, view);
            Mat4 mvp = Mat4::mul(vp, model);

            PushConstants pc{};
            std::memcpy(pc.mvp, mvp.m, sizeof(pc.mvp));
            pc.time = 0.f;
            vkCmdPushConstants(cmd, pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);

            vkCmdDrawIndexed(cmd, index_count, 1, 0, 0, 0);
        } else {
            vkCmdDraw(cmd, 3, 1, 0, 0);
        }

        vkCmdEndRenderPass(cmd);
        VK_CHECK(vkEndCommandBuffer(cmd));

        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VkFence fence;
        VK_CHECK(vkCreateFence(device, &fci, nullptr, &fence));

        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers    = &cmd;
        VK_CHECK(vkQueueSubmit(queue, 1, &submit, fence));
        VK_CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));

        // --- Read back the (linear-tiled, host-visible) color image and
        // write it out as a PPM (P6 binary), stripping alpha. ---
        VkImageSubresource subres{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
        VkSubresourceLayout layout;
        vkGetImageSubresourceLayout(device, color_image, &subres, &layout);

        void* mapped;
        VK_CHECK(vkMapMemory(device, color_memory, 0, VK_WHOLE_SIZE, 0, &mapped));
        auto* pixels = static_cast<const uint8_t*>(mapped);

        std::ofstream out(args.output_path, std::ios::binary);
        if (!out) throw std::runtime_error("could not open output file: " + args.output_path);
        out << "P6\n" << args.width << " " << args.height << "\n255\n";
        for (uint32_t y = 0; y != args.height; ++y) {
            const uint8_t* row = pixels + layout.offset + y * layout.rowPitch;
            for (uint32_t x = 0; x != args.width; ++x) out.write(reinterpret_cast<const char*>(row + x * 4), 3);
        }
        vkUnmapMemory(device, color_memory);

        std::cerr << "wrote " << args.output_path << " (" << args.width << "x" << args.height << ")\n";

        vkDestroyFence(device, fence, nullptr);
        vkDestroyCommandPool(device, cmd_pool, nullptr);
        vkDestroyPipeline(device, pipeline, nullptr);
        vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
        vkDestroyShaderModule(device, shader_module, nullptr);
        vkDestroyFramebuffer(device, framebuffer, nullptr);
        vkDestroyRenderPass(device, render_pass, nullptr);
        vkDestroyImageView(device, color_view, nullptr);
        vkDestroyImage(device, color_image, nullptr);
        vkFreeMemory(device, color_memory, nullptr);
        if (mesh_mode) {
            vkDestroyImageView(device, depth_view, nullptr);
            vkDestroyImage(device, depth_image, nullptr);
            vkFreeMemory(device, depth_memory, nullptr);
            vkDestroyBuffer(device, vertex_buffer.buffer, nullptr);
            vkFreeMemory(device, vertex_buffer.memory, nullptr);
            vkDestroyBuffer(device, index_buffer.buffer, nullptr);
            vkFreeMemory(device, index_buffer.memory, nullptr);
        }
        vkDestroyDevice(device, nullptr);
        vkDestroyInstance(instance, nullptr);

        return EXIT_SUCCESS;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return EXIT_FAILURE;
    }
}
