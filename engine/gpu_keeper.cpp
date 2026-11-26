// SPDX-License-Identifier: GPL-3.0-only
// "GPU clock keep" (settings switch, aimed at AMD/Intel). Those GPUs lower their clock when work
// arrives in bursts (one inference, then idle until the next source frame) and ramp up only after a
// delay, so each inference runs at a low clock (RX 6600 generated too little at ~60% load).
// While generation runs, a small compute shader is dispatched back to back on its own Vulkan device
// and lowest-priority queue, so the GPU never looks idle to its power management.
// Each dispatch is tuned to about 0.25 ms on a few workgroups, leaving almost all compute units free.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <vulkan/vulkan.h>
#include "gpu.h"
#include "option.h"
#include "gpu_keeper.h"
#include <thread>
#include <atomic>
#include <vector>
#include <chrono>
#include <cstring>
#include <algorithm>

namespace {
const char kShader[]=R"(#version 450
layout (binding = 0) buffer keep_blob { float d[]; };
layout (push_constant) uniform parameter { int iters; } p;
void main()
{
    uint i = gl_GlobalInvocationID.x;
    float v = d[i];
    for (int k = 0; k < p.iters; k++) v = v * 1.00001 + 0.00001;
    d[i] = v;
}
)";
constexpr uint32_t kGroups=4,kLocal=64,kCount=kGroups*kLocal;

