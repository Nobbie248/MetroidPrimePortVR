# SPDX-License-Identifier: GPL-3.0-or-later
"""Apply PrimedGun's Quest patches to the Dawn source tree the port pins (encounter/dawn).

Vulkan multiview, so one render pass draws both eyes:
- Tint: the `@builtin(view_index)` input (vertex and fragment stages), behind
  `enable chromium_experimental_multiview;`, emitted as SPIR-V ViewIndex (MultiView capability).
- Dawn: the `DawnMultiview` feature (Vulkan's `multiview`, at least two views), and the
  `DawnRenderPassMultiview` / `DawnRenderPipelineMultiview` chained structs that carry a view mask.
  The mask is part of the attachment state, so a pipeline only binds in a pass with the same mask;
  render attachments may then be 2D-array views with a layer per view. The Vulkan backend sets it on
  dynamic rendering (VkRenderingInfo / VkPipelineRenderingCreateInfo) and on render pass objects
  (subpass view masks, or VkRenderPassMultiviewCreateInfo).

Direct presentation (aurora_vulkan_interop.inc, after Wiicompiled_VR's Windows Vulkan binding): while
Aurora has installed its hooks, Dawn creates its VkInstance and VkDevice through the OpenXR runtime
(XR_KHR_vulkan_enable2), so the session can be bound to Dawn's own device; C entry points
(aurora/dawn_vulkan_abi.h) hand Aurora the device's handles and lock, and wrap the runtime's swapchain
images as Dawn textures.

Every edit is anchored on Dawn source text, asserts that the anchor occurs once, and is skipped when
already applied, so the script can run again on a patched tree. Usage: apply.py <dawn source root>
"""
from pathlib import Path
import sys

root = Path(sys.argv[1]).resolve()


def edit(rel, old, new):
    """Replace the one occurrence of `old` with `new` in `rel`, unless `new` is already there."""
    path = root / rel
    text = path.read_text(encoding="utf-8")
    if new in text:
        return
    count = text.count(old)
    assert count == 1, f"{rel}: expected one occurrence of anchor, found {count}:\n{old}"
    path.write_text(text.replace(old, new, 1), encoding="utf-8", newline="")


def insert_after(rel, anchor, text):
    edit(rel, anchor, anchor + text)


def insert_before(rel, anchor, text):
    edit(rel, anchor, text + anchor)


# ---------------------------------------------------------------------------------------------
# Tint: the view_index builtin and the chromium_experimental_multiview extension.
# ---------------------------------------------------------------------------------------------
TINT = "src/tint/lang"

insert_after(f"{TINT}/core/core.def", "  primitive_index\n", "  view_index\n")
insert_after(f"{TINT}/wgsl/wgsl.def",
             "  // A Chromium-specific extension for barycentric_coord support\n"
             "  chromium_experimental_barycentric_coord\n",
             "  // A PrimedGun extension for @builtin(view_index) under Vulkan multiview\n"
             "  chromium_experimental_multiview\n")

# Appended last, so no existing enumerator moves.
edit(f"{TINT}/core/enums.h",
     "    kWorkgroupIndex,\n};\n\n/// @param value the enum value\n/// @returns the string for the given enum value\n"
     "std::string_view ToString(BuiltinValue value);",
     "    kWorkgroupIndex,\n    kViewIndex,\n};\n\n/// @param value the enum value\n"
     "/// @returns the string for the given enum value\nstd::string_view ToString(BuiltinValue value);")
edit(f"{TINT}/core/enums.h",
     '    "workgroup_index",\n};\n\n/// Builtin depth mode',
     '    "workgroup_index",\n    "view_index",\n};\n\n/// Builtin depth mode')
edit(f"{TINT}/core/enums.cc",
     '    if (str == "workgroup_index") {\n        return BuiltinValue::kWorkgroupIndex;\n    }\n'
     '    return BuiltinValue::kUndefined;',
     '    if (str == "workgroup_index") {\n        return BuiltinValue::kWorkgroupIndex;\n    }\n'
     '    if (str == "view_index") {\n        return BuiltinValue::kViewIndex;\n    }\n'
     '    return BuiltinValue::kUndefined;')
