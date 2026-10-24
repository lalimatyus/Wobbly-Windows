#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <dcomp.h>
#include <dwmapi.h>
#include <wrl/client.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include "mesh_bridge.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;

namespace {
constexpr UINT kFrameMessage = WM_APP + 1;
constexpr UINT kGrid = 20;
constexpr char kShaders[] = R"(
cbuffer Bend : register(b0) { float4 options; float4 controls[16]; };
struct Vertex { float2 position : POSITION; float2 uv : TEXCOORD0; };
struct Pixel { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };
Pixel VSMain(Vertex input) {
    Pixel output;
    float2 p = input.position;
    float envelope = (1.0 - p.x * p.x) * (1.0 - p.y * p.y);
    if (options.z > 0.5) {
        float u = input.uv.x;
        float v = input.uv.y;
        float4 bx = float4(pow(1-u, 3), 3*u*pow(1-u, 2),
                           3*u*u*(1-u), u*u*u);
        float4 by = float4(pow(1-v, 3), 3*v*pow(1-v, 2),
                           3*v*v*(1-v), v*v*v);
        float2 displacement = 0;
        [unroll] for (int y = 0; y < 4; ++y)
            [unroll] for (int x = 0; x < 4; ++x)
                displacement += controls[y * 4 + x].xy * bx[x] * by[y];
        p += displacement;
    } else {
        p += options.xy * envelope;
    }
    output.position = float4(p * options.w, 0.0, 1.0);
    output.uv = input.uv;
    return output;
}
Texture2D windowImage : register(t0);
SamplerState linearSampler : register(s0);
float4 PSMain(Pixel input) : SV_TARGET {
    return windowImage.Sample(linearSampler, input.uv);
}
)";

struct Vertex {
    float x, y, u, v;
};

struct Bend {
    float options[4];
    float controls[16][4];
};

bool ReadSlot(const mesh_bridge::Slot& slot, mesh_bridge::Payload& payload) {
    bool consistent = false;
    for (int attempt = 0; attempt < 3; ++attempt) {
        LONG before = slot.sequence;
        if (before & 1) continue;
        MemoryBarrier();
        payload = slot.payload;
        MemoryBarrier();
        LONG after = slot.sequence;
        if (before == after && !(after & 1)) {
            consistent = true;
            break;
        }
    }
    if (!consistent || !payload.active || !payload.hwnd) return false;
    ULONGLONG now = GetTickCount64();
    return payload.timestamp <= now && now - payload.timestamp <= 500;
}

class Preview {
public:
    explicit Preview(HWND target, bool physicsMode = false,
                     bool overlayTest = false, bool managed = false)
        : target_(target), physicsMode_(physicsMode),
          overlayTest_(overlayTest), managed_(managed) {
        try {
            CreateWindowAndDevice();
            if (target_) StartCapture();
            else if (!overlayTest_) CreateTestTexture();
            InvalidateRect(window_, nullptr, FALSE);
        } catch (...) {
            StopCapture();
            if (window_ && IsWindow(window_)) DestroyWindow(window_);
            throw;
        }
    }

    ~Preview() {
        StopCapture();
        if (window_ && IsWindow(window_)) DestroyWindow(window_);
        if (bridge_) UnmapViewOfFile(bridge_);
        if (bridgeHandle_) CloseHandle(bridgeHandle_);
    }

    HWND window() const { return window_; }
    bool failed() const { return failed_; }