struct Keeper{
 VkInstance inst=VK_NULL_HANDLE;VkDevice dev=VK_NULL_HANDLE;VkQueue queue=VK_NULL_HANDLE;VkBuffer buf=VK_NULL_HANDLE;VkDeviceMemory mem=VK_NULL_HANDLE;
 VkShaderModule shader=VK_NULL_HANDLE;VkDescriptorSetLayout dsl=VK_NULL_HANDLE;VkPipelineLayout layout=VK_NULL_HANDLE;VkPipeline pipe=VK_NULL_HANDLE;
 VkDescriptorPool dpool=VK_NULL_HANDLE;VkDescriptorSet set=VK_NULL_HANDLE;VkCommandPool pool=VK_NULL_HANDLE;VkCommandBuffer cb=VK_NULL_HANDLE;VkFence fence=VK_NULL_HANDLE;
 std::thread thread;std::atomic<bool> stop{false},active{false};std::atomic<double> dispatchMs{0};std::atomic<uint64_t> dispatches{0};
 ~Keeper(){stop=true;if(thread.joinable())thread.join();
  if(dev){vkDeviceWaitIdle(dev);if(fence)vkDestroyFence(dev,fence,nullptr);if(pool)vkDestroyCommandPool(dev,pool,nullptr);if(dpool)vkDestroyDescriptorPool(dev,dpool,nullptr);
   if(pipe)vkDestroyPipeline(dev,pipe,nullptr);if(layout)vkDestroyPipelineLayout(dev,layout,nullptr);if(dsl)vkDestroyDescriptorSetLayout(dev,dsl,nullptr);if(shader)vkDestroyShaderModule(dev,shader,nullptr);
   if(buf)vkDestroyBuffer(dev,buf,nullptr);if(mem)vkFreeMemory(dev,mem,nullptr);vkDestroyDevice(dev,nullptr);}
  if(inst)vkDestroyInstance(inst,nullptr);}
 bool init(uint32_t vendor,uint32_t device){
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.apiVersion=VK_API_VERSION_1_1;VkInstanceCreateInfo ii{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ii.pApplicationInfo=&app;
  if(vkCreateInstance(&ii,nullptr,&inst)){inst=VK_NULL_HANDLE;return false;}
  uint32_t n=0;vkEnumeratePhysicalDevices(inst,&n,nullptr);std::vector<VkPhysicalDevice> pd(n);vkEnumeratePhysicalDevices(inst,&n,pd.data());VkPhysicalDevice phys=VK_NULL_HANDLE;
  for(auto p:pd){VkPhysicalDeviceProperties pp;vkGetPhysicalDeviceProperties(p,&pp);if(pp.vendorID==vendor&&pp.deviceID==device){phys=p;break;}}
  if(!phys)return false;
  // Prefer a compute-only family (AMD exposes one), else any compute family; priority 0 = lowest.
  uint32_t qn=0;vkGetPhysicalDeviceQueueFamilyProperties(phys,&qn,nullptr);std::vector<VkQueueFamilyProperties> qf(qn);vkGetPhysicalDeviceQueueFamilyProperties(phys,&qn,qf.data());
  uint32_t family=UINT32_MAX;for(uint32_t i=0;i<qn;i++)if((qf[i].queueFlags&VK_QUEUE_COMPUTE_BIT)&&!(qf[i].queueFlags&VK_QUEUE_GRAPHICS_BIT)){family=i;break;}
  if(family==UINT32_MAX)for(uint32_t i=0;i<qn;i++)if(qf[i].queueFlags&VK_QUEUE_COMPUTE_BIT){family=i;break;}
  if(family==UINT32_MAX)return false;
  float prio=0;VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};qi.queueFamilyIndex=family;qi.queueCount=1;qi.pQueuePriorities=&prio;
  VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};di.queueCreateInfoCount=1;di.pQueueCreateInfos=&qi;
  if(vkCreateDevice(phys,&di,nullptr,&dev)){dev=VK_NULL_HANDLE;return false;}
  vkGetDeviceQueue(dev,family,0,&queue);
  VkPhysicalDeviceMemoryProperties mp;vkGetPhysicalDeviceMemoryProperties(phys,&mp);
  VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bi.size=kCount*sizeof(float);bi.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  if(vkCreateBuffer(dev,&bi,nullptr,&buf))return false;VkMemoryRequirements req;vkGetBufferMemoryRequirements(dev,buf,&req);
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=req.size;ai.memoryTypeIndex=UINT32_MAX;
  for(uint32_t i=0;i<mp.memoryTypeCount;i++)if((req.memoryTypeBits&(1u<<i))&&(mp.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)){ai.memoryTypeIndex=i;break;}
  if(ai.memoryTypeIndex==UINT32_MAX||vkAllocateMemory(dev,&ai,nullptr,&mem)||vkBindBufferMemory(dev,buf,mem,0))return false;
  ncnn::Option opt;opt.use_fp16_storage=false;opt.use_fp16_packed=false;opt.use_fp16_arithmetic=false;opt.use_int8_storage=false;
  std::vector<uint32_t> spirv;if(ncnn::compile_spirv_module(kShader,(int)strlen(kShader),opt,spirv)||spirv.empty())return false;
  VkShaderModuleCreateInfo si{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};si.codeSize=spirv.size()*4;si.pCode=spirv.data();if(vkCreateShaderModule(dev,&si,nullptr,&shader))return false;
  VkDescriptorSetLayoutBinding b{};b.binding=0;b.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;b.descriptorCount=1;b.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT;
  VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};dli.bindingCount=1;dli.pBindings=&b;if(vkCreateDescriptorSetLayout(dev,&dli,nullptr,&dsl))return false;
  VkPushConstantRange pc{VK_SHADER_STAGE_COMPUTE_BIT,0,4};VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pli.setLayoutCount=1;pli.pSetLayouts=&dsl;pli.pushConstantRangeCount=1;pli.pPushConstantRanges=&pc;
  if(vkCreatePipelineLayout(dev,&pli,nullptr,&layout))return false;
  // ncnn's preamble declares the workgroup size as specialization constants 233-235.
  uint32_t local[3]={kLocal,1,1};VkSpecializationMapEntry me[3]={{233,0,4},{234,4,4},{235,8,4}};VkSpecializationInfo spec{3,me,sizeof local,local};
  VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};ci.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};ci.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;ci.stage.module=shader;ci.stage.pName="main";ci.stage.pSpecializationInfo=&spec;ci.layout=layout;
  if(vkCreateComputePipelines(dev,VK_NULL_HANDLE,1,&ci,nullptr,&pipe))return false;
  VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1};VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};dpi.maxSets=1;dpi.poolSizeCount=1;dpi.pPoolSizes=&ps;
  if(vkCreateDescriptorPool(dev,&dpi,nullptr,&dpool))return false;
  VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};dai.descriptorPool=dpool;dai.descriptorSetCount=1;dai.pSetLayouts=&dsl;if(vkAllocateDescriptorSets(dev,&dai,&set))return false;
  VkDescriptorBufferInfo dbi{buf,0,VK_WHOLE_SIZE};VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};w.dstSet=set;w.descriptorCount=1;w.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;w.pBufferInfo=&dbi;vkUpdateDescriptorSets(dev,1,&w,0,nullptr);
  VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};cpi.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;cpi.queueFamilyIndex=family;if(vkCreateCommandPool(dev,&cpi,nullptr,&pool))return false;
  VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ca.commandPool=pool;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=1;if(vkAllocateCommandBuffers(dev,&ca,&cb))return false;
  VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};if(vkCreateFence(dev,&fi,nullptr,&fence))return false;
  thread=std::thread([this]{run();});return true;}
 bool dispatch(int iters){
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;if(vkBeginCommandBuffer(cb,&bi))return false;
  vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_COMPUTE,pipe);vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&set,0,nullptr);
  vkCmdPushConstants(cb,layout,VK_SHADER_STAGE_COMPUTE_BIT,0,4,&iters);vkCmdDispatch(cb,kGroups,1,1);if(vkEndCommandBuffer(cb))return false;
  VkSubmitInfo s{VK_STRUCTURE_TYPE_SUBMIT_INFO};s.commandBufferCount=1;s.pCommandBuffers=&cb;
  if(vkQueueSubmit(queue,1,&s,fence))return false;
  // a wait that times out (GPU stalled over 1 s) left the command buffer and fence pending, and the next
  // dispatch re-recorded and resubmitted them (invalid Vulkan use). Wait for the device before reusing either.
  VkResult r=vkWaitForFences(dev,1,&fence,VK_TRUE,1000000000ull);if(r!=VK_SUCCESS)vkDeviceWaitIdle(dev);
  return !vkResetFences(dev,1,&fence)&&r==VK_SUCCESS;}
 void run(){
  // The iteration count follows the measured time so one dispatch stays near 0.25 ms on any GPU and clock.
  int iters=2000,failures=0;double ema=0;
  while(!stop){
   if(!active){Sleep(5);continue;}
   // Give up after 3 failures in a row (device lost): the keep is optional and must never spin on a broken device.
   auto t0=std::chrono::steady_clock::now();if(!dispatch(iters)){if(++failures>=3)return;Sleep(50);continue;}failures=0;
   double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t0).count();
   ema=ema?ema*.9+ms*.1:ms;dispatchMs=ema;++dispatches;
   if(ms<.15&&iters<4000000)iters=iters*5/4+1;else if(ms>.4&&iters>50)iters=iters*4/5;
  }
 }
};
}

void* pk_keeper_start(uint32_t vendor,uint32_t device){Keeper* k=new(std::nothrow) Keeper;if(!k)return nullptr;try{if(!k->init(vendor,device)){delete k;return nullptr;}}catch(...){delete k;return nullptr;}return k;}
void pk_keeper_active(void* p,bool on){if(p)static_cast<Keeper*>(p)->active=on;}
double pk_keeper_dispatch_ms(void* p){return p?static_cast<Keeper*>(p)->dispatchMs.load():-1;}
uint64_t pk_keeper_dispatches(void* p){return p?static_cast<Keeper*>(p)->dispatches.load():0;}
void pk_keeper_stop(void* p){delete static_cast<Keeper*>(p);}
