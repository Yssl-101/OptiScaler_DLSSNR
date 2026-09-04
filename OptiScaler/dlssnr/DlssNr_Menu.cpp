#include "pch.h"
#include "DlssNrFeature_Vk.h"

#include "DlssNr.h"
#include "DlssNr_ExposureScan.h"


#include <Config.h>
#include <menu/menu_common.h>

#include <imgui/imgui.h>

#include <string>
#include <unordered_map>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace DlssNr
{

// The "(?)" marker every control carries, matching the rest of the menu.
static void HelpMarker(const char* tip)
{
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");

    if (ImGui::IsItemHovered())
    {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 40.0f);
        ImGui::TextUnformatted(tip);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// A slider that only writes its value when the handle is released.
//
// Some controls -- intensity, the structure and tone strengths -- are read by the model once, when
// the feature is built, so changing one rebuilds the whole feature. Writing on every pixel of a drag
// meant a rebuild per frame, felt as the picture hitching while you scrub. The slider still tracks
// live under the cursor; only the commit that triggers the rebuild waits for release. Cheap controls
// that are just shader constants (detail, colour, paper white) do not use this -- they can afford to
// apply live.
static bool DeferredSlider(const char* label, CustomOptional<float>* opt, float mn, float mx,
                           float def, const char* fmt = "%.2f")
{
    static std::unordered_map<std::string, float> pending;

    auto it = pending.find(label);
    float value = it != pending.end() ? it->second : opt->value_or_default();
    bool changed = false;

    if (ImGui::SliderFloat(label, &value, mn, mx, fmt))
        pending[label] = value;

    if (ImGui::IsItemDeactivatedAfterEdit())
    {
        auto committed = pending.find(label);

        if (committed != pending.end())
        {
            *opt = std::clamp(committed->second, mn, mx);
            pending.erase(committed);
            changed = true;
        }
    }

    ImGui::SameLine();

    const std::string resetId = std::string("Reset##") + label;
    if (ImGui::SmallButton(resetId.c_str()))
    {
        *opt = def;
        pending.erase(std::string(label));   // drop any in-flight drag so the reset actually sticks
        changed = true;
    }

    return changed;
}

void RenderMenu(Config* config, float menuResScale)
{

    // DLSS Neural Rendering -----------------------------
    ImGui::Spacing();
    if (auto ch = ScopedCollapsingHeader("DLSS 神经渲染"); ch.IsHeaderOpen())
    {
        ScopedIndent indent {};
        ImGui::Spacing();

        bool enabled = config->DlssNrEnabled.value_or_default();
        if (ImGui::Checkbox("启用神经渲染", &enabled))
            config->DlssNrEnabled = enabled;

        HelpMarker("在上采样器输出上合成细节，发生在帧生成看到画面之前。

需要两个名称相近的文件放在 OptiScaler 旁，仅差一个字符：
  nvngx_dlssnr.dll       NVIDIA 的模型（约 165 MB）——由你提供
  nvngx.dll_dlssnr.dll   转发器（约 13 KB）——随本包附带
未公开且直接驱动，因此全部不受官方支持。");

        // The toggle can be bound to a key, and nobody would think to look for it under Keybinds
        // unless told. Dimmed, because it is a note rather than a setting.
        ImGui::TextDisabled("可用按键切换——请在「按键绑定」下绑定「神经渲染」。");

        bool applyModel = config->DlssNrApplyModel.value_or_default();
        if (ImGui::Checkbox("应用模型", &applyModel))
            config->DlssNrApplyModel = applyModel;

        HelpMarker("是否应用模型的编辑。关闭时显示干净的上采样画面，而该 pass 仍在运行——
因此配合「冻结帧」（位于对比区）可以冻结一帧，切换此项来对比同一冻结帧
在有无神经渲染下的差异。日常使用请保持开启。");

        // Either backend. The two keep separate state, and on a native Vulkan game the D3D12 side
        // is never touched -- so asking only that one reports "waiting for the upscaler" over a pass
        // that is demonstrably running.
        const bool vulkan = DlssNr::IsRunningVk();

        // Turning the pass off does not release the model, so the feature handle stays alive and
        // IsRunning keeps answering yes. Reporting a cost from that was wrong in the way that matters
        // most: the toggle is how anyone A/Bs this, so the one moment the number is read is the one
        // moment it describes the frame before last.
        if (!enabled)
        {
            ImGui::TextDisabled("已关闭。模型保持加载，重新开启即时生效。");
        }
        else if (!DlssNr::IsRunning() && !vulkan)
        {
            const char* reason = DlssNr::FailureReason();

            if (reason[0] != 0)
            {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.35f, 1.0f), "本会话已关闭：%s。", reason);
                ImGui::SameLine();

                if (ImGui::SmallButton("重试"))
                    DlssNr::RetryAfterFailure();
            }
            else if (enabled)
                ImGui::TextUnformatted("等待上采样器运行。");
        }
        else
        {
            // The cost belongs here rather than only in the upscaler's breakdown: that tooltip needs
            // OptiScaler's own upscaler to have run, and with native DLSS passing through there is
            // nothing in it to hang this off.
            // Either backend's timer. They measure the same thing by different means, and only one
            // of them is running.
            const auto ms = vulkan ? DlssNr::LastGpuTimeVk() : DlssNr::LastGpuTime();

            // With "应用模型" off the pass STILL RUNS (so Hold-frame A/B can toggle its edit on
            // a frozen frame) -- it only outputs the clean frame. So the cost is real, and saying so
            // stops the reading looking like a bug. Enable Neural Rendering off is what zeroes it.
            const char* runSuffix =
                !config->DlssNrApplyModel.value_or_default() ? "（模型运行中，编辑已隐藏）" : "";

            if (ms.has_value())
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), "运行中%s - 每帧 %.2f 毫秒%s",
                                   vulkan ? "（原生 Vulkan）" : "", ms.value(), runSuffix);
            else if (vulkan)
                // Measured but not yet read: the first few frames are still in the query ring.
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), "原生 Vulkan 运行中 - %llu 帧%s",
                                   DlssNr::FramesVk(), runSuffix);
            else
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), "运行中。%s", runSuffix);

            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("整个 pass：暂存拷贝与合成，以及模型本身。只计时模型会让数字显得好看。

与本窗口底部的帧时间对比，即可看出它消耗了多少。");
        }

        ImGui::Spacing();
        ImGui::PushItemWidth(220.0f * menuResScale);

        // Any percentage, rather than a handful of steps somebody chose in advance. The lower bound
        // is 25%: below that the model is working on so little of the picture that its answer no
        // longer survives being enlarged onto it.
        // Applied when the handle is let go, not while it is moving.
        //
        // Every distinct value here is a different working size, and a different working size tears
        // down the scratch textures and rebuilds the model. Writing it on each pixel of a drag meant
        // dozens of rebuilds in a second, which is felt as the whole frame hitching. The slider still
        // reads live; only the commit waits.
        static int pendingScale = -1;

        int scalePercent = pendingScale >= 0
                               ? pendingScale
                               : (int) lroundf(config->DlssNrWorkingScale.value_or_default() * 100.0f);

        if (ImGui::SliderInt("模型分辨率", &scalePercent, 25, 200, "%d%%"))
            pendingScale = scalePercent;

        if (ImGui::IsItemDeactivatedAfterEdit() && pendingScale >= 0)
        {
            config->DlssNrWorkingScale = std::clamp(pendingScale, 25, 200) / 100.0f;
            pendingScale = -1;
        }

        if (scalePercent > 100)
            ImGui::TextDisabled("超采样 %.2fx：模型以高于原生分辨率运行，随后
采样回降。实验性且开销大——耗时随面积增长。",
                                scalePercent / 100.0f);

        if (scalePercent > 100)
        {
            static const char* dsNames[] = { "FSR1", "双三次", "Catmull-Rom", "兰佐斯2",
                                             "兰佐斯3", "凯撒2", "凯撒3", "MAGIC" };
            int ds = (int) config->DlssNrScalingDownscaler.value_or_default();
            if (ds < 0 || ds >= IM_ARRAYSIZE(dsNames))
                ds = (int) Scaler::Lanczos3;

            if (ImGui::Combo("降采样器（神经渲染）", &ds, dsNames, IM_ARRAYSIZE(dsNames)))
                config->DlssNrScalingDownscaler = (Scaler) ds;

            HelpMarker("把模型高于原生分辨率的答案平均回显示尺寸的滤镜——
这正是让超采样减少而非增加噪声的关键。更锐利的滤镜（兰佐斯3、凯撒3）
保留最多细节；更柔和的（双三次、Catmull-Rom）对振铃更温和。
与输出缩放降采样器相互独立，两者可不同并可同时运行。");
        }

        HelpMarker("模型以画面多大比例工作。成本随此值的平方下降，
因此一半分辨率约为四分之一耗时。

画面本身从不缩小。只有模型的贡献在小尺寸计算再放大，
所以无论此值如何，底下的画面都不受影响。

代价是：模型添加的明暗是宽泛的、经得起放大；
而它合成的精细结构则不然，会变柔。当该 pass 的成本
高于你愿为它返回的细节所付时，就值得用。

无论此值如何，画面本身始终保留完整细节——只有模型
自己的工作在小尺寸完成。");

        // Meaningful only when the model runs BELOW the frame's size. At 100% -- and above, where
        // supersampling composites its down-legged answer at native -- the residual collapses to the
        // model's own picture and the two modes are identical, so the control says so by going grey.
        {
            const bool reduced = config->DlssNrWorkingScale.value_or_default() < 0.999f;

            if (!reduced)
                ImGui::BeginDisabled();

            static const char* enlargeNames[] = { "经典", "匹配残差" };
            int enlarge = config->DlssNrTransfer.value_or_default() == 1 ? 1 : 0;

            if (ImGui::Combo("放大方式", &enlarge, enlargeNames, IM_ARRAYSIZE(enlargeNames)))
                config->DlssNrTransfer = (uint32_t) enlarge;

            if (!reduced)
                ImGui::EndDisabled();

            HelpMarker("当模型以低于画面尺寸运行时，其工作如何被放大回来。

经典：把模型的小图直接与全尺寸画面合成。两者之间的差异既来自
缩小的模糊，也来自模型的编辑，而合成无法区分它们——它把模糊当成
画面已有、模型从未见过的亮度。模型分辨率越低，误差越大，
在 50% 时就表现为色偏。

匹配残差：只把模型的差值放大回来，叠加到画面自身的代理上，
因此被比较的两张图都是全尺寸，唯一来自小栅格的只有编辑本身。

在 100% 或以上无效果：没有残差可携带，两者相同
（超采样会在此之前把答案降回画面尺寸）。

源自 hhkbble 在此分支上的多 pass 工作。");
        }

        ImGui::SeparatorText("效果落地比例");

        float transfer = config->DlssNrTransferStrength.value_or_default();
        if (ImGui::SliderFloat("细节强度", &transfer, 0.0f, 2.0f, "%.2f"))
            config->DlssNrTransferStrength = transfer;

        ImGui::SameLine();
        if (ImGui::SmallButton("重置##detail"))
            config->DlssNrTransferStrength = 1.0f;

        HelpMarker("画面向模型画面靠拢的程度。

模型的答案不是叠加到画面上——它是一张完整的画面，
已重新缩放使其亮度落到原图指示的位置。此值在两者之间混合，
因此两端都是真实画面，中间任意值也都是。

0 精确返回上采样器的输出。1 是模型的画面。

超过 1 会沿同方向继续越过去，这不是模型要求的——
用它观察模型做了什么，再调回来。想要更强效果就推这个：
强度（Intensity）属于模型内部，由它决定如何利用。");

        float colour = config->DlssNrColourStrength.value_or_default();
        if (ImGui::SliderFloat("色彩强度", &colour, 0.0f, 4.0f, "%.2f"))
            config->DlssNrColourStrength = colour;

        ImGui::SameLine();
        if (ImGui::SmallButton("重置##colour"))
            config->DlssNrColourStrength = 1.0f;

        HelpMarker("模型的颜色是否随其亮度一起生效。

0 完全保留游戏自身色调——每个像素都是原始颜色，
仅亮度承载模型的判断。游戏级准确的颜色，加上细节。
1 同时带入模型自己的色调，并钳制到 AP1，
因此不会要求不可达的颜色。

它本身不会偏移色相：它在两张成品画面之间插值，
而非向其中一张叠加色差——后者正是过去让暖色主体
回来变绿的原因。

超过 1 会过饱和：颜色保持色相但更鲜艳，
并在显示器能显示的边缘处滚降，而不是削平成一团过曝。
1 是模型自身的颜色；想要更冲击可继续往上推。");

        // Experimental. 0 off (soft knee), 1 Neutwo + our composition, 2 Neutwo + pure-inverse replace,
        // 3 hybrid+composed, 4 hybrid+replace (identity midtones + unclipped highlights). Always shown.
        static const char* reversibleNames[] = { "关闭（软拐点）", "Neutwo 代理 + 合成",
                                                 "Neutwo 代理 + 替换", "混合代理 + 合成",
                                                 "混合代理 + 替换" };
        int reversible = (int) config->DlssNrReversibleMode.value_or_default();
        if (reversible < 0 || reversible > 4)
            reversible = 0;
        if (ImGui::Combo("可逆代理（实验性）", &reversible, reversibleNames,
                         IM_ARRAYSIZE(reversibleNames)))
            config->DlssNrReversibleMode = (uint32_t) reversible;

        HelpMarker("模型看到什么，以及它的答案如何返回。

关闭（软拐点）：默认。它把高光压得很狠，模型无法解析其中的细节
——柔光场景没问题，亮场景偏弱。

Neutwo 合成：一条不削波的曲线，让模型看到高光细节，
再配合上面的所有（细节/色彩强度、高光保护、调色）。在亮场景占优，
但该曲线也压缩中间调，柔光内容下可能比关闭更差。
它还会偏移白点——切换时请重新检查。

混合合成：两者之长，推荐使用。中间调恒等——在柔光处与关闭一样好——
只有高光走不削波滚降，既找回关闭所压掉的细节，又不放弃
Neutwo 所牺牲的中间调。它几乎不偏移白点。

替换：原始模型直接经精确逆变换返回，不做任何合成
——无保护、无调色、无强度。没有强光时很漂亮，但强光在运动中会闪烁。
属于参考，不是日常设置。

混合替换：与替换一样是原始模型，但走混合曲线——
解码在中间调恒等，闪烁被限制在真正的亮高光，而非处处。
保留替换的大部分细节，却稳定得多。若你爱替换的观感但受不了闪烁，用这个。

关闭与之前逐字节相同。");

        ImGui::SeparatorText("模型");

        ImGui::TextUnformatted("在模型构建时读取，更改后片刻即重建。");

        static const char* nrPresetNames[] = { "默认", "预设 1", "预设 2", "预设 3" };
        int preset = (int) config->DlssNrPreset.value_or_default();
        if (ImGui::Combo("模型预设", &preset, nrPresetNames, IM_ARRAYSIZE(nrPresetNames)))
            config->DlssNrPreset = (uint32_t) preset;

        HelpMarker("默认把选择权交给模型。

与超分辨率或光线重建的预设不是同一套刻度——
相同的数字在这里含义不同。");

        static const char* nrStyleNames[] = { "默认（标准）", "自然", "电影感" };
        int style = (int) config->DlssNrStyle.value_or_default();

        if (style > 2)
            style = 2;

        if (ImGui::Combo("风格", &style, nrStyleNames, IM_ARRAYSIZE(nrStyleNames)))
            config->DlssNrStyle = (uint32_t) style;

        HelpMarker("模型自身的处理风格。

默认（标准）：最强。提升局部对比、加深光照，
可能过饱和或显得风格化——多数「模型改变了我的游戏观感」
都来自这个风格。

自然：同样的细节工作但手法更温和。让肤色与色调平衡
更接近游戏原渲染。

电影感：压低光泽与过度处理，呈现电影质感。

在模型构建时读取，更改后片刻即重建。名称来自社区测试；
NVIDIA 在二进制里没有提供名称。");

        DeferredSlider("强度", &config->DlssNrIntensity, 0.0f, 2.0f, 1.0f);

        HelpMarker("模型自身的强度控制，在模型内部生效。与上面的细节强度不同，
后者是对结果进行事后缩放。");

        DeferredSlider("局部结构", &config->DlssNrLocalStructure, 0.0f, 2.0f, 1.0f);

        DeferredSlider("局部色调", &config->DlssNrLocalTone, 0.0f, 2.0f, 1.0f);


        DeferredSlider("皮肤结构", &config->DlssNrSkinStructure, -1.0f, 2.0f, -1.0f);

        HelpMarker("-1 表示跟随局部结构，也是模型自身的默认值——它不是强度为零。
0 及以上则独立于画面其余部分设置皮肤。");

        bool autoMask = config->DlssNrAutoMask.value_or_default();
        if (ImGui::Checkbox("自动皮肤掩码", &autoMask))
            config->DlssNrAutoMask = autoMask;

        HelpMarker("让模型自行识别皮肤，而非均匀处理整帧。");

        ImGui::SeparatorText("色彩");

        ImGui::TextDisabled("模型是用成品、sRGB 编码的画面训练的。上采样器的输出不是这种：
它是线性的、开放式的。这些选项决定如何把它映射成模型认得的东西。
若游戏报告某帧已做过色调映射，则原样跳过，这些都不生效。");

        {
        // Logarithmic, because the useful range is not linear. A quarter to 240: the low end because
        // a frame the game already tone mapped wants roughly 1, the high end because there is no
        // principled ceiling -- this is a divisor on an open-ended linear buffer, and how far up a
        // given game needs to go is a property of that game's exposure, not of anything we can bound.
        // One tester was still improving at 100. A linear slider over that span would spend nine
        // tenths of its travel on values nobody needs and never reach the ones they do.
        // One dropdown, because there is one answer.
        //
        // This was two checkboxes that could both be on, and every attempt to stop that was a patch
        // on a shape that should not have existed. Greying deadlocked -- each disabled the other, so
        // once both were set the only way out was a button the notice never mentioned. Clearing
        // worked but silently undid a setting somebody had made. Both were ways to stop an illegal
        // state being REACHED; a single choice cannot reach it, because there is only one value to
        // be in.
        //
        // Each option also says whether it can actually do anything in THIS game, in colour, so the
        // choice is made on what is available rather than on what sounds best.
        {
            const auto ex = DlssNr::GameExposureStatus();
            const bool vk = DlssNr::IsRunningVk();
            const bool haveExposure = vk ? DlssNr::ExposureOfferedVk() : ex.everOffered;

            const float anchorNow = DlssNr::ExposureScan::BestValue();
            const bool haveAnchor = !DlssNr::ExposureScan::Anchors().empty();

            static const char* sourceNames[] = { "仅纸张白", "游戏自身曝光",
                                                 "扫描找到的缓冲" };

            int source = (int) config->DlssNrWhitePointSource.value_or_default();

            if (source < 0 || source > 2)
                source = 0;

            if (ImGui::Combo("白点来源", &source, sourceNames, IM_ARRAYSIZE(sourceNames)))
            {
                config->DlssNrWhitePointSource = (uint32_t) source;

                // Nothing else to set. The scan asks the source whether it is wanted, so choosing
                // it here is the whole of switching it on -- there is no second flag to keep in
                // step, and so no way for the two to disagree.
            }

            HelpMarker("用于除画面的那个数字从何而来。

仅纸张白——只用下面的滑块，别无其他。适合曝光从不变化的游戏，
一旦变化就不对了：一个常数无法同时服务洞穴与旷野。

游戏自身曝光——从游戏交给上采样器的纹理中读取。
这是最好的来源，因为它由上游决定，本 pass 的任何操作都无法改变它。
并非每个游戏都提供。

扫描找到的缓冲——用于计算了曝光却从不传出的游戏。属于猜测：
候选按形状匹配，在 GTA V 中最佳的那个以自身尺度跟踪真实曝光，
由锚点的比值抵消。需先锚定一次，之后还要核对。");

            // Availability, in colour, for the option currently chosen.
            if (source == 1)
            {
                if (!vk && ex.seenFrames == 0)
                    ImGui::TextDisabled("等待画面帧……");
                else if (!haveExposure)
                    ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.25f, 1.0f),
                                       "该游戏不提供曝光值——当前使用白点。建议改用扫描。");
                else if (vk)
                    ImGui::TextColored(ImVec4(0.45f, 0.8f, 0.45f, 1.0f),
                                       "该游戏提供曝光值，且正在被读取。");
                else if (ex.exposure > 1e-6f)
                {
                    const float trim =
                        std::clamp(config->DlssNrWhitePointTrim.value_or_default(), 0.25f, 4.0f);
                    ImGui::TextColored(ImVec4(0.45f, 0.8f, 0.45f, 1.0f),
                                       "游戏曝光 %.4f  ->  白点 %.2f%s", ex.exposure,
                                       ex.preExposure / ex.exposure * trim,
                                       ex.offeredNow ? "" : "（保留：本帧缺失）");
                }
                else
                    ImGui::TextDisabled("正在读取曝光……");
            }
            else if (source == 2)
            {
                // "Nothing found" and "found several, none of them moving" are different states,
                // and this said the first for both. In GTA V the log carried eight candidates while
                // the panel claimed there were none, which reads as the scan being broken when what
                // it actually needs is for the light to change.
                if (anchorNow <= 0.0f)
                {
                    const unsigned int watching = (unsigned int) DlssNr::ExposureScan::Report().size();

                    if (watching == 0)
                        ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.25f, 1.0f),
                                           "该游戏内没有任何像曝光值的形状。");
                    else
                        ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.25f, 1.0f),
                                           "正在监视 %u 个，尚无移动——请在明暗之间走动。",
                                           watching);
                }
                else if (!haveAnchor)
                    ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.25f, 1.0f),
                                       "找到一个。调整下方白点直到画面正确，然后按「在此锚定」。");
                // Once anchored, the scan -> white point readout sits above the sliders below; it is
                // not repeated up here.
            }
            else if (haveExposure)
            {
                ImGui::TextColored(ImVec4(0.45f, 0.8f, 0.45f, 1.0f),
                                   "该游戏提供曝光值——上面的选项会使用它。");
            }
        }






        // A measured suggestion for paper white used to sit here and has been withdrawn.
        //
        // It took the 90th percentile of per-tile peak luminance from the untouched frame, which is a
        // statement about scene content rather than about the buffer's scale. In Nioh 3, where the
        // right answer is about 240, it offered 8 -- because most tiles are shadow and the percentile
        // sits wherever most tiles are. The guard meant to catch that compared each tile against the
        // frame's own brightest, which is scale-free and therefore passes on a black screen: the same
        // relative-threshold mistake the white point meter was removed for, made a second time.
        //
        // A wrong number offered confidently is worse than no number, so nothing is offered. What
        // replaces it has to be a measurement of the game's own exposure rather than of its scenery:
        // the exposure texture where a game supplies one, and otherwise the ratio between the
        // scene-referred buffer and the finished frame, which is that exposure by definition.

        // Two controls, not one control with two meanings.
        //
        // These are different quantities. The manual path wants an absolute divisor on an open-ended
        // linear buffer -- Nioh 3 needs about 240 -- and the exposure path wants a multiplier on a
        // number the game already supplied, where 1 is correct and anything far from it says the read
        // is wrong rather than that somebody prefers it.
        //
        // They used to share one stored value, narrowed to 0.25..4 when the toggle was on. That kept
        // a ruinous value unreachable but left two worse problems: moving the slider in one mode
        // silently destroyed the number found in the other, and there was no way back to "just take
        // the game's answer" short of knowing that the number for it was 1. Separate values fix both.
        // Switching modes is now non-destructive in both directions.
        // The trim belongs to both automatic sources, since both end in "the game's number times a
        // little". Only the manual source gets the absolute slider.
        // One slider per source, each remembering its own number.
        //
        // A trim on the game's exposure and a trim on a buffer the scan found are trims on different
        // things, and a value found against one means nothing against the other. Sharing them meant
        // changing source silently carried a number across, so a picture that had been tuned came
        // back wrong for a reason nothing on screen explained.
        //
        // The scan before it is anchored is the exception, and it has to be: anchoring captures an
        // absolute white point, so there must be an absolute slider to set. Showing a trim there
        // asked people to "set paper white below" next to a control that was not paper white.
        const int wpSource = (int) config->DlssNrWhitePointSource.value_or_default();

        // Which anchor row the paper-white slider edits, or -1 for the live unanchored point. Menu-
        // local and not persisted; the anchor block below sets it when a row is clicked. Declared
        // here because both the slider (this block) and the table (below) read it in the same frame.
        static int selectedAnchor = -1;
        auto anchors = DlssNr::ExposureScan::Anchors();
        if (selectedAnchor >= (int) anchors.size())
            selectedAnchor = -1;

        if (wpSource == 2)
        {
            const bool editingRow = selectedAnchor >= 0 && selectedAnchor < (int) anchors.size();

            // The single scan -> white point readout, above the sliders it explains.
            if (!anchors.empty())
            {
                const float liveScan = DlssNr::ExposureScan::BestValue();

                if (liveScan > 0.0f)
                {
                    const float w = DlssNr::ExposureScan::AnchoredWhitePoint(
                        liveScan, config->DlssNrScanInverted.value_or_default(),
                        config->DlssNrScanTrim.value_or_default());

                    ImGui::TextColored(ImVec4(0.45f, 0.8f, 0.45f, 1.0f),
                                       "扫描 %.5f  ->  白点 %.2f（%u 个点%s）", liveScan, w,
                                       (unsigned) anchors.size(), anchors.size() == 1 ? "" : "s");
                }
            }

            // Paper white shows only when there is a point to set: before the first anchor, or when a
            // row is selected to edit. Once points exist and none is selected, the white point is fixed
            // by the anchors and only the trim adjusts the live picture -- so the trim takes the
            // slider's place, the same shape as the game-exposure source.
            const bool showPaperWhite = anchors.empty() || editingRow;

            if (showPaperWhite)
            {
                float pw = editingRow ? anchors[selectedAnchor].white
                                      : config->DlssNrWhitePointScale.value_or_default();

                char lbl[48];
                if (editingRow)
                    snprintf(lbl, sizeof(lbl), "白点（正在编辑第 %d 点）", selectedAnchor + 1);
                else
                    snprintf(lbl, sizeof(lbl), "白点（纸张白）");

                if (ImGui::SliderFloat(lbl, &pw, 0.25f, 2000.0f, "%.2fx", ImGuiSliderFlags_Logarithmic))
                {
                    if (editingRow)
                    {
                        DlssNr::ExposureScan::AnchorSetWhite(selectedAnchor, pw);
                        config->DlssNrScanAnchors = DlssNr::ExposureScan::SerializeAnchors();
                    }
                    else
                        config->DlssNrWhitePointScale = pw;
                }

                HelpMarker("所选校准点的白点，或——未选中任何行时——下一次按「锚定」时
捕获的值。

调到此处画面正确，再锚定。移动到差异很大的光照下再重复：
两个点即固定缓冲的真实关系，白点在两点之间保持。
点击下方某行可回来调整该点；再次点击则松开。");
            }

            // The trim multiplies the interpolated result, and in the steady state it is the control
            // that stands in for paper white: adjust it until the picture looks right in the current
            // light, then Anchor bakes that trimmed value into a new point and resets the trim to 1.
            if (!anchors.empty())
            {
                float trim = config->DlssNrScanTrim.value_or_default();

                if (ImGui::SliderFloat("微调（× 扫描值）", &trim, 0.25f, 4.0f, "%.2fx",
                                       ImGuiSliderFlags_Logarithmic))
                    config->DlssNrScanTrim = std::clamp(trim, 0.25f, 4.0f);

                ImGui::SameLine();

                if (ImGui::SmallButton("重置##scantrim"))
                    config->DlssNrScanTrim = 1.0f;

                HelpMarker("扫描白点的乘数，也是你在锚点之间调整的控件：
调到当前光照下画面正确，再按「在此锚定」——
它把微调后的值固化为新点，并把微调重置为 1。");
            }
        }
        else if (wpSource == 1)
        {
            const bool ofScan = false;

            float trim = ofScan ? config->DlssNrScanTrim.value_or_default()
                                : config->DlssNrWhitePointTrim.value_or_default();

            if (ImGui::SliderFloat(ofScan ? "微调（× 扫描值）" : "微调（× 游戏曝光）", &trim,
                                   0.25f, 4.0f, "%.2fx", ImGuiSliderFlags_Logarithmic))
            {
                if (ofScan)
                    config->DlssNrScanTrim = std::clamp(trim, 0.25f, 4.0f);
                else
                    config->DlssNrWhitePointTrim = std::clamp(trim, 0.25f, 4.0f);
            }

            ImGui::SameLine();

            // Deliberately always present rather than greyed at 1. The point of it is that the safe
            // value is one click away without having to know what the safe value is.
            if (ImGui::SmallButton("重置##wptrim"))
            {
                if (ofScan)
                    config->DlssNrScanTrim = 1.0f;
                else
                    config->DlssNrWhitePointTrim = 1.0f;
            }

            HelpMarker("游戏所供曝光的乘数。1.00x 即精确采用其数值，这里就是正确答案。

这不是糊弄因子。若某游戏需要微调远离 1 才好看，
说明读取到的曝光对该游戏是错的，而非游戏需要微调。
约 0.8 到 1.25 是诚实的调校；要拉到 4 说明上游某处坏了，
微调只是在掩盖它。

你手动设的白点被单独保存，关闭上面的选项后会原样恢复。");
        }
        else
        {
            // Logarithmic, because the useful range is not linear. A quarter to 2000: the low end
            // because a frame the game already tone mapped wants roughly 1, the high end because
            // there is no principled ceiling -- this is a divisor on an open-ended linear buffer, and
            // how far up a given game needs to go is a property of that game's exposure rather than
            // of anything that can be bounded here. One tester was still improving at 100.
            float wpScale = config->DlssNrWhitePointScale.value_or_default();

            if (ImGui::SliderFloat("白点（纸张白）", &wpScale, 0.25f, 2000.0f, "%.2fx",
                                   ImGuiSliderFlags_Logarithmic))
                config->DlssNrWhitePointScale = wpScale;

        HelpMarker("画面在交给模型之前所除的那个值。没有别的白点；这就是全部。

模型是用白色位于 1 的成品画面训练的。上采样器的输出是线性、
开放式的，因此必须有东西说明白色在哪——当游戏的 DLSS 缓冲是线性 HDR 时，
这个数很少接近 1。在《怪物猎人：荒野》中实测要 16 或更高，
模型的细节才能到达画面；而适合阴凉营地的值，对同一款游戏的
白天场景仍太小。

太低则几乎每个像素都触发软拐点：模型看到的是一张平白的近白画面，
它的答案被缩放掉，只有色相幸存——表现为色偏而非细节丢失。
太高则看到欠曝画面，答案退化，且同一数值在输出时还会放大该误差。

调高直到画面不再改善。超过该点它不会趋于平台，
而是朝反方向变差。

它曾是实测白点的乘数。该测量已被移除：它读的是场景亮度
而非白色应处的位置，把画面交给了暗三倍的模型，
高光路径也无可回馈。

在强度为零时，无论此值如何，画面仍逐字节相同。");
        }

        // Highlight guard, directly under the white point / trim -- it bounds the model's edit and
        // belongs with the exposure controls it works alongside.
        float maxRatio = config->DlssNrMaxRatio.value_or_default();
        if (ImGui::SliderFloat("高光保护", &maxRatio, 1.0f, 8.0f, "%.1fx"))
            config->DlssNrMaxRatio = maxRatio;

        ImGui::SameLine();
        if (ImGui::SmallButton("重置##guard"))
            config->DlssNrMaxRatio = 2.0f;

        HelpMarker("该 pass 让任一像素移动的最大幅度，以它原值的倍数为计，
双向都限——像素变亮不能超过此值，变暗不能超过其倒数。
灯光处模型最没话说，重缩放其答案伤害最大；2× 保留细节，
同时防止条形灯变成一串彩色格子。仅当亮区看起来被削平时才调高。");

        // Directly under the white point, because that is the number it moves and the number the
        // anchor captures. It used to sit under Inspect, a whole section away from the slider it
        // reads, which left "在此锚定" looking like a control for something else entirely.
        {
            // No checkbox here any more.
            //
            // The dropdown above says whether the scan is the white point's source, and that is
            // the only reason anybody using this would want it running. A second control could
            // only agree with the dropdown or contradict it, and both were on offer: it began as
            // a redundant question and became a way to switch off the thing the chosen source
            // depended on.
            //
            // The ini key survives as a developer override for the one case a user has no reason
            // to want -- running the scan in a game that supplies a REAL exposure, so the log can
            // compare the two. That is validation, and validation does not need a widget.
            //
            // Worth keeping written down, since the panel no longer says it: the scan matches
            // buffers by SHAPE, and shape is a weak filter. In GTA V -- a game that supplies a
            // real exposure, so the right answer sat visible beside it -- the best candidate was
            // a 1x1 R32_FLOAT that climbed in a straight line for seventeen minutes while the
            // true exposure held still. Their ratio moved 14x. That is an accumulator, not an
            // eye adaptation.

                // Only where it means something. The lamp reads the scan, so offering it beside a
                // white point that comes from the game's own exposure is offering a control that
                // cannot light up.
                bool meter = config->DlssNrScanMeter.value_or_default();

                if (config->DlssNrWhitePointSource.value_or_default() == 2 &&
                    ImGui::Checkbox("屏幕显示测光表", &meter))
                    config->DlssNrScanMeter = meter;

                HelpMarker("角落的一盏灯：红代表暗、绿代表全亮，之间是过渡色，旁边带读数。

用于一眼看出扫描是在「跟踪」而非仅仅运行。走进阴影它应滑向红色；
走出来应变为绿色。若方向反了，就是上面那个设置（数值方向相反）的用途。

纯读数。不改变任何东西。");

            // Shown when the scan is actually running, whichever way it got switched on.
            if (DlssNr::ExposureScan::Scanning())
            {
                // Anchoring: one press, then it never needs touching again.
                //
                // The absolute white point cannot come out of a buffer whose units are unknown.
                // Every value AFTER the first can: only the ratio against the anchor is used, so
                // whatever the number means, it cancels. That is why this is a button and not a
                // measurement -- the one thing a person can supply that no amount of cleverness
                // can is "this looks right to me".
                int which = 0;
                float low = 0.0f, high = 0.0f;
                const float live = DlssNr::ExposureScan::BestValue(&which, &low, &high);

                const bool isSource = config->DlssNrWhitePointSource.value_or_default() == 2;

                // Anchor captures (currentScan, currentPaperWhite) and ADDS a row -- it does not
                // replace. One row is the old single-anchor ratio law; add a second in different
                // light and the white point is interpolated between the points, so it holds across
                // the whole range instead of only near one anchor. Greyed unless the scan is the
                // chosen source and it currently has a value to capture.
                ImGui::BeginDisabled(live <= 0.0f || !isSource);

                if (ImGui::Button("在此锚定"))
                {
                    // What to capture. Before the first point, the paper white above (an absolute value
                    // with the wide range a fresh game needs). After that, the EFFECTIVE white point the
                    // picture is showing right now -- the interpolated value times the Trim the user just
                    // dialed in -- so a second point in different light captures the trimmed look, not a
                    // frozen paper white (which would make two equal whites and a flat, non-tracking
                    // curve). The trim is reset afterwards: the new point, which the picture now passes
                    // through exactly, must not be multiplied by it a second time.
                    const float captureWhite =
                        anchors.empty()
                            ? std::max(0.01f, config->DlssNrWhitePointScale.value_or_default())
                            : std::max(0.01f, DlssNr::ExposureScan::AnchoredWhitePoint(
                                                  live, config->DlssNrScanInverted.value_or_default(),
                                                  config->DlssNrScanTrim.value_or_default()));

                    if (DlssNr::ExposureScan::AnchorAdd(live, captureWhite))
                    {
                        config->DlssNrScanAnchors = DlssNr::ExposureScan::SerializeAnchors();
                        config->DlssNrScanTrim = 1.0f;
                        selectedAnchor = -1;
                    }
                }

                ImGui::EndDisabled();

                HelpMarker("先把画面调到正确，再按它——把当前观感固化为一个点。
第一个点用上面的白点滑块；之后每个点，走到不同光照下用微调，
锚定会把它烘焙成新点。

第一次按校准一个点——白点从此按比值跟随扫描，与之前一样。
走到差异很大的光照下，重新设白点，再按一次：
第二个点即钉死缓冲的真实曲线，两点之间全部正确，
而非只在单个锚点附近。最多八个。

该表按游戏区分且可分享：一人校准某游戏，
拿到该配置的所有人都得到相同数字。");

                if (!isSource)
                    ImGui::TextDisabled("（扫描仅在监视——上方白点来自其他来源）");

                if (!anchors.empty())
                {
                    // The row nearest the live scan value (in log space) is the one driving the
                    // picture right now; mark it so the user can see which calibration is in effect.
                    int active = 0;
                    float bestDist = 1e30f;
                    const float liveLog = std::log(std::max(live, 1e-6f));

                    for (size_t i = 0; i < anchors.size(); ++i)
                    {
                        const float d =
                            std::fabs(std::log(std::max(anchors[i].scan, 1e-6f)) - liveLog);
                        if (d < bestDist)
                        {
                            bestDist = d;
                            active = (int) i;
                        }
                    }

                    for (size_t i = 0; i < anchors.size(); ++i)
                    {
                        ImGui::PushID((int) i);

                        // Delete first, so its click is never swallowed by the row-wide Selectable.
                        if (ImGui::SmallButton("x"))
                        {
                            DlssNr::ExposureScan::AnchorRemove((int) i);
                            config->DlssNrScanAnchors = DlssNr::ExposureScan::SerializeAnchors();
                            if (selectedAnchor == (int) i)
                                selectedAnchor = -1;
                            else if (selectedAnchor > (int) i)
                                --selectedAnchor;
                            ImGui::PopID();
                            continue;
                        }

                        ImGui::SameLine();

                        const bool sel = (int) i == selectedAnchor;
                        char row[96];
                        snprintf(row, sizeof(row), "%s 扫描 %.4f  ->  白点 %.2f%s",
                                 ((int) i == active && isSource) ? ">" : "  ", anchors[i].scan,
                                 anchors[i].white, sel ? "（编辑中）" : "");

                        // Click selects the row (slider edits it); click again deselects (slider
                        // returns to the live unanchored point).
                        if (ImGui::Selectable(row, sel))
                            selectedAnchor = sel ? -1 : (int) i;

                        ImGui::PopID();
                    }

                    ImGui::TextDisabled("点击行用上方滑块编辑；再次点击回到实时点。> 表示当前生效的点。");
                }

                // The direction flag only means anything with a single point; with two or more the
                // direction the white point moves is already fixed by the data.
                if (anchors.size() == 1)
                {
                    bool inverted = config->DlssNrScanInverted.value_or_default();
                    if (ImGui::Checkbox("数值方向相反", &inverted))
                        config->DlssNrScanInverted = inverted;

                    HelpMarker("若画面在本应变好的方向上变差，就翻转此项。多数引擎存储的曝光
随场景变亮而下降；有些存储其倒数，而按形状找到的缓冲
不会说明是哪种。在不同光照下加第二个锚点，它会自动判定，
此选项随之消失。");
                }

                // The scan -> white point readout is shown above the sliders now, not here.

                // Everything below is read-out rather than control: what the scan is looking at and
                // how to tell whether it found the right thing. Folded away because the two decisions
                // that matter -- anchor, and which way the number runs -- are above it.
                if (ImGui::TreeNode("高级"))
                {

                    const auto found = DlssNr::ExposureScan::Report();
                    const char* why = DlssNr::ExposureScan::Status();

                    if (found.empty())
                    {
                        ImGui::TextDisabled("%s", why != nullptr && why[0] != 0
                                                      ? why
                                                      : "尚未匹配到任何内容。");
                    }
                    else
                    {
                        for (size_t i = 0; i < found.size(); ++i)
                        {
                            const auto& c = found[i];

                            if (c.reads == 0)
                            {
                                ImGui::TextDisabled("%zu. %s -- 尚未读取", i + 1, c.shape.c_str());
                                continue;
                            }

                            // Moving is the whole signal, so it is the thing that is coloured.
                            ImGui::TextColored(c.moves ? ImVec4(0.45f, 0.8f, 0.45f, 1.0f)
                                                       : ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
                                               "%zu. %s = %.5f（区间 %.5f..%.5f）%s", i + 1,
                                               c.shape.c_str(), c.latest, c.lowest, c.highest,
                                               c.moves ? "在变化" : "目前平坦");
                        }

                        ImGui::TextDisabled("从阴影走到日光下。真实曝光会变化。");
                        ImGui::TextDisabled("只会单调上升的是计数器，不是曝光。");
                    }

                    ImGui::TreePop();
                }
            }
        }


        }

        ImGui::SeparatorText("对比");

        // Freeze the frame the model works on, so a setting change re-renders it in place -- the only
        // clean way to A/B our own settings (a moving scene confounds every other comparison). See
        // design/frame-hold.md.
        bool held = config->DlssNrHoldFrame.value_or_default();
        if (ImGui::Checkbox("冻结帧", &held))
            config->DlssNrHoldFrame = held;

        HelpMarker("冻结模型处理的画面。冻结期间，改白点、强度、可逆模式、模型预设
——上采样器之下的任何项——只有该设置变化，场景不动。

它无法展示：DLSS/FSR/XeSS 上采样预设或任何上游内容
（冻结帧不会重跑上采样器），以及游戏自身的 HUD 和后处理，
它们在此 pass 之后运行并持续更新。冻结期间白点停止测量、保持其值，
因此不会漂移而干扰对比。

隐藏菜单它仍保持冻结。取消勾选即恢复。");

        static const char* compareNames[] = { "关闭", "并排", "擦除" };
        int compare = (int) config->DlssNrCompare.value_or_default();
        if (ImGui::Combo("对比", &compare, compareNames, IM_ARRAYSIZE(compareNames)))
            config->DlssNrCompare = (uint32_t) compare;

        HelpMarker("让该 pass 与其自身对照，可同时看到两者，而非切换后再靠记忆。

并排：把整帧放进两半，左侧原样、右侧编辑后。
两半都被横向压缩以适应，因此适合查看而非游玩。

擦除：在分割处切开单帧，不做任何重采样，
画面形状正确、可正常游玩。拖动下方分割位置；它是存储的设置，
关闭菜单后保持不变。

两者都无需菜单打开即可持续工作。接缝处以一条细线标示。");

        if (compare != 0)
        {
            bool swap = config->DlssNrCompareSwap.value_or_default();
            if (ImGui::Checkbox("交换两侧", &swap))
                config->DlssNrCompareSwap = swap;

            bool tags = config->DlssNrCompareTags.value_or_default();
            if (ImGui::Checkbox("标注两侧", &tags))
                config->DlssNrCompareTags = tags;

            HelpMarker("把哪边是哪边直接写进画面，这样截图离开本机后仍能说明。
绘制在画面自身平面内：擦除模式下分割线对标签的显现与隐藏
与对图像完全一致，且无需拖动。交换两侧会让标签随其画面一起移动。");

            if (tags)
            {
                float tagScale = config->DlssNrTagScale.value_or_default();
                if (ImGui::SliderFloat("标签大小", &tagScale, 0.5f, 5.0f, "%.1fx"))
                    config->DlssNrTagScale = std::clamp(tagScale, 0.5f, 5.0f);
            }

            HelpMarker("把编辑后的画面放到另一侧。

在你确定偏爱哪边后值得一做：眼睛对左右并不公平，
差异可能仅因位置就看起来像改进。若交换后同一侧仍胜出，
说明你看到的是该 pass 的效果，而非位置错觉。");
        }

        if (compare == 1)
        {
            float zoom = config->DlssNrCompareZoom.value_or_default();
            if (ImGui::SliderFloat("缩放", &zoom, 1.0f, 2.0f, "%.2f"))
                config->DlssNrCompareZoom = std::clamp(zoom, 1.0f, 2.0f);

            HelpMarker("每一半显示画面的多少。

半屏是画面宽度的一半、高度相同，因此画面无法
在保持形状的同时填满它。

为 1 时整帧以正确比例呈现，上下带黑条。
为 2 时半屏被填满，两侧被裁掉。
中间任意值在两者之间取舍。");
        }

        if (compare == 2)
        {
            float split = config->DlssNrCompareSplit.value_or_default();
            if (ImGui::SliderFloat("分割位置", &split, 0.0f, 1.0f, "%.2f"))
                config->DlssNrCompareSplit = std::clamp(split, 0.0f, 1.0f);

            HelpMarker("擦除的切割位置。其左侧是上采样器产出的画面，
右侧是模型编辑后的画面。");
        }

        static const char* debugNames[] = { "关闭", "代理（模型所见）", "模型原始输出",
                                            "差异（放大）" };
        int debugView = (int) config->DlssNrDebugView.value_or_default();
        if (ImGui::Combo("调试视图", &debugView, debugNames, IM_ARRAYSIZE(debugNames)))
            config->DlssNrDebugView = (uint32_t) debugView;

        HelpMarker("代理是交给模型的画面——若它看起来不对，说明白点错了，
下游一切都不必再评。

差异显示模型实际改了什么，放大二十倍并以灰色居中。
那里一片平灰意味着它什么都没做。");

        ImGui::PopItemWidth();
    }
}

} // namespace DlssNr

