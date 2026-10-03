// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "vr/openxr_d3d12.h"
#include "vr/openxr_vulkan_win32.h"
#include <variant>
namespace PortVr {
// Runtime API selection; both backends expose the same pacing/ownership contract.
class OpenXRWindowsBackend {
    std::variant<std::unique_ptr<OpenXRD3D12Backend>, std::unique_ptr<OpenXRWindowsVulkanBackend>> backend_;
public:
    OpenXRWindowsBackend(OpenXRLogCallback logger, bool vulkan) {
        if (vulkan) backend_ = std::make_unique<OpenXRWindowsVulkanBackend>(logger);
        else backend_ = std::make_unique<OpenXRD3D12Backend>(logger);
    }
    const OpenXRD3D12GraphicsRequirements& GraphicsRequirements() {
        return std::get<0>(backend_)->GraphicsRequirements();
    }
    bool QueryGraphicsRequirements(OpenXRRuntime& runtime) {
        return std::visit([&](auto& backend) -> bool { return backend->QueryGraphicsRequirements(runtime); }, backend_);
    }
    bool BindAurora(OpenXRRuntime& runtime) {
        return std::visit([&](auto& backend) -> bool { return backend->BindAurora(runtime); }, backend_);
    }
    void SetRenderScale(float scale) {
        std::visit([&](auto& backend) { backend->SetRenderScale(scale); }, backend_);
    }
    OpenXRBeginStatus BeginFrame(const OpenXRPresentation& presentation, OpenXRBackendFrame& frame) {
        return std::visit([&](auto& backend) -> OpenXRBeginStatus { return backend->BeginFrame(presentation, frame); }, backend_);
    }
    OpenXRSubmissionStatus WaitForSubmission(const OpenXRBackendFrame& frame, uint32_t timeout) {
        return std::visit([&](auto& backend) -> OpenXRSubmissionStatus { return backend->WaitForSubmission(frame, timeout); }, backend_);
    }
    bool TryCancelPendingFrame(OpenXRBackendFrame& frame) {
        return std::visit([&](auto& backend) -> bool { return backend->TryCancelPendingFrame(frame); }, backend_);
    }
    bool RepeatFrame(const OpenXRBackendFrame& frame) {
        return std::visit([&](auto& backend) -> bool { return backend->RepeatFrame(frame); }, backend_);
    }
    bool FinishFrame(OpenXRBackendFrame& frame, bool submit) {
        return std::visit([&](auto& backend) -> bool { return backend->FinishFrame(frame, submit); }, backend_);
    }
    OpenXRBeginStatus PreparePacket(const OpenXRPresentation& presentation, OpenXRBackendFrame& frame) {
        return std::visit([&](auto& backend) -> OpenXRBeginStatus { return backend->PreparePacket(presentation, frame); }, backend_);
    }
    bool TryCancelPendingPacket(OpenXRBackendFrame& frame) {
        return std::visit([&](auto& backend) -> bool { return backend->TryCancelPendingPacket(frame); }, backend_);
    }
    OpenXRBeginStatus BeginFrameForPacket(const OpenXRBackendFrame& packet, OpenXRBackendFrame& frame) {
        return std::visit([&](auto& backend) -> OpenXRBeginStatus { return backend->BeginFrameForPacket(packet, frame); }, backend_);
    }
    OpenXRSubmissionStatus CopyRenderedEyes(const OpenXRBackendFrame& frame) {
        return std::visit([&](auto& backend) -> OpenXRSubmissionStatus { return backend->CopyRenderedEyes(frame); }, backend_);
    }
    OpenXRBeginStatus KeepAliveCycle() {
        return std::visit([&](auto& backend) -> OpenXRBeginStatus { return backend->KeepAliveCycle(); }, backend_);
    }
    bool Shutdown() {
        return std::visit([&](auto& backend) -> bool { return backend->Shutdown(); }, backend_);
    }
    bool PanelLayerAvailable() {
        return std::visit([&](auto& backend) -> bool { return backend->PanelLayerAvailable(); }, backend_);
    }
    int64_t SwapchainFormat() {
        return std::visit([&](auto& backend) -> int64_t { return backend->SwapchainFormat(); }, backend_);
    }
    const std::string& LastError() {
        return std::visit([&](auto& backend) -> const std::string& { return backend->LastError(); }, backend_);
    }
};
}
