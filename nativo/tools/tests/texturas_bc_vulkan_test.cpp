// On-device integration check: upload decoded BC1-5 to uncompressed Vulkan
// cubemaps, then read back every face/mip and verify their bytes. No game data.
#include "../../app/src/nfsmw_texturas_bc.h"
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

void Check(bool ok, const char* step) {
  if (!ok) { std::fprintf(stderr, "FAIL: %s\n", step); std::exit(1); }
}
void Ok(VkResult result, const char* step) {
  if (result != VK_SUCCESS) { std::fprintf(stderr, "FAIL: %s (%d)\n", step, int(result)); std::exit(1); }
}
int main() {
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "NFSMW BC compatibility test";
  app.apiVersion = VK_API_VERSION_1_0;
  VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.pApplicationInfo = &app;
  VkInstance instance;
  Ok(vkCreateInstance(&ici,nullptr,&instance), "create instance");
  uint32_t count = 0;
  Ok(vkEnumeratePhysicalDevices(instance,&count,nullptr), "enumerate GPUs");
  Check(count != 0,"GPU present");
  std::vector<VkPhysicalDevice> gpus(count);
  Ok(vkEnumeratePhysicalDevices(instance,&count,gpus.data()), "get GPUs");
  VkPhysicalDevice gpu = gpus[0];
  VkPhysicalDeviceProperties properties{};
  vkGetPhysicalDeviceProperties(gpu,&properties);
  std::printf("GPU: %s Vulkan %u.%u.%u\n", properties.deviceName,
      VK_VERSION_MAJOR(properties.apiVersion),VK_VERSION_MINOR(properties.apiVersion),VK_VERSION_PATCH(properties.apiVersion));
  vkGetPhysicalDeviceQueueFamilyProperties(gpu,&count,nullptr);
  std::vector<VkQueueFamilyProperties> families(count);
  vkGetPhysicalDeviceQueueFamilyProperties(gpu,&count,families.data());
  uint32_t family = 0;
  while (family < count && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) ++family;
  Check(family < count,"graphics queue");
  float priority = 1;
  VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qci.queueFamilyIndex = family; qci.queueCount = 1; qci.pQueuePriorities = &priority;
  VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
  VkDevice device;
  Ok(vkCreateDevice(gpu,&dci,nullptr,&device), "create device");
  VkQueue queue;
  vkGetDeviceQueue(device,family,0,&queue);
  VkPhysicalDeviceMemoryProperties mp{};
  vkGetPhysicalDeviceMemoryProperties(gpu,&mp);
  const auto allocate = [&](VkMemoryRequirements req, VkMemoryPropertyFlags flags) {
    uint32_t type = 0;
    while (type < mp.memoryTypeCount && (!(req.memoryTypeBits & (1u << type)) ||
        (mp.memoryTypes[type].propertyFlags & flags) != flags)) ++type;
    Check(type < mp.memoryTypeCount,"memory type");
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size; ai.memoryTypeIndex = type;
    VkDeviceMemory memory;
    Ok(vkAllocateMemory(device,&ai,nullptr,&memory), "allocate memory");
    return memory;
  };
  VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bci.size = 8192; bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  VkBuffer buffer;
  Ok(vkCreateBuffer(device,&bci,nullptr,&buffer), "create staging buffer");
  VkMemoryRequirements req{};
  vkGetBufferMemoryRequirements(device,buffer,&req);
  VkDeviceMemory bm = allocate(req,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  Ok(vkBindBufferMemory(device,buffer,bm,0), "bind buffer");
  void* mapped;
  Ok(vkMapMemory(device,bm,0,VK_WHOLE_SIZE,0,&mapped), "map staging buffer");
  auto bytes = static_cast<uint8_t*>(mapped);
  VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pci.queueFamilyIndex = family; pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  VkCommandPool pool;
  Ok(vkCreateCommandPool(device,&pci,nullptr,&pool), "create command pool");
  VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cai.commandPool = pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
  VkCommandBuffer cmd;
  Ok(vkAllocateCommandBuffers(device,&cai,&cmd), "allocate command buffer");
  VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  VkFence fence;
  Ok(vkCreateFence(device,&fci,nullptr,&fence), "create fence");
  const VkFormat compressed[] = {VK_FORMAT_BC1_RGBA_UNORM_BLOCK,VK_FORMAT_BC2_UNORM_BLOCK,
      VK_FORMAT_BC3_UNORM_BLOCK,VK_FORMAT_BC4_UNORM_BLOCK,VK_FORMAT_BC5_UNORM_BLOCK};
  for (uint32_t kind = 1; kind <= 5; ++kind) {
    const auto bc = static_cast<nfsmw::bc::Formato>(kind);
    std::vector<uint8_t> input, decoded;
    for (uint32_t n = 0; n < 4; ++n) for (uint32_t face = 0; face < 6; ++face) {
      const uint32_t w = 8 >> n, blocks = ((w+3)/4)*((w+3)/4);
      for (uint32_t b = 0; b < blocks; ++b) {
        for (uint32_t i = 0; i < nfsmw::bc::BytesBloque(bc); ++i)
          input.push_back(uint8_t(kind*17 + n*11 + face*3 + b + i));
      }
    }
    std::array<uint32_t,16> offsets{};
    Check(nfsmw::bc::Convertir(bc,input,8,8,6,4,decoded,offsets), "decode cube");
    Check(decoded.size() <= 4096,"staging capacity");
    std::memcpy(bytes,decoded.data(),decoded.size());
    std::memset(bytes+4096,0xCD,4096);
    const VkFormat host = kind < 4 ? VK_FORMAT_R8G8B8A8_UNORM : kind == 4 ? VK_FORMAT_R8_UNORM : VK_FORMAT_R8G8_UNORM;
    VkImageCreateInfo image_ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image_ci.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    image_ci.imageType = VK_IMAGE_TYPE_2D; image_ci.format = host; image_ci.extent = {8,8,1};
    image_ci.mipLevels = 4; image_ci.arrayLayers = 6; image_ci.samples = VK_SAMPLE_COUNT_1_BIT;
    image_ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    VkImage image;
    Ok(vkCreateImage(device,&image_ci,nullptr,&image), "create decoded image");
    vkGetImageMemoryRequirements(device,image,&req);
    VkDeviceMemory im = allocate(req,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    Ok(vkBindImageMemory(device,image,im,0), "bind image");
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = image; vci.viewType = VK_IMAGE_VIEW_TYPE_CUBE; vci.format = host;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,4,0,6};
    VkImageView view;
    Ok(vkCreateImageView(device,&vci,nullptr,&view), "create cube view");
    Ok(vkResetCommandBuffer(cmd,0), "reset command buffer");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    Ok(vkBeginCommandBuffer(cmd,&begin), "begin commands");
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image; barrier.subresourceRange = vci.subresourceRange;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
    VkBufferImageCopy copies[4]{};
    for (uint32_t n = 0; n < 4; ++n) {
      copies[n].bufferOffset = offsets[n]; copies[n].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,n,0,6};
      copies[n].imageExtent = {8u >> n,8u >> n,1};
    }
    vkCmdCopyBufferToImage(cmd,buffer,image,VK_IMAGE_LAYOUT_GENERAL,4,copies);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
    for (auto& copy : copies) copy.bufferOffset += 4096;
    vkCmdCopyImageToBuffer(cmd,image,VK_IMAGE_LAYOUT_GENERAL,buffer,4,copies);
    VkMemoryBarrier host_barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    host_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; host_barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host_barrier,0,nullptr,0,nullptr);
    Ok(vkEndCommandBuffer(cmd), "end commands");
    Ok(vkResetFences(device,1,&fence), "reset fence");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1; submit.pCommandBuffers = &cmd;
    Ok(vkQueueSubmit(queue,1,&submit,fence), "submit");
    Ok(vkWaitForFences(device,1,&fence,VK_TRUE,10000000000ull), "wait for GPU");
    for (uint32_t n = 0; n < 4; ++n) {
      const size_t size = size_t(8 >> n)*(8 >> n)*6*nfsmw::bc::Canales(bc);
      Check(!std::memcmp(bytes+4096+offsets[n],decoded.data()+offsets[n],size), "GPU readback matches decoded mip/faces");
    }
    VkFormatProperties fp{};
    vkGetPhysicalDeviceFormatProperties(gpu,compressed[kind-1],&fp);
    std::printf("PASS: BC%u -> format %d, 6 faces / 4 mips uploaded and read back (native BC sampled=%s)\n",
        kind,int(host),(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) ? "yes" : "no");
    vkDestroyImageView(device,view,nullptr); vkDestroyImage(device,image,nullptr); vkFreeMemory(device,im,nullptr);
  }
  vkDestroyFence(device,fence,nullptr); vkDestroyCommandPool(device,pool,nullptr);
  vkUnmapMemory(device,bm); vkDestroyBuffer(device,buffer,nullptr); vkFreeMemory(device,bm,nullptr);
  vkDestroyDevice(device,nullptr); vkDestroyInstance(instance,nullptr);
  std::puts("PASS: Vulkan CPU texture conversion integration");
}
