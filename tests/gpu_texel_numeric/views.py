#!/usr/bin/env python3
"""Exercise the production texel-view key/store through backing and format changes."""
import os
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
gpu=root/'app/src/main/cpp/skyline/gpu'
def function(text,signature):
 start=text.index(signature);end=text.index('{',start)+1;depth=1
 while depth:
  depth+=(text[end]=='{')-(text[end]=='}');end+=1
 return text[start:end]
header=(gpu/'buffer.h').read_text();source=(gpu/'buffer.cpp').read_text()
facade=r'''
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <cassert>
#include <cstdio>
using VkBuffer=uint64_t;using VkDeviceSize=uint64_t;using VkFormat=uint32_t;
namespace vk {
using DeviceSize=VkDeviceSize;using Format=VkFormat;
struct BufferViewCreateInfo {VkBuffer buffer;Format format;DeviceSize offset,range;};
namespace raii {struct BufferView {
 static inline unsigned alive{};
 BufferViewCreateInfo info;
 BufferView(int,BufferViewCreateInfo value):info(value){++alive;}
 ~BufferView(){--alive;}
};}
}
namespace skyline::gpu {
class Buffer {public:
'''
facade+=function(header,'struct TexelBufferViewKey {')+';\n'+function(header,'struct TexelBufferViewKeyHash {')+';\n'
facade+='''
std::unordered_map<TexelBufferViewKey,std::unique_ptr<vk::raii::BufferView>,TexelBufferViewKeyHash> texelBufferViewStore;
struct {int vkDevice{};}gpu;
VkBuffer backing=1; VkBuffer GetBacking() const {return backing;}
vk::raii::BufferView* GetTexelBufferView(vk::DeviceSize,vk::DeviceSize,vk::Format);
};
'''+function(source,'vk::raii::BufferView *Buffer::GetTexelBufferView(')+r'''
}
int main() {
 using skyline::gpu::Buffer;
 {
 Buffer owner;
 auto original=owner.GetTexelBufferView(256,128,98);
 assert(owner.GetTexelBufferView(256,128,98)==original);
 auto signedView=owner.GetTexelBufferView(256,128,99);
 assert(signedView!=original);
 auto offset=owner.GetTexelBufferView(512,128,98);
 auto range=owner.GetTexelBufferView(256,256,98);
 assert(offset!=original && range!=original);
 owner.backing=2;
 auto recreated=owner.GetTexelBufferView(256,128,98);
 assert(recreated!=original && recreated->info.buffer==2 && original->info.buffer==1);
 {
 Buffer other; auto sameHandle=other.GetTexelBufferView(256,128,98);
 assert(sameHandle!=original && sameHandle->info.buffer==original->info.buffer);
 }
 assert(vk::raii::BufferView::alive==5);
 }
 assert(vk::raii::BufferView::alive==0);
 std::puts("PASS production texel-view cache: format/offset/range/backing/owner identity; old views retained until owner release");
}
'''
with tempfile.TemporaryDirectory(prefix='texel-views-') as tmp:
 out=Path(tmp);(out/'test.cpp').write_text(facade)
 subprocess.run([os.environ.get('CXX','c++'),'-std=c++20','-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all',out/'test.cpp','-o',out/'test'],check=True)
 subprocess.run([out/'test'],check=True)