    void Run() {
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

private:
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam,
                                       LPARAM lparam) {
        auto* self = reinterpret_cast<Preview*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            self = static_cast<Preview*>(create->lpCreateParams);
            self->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (self) {
            switch (message) {
            case kFrameMessage:
                try { self->ReceiveFrames(); }
                catch (winrt::hresult_error const& error) {
                    std::fwprintf(stderr, L"Capture error: 0x%08X %ls\n",
                                  static_cast<unsigned>(error.code()), error.message().c_str());
                    if (self->managed_) {
                        self->failed_ = true;
                        self->StopCapture();
                    }
                    else DestroyWindow(window);
                }
                return 0;
            case WM_PAINT: {
                PAINTSTRUCT paint;
                BeginPaint(window, &paint);
                bool destroyAfterPaint = false;
                try { self->Render(); }
                catch (winrt::hresult_error const& error) {
                    std::fwprintf(stderr, L"Render error: 0x%08X %ls\n",
                                  static_cast<unsigned>(error.code()), error.message().c_str());
                    if (self->managed_) {
                        self->failed_ = true;
                        self->StopCapture();
                    } else destroyAfterPaint = true;
                }
                EndPaint(window, &paint);
                if (destroyAfterPaint) DestroyWindow(window);
                if (!destroyAfterPaint && !self->overlayTest_ &&
                    self->capturedView_ && !IsIconic(window))
                    InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
            case WM_ERASEBKGND:
                return 1;
            case WM_NCHITTEST:
                if (self->overlayTest_) return HTTRANSPARENT;
                break;
            case WM_TIMER:
                if (self->overlayTest_) {
                    if (wparam == 2) DestroyWindow(window);
                    else if (wparam == 1) {
                        InvalidateRect(window, nullptr, FALSE);
                    }
                    return 0;
                }
                break;
            case WM_SIZE:
                if (self->swapChain_ && LOWORD(lparam) && HIWORD(lparam))
                    self->Resize(LOWORD(lparam), HIWORD(lparam));
                return 0;
            case WM_DESTROY:
                if (!self->managed_) PostQuitMessage(0);
                return 0;
            case WM_NCDESTROY:
                SetWindowLongPtrW(window, GWLP_USERDATA, 0);
                break;
            }
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    void CreateWindowAndDevice() {
        WNDCLASSW windowClass{};
        windowClass.lpfnWndProc = WindowProc;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.lpszClassName = L"WobblyWgcMeshPreview";
        if (!RegisterClassW(&windowClass) &&
            GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            winrt::throw_last_error();
        DWORD style = overlayTest_ ? WS_POPUP : WS_OVERLAPPEDWINDOW;
        DWORD exStyle = overlayTest_ ?
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
            WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_NOREDIRECTIONBITMAP : 0;
        window_ = CreateWindowExW(exStyle, windowClass.lpszClassName,
                                  L"Wobbly GPU mesh preview", style,
                                  CW_USEDEFAULT, CW_USEDEFAULT,
                                  overlayTest_ ? 1 : 1050, overlayTest_ ? 1 : 750,
                                  nullptr, nullptr, windowClass.hInstance, this);
        if (!window_) winrt::throw_last_error();
        if (overlayTest_ &&
            !SetLayeredWindowAttributes(window_, 0, 255, LWA_ALPHA))
            winrt::throw_last_error();

        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        D3D_FEATURE_LEVEL level;
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE,
                                       nullptr, flags, nullptr, 0, D3D11_SDK_VERSION,
                                       &device_, &level, &context_);
        if (FAILED(hr)) {
            std::fwprintf(stderr, L"Hardware D3D11 unavailable; using WARP.\n");
            winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP,
                nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &device_, &level, &context_));
        }

        ComPtr<IDXGIDevice> dxgi;
        winrt::check_hresult(device_.As(&dxgi));
        ComPtr<IDXGIAdapter> adapter;
        winrt::check_hresult(dxgi->GetAdapter(&adapter));
        ComPtr<IDXGIFactory2> factory;
        winrt::check_hresult(adapter->GetParent(IID_PPV_ARGS(&factory)));
        DXGI_SWAP_CHAIN_DESC1 description{};
        description.Width = overlayTest_ ? 1 : 1050;
        description.Height = overlayTest_ ? 1 : 750;
        description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        description.BufferCount = 2;
        description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        if (overlayTest_) {
            description.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
            winrt::check_hresult(factory->CreateSwapChainForComposition(
                device_.Get(), &description, nullptr, &swapChain_));
            winrt::check_hresult(DCompositionCreateDevice(
                dxgi.Get(), IID_PPV_ARGS(&compositionDevice_)));
            winrt::check_hresult(compositionDevice_->CreateTargetForHwnd(
                window_, TRUE, &compositionTarget_));
            winrt::check_hresult(compositionDevice_->CreateVisual(&compositionVisual_));
            winrt::check_hresult(compositionVisual_->SetContent(swapChain_.Get()));
            winrt::check_hresult(compositionTarget_->SetRoot(compositionVisual_.Get()));
            winrt::check_hresult(compositionDevice_->Commit());
        } else {
            winrt::check_hresult(factory->CreateSwapChainForHwnd(
                device_.Get(), window_, &description, nullptr, nullptr, &swapChain_));
        }
        CreateRenderTarget();
        CreatePipeline();
        if (overlayTest_) {
            if (target_) UpdateOverlayBounds();
            SetTimer(window_, 1, 16, nullptr);
            if (!managed_) SetTimer(window_, 2, 5000, nullptr);
        } else {
            ShowWindow(window_, SW_SHOW);
        }
    }

    void CreateRenderTarget() {
        ComPtr<ID3D11Texture2D> backBuffer;
        winrt::check_hresult(swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer)));
        winrt::check_hresult(device_->CreateRenderTargetView(backBuffer.Get(),
                                                             nullptr, &renderTarget_));
    }

    void Resize(UINT width, UINT height) {
        renderTarget_.Reset();
        context_->OMSetRenderTargets(0, nullptr, nullptr);
        winrt::check_hresult(swapChain_->ResizeBuffers(0, width, height,
                                                         DXGI_FORMAT_UNKNOWN, 0));
        CreateRenderTarget();
    }

    bool UpdateOverlayBounds(double physicsX = 0.0, double physicsY = 0.0) {
        if (!target_ || !IsWindow(target_)) {
            if (managed_) {
                if (overlayVisible_) ShowWindow(window_, SW_HIDE);
                overlayVisible_ = false;
            } else {
                DestroyWindow(window_);
            }
            return false;
        }
        DWORD cloaked = 0;
        DwmGetWindowAttribute(target_, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
        if (!IsWindowVisible(target_) || IsIconic(target_) || cloaked) {
            if (overlayVisible_) ShowWindow(window_, SW_HIDE);
            overlayVisible_ = false;
            return false;
        }
        RECT windowBounds{};
        if (!GetWindowRect(target_, &windowBounds))
            return false;
        if (!haveFrameInsets_) {
            RECT visibleBounds{};
            if (SUCCEEDED(DwmGetWindowAttribute(target_,
                    DWMWA_EXTENDED_FRAME_BOUNDS,
                    &visibleBounds, sizeof(visibleBounds))) &&
                visibleBounds.right > visibleBounds.left &&
                visibleBounds.bottom > visibleBounds.top) {
                frameInsets_ = {visibleBounds.left - windowBounds.left,
                                visibleBounds.top - windowBounds.top,
                                visibleBounds.right - windowBounds.right,
                                visibleBounds.bottom - windowBounds.bottom};
            }
            haveFrameInsets_ = true;
        }
        RECT source{windowBounds.left + frameInsets_.left,
                    windowBounds.top + frameInsets_.top,
                    windowBounds.right + frameInsets_.right,
                    windowBounds.bottom + frameInsets_.bottom};
        const int width = source.right - source.left;
        const int height = source.bottom - source.top;
        if (width < 1 || height < 1 || width > 16000 || height > 16000)
            return false;
        const int canvasWidth = width * 5 / 4;
        const int canvasHeight = height * 5 / 4;
        RECT desired{source.left - (canvasWidth - width) / 2,
                     source.top - (canvasHeight - height) / 2,
                     source.left - (canvasWidth - width) / 2 + canvasWidth,
                     source.top - (canvasHeight - height) / 2 + canvasHeight};
        const LONG shiftX = static_cast<LONG>(std::lround(physicsX));
        const LONG shiftY = static_cast<LONG>(std::lround(physicsY));
        OffsetRect(&desired, shiftX, shiftY);
        if (!EqualRect(&desired, &overlayBounds_)) {
            if (!SetWindowPos(window_, nullptr, desired.left, desired.top,
                              canvasWidth, canvasHeight,
                              SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOZORDER))
                return false;
            overlayBounds_ = desired;
            ++overlayMoves_;
        }
        targetWindowBounds_ = windowBounds;
        sourceBounds_ = source;
        overlayPhysicsX_ = physicsX;
        overlayPhysicsY_ = physicsY;
        if (!overlayVisible_) {
            ShowWindow(window_, SW_SHOWNOACTIVATE);
            overlayVisible_ = true;
            std::wprintf(L"Overlay aligned: source=%dx%d canvas=%dx%d capture=%ux%u\n",
                width, height, canvasWidth, canvasHeight,
                textureWidth_, textureHeight_);
        }
        return true;
    }

    void CreatePipeline() {
        ComPtr<ID3DBlob> vertexBlob, pixelBlob, errors;
        auto compile = [&](const char* entry, const char* profile, ID3DBlob** output) {
            HRESULT hr = D3DCompile(kShaders, sizeof(kShaders) - 1, nullptr,
                                    nullptr, nullptr, entry, profile, 0, 0,
                                    output, &errors);
            if (FAILED(hr)) {
                if (errors) std::fprintf(stderr, "%s\n",
                                          static_cast<const char*>(errors->GetBufferPointer()));
                winrt::check_hresult(hr);
            }
            errors.Reset();
        };
        compile("VSMain", "vs_4_0", &vertexBlob);
        compile("PSMain", "ps_4_0", &pixelBlob);
        winrt::check_hresult(device_->CreateVertexShader(vertexBlob->GetBufferPointer(),
            vertexBlob->GetBufferSize(), nullptr, &vertexShader_));
        winrt::check_hresult(device_->CreatePixelShader(pixelBlob->GetBufferPointer(),
            pixelBlob->GetBufferSize(), nullptr, &pixelShader_));
        const D3D11_INPUT_ELEMENT_DESC layout[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
        };
        winrt::check_hresult(device_->CreateInputLayout(layout, 2,
            vertexBlob->GetBufferPointer(), vertexBlob->GetBufferSize(), &inputLayout_));

        std::vector<Vertex> vertices;
        std::vector<uint16_t> indices;
        vertices.reserve((kGrid + 1) * (kGrid + 1));
        indices.reserve(kGrid * kGrid * 6);
        for (UINT y = 0; y <= kGrid; ++y)
            for (UINT x = 0; x <= kGrid; ++x) {
                float u = static_cast<float>(x) / kGrid;
                float v = static_cast<float>(y) / kGrid;
                vertices.push_back({u * 1.8f - 0.9f, 0.9f - v * 1.8f, u, v});
            }
        for (UINT y = 0; y < kGrid; ++y)
            for (UINT x = 0; x < kGrid; ++x) {
                uint16_t a = static_cast<uint16_t>(y * (kGrid + 1) + x);
                uint16_t b = static_cast<uint16_t>(a + 1);
                uint16_t c = static_cast<uint16_t>(a + kGrid + 1);
                uint16_t d = static_cast<uint16_t>(c + 1);
                indices.insert(indices.end(), {a, b, c, b, d, c});
            }
        indexCount_ = static_cast<UINT>(indices.size());
        D3D11_BUFFER_DESC buffer{};
        buffer.Usage = D3D11_USAGE_IMMUTABLE;
        buffer.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        buffer.ByteWidth = static_cast<UINT>(vertices.size() * sizeof(Vertex));
        D3D11_SUBRESOURCE_DATA data{vertices.data()};
        winrt::check_hresult(device_->CreateBuffer(&buffer, &data, &vertexBuffer_));
        buffer.BindFlags = D3D11_BIND_INDEX_BUFFER;
        buffer.ByteWidth = static_cast<UINT>(indices.size() * sizeof(uint16_t));
        data.pSysMem = indices.data();
        winrt::check_hresult(device_->CreateBuffer(&buffer, &data, &indexBuffer_));
        buffer.Usage = D3D11_USAGE_DEFAULT;
        buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        buffer.ByteWidth = sizeof(Bend);
        winrt::check_hresult(device_->CreateBuffer(&buffer, nullptr, &bendBuffer_));

        D3D11_SAMPLER_DESC sampler{};
        sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.MaxLOD = D3D11_FLOAT32_MAX;
        winrt::check_hresult(device_->CreateSamplerState(&sampler, &sampler_));
        D3D11_BLEND_DESC blend{};
        blend.RenderTarget[0].BlendEnable = TRUE;
        blend.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        winrt::check_hresult(device_->CreateBlendState(&blend, &blendState_));
    }

    void StartCapture() {
        auto interop = winrt::get_activation_factory<GraphicsCaptureItem,
                                                     IGraphicsCaptureItemInterop>();
        winrt::check_hresult(interop->CreateForWindow(
            target_, winrt::guid_of<GraphicsCaptureItem>(), winrt::put_abi(item_)));
        ComPtr<IDXGIDevice> dxgi;
        winrt::check_hresult(device_.As(&dxgi));
        winrt::com_ptr<::IInspectable> inspectable;
        winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(
            dxgi.Get(), inspectable.put()));
        captureDevice_ = inspectable.as<IDirect3DDevice>();
        frameSize_ = item_.Size();
        pool_ = Direct3D11CaptureFramePool::CreateFreeThreaded(
            captureDevice_, DirectXPixelFormat::B8G8R8A8UIntNormalized,
            2, frameSize_);
        frameToken_ = pool_.FrameArrived([window = window_](auto const&, auto const&) {
            PostMessageW(window, kFrameMessage, 0, 0);
        });
        session_ = pool_.CreateCaptureSession(item_);
        session_.IsCursorCaptureEnabled(false);
        session_.StartCapture();
    }

    void StopCapture() noexcept {
        try {
            if (pool_) pool_.FrameArrived(frameToken_);
            if (session_) session_.Close();
            if (pool_) pool_.Close();
        } catch (...) {
        }
        session_ = nullptr;
        pool_ = nullptr;
        item_ = nullptr;
        captureDevice_ = nullptr;
        ID3D11ShaderResourceView* empty = nullptr;
        if (context_) context_->PSSetShaderResources(0, 1, &empty);
        capturedView_.Reset();
        capturedTexture_.Reset();
        textureWidth_ = textureHeight_ = 0;
        haveFrameInsets_ = false;
        overlayBounds_ = {};
        if (overlayVisible_) ShowWindow(window_, SW_HIDE);
        overlayVisible_ = false;
    }

    void CreateTestTexture() {
        constexpr UINT size = 256;
        std::vector<uint32_t> pixels(size * size);
        for (UINT y = 0; y < size; ++y)
            for (UINT x = 0; x < size; ++x) {
                bool edge = x < 4 || y < 4 || x >= size - 4 || y >= size - 4;
                pixels[y * size + x] = edge ? 0xFFFFFFFFu
                    : (((x / 16) ^ (y / 16)) & 1) ? 0xFF00E0E0u : 0xFF903090u;
            }
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = desc.Height = size;
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA initial{pixels.data(), size * 4};
        winrt::check_hresult(device_->CreateTexture2D(&desc, &initial,
                                                       &capturedTexture_));
        winrt::check_hresult(device_->CreateShaderResourceView(
            capturedTexture_.Get(), nullptr, &capturedView_));
        textureWidth_ = textureHeight_ = size;
    }

    void ReceiveFrames() {
        if (!pool_) return;
        bool resize = false;
        while (auto frame = pool_.TryGetNextFrame()) {
            auto size = frame.ContentSize();
            auto access = frame.Surface().as<
                ::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
            ComPtr<ID3D11Texture2D> source;
            winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(&source)));
            D3D11_TEXTURE2D_DESC desc{};
            source->GetDesc(&desc);
            if (!capturedTexture_ || desc.Width != textureWidth_ ||
                desc.Height != textureHeight_) {
                textureWidth_ = desc.Width;
                textureHeight_ = desc.Height;
                desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                desc.Usage = D3D11_USAGE_DEFAULT;
                desc.CPUAccessFlags = 0;
                desc.MiscFlags = 0;
                capturedView_.Reset();
                capturedTexture_.Reset();
                winrt::check_hresult(device_->CreateTexture2D(&desc, nullptr,
                                                               &capturedTexture_));
                winrt::check_hresult(device_->CreateShaderResourceView(
                    capturedTexture_.Get(), nullptr, &capturedView_));
                if (overlayTest_)
                    std::wprintf(L"Captured window texture: %ux%u\n",
                                 textureWidth_, textureHeight_);
            }
            context_->CopyResource(capturedTexture_.Get(), source.Get());
            ++capturedFrames_;
            frame.Close();
            if (size.Width > 0 && size.Height > 0 &&
                (size.Width != frameSize_.Width || size.Height != frameSize_.Height)) {
                frameSize_ = size;
                resize = true;
            }
        }
        if (resize) pool_.Recreate(captureDevice_,
            DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, frameSize_);
        InvalidateRect(window_, nullptr, FALSE);
    }

    void TryOpenBridge() {
        if (bridge_ || GetTickCount64() - lastBridgeOpenAttempt_ < 1000) return;
        lastBridgeOpenAttempt_ = GetTickCount64();
        HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, mesh_bridge::name);
        if (!mapping) {
            DWORD error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != lastBridgeOpenError_)
                std::fwprintf(stderr, L"Mesh bridge open failed: %u\n", error);
            lastBridgeOpenError_ = error;
            return;
        }
        lastBridgeOpenError_ = ERROR_SUCCESS;
        auto* data = static_cast<const mesh_bridge::Data*>(
            MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(mesh_bridge::Data)));
        if (!data) {
            CloseHandle(mapping);
            return;
        }
        bridgeHandle_ = mapping;
        bridge_ = data;
    }

    bool BridgeReady() {
        TryOpenBridge();
        return bridge_ && bridge_->magic == mesh_bridge::magic &&
            bridge_->version == mesh_bridge::version &&
            bridge_->size == sizeof(mesh_bridge::Data);
    }

    bool ReadPhysics(mesh_bridge::Payload& payload) {
        if (!BridgeReady()) return false;
        for (auto const& slot : bridge_->slots) {
            mesh_bridge::Payload copy{};
            if (!ReadSlot(slot, copy) ||
                copy.hwnd != reinterpret_cast<UINT_PTR>(target_)) continue;
            payload = copy;
            return true;
        }
        return false;
    }

    bool MakePhysicsBend(Bend& bend, double& translationX,
                         double& translationY) {
        mesh_bridge::Payload snapshot{};
        if (!ReadPhysics(snapshot)) return false;
        if (!std::isfinite(snapshot.width) || !std::isfinite(snapshot.height) ||
            snapshot.width < 1 || snapshot.height < 1 ||
            snapshot.width > 20000 || snapshot.height > 20000)
            return false;
        double baseX = 0, baseY = 0, centerX = 0, centerY = 0;
        for (auto const& point : snapshot.points) {
            if (!std::isfinite(point.baseX) || !std::isfinite(point.baseY) ||
                !std::isfinite(point.x) || !std::isfinite(point.y))
                return false;
            baseX += point.baseX;
            baseY += point.baseY;
            centerX += point.x;
            centerY += point.y;
        }
        baseX /= 16; baseY /= 16;
        centerX /= 16; centerY /= 16;
        translationX = centerX - baseX;
        translationY = centerY - baseY;
        if (!std::isfinite(translationX) || !std::isfinite(translationY))
            return false;
        translationX = std::clamp(translationX, -4096.0, 4096.0);
        translationY = std::clamp(translationY, -4096.0, 4096.0);
        double denomX = 0, denomY = 0, xx = 0, xy = 0, yx = 0, yy = 0;
        for (auto const& point : snapshot.points) {
            double bx = point.baseX - baseX;
            double by = point.baseY - baseY;
            double px = point.x - centerX;
            double py = point.y - centerY;
            denomX += bx * bx;
            denomY += by * by;
            xx += bx * px;
            xy += bx * py;
            yx += by * px;
            yy += by * py;
        }
        if (denomX < 1 || denomY < 1) return false;
        double m11 = xx / denomX, m12 = xy / denomX;
        double m21 = yx / denomY, m22 = yy / denomY;
        for (int i = 0; i < 16; ++i) {
            auto const& point = snapshot.points[i];
            double bx = point.baseX - baseX;
            double by = point.baseY - baseY;
            double dx = (point.x - centerX) - m11 * bx - m21 * by;
            double dy = (point.y - centerY) - m12 * bx - m22 * by;
            bend.controls[i][0] = static_cast<float>(
                std::clamp(1.8 * dx / snapshot.width, -0.25, 0.25));
            bend.controls[i][1] = static_cast<float>(
                std::clamp(-1.8 * dy / snapshot.height, -0.25, 0.25));
        }
        bend.options[2] = 1;
        return true;
    }

    void Render() {
        if (!capturedView_ || !renderTarget_) return;
        auto now = std::chrono::steady_clock::now();
        float t = std::chrono::duration<float>(now - started_).count();
        Bend bend{};
        double translationX = 0.0, translationY = 0.0;
        bend.options[3] = overlayTest_ ? (8.0f / 9.0f) : 1.0f;
        if (physicsMode_) {
            bridgeActive_ = MakePhysicsBend(bend, translationX, translationY);
        } else {
            bend.options[0] = 0.16f * std::sin(t * 2.0f);
            bend.options[1] = 0.12f * std::sin(t * 1.6f + 0.7f);
        }
        if (overlayTest_ && !UpdateOverlayBounds(
                bridgeActive_ ? translationX : 0.0,
                bridgeActive_ ? translationY : 0.0))
            return;
        RECT rect{};
        GetClientRect(window_, &rect);
        if (rect.right <= 0 || rect.bottom <= 0) return;
        context_->UpdateSubresource(bendBuffer_.Get(), 0, nullptr, &bend, 0, 0);
        D3D11_VIEWPORT viewport{0, 0, static_cast<float>(rect.right),
                                static_cast<float>(rect.bottom), 0, 1};
        context_->RSSetViewports(1, &viewport);
        const float clear[] = {overlayTest_ ? 0.0f : 0.06f,
                               overlayTest_ ? 0.0f : 0.07f,
                               overlayTest_ ? 0.0f : 0.09f,
                               overlayTest_ ? 0.0f : 1.0f};
        context_->ClearRenderTargetView(renderTarget_.Get(), clear);
        ID3D11RenderTargetView* target = renderTarget_.Get();
        context_->OMSetRenderTargets(1, &target, nullptr);
        context_->OMSetBlendState(blendState_.Get(), nullptr, 0xFFFFFFFF);
        UINT stride = sizeof(Vertex), offset = 0;
        ID3D11Buffer* vertices = vertexBuffer_.Get();
        context_->IASetVertexBuffers(0, 1, &vertices, &stride, &offset);
        context_->IASetIndexBuffer(indexBuffer_.Get(), DXGI_FORMAT_R16_UINT, 0);
        context_->IASetInputLayout(inputLayout_.Get());
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(vertexShader_.Get(), nullptr, 0);
        ID3D11Buffer* constants = bendBuffer_.Get();
        context_->VSSetConstantBuffers(0, 1, &constants);
        context_->PSSetShader(pixelShader_.Get(), nullptr, 0);
        ID3D11ShaderResourceView* image = capturedView_.Get();
        context_->PSSetShaderResources(0, 1, &image);
        ID3D11SamplerState* sampler = sampler_.Get();
        context_->PSSetSamplers(0, 1, &sampler);
        if (!overlayTest_ || bridgeActive_)
            context_->DrawIndexed(indexCount_, 0, 0);
        swapChain_->Present(1, 0);
        ++renderedFrames_;
        if (now - lastStats_ >= std::chrono::seconds(1)) {
            wchar_t title[200];
            std::swprintf(title, std::size(title),
                L"Wobbly GPU preview | capture %llu fps | render %llu fps | %ux%u | physics %ls",
                capturedFrames_, renderedFrames_, textureWidth_, textureHeight_,
                physicsMode_ ? (bridgeActive_ ? L"active" : L"waiting") : L"demo");
            if (!overlayTest_) SetWindowTextW(window_, title);
            std::wprintf(L"%ls\n", title);
            if (overlayTest_) {
                std::wprintf(L"Overlay tracking: HWND=%p window=(%ld,%ld) source=(%ld,%ld) "
                             L"physics=(%.1f,%.1f) overlay=(%ld,%ld) moves=%llu\n",
                             target_,
                             targetWindowBounds_.left, targetWindowBounds_.top,
                             sourceBounds_.left, sourceBounds_.top,
                             overlayPhysicsX_, overlayPhysicsY_,
                             overlayBounds_.left, overlayBounds_.top,
                             overlayMoves_);
                overlayMoves_ = 0;
            }
            capturedFrames_ = renderedFrames_ = 0;
            lastStats_ = now;
        }
    }

    HWND target_ = nullptr;
    HWND window_ = nullptr;
    bool physicsMode_ = false;
    bool overlayTest_ = false;
    bool managed_ = false;
    bool failed_ = false;
    bool overlayVisible_ = false;
    RECT overlayBounds_{};
    RECT targetWindowBounds_{};
    RECT sourceBounds_{};
    RECT frameInsets_{};
    bool haveFrameInsets_ = false;
    unsigned long long overlayMoves_ = 0;
    double overlayPhysicsX_ = 0.0, overlayPhysicsY_ = 0.0;
    bool bridgeActive_ = false;
    HANDLE bridgeHandle_ = nullptr;
    const mesh_bridge::Data* bridge_ = nullptr;
    ULONGLONG lastBridgeOpenAttempt_ = 0;
    DWORD lastBridgeOpenError_ = ERROR_SUCCESS;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGISwapChain1> swapChain_;
    ComPtr<IDCompositionDevice> compositionDevice_;
    ComPtr<IDCompositionTarget> compositionTarget_;
    ComPtr<IDCompositionVisual> compositionVisual_;
    ComPtr<ID3D11RenderTargetView> renderTarget_;
    ComPtr<ID3D11VertexShader> vertexShader_;
    ComPtr<ID3D11PixelShader> pixelShader_;
    ComPtr<ID3D11InputLayout> inputLayout_;
    ComPtr<ID3D11Buffer> vertexBuffer_, indexBuffer_, bendBuffer_;
    ComPtr<ID3D11SamplerState> sampler_;
    ComPtr<ID3D11BlendState> blendState_;
    ComPtr<ID3D11Texture2D> capturedTexture_;
    ComPtr<ID3D11ShaderResourceView> capturedView_;
    UINT textureWidth_ = 0, textureHeight_ = 0, indexCount_ = 0;
    GraphicsCaptureItem item_{nullptr};
    IDirect3DDevice captureDevice_{nullptr};
    Direct3D11CaptureFramePool pool_{nullptr};
    GraphicsCaptureSession session_{nullptr};
    winrt::event_token frameToken_{};
    winrt::Windows::Graphics::SizeInt32 frameSize_{};
    std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point lastStats_ = started_;
    unsigned long long capturedFrames_ = 0, renderedFrames_ = 0;
};