insert_after(f"{TINT}/core/enums.cc",
             '        case BuiltinValue::kWorkgroupIndex:\n            return "workgroup_index";\n',
             '        case BuiltinValue::kViewIndex:\n            return "view_index";\n')

edit(f"{TINT}/wgsl/enums.h",
     "    kSubgroups,\n};\n\n/// @param value the enum value\n/// @returns the string for the given enum value\n"
     "std::string_view ToString(Extension value);",
     "    kSubgroups,\n    kChromiumExperimentalMultiview,\n};\n\n/// @param value the enum value\n"
     "/// @returns the string for the given enum value\nstd::string_view ToString(Extension value);")
edit(f"{TINT}/wgsl/enums.h",
     '    "subgroups",\n};\n\n/// All extensions',
     '    "subgroups",\n    "chromium_experimental_multiview",\n};\n\n/// All extensions')
edit(f"{TINT}/wgsl/enums.h",
     "    Extension::kSubgroups,\n};\n\n/// An enumerator of WGSL language features",
     "    Extension::kSubgroups,\n    Extension::kChromiumExperimentalMultiview,\n};\n\n"
     "/// An enumerator of WGSL language features")
edit(f"{TINT}/wgsl/enums.cc",
     '    if (str == "subgroups") {\n        return Extension::kSubgroups;\n    }\n'
     '    return Extension::kUndefined;',
     '    if (str == "subgroups") {\n        return Extension::kSubgroups;\n    }\n'
     '    if (str == "chromium_experimental_multiview") {\n'
     '        return Extension::kChromiumExperimentalMultiview;\n    }\n'
     '    return Extension::kUndefined;')
insert_after(f"{TINT}/wgsl/enums.cc",
             '        case Extension::kSubgroups:\n            return "subgroups";\n',
             '        case Extension::kChromiumExperimentalMultiview:\n'
             '            return "chromium_experimental_multiview";\n')

insert_before(f"{TINT}/wgsl/resolver/validator.cc",
              "        case core::BuiltinValue::kBarycentricCoord: {\n"
              "            if (!enabled_extensions_.Contains(\n",
              "        case core::BuiltinValue::kViewIndex: {\n"
              "            if (!enabled_extensions_.Contains(wgsl::Extension::kChromiumExperimentalMultiview)) {\n"
              "                AddError(attr->source)\n"
              '                    << "use of " << style::Attribute("@builtin")\n'
              '                    << style::Code("(", style::Enum(builtin), ")")\n'
              '                    << " requires enabling extension "\n'
              '                    << style::Code("chromium_experimental_multiview");\n'
              "                return false;\n"
              "            }\n"
              "            if (!type->Is<core::type::U32>()) {\n"
              '                err_builtin_type("u32");\n'
              "                return false;\n"
              "            }\n"
              "            if (stage != ast::PipelineStage::kNone &&\n"
              "                !((stage == ast::PipelineStage::kVertex ||\n"
              "                   stage == ast::PipelineStage::kFragment) &&\n"
              "                  is_input)) {\n"
              "                is_stage_mismatch = true;\n"
              "            }\n"
              "            break;\n"
              "        }\n")

insert_before(f"{TINT}/core/ir/io_attribute_validator.cc",
              "/// @returns an appropriate BuiltInCheck for @p builtin, ICEs when one isn't defined\n",
              "constexpr BuiltInChecker kViewIndexChecker{\n"
              "    .valid_usages = EnumSet<IOAttributeUsage>{IOAttributeUsage::kVertexInputUsage,\n"
              "                                              IOAttributeUsage::kFragmentInputUsage},\n"
              "    .type_check = [](const core::type::Type* ty, const Properties&) -> bool {\n"
              "        return ty->Is<core::type::U32>();\n"
              "    },\n"
              '    .type_error = "must be an u32",\n'
              "};\n\n")
