#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;

namespace {
constexpr UINT kFrameMessage = WM_APP + 1;
constexpr UINT kGrid = 20;
constexpr char kShaders[] = R"(
cbuffer Bend : register(b0) { float2 amount; float2 padding; };
struct Vertex { float2 position : POSITION; float2 uv : TEXCOORD0; };
struct Pixel { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };
Pixel VSMain(Vertex input) {
    Pixel output;
    float2 p = input.position;
    float envelope = (1.0 - p.x * p.x) * (1.0 - p.y * p.y);
    p += amount * envelope;
    output.position = float4(p, 0.0, 1.0);
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
    float x, y, pad1, pad2;
};

class Preview {
public:
    explicit Preview(HWND target) : target_(target) {
        CreateWindowAndDevice();
        if (target_) StartCapture();
        else CreateTestTexture();
        InvalidateRect(window_, nullptr, FALSE);
    }

    ~Preview() {
        if (pool_) pool_.FrameArrived(frameToken_);
        if (session_) session_.Close();
        if (pool_) pool_.Close();
    }

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
                    DestroyWindow(window);
                }
                return 0;
            case WM_PAINT: {
                PAINTSTRUCT paint;
                BeginPaint(window, &paint);
                self->Render();
                EndPaint(window, &paint);
                if (self->capturedView_ && !IsIconic(window))
                    InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
            case WM_ERASEBKGND:
                return 1;
            case WM_SIZE:
                if (self->swapChain_ && LOWORD(lparam) && HIWORD(lparam))
                    self->Resize(LOWORD(lparam), HIWORD(lparam));
                return 0;
            case WM_DESTROY:
                PostQuitMessage(0);
                return 0;
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
        if (!RegisterClassW(&windowClass)) winrt::throw_last_error();
        window_ = CreateWindowExW(0, windowClass.lpszClassName,
                                  L"Wobbly GPU mesh preview", WS_OVERLAPPEDWINDOW,
                                  CW_USEDEFAULT, CW_USEDEFAULT, 1050, 750,
                                  nullptr, nullptr, windowClass.hInstance, this);
        if (!window_) winrt::throw_last_error();

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
        description.Width = 1050;
        description.Height = 750;
        description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        description.BufferCount = 2;
        description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        winrt::check_hresult(factory->CreateSwapChainForHwnd(
            device_.Get(), window_, &description, nullptr, nullptr, &swapChain_));
        CreateRenderTarget();
        CreatePipeline();
        ShowWindow(window_, SW_SHOW);
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

    void Render() {
        if (!capturedView_ || !renderTarget_) return;
        RECT rect{};
        GetClientRect(window_, &rect);
        if (rect.right <= 0 || rect.bottom <= 0) return;
        auto now = std::chrono::steady_clock::now();
        float t = std::chrono::duration<float>(now - started_).count();
        Bend bend{0.16f * std::sin(t * 2.0f),
                  0.12f * std::sin(t * 1.6f + 0.7f), 0, 0};
        context_->UpdateSubresource(bendBuffer_.Get(), 0, nullptr, &bend, 0, 0);
        D3D11_VIEWPORT viewport{0, 0, static_cast<float>(rect.right),
                                static_cast<float>(rect.bottom), 0, 1};
        context_->RSSetViewports(1, &viewport);
        const float clear[] = {0.06f, 0.07f, 0.09f, 1};
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
        context_->DrawIndexed(indexCount_, 0, 0);
        swapChain_->Present(1, 0);
        ++renderedFrames_;
        if (now - lastStats_ >= std::chrono::seconds(1)) {
            wchar_t title[160];
            std::swprintf(title, std::size(title),
                L"Wobbly GPU preview | capture %llu fps | render %llu fps | %ux%u",
                capturedFrames_, renderedFrames_, textureWidth_, textureHeight_);
            SetWindowTextW(window_, title);
            std::wprintf(L"%ls\n", title);
            capturedFrames_ = renderedFrames_ = 0;
            lastStats_ = now;
        }
    }

    HWND target_ = nullptr;
    HWND window_ = nullptr;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGISwapChain1> swapChain_;
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
} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
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
        Preview preview(target);
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
