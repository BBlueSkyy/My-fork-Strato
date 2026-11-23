#pragma once
#include "context.h"
#include <fstream>
#include <sstream>
#include <string>

struct Recipe {
    vk::DescriptorSetLayoutCreateFlags flags;
    vk::PipelineLayoutCreateFlags pipelineFlags;
    std::vector<vk::DescriptorSetLayoutBinding> bindings;
    std::vector<vk::PushConstantRange> ranges;
    vk::DescriptorSetLayoutCreateInfo Info() const {
        return {.flags=flags,.bindingCount=static_cast<uint32_t>(bindings.size()),.pBindings=bindings.data()};
    }
};
inline std::vector<Recipe> Load(const char *path) {
    std::ifstream file(path);if(!file)throw std::runtime_error("missing capture layout fixture");
    std::vector<Recipe> out;std::string line;
    while(std::getline(file,line)) {
        if(line.empty()||line[0]=='#')continue;
        std::istringstream s(line);uint32_t flags,pflags,nb,nr;s>>flags>>pflags>>nb>>nr;
        Recipe r{vk::DescriptorSetLayoutCreateFlags(flags),vk::PipelineLayoutCreateFlags(pflags),{}, {}};
        for(uint32_t i=0;i<nb;++i){uint32_t n,t,c,st;s>>n>>t>>c>>st;r.bindings.push_back({.binding=n,.descriptorType=vk::DescriptorType(t),.descriptorCount=c,.stageFlags=vk::ShaderStageFlags(st)});}
        for(uint32_t i=0;i<nr;++i){uint32_t st,o,n;s>>st>>o>>n;r.ranges.push_back({.stageFlags=vk::ShaderStageFlags(st),.offset=o,.size=n});}
        if(!s)throw std::runtime_error("invalid fixture row");out.push_back(std::move(r));
    }
    return out;
}