insert_before(f"{TINT}/core/ir/io_attribute_validator.cc",
              "        case BuiltinValue::kBarycentricCoord:\n            return kBarycentricCoordChecker;\n",
              "        case BuiltinValue::kViewIndex:\n            return kViewIndexChecker;\n")

insert_before(f"{TINT}/spirv/writer/printer/printer.cc",
              "            case core::BuiltinValue::kBarycentricCoord:\n"
              '                module_.PushExtension("SPV_KHR_fragment_shader_barycentric");\n',
              "            case core::BuiltinValue::kViewIndex:\n"
              '                module_.PushExtension("SPV_KHR_multiview");\n'
              "                module_.PushCapability(SpvCapabilityMultiView);\n"
              "                return SpvBuiltInViewIndex;\n")

# ---------------------------------------------------------------------------------------------
# Dawn API: the feature and the two chained structs (generated into webgpu.h).
# ---------------------------------------------------------------------------------------------
DAWN_JSON = "src/dawn/dawn.json"
edit(DAWN_JSON,
     '            {"value": 66, "name": "dawn allow undefined load store op", "tags": ["dawn", "native"]}\n'
     '        ]',
     '            {"value": 66, "name": "dawn allow undefined load store op", "tags": ["dawn", "native"]},\n'
     '            {"value": 67, "name": "dawn multiview", "tags": ["dawn"]}\n'
     '        ]')
edit(DAWN_JSON,
     '            {"value": 81, "name": "adapter properties drm", "tags": ["dawn", "native"]}\n'
     '        ]',
     '            {"value": 81, "name": "adapter properties drm", "tags": ["dawn", "native"]},\n'
     '            {"value": 82, "name": "dawn render pass multiview", "tags": ["dawn"]},\n'
     '            {"value": 83, "name": "dawn render pipeline multiview", "tags": ["dawn"]}\n'
     '        ]')
insert_before(DAWN_JSON,
              '    "render pass descriptor resolve rect": {\n',
              '    "dawn render pass multiview": {\n'
              '        "category": "structure",\n'
              '        "tags": ["dawn"],\n'
              '        "chained": "in",\n'
              '        "chain roots": ["render pass descriptor"],\n'
              '        "members": [\n'
              '            {"name": "view mask", "type": "uint32_t"}\n'
              '        ]\n'
              '    },\n'
              '    "dawn render pipeline multiview": {\n'
              '        "category": "structure",\n'
              '        "tags": ["dawn"],\n'
              '        "chained": "in",\n'
              '        "chain roots": ["render pipeline descriptor"],\n'
              '        "members": [\n'
              '            {"name": "view mask", "type": "uint32_t"}\n'
              '        ]\n'
              '    },\n')

NATIVE = "src/dawn/native"
insert_before(f"{NATIVE}/Features.cpp",
              "\n    // Comment to separate the } so it is clearer what to copy-paste to add a feature.\n",
              "    {Feature::DawnMultiview,\n"
              '     {"Supports DawnRenderPassMultiview / DawnRenderPipelineMultiview view masks and the "\n'
              '      "\\"enable chromium_experimental_multiview;\\" directive (@builtin(view_index)) in WGSL.",\n'
              '      "https://registry.khronos.org/vulkan/specs/latest/man/html/VK_KHR_multiview.html",\n'
              "      FeatureInfo::FeatureState::Stable}},\n")
insert_after(f"{NATIVE}/Device.cpp",
             "    if (mEnabledFeatures.IsEnabled(Feature::SubgroupSizeControl)) {\n"
             "        mWGSLAllowedFeatures.extensions.insert(tint::wgsl::Extension::kSubgroupSizeControl);\n"
             "    }\n",
             "    if (mEnabledFeatures.IsEnabled(Feature::DawnMultiview)) {\n"
             "        mWGSLAllowedFeatures.extensions.insert(\n"
             "            tint::wgsl::Extension::kChromiumExperimentalMultiview);\n"
             "    }\n")

