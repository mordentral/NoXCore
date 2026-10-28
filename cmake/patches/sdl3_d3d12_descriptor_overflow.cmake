# SDL 3.4.16's D3D12 backend ("FIXME: need to error on overflow" in
# D3D12_INTERNAL_WriteGPUDescriptors) moves to a fresh GPU descriptor heap only once the current
# one is already full, so a draw binding several descriptors near the end of a heap writes past it:
# with SDL's renderer, a frame of about 2000 draws fills the 2048-sampler heap and a three-sampler
# draw at index 2046 writes into memory that isn't the heap (the D3D12 debug layer stops on it,
# INVALID_DESCRIPTOR_HANDLE). This moves to a fresh heap when the draw's descriptors don't fit.
# Run as the FetchContent PATCH_COMMAND, in the SDL source directory.
set(file "src/gpu/d3d12/SDL_gpu_d3d12.c")
file(READ "${file}" text)
set(before "commandBuffer->gpuDescriptorHeaps[heapType]->currentDescriptorIndex >= commandBuffer->gpuDescriptorHeaps[heapType]->maxDescriptors")
set(after "commandBuffer->gpuDescriptorHeaps[heapType]->currentDescriptorIndex + resourceHandleCount > commandBuffer->gpuDescriptorHeaps[heapType]->maxDescriptors")
string(FIND "${text}" "${after}" done)
if(done EQUAL -1)
    string(FIND "${text}" "${before}" at)
    if(at EQUAL -1)
        message(FATAL_ERROR "sdl3_d3d12_descriptor_overflow: the code it patches isn't in ${file} (a new SDL?)")
    endif()
    string(REPLACE "${before}" "${after}" text "${text}")
    file(WRITE "${file}" "${text}")
endif()
