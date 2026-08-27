#include "render/renderdoc/renderdoc_capture.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

// 解析 RENDERDOC_GetAPI：Windows 经 GetProcAddress，POSIX 经 dlsym。
pRENDERDOC_GetAPI ResolveGetApi(void* handle) {
#ifdef _WIN32
    return reinterpret_cast<pRENDERDOC_GetAPI>(
        GetProcAddress(reinterpret_cast<HMODULE>(handle), "RENDERDOC_GetAPI"));
#else
    return reinterpret_cast<pRENDERDOC_GetAPI>(dlsym(handle, "RENDERDOC_GetAPI"));
#endif
}

}  // namespace

void RenderDocCapture::LibCloser::operator()(void* h) const noexcept {
    if (h == nullptr) {
        return;
    }
#ifdef _WIN32
    FreeLibrary(reinterpret_cast<HMODULE>(h));
#else
    dlclose(h);
#endif
}

RenderDocCapture::RenderDocCapture() {
    const char* e = std::getenv("DGC_RENDERDOC");
    enabled_ = (e != nullptr && e[0] != '\0' && std::strcmp(e, "0") != 0);
}

RenderDocCapture::~RenderDocCapture() = default;

void RenderDocCapture::EnsureLoaded() {
    if (!enabled_ || tried_) {
        return;
    }
    tried_ = true;

    // 运行时动态加载：Windows renderdoc.dll / POSIX librenderdoc.so。
    void* handle = nullptr;
#ifdef _WIN32
    handle = reinterpret_cast<void*>(LoadLibraryA("renderdoc.dll"));
#else
    handle = dlopen("librenderdoc.so", RTLD_NOW | RTLD_LOCAL);
#endif
    if (handle == nullptr) {
        std::fprintf(stderr,
                     "[RenderDoc] renderdoc library not found; programmatic capture disabled\n");
        return;
    }
    lib_.reset(handle);

    pRENDERDOC_GetAPI getApi = ResolveGetApi(handle);
    if (getApi == nullptr) {
        std::fprintf(stderr,
                     "[RenderDoc] RENDERDOC_GetAPI symbol missing; programmatic capture disabled\n");
        return;
    }

    RENDERDOC_API_1_1_1* api = nullptr;
    if (getApi(eRENDERDOC_API_Version_1_1_1, reinterpret_cast<void**>(&api)) != 1 ||
        api == nullptr) {
        std::fprintf(stderr,
                     "[RenderDoc] RENDERDOC_API_1_1_1 unsupported; programmatic capture disabled\n");
        return;
    }
    api_ = api;
    available_ = true;
    configureFromEnv();
}

void RenderDocCapture::configureFromEnv() {
    if (!available_ || api_ == nullptr) {
        return;
    }
    // 捕获目录由 DGC_RENDERDOC_DIR 驱动（可选）。
    if (const char* dir = std::getenv("DGC_RENDERDOC_DIR")) {
        if (dir[0] != '\0' && api_->SetCaptureFilePathTemplate != nullptr) {
            api_->SetCaptureFilePathTemplate(dir);
        }
    }
}

void RenderDocCapture::startFrameCapture(void* device) {
    EnsureLoaded();
    if (!available_ || device == nullptr || api_->StartFrameCapture == nullptr) {
        return;
    }
    api_->StartFrameCapture(reinterpret_cast<RENDERDOC_DevicePointer>(device), nullptr);
}

void RenderDocCapture::endFrameCapture(void* device) {
    if (!available_ || device == nullptr || api_->EndFrameCapture == nullptr) {
        return;
    }
    api_->EndFrameCapture(reinterpret_cast<RENDERDOC_DevicePointer>(device), nullptr);
}