# The view mask is part of the attachment state: passes and pipelines must agree on it.
insert_after(f"{NATIVE}/AttachmentState.h",
             "    bool HasPixelLocalStorage() const;\n",
             "    // DawnRenderPassMultiview / DawnRenderPipelineMultiview; 0 without multiview.\n"
             "    uint32_t GetViewMask() const;\n")
insert_after(f"{NATIVE}/AttachmentState.h",
             "    std::vector<wgpu::TextureFormat> mStorageAttachmentSlots;\n",
             "    uint32_t mViewMask = 0;\n")
insert_after(f"{NATIVE}/AttachmentState.cpp",
             "    mHasPLS = layout->HasPixelLocalStorage();\n"
             "    mStorageAttachmentSlots = layout->GetStorageAttachmentSlots();\n",
             "    if (auto* multiview = descriptor.Get<DawnRenderPipelineMultiview>()) {\n"
             "        mViewMask = multiview->viewMask;\n"
             "    }\n")
insert_before(f"{NATIVE}/AttachmentState.cpp",
              "    DAWN_CHECK(mSampleCount > 0);\n    SetContentHash(ComputeContentHash());\n}\n",
              "    if (auto* multiview = descriptor.Get<DawnRenderPassMultiview>()) {\n"
              "        mViewMask = multiview->viewMask;\n"
              "    }\n\n")
insert_after(f"{NATIVE}/AttachmentState.cpp",
             "    mStorageAttachmentSlots = blueprint.mStorageAttachmentSlots;\n",
             "    mViewMask = blueprint.mViewMask;\n")
edit(f"{NATIVE}/AttachmentState.cpp",
     "    // Check PLS\n    if (a->mHasPLS != b->mHasPLS) {",
     "    if (a->mViewMask != b->mViewMask) {\n        return false;\n    }\n\n"
     "    // Check PLS\n    if (a->mHasPLS != b->mHasPLS) {")
edit(f"{NATIVE}/AttachmentState.cpp",
     "    // Hash the PLS state\n    HashCombine(&hash, mHasPLS);",
     "    HashCombine(&hash, mViewMask);\n\n    // Hash the PLS state\n    HashCombine(&hash, mHasPLS);")
insert_before(f"{NATIVE}/AttachmentState.cpp",
              "const std::vector<wgpu::TextureFormat>& AttachmentState::GetStorageAttachmentSlots() const {\n",
              "uint32_t AttachmentState::GetViewMask() const {\n    return mViewMask;\n}\n\n")

# Validation: layered attachments only for multiview passes, one layer per view.
edit(f"{NATIVE}/CommandEncoder.cpp",
     "    // Currently we do not support layered rendering.\n"
     "    DAWN_INVALID_IF(attachment->GetLayerCount() > 1,",
     "    // Layered rendering only through DawnMultiview: ValidateRenderPassDescriptor checks the\n"
     "    // layers against the pass's view mask.\n"
     "    DAWN_INVALID_IF(attachment->GetLayerCount() > 1 &&\n"
     "                        !attachment->GetDevice()->HasFeature(Feature::DawnMultiview),")