class BridgeScanner {
public:
    ~BridgeScanner() {
        if (data_) UnmapViewOfFile(data_);
        if (mapping_) CloseHandle(mapping_);
    }

    std::vector<HWND> ActiveWindows() {
        if (!data_) {
            ULONGLONG now = GetTickCount64();
            if (now - lastOpenAttempt_ < 1000) return {};
            lastOpenAttempt_ = now;
            mapping_ = OpenFileMappingW(FILE_MAP_READ, FALSE, mesh_bridge::name);
            if (!mapping_) return {};
            data_ = static_cast<const mesh_bridge::Data*>(MapViewOfFile(
                mapping_, FILE_MAP_READ, 0, 0, sizeof(mesh_bridge::Data)));
            if (!data_) {
                CloseHandle(mapping_);
                mapping_ = nullptr;
                return {};
            }
        }
        if (data_->magic != mesh_bridge::magic ||
            data_->version != mesh_bridge::version ||
            data_->size != sizeof(mesh_bridge::Data)) return {};
        std::vector<HWND> windows;
        windows.reserve(mesh_bridge::slot_count);
        for (auto const& slot : data_->slots) {
            mesh_bridge::Payload payload{};
            if (!ReadSlot(slot, payload)) continue;
            HWND hwnd = reinterpret_cast<HWND>(static_cast<UINT_PTR>(payload.hwnd));
            if (!IsWindow(hwnd) || !IsWindowVisible(hwnd) || IsIconic(hwnd)) continue;
            DWORD cloaked = 0;
            DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
            if (!cloaked && std::find(windows.begin(), windows.end(), hwnd) == windows.end())
                windows.push_back(hwnd);
        }
        return windows;
    }

private:
    HANDLE mapping_ = nullptr;
    const mesh_bridge::Data* data_ = nullptr;
    ULONGLONG lastOpenAttempt_ = 0;
};