insert_before(f"{NATIVE}/CommandEncoder.cpp",
              "    if (auto area = descriptor.Get<RenderPassRenderAreaRect>()) {\n"
              "        DAWN_INVALID_IF(!device->HasFeature(Feature::RenderPassRenderArea),\n",
              "    {\n"
              "        const auto* multiview = descriptor.Get<DawnRenderPassMultiview>();\n"
              "        DAWN_INVALID_IF(multiview != nullptr && !device->HasFeature(Feature::DawnMultiview),\n"
              '                        "DawnRenderPassMultiview can\'t be used without %s.",\n'
              "                        ToAPI(Feature::DawnMultiview));\n"
              "        DAWN_INVALID_IF(multiview != nullptr && multiview->viewMask == 0,\n"
              '                        "DawnRenderPassMultiview has an empty view mask.");\n'
              "        // Every attachment has exactly one layer per view (one layer without multiview).\n"
              "        uint32_t layers = 1u;\n"
              "        if (multiview != nullptr) {\n"
              "            layers = 0u;\n"
              "            for (uint32_t mask = multiview->viewMask; mask != 0u; mask >>= 1u) {\n"
              "                ++layers;\n"
              "            }\n"
              "        }\n"
              "        for (const auto& attachment : descriptor->colorAttachments) {\n"
              "            DAWN_INVALID_IF(attachment.view != nullptr && attachment.view->GetLayerCount() != layers,\n"
              '                            "Color attachment %s has %u layers where the view mask needs %u.",\n'
              "                            attachment.view, attachment.view->GetLayerCount(), layers);\n"
              "        }\n"
              "        if (descriptor->depthStencilAttachment != nullptr) {\n"
              "            const TextureViewBase* view = descriptor->depthStencilAttachment->view;\n"
              "            DAWN_INVALID_IF(view != nullptr && view->GetLayerCount() != layers,\n"
              '                            "Depth-stencil attachment %s has %u layers where the view mask needs %u.",\n'
              "                            view, view->GetLayerCount(), layers);\n"
              "        }\n"
              "    }\n\n")
# Lazy clears handle the whole subresource range of an attachment: under multiview, its layers.
edit(f"{NATIVE}/CommandBuffer.cpp",
     "        bool hasResolveTarget = attachmentInfo.resolveTarget != nullptr;\n\n"
     "        DAWN_CHECK(view->GetLayerCount() == 1);\n",
     "        bool hasResolveTarget = attachmentInfo.resolveTarget != nullptr;\n\n"
     "        // One layer per view under DawnMultiview.\n"
     "        DAWN_CHECK(view->GetLayerCount() == 1 || renderPass->attachmentState->GetViewMask() != 0);\n")
edit(f"{NATIVE}/CommandBuffer.cpp",
     "        TextureViewBase* view = attachmentInfo.view.Get();\n"
     "        DAWN_CHECK(view->GetLayerCount() == 1);\n"
     "        DAWN_CHECK(view->GetLevelCount() == 1);\n"
     "        SubresourceRange range = view->GetSubresourceRange();\n\n"
     "        SubresourceRange depthRange = range;\n",
     "        TextureViewBase* view = attachmentInfo.view.Get();\n"
     "        // One layer per view under DawnMultiview.\n"
     "        DAWN_CHECK(view->GetLayerCount() == 1 || renderPass->attachmentState->GetViewMask() != 0);\n"
     "        DAWN_CHECK(view->GetLevelCount() == 1);\n"
     "        SubresourceRange range = view->GetSubresourceRange();\n\n"
     "        SubresourceRange depthRange = range;\n")
edit(f"{NATIVE}/RenderPipeline.cpp",
     "MaybeError ValidateRenderPipelineDescriptor(DeviceBase* device,\n"
     "                                            const RenderPipelineDescriptor* descriptor) {\n"
     "    UnpackedPtr<RenderPipelineDescriptor> unpacked;\n"
     "    DAWN_TRY_ASSIGN(unpacked, ValidateAndUnpack(descriptor));\n",
     "MaybeError ValidateRenderPipelineDescriptor(DeviceBase* device,\n"
     "                                            const RenderPipelineDescriptor* descriptor) {\n"
     "    UnpackedPtr<RenderPipelineDescriptor> unpacked;\n"
     "    DAWN_TRY_ASSIGN(unpacked, ValidateAndUnpack(descriptor));\n"
     "\n"
     "    if (const auto* multiview = unpacked.Get<DawnRenderPipelineMultiview>()) {\n"
     "        DAWN_INVALID_IF(!device->HasFeature(Feature::DawnMultiview),\n"
     '                        "DawnRenderPipelineMultiview can\'t be used without %s.",\n'
     "                        ToAPI(Feature::DawnMultiview));\n"
     "        DAWN_INVALID_IF(multiview->viewMask == 0,\n"
     '                        "DawnRenderPipelineMultiview has an empty view mask.");\n'
     "    }\n")

# ---------------------------------------------------------------------------------------------
# Vulkan backend.
# ---------------------------------------------------------------------------------------------
VK = f"{NATIVE}/vulkan"
insert_after(f"{VK}/VulkanInfo.h",
             "    VkPhysicalDeviceShaderSubgroupUniformControlFlowFeaturesKHR\n"
             "        shaderSubgroupUniformControlFlowFeatures;\n",
             "    VkPhysicalDeviceMultiviewFeatures multiviewFeatures;\n")
insert_after(f"{VK}/VulkanInfo.h",
             "    VkPhysicalDeviceDrmPropertiesEXT drmProperties;\n",
             "    VkPhysicalDeviceMultiviewProperties multiviewProperties;\n")
insert_after(f"{VK}/VulkanInfo.cpp",
             "    featuresChain.Add(&info.extendedDynamicStateFeatures,\n"
             "                      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT);\n",
             "    // Multiview is core in Vulkan 1.1, which Dawn requires.\n"
             "    featuresChain.Add(&info.multiviewFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES);\n"
             "    propertiesChain.Add(&info.multiviewProperties,\n"
             "                        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_PROPERTIES);\n")
insert_after(f"{VK}/PhysicalDeviceVk.cpp",
             "    if (mDeviceInfo.features.shaderClipDistance == VK_TRUE) {\n"
             "        EnableFeature(Feature::ClipDistances);\n"
             "    }\n",
             "\n"
             "    if (mDeviceInfo.multiviewFeatures.multiview == VK_TRUE &&\n"
             "        mDeviceInfo.multiviewProperties.maxMultiviewViewCount >= 2) {\n"
             "        EnableFeature(Feature::DawnMultiview);\n"
             "    }\n")
insert_before(f"{VK}/DeviceVk.cpp",
              "    // Find a universal queue family\n",
              "    if (HasFeature(Feature::DawnMultiview)) {\n"
              "        usedKnobs.multiviewFeatures = {};\n"
              "        usedKnobs.multiviewFeatures.multiview = VK_TRUE;\n"
              "        featuresChain.Add(&usedKnobs.multiviewFeatures,\n"
              "                          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES);\n"
              "    }\n\n")

edit(f"{VK}/RenderPassCache.h",
     "    uint32_t sampleCount;\n};",
     "    uint32_t sampleCount;\n    // Vulkan multiview (DawnMultiview); 0 renders a single view.\n    uint32_t viewMask = 0;\n};")
insert_after(f"{VK}/RenderPassCache.cpp",
             "    HashCombine(&hash, query.sampleCount);\n",
             "    HashCombine(&hash, query.viewMask);\n")
insert_after(f"{VK}/RenderPassCache.cpp",
             "    if (a.sampleCount != b.sampleCount) {\n        return false;\n    }\n",
             "\n    if (a.viewMask != b.viewMask) {\n        return false;\n    }\n")
insert_after(f"{VK}/RenderPassCache.cpp",
             "            RenderPassCreateInfo2 passInfo2;\n"
             "            InitializePassInfo(mDevice, query, passInfo2);\n",
             "            for (uint32_t subpass = 0; subpass < passInfo2.createInfo.subpassCount; ++subpass) {\n"
             "                passInfo2.subpassDescs[subpass].viewMask = query.viewMask;\n"
             "            }\n")
insert_after(f"{VK}/RenderPassCache.cpp",
             "            RenderPassCreateInfo passInfo;\n"
             "            InitializePassInfo(mDevice, query, passInfo);\n",
             "            std::array<uint32_t, 2> viewMasks = {query.viewMask, query.viewMask};\n"
             "            VkRenderPassMultiviewCreateInfo multiview = {};\n"
             "            if (query.viewMask != 0) {\n"
             "                multiview.sType = VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO;\n"
             "                multiview.pNext = passInfo.createInfo.pNext;\n"
             "                multiview.subpassCount = passInfo.createInfo.subpassCount;\n"
             "                multiview.pViewMasks = viewMasks.data();\n"
             "                passInfo.createInfo.pNext = &multiview;\n"
             "            }\n")