struct ActiveView {
    std::unique_ptr<Preview> preview;
    ULONGLONG lastSeen;
};

int RunIntegrated() {
    BridgeScanner scanner;
    std::unordered_map<HWND, ActiveView> views;
    std::unordered_map<HWND, ULONGLONG> retryAfter;
    HWND lastRaised = nullptr;
    ULONGLONG lastScan = 0;
    std::wprintf(L"Following up to %u active Wobbly Windows mesh slots. "
                 L"Close this console to stop.\n", mesh_bridge::slot_count);
    for (;;) {
        DWORD wait = MsgWaitForMultipleObjectsEx(
            0, nullptr, 16, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (wait == WAIT_FAILED) winrt::throw_last_error();
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return 0;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        ULONGLONG now = GetTickCount64();
        if (now - lastScan < 16) continue;
        lastScan = now;
        auto active = scanner.ActiveWindows();
        for (HWND hwnd : active) {
            auto existing = views.find(hwnd);
            if (existing != views.end()) {
                existing->second.lastSeen = now;
                continue;
            }
            if (now < retryAfter[hwnd]) continue;
            try {
                auto preview = std::make_unique<Preview>(hwnd, true, true, true);
                views.emplace(hwnd, ActiveView{std::move(preview), now});
                retryAfter.erase(hwnd);
                std::wprintf(L"Following HWND=%p (active views=%zu)\n",
                             hwnd, views.size());
            } catch (winrt::hresult_error const& error) {
                retryAfter[hwnd] = now + 2000;
                std::fwprintf(stderr, L"Capture start failed for HWND=%p: 0x%08X %ls\n",
                              hwnd, static_cast<unsigned>(error.code()),
                              error.message().c_str());
            }
        }
        for (auto it = views.begin(); it != views.end();) {
            if (it->second.preview->failed() || !IsWindow(it->first) ||
                now - it->second.lastSeen > 750) {
                std::wprintf(L"Stopped following HWND=%p\n", it->first);
                if (it->second.preview->failed()) retryAfter[it->first] = now + 2000;
                it = views.erase(it);
            } else ++it;
        }
        HWND foreground = GetForegroundWindow();
        auto front = views.find(foreground);
        if (front != views.end() && lastRaised != foreground) {
            SetWindowPos(front->second.preview->window(), HWND_TOPMOST,
                         0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            lastRaised = foreground;
        } else if (front == views.end()) lastRaised = nullptr;
    }
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        if (argc > 1 && std::wcscmp(argv[1], L"--self-test") == 0) {
            ComPtr<ID3D11Device> device;
            D3D_FEATURE_LEVEL level;
            winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE,
                nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                D3D11_SDK_VERSION, &device, &level, nullptr));
            ComPtr<ID3DBlob> vertex, pixel;
            winrt::check_hresult(D3DCompile(kShaders, sizeof(kShaders) - 1,
                nullptr, nullptr, nullptr, "VSMain", "vs_4_0", 0, 0,
                &vertex, nullptr));
            winrt::check_hresult(D3DCompile(kShaders, sizeof(kShaders) - 1,
                nullptr, nullptr, nullptr, "PSMain", "ps_4_0", 0, 0,
                &pixel, nullptr));
            bool supported = false;
            try {
                supported = GraphicsCaptureSession::IsSupported();
                std::wprintf(L"WGC=%ls\n", supported ? L"available" : L"unavailable");
            } catch (winrt::hresult_error const& error) {
                std::wprintf(L"WGC query failed: 0x%08X %ls\n",
                    static_cast<unsigned>(error.code()), error.message().c_str());
            }
            std::wprintf(L"D3D11=available shaders=compiled\n");
            return supported ? 0 : 1;
        }
        if (argc > 1 && std::wcscmp(argv[1], L"--demo") == 0) {
            std::wprintf(L"GPU mesh demo without window capture. Close the preview to stop.\n");
            Preview preview(nullptr);
            preview.Run();
            return 0;
        }
        bool followBridge = argc > 1 && std::wcscmp(argv[1], L"--follow-bridge") == 0;
        if (followBridge) return RunIntegrated();
        bool overlayTest = argc > 1 && std::wcscmp(argv[1], L"--overlay-test") == 0;
        bool physicsMode = overlayTest ||
            (argc > 1 && std::wcscmp(argv[1], L"--physics") == 0);
        std::wprintf(L"Activate a normal window to capture within five seconds.\n");
        for (int seconds = 5; seconds > 0; --seconds) {
            std::wprintf(L"%d...\n", seconds);
            Sleep(1000);
        }
        HWND target = GetForegroundWindow();
        if (!target || !IsWindow(target))
            throw std::runtime_error("No window selected");
        wchar_t title[256]{};
        GetWindowTextW(target, title, std::size(title));
        std::wprintf(L"Capturing HWND=%p (%ls). Close the preview to stop.\n",
                     target, title);
        Preview preview(target, physicsMode, overlayTest);
        preview.Run();
        return 0;
    } catch (winrt::hresult_error const& error) {
        std::fwprintf(stderr, L"Error: 0x%08X %ls\n",
                      static_cast<unsigned>(error.code()), error.message().c_str());
    } catch (std::exception const& error) {
        std::fprintf(stderr, "Error: %s\n", error.what());
    }
    return 1;
}