edit(f"{VK}/StreamImplVk.cpp",
     "    StreamIn(sink, t.colorMask.to_ulong(), t.resolveTargetMask.to_ulong(), t.sampleCount);\n",
     "    StreamIn(sink, t.colorMask.to_ulong(), t.resolveTargetMask.to_ulong(), t.sampleCount, t.viewMask);\n")
edit(f"{VK}/CommandBufferVk.cpp",
     "    renderInfo.layerCount = 1;\n    renderInfo.viewMask = 0;\n",
     "    renderInfo.layerCount = 1;\n    renderInfo.viewMask = renderPass->attachmentState->GetViewMask();\n")
insert_after(f"{VK}/CommandBufferVk.cpp",
             "        query.SetSampleCount(renderPass->attachmentState->GetSampleCount());\n",
             "        query.viewMask = renderPass->attachmentState->GetViewMask();\n")
edit(f"{VK}/RenderPipelineVk.cpp",
     "        pipelineRenderingCreateInfo.viewMask = 0;\n",
     "        pipelineRenderingCreateInfo.viewMask = GetAttachmentState()->GetViewMask();\n")
insert_after(f"{VK}/RenderPipelineVk.cpp",
             "        query.SetSampleCount(GetSampleCount());\n",
             "        query.viewMask = GetAttachmentState()->GetViewMask();\n")

# ---------------------------------------------------------------------------------------------
# Direct presentation (aurora_vulkan_interop.inc, Aurora's aurora/dawn_vulkan_abi.h): the OpenXR
# runtime creates Dawn's Vulkan instance and device through hooks, and Dawn wraps the runtime's
# swapchain images, so the eyes are copied straight into them on Dawn's queue.
# ---------------------------------------------------------------------------------------------
here = Path(__file__).resolve().parent
repo = here.parent.parent


def copy_in(source, rel):
    path = root / rel
    data = source.read_bytes()
    if not path.exists() or path.read_bytes() != data:
        path.write_bytes(data)


copy_in(here / "aurora_vulkan_hooks.h", f"{VK}/aurora_vulkan_hooks.h")
copy_in(here / "aurora_vulkan_interop.inc", f"{VK}/aurora_vulkan_interop.inc")
copy_in(repo / "extern/aurora/include/aurora/dawn_vulkan_abi.h", f"{VK}/aurora_dawn_vulkan_abi.h")
backend = root / f"{VK}/VulkanBackend.cpp"
backend_text = backend.read_text(encoding="utf-8")
if '#include "aurora_vulkan_interop.inc"' not in backend_text:
    backend.write_text(backend_text + '\n// PrimedGun: direct presentation entry points.\n'
                       '#include "aurora_vulkan_interop.inc"\n', encoding="utf-8", newline="")
insert_before(f"{VK}/VulkanFunctions.cpp", "namespace dawn::native::vulkan {\n",
              '#include "aurora_vulkan_hooks.h"\n\n')
insert_after(f"{VK}/VulkanFunctions.cpp", "    GET_GLOBAL_PROC(CreateInstance);\n",
             "    // PrimedGun: through the OpenXR runtime while Aurora has hooks installed.\n"
             "    CreateInstance = AuroraCreateInstance(GetInstanceProcAddr);\n")
insert_after(f"{VK}/VulkanFunctions.cpp", "    GET_INSTANCE_PROC(CreateDevice);\n",
             "    CreateDevice = AuroraCreateDevice(GetInstanceProcAddr, instance);\n")
insert_after(f"{VK}/VulkanFunctions.cpp", "    GET_INSTANCE_PROC(EnumeratePhysicalDevices);\n",
             "    EnumeratePhysicalDevices = AuroraEnumeratePhysicalDevices(GetInstanceProcAddr, instance);\n")

print(f"PrimedGun Dawn patches applied to {root}")
