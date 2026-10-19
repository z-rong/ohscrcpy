/*
 * Copyright (c) 2024-2025 Huawei Device Co., Ltd.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * @file virtual_display.cpp
 * @brief 无物理显示器时补一块虚拟屏并设为默认屏（用法见 virtual_display.h）
 */

#include "virtual_display.h"

#include <cinttypes>
#include <unistd.h>
#include <vector>

#include "hilog/log.h"

// 窗口管理（libdm，inner kit）
#include "display_manager.h"
#include "dm_common.h"
#include "screen_manager.h"
#include "screen.h"

#undef LOG_TAG
#define LOG_TAG "ohscrcpy_vdisplay"
#undef LOG_DOMAIN
#define LOG_DOMAIN 0xD002B04

using namespace OHOS;

namespace ohscrcpy {
namespace {

/**
 * 屏名必须落在 WMS 的虚拟屏接入白名单里：
 *   window_scene/screen_session_manager_client/src/screen_session_manager_client.cpp:33
 *   VIRTUAL_SCREEN_NAME_SET = { HiCar, SuperLauncher, CastEngine, DevEcoViewer,
 *                               CeliaView, PadWithCar, CooperationExtend }
 * 且优选同时属于 INDIVIDUAL_SCREEN_GROUP_SET（screen_session_manager.cpp:214），
 * 这样该屏会拿到自己的独立屏幕组，可以独立承载内容。二者的交集是：
 *   DevEcoViewer / HiCar / SuperLauncher / CeliaView
 * 其中 "DevEcoViewer" 语义上就是「在 PC 上看设备画面」，与本工具用途一致。
 * 注意不要用 "CastEngine"：它被建成镜像型投屏屏（SCREEN_MIRROR + PHYSICAL_MIRROR），
 * 必须有真实源屏，对无屏设备无用。
 */
constexpr const char *kVirtualScreenName = "DevEcoViewer";

/** 与同平台正常出图设备（swqaewqe 1920x1080）实测的 VPR 保持一致 */
constexpr float kVirtualScreenDensity = 1.5f;

/** 设为默认屏后等待其生效的最长时间（20 x 100ms = 2s） */
constexpr int kWaitDefaultDisplayRetries = 20;
constexpr int kWaitDefaultDisplayIntervalMs = 100;

Rosen::ScreenId g_screenId = Rosen::SCREEN_ID_INVALID;

/** 查找已存在的同名虚拟屏（可能是上次遗留的），返回其 ScreenId 或 SCREEN_ID_INVALID */
Rosen::ScreenId FindOurVirtualScreen()
{
    std::vector<sptr<Rosen::Screen>> screens;
    if (Rosen::ScreenManager::GetInstance().GetAllScreens(screens) != Rosen::DMError::DM_OK) {
        HILOG_WARN(LOG_CORE, "GetAllScreens 失败");
        return Rosen::SCREEN_ID_INVALID;
    }
    for (const auto &screen : screens) {
        if (screen != nullptr && screen->GetName() == kVirtualScreenName) {
            return screen->GetId();
        }
    }
    return Rosen::SCREEN_ID_INVALID;
}
} // namespace

bool HasDefaultDisplay()
{
    return Rosen::DisplayManager::GetInstance().GetDefaultDisplay() != nullptr;
}

bool HasRealDisplay()
{
    std::vector<sptr<Rosen::Screen>> screens;
    if (Rosen::ScreenManager::GetInstance().GetAllScreens(screens) != Rosen::DMError::DM_OK) {
        // 枚举失败时退回「有默认屏就当成有真实屏」，保持原有行为
        return HasDefaultDisplay();
    }
    for (const auto &screen : screens) {
        if (screen != nullptr && screen->GetName() != kVirtualScreenName) {
            HILOG_INFO(LOG_CORE, "发现真实屏幕: %{public}s (id=%{public}" PRIu64 ")",
                       screen->GetName().c_str(), screen->GetId());
            return true;
        }
    }
    return false;
}

bool CreateVirtualDisplayAsDefault(int32_t width, int32_t height,
                                   OHOS::sptr<OHOS::Surface> targetSurface,
                                   uint32_t densityDpi)
{
    if (targetSurface == nullptr) {
        HILOG_ERROR(LOG_CORE, "targetSurface 为空：虚拟屏必须有一个渲染目标 Surface，"
                              "否则 RS 会把该屏置为 DISABLED");
        return false;
    }

    Rosen::ScreenId existing = FindOurVirtualScreen();
    if (existing != Rosen::SCREEN_ID_INVALID) {
        // 复用已有的同名虚拟屏（通常是上次进程被强杀时留下的）。
        // 之所以复用而不是销毁重建：AbstractScreenController::DestroyVirtualScreen 对
        // 「默认虚拟屏」直接返回 DM_ERROR_INVALID_PARAM（"Not destroy default virtual screen"），
        // 而 SetVirtualScreenSurface 没有创建者归属检查，可以安全地把输出目标换成
        // 本次的编码器 Surface。
        g_screenId = existing;
        Rosen::DMError ret = Rosen::ScreenManager::GetInstance().SetVirtualScreenSurface(
            g_screenId, targetSurface);
        HILOG_WARN(LOG_CORE,
                   "复用遗留虚拟屏 screenId=%{public}" PRIu64 "，SetVirtualScreenSurface ret=%{public}d",
                   g_screenId, static_cast<int32_t>(ret));
        if (ret != Rosen::DMError::DM_OK) {
            HILOG_ERROR(LOG_CORE, "替换虚拟屏输出 Surface 失败");
            return false;
        }
    } else {
        Rosen::VirtualScreenOption option;
        option.name_      = kVirtualScreenName;
        option.width_     = static_cast<uint32_t>(width > 0 ? width : 1920);
        option.height_    = static_cast<uint32_t>(height > 0 ? height : 1080);
        option.density_   = kVirtualScreenDensity;
        option.surface_   = targetSurface;
        option.flags_     = 0;
        option.isForShot_ = true;

        g_screenId = Rosen::ScreenManager::GetInstance().CreateVirtualScreen(option);
        if (g_screenId == Rosen::SCREEN_ID_INVALID) {
            HILOG_ERROR(LOG_CORE,
                        "CreateVirtualScreen 失败（屏名=%{public}s；检查是否在 WMS 白名单内、"
                        "以及调用方是否有创建权限）", kVirtualScreenName);
            return false;
        }
        HILOG_INFO(LOG_CORE,
                   "虚拟屏已创建: screenId=%{public}" PRIu64 ", %{public}ux%{public}u, name=%{public}s",
                   g_screenId, option.width_, option.height_, kVirtualScreenName);
    }

    // 运行时可改密度：VPR = dpi/160。这是清晰度的关键开关 —— VPR 1.5 等于让 UI 按
    // 1280x720 逻辑分辨率排版再放大 1.5 倍，文字/图标边缘会发虚；传 160 即 1:1。
    if (densityDpi > 0) {
        auto screen = Rosen::ScreenManager::GetInstance().GetScreenById(g_screenId);
        if (screen == nullptr) {
            HILOG_WARN(LOG_CORE, "GetScreenById(%{public}" PRIu64 ") 返回空，无法设置密度", g_screenId);
        } else {
            Rosen::DMError dret = screen->SetDensityDpi(densityDpi);
            HILOG_WARN(LOG_CORE, "SetDensityDpi(%{public}u -> VPR %{public}.2f) ret=%{public}d",
                       densityDpi, static_cast<double>(densityDpi) / 160.0, static_cast<int32_t>(dret));
        }
    }

    // 设为默认屏：DMS 会写入 defaultRsScreenId_ 并 onConnect_，
    // 之后 GetDefaultDisplaySync() 才能返回该屏，新建窗口也才会归属到它。
    // 已经是默认屏时（复用遗留屏的情形）无需重复设置。
    if (!HasDefaultDisplay()) {
        bool ok = Rosen::DisplayManager::GetInstance().SetVirtualScreenAsDefault(g_screenId);
        HILOG_INFO(LOG_CORE, "SetVirtualScreenAsDefault(%{public}" PRIu64 ") = %{public}d",
                   g_screenId, static_cast<int32_t>(ok));
        if (!ok) {
            return false;
        }
    } else {
        HILOG_INFO(LOG_CORE, "默认屏已存在（复用遗留虚拟屏），无需重复设置");
    }

    // WMS 侧还要走 CheckIfNeedConnectScreen() 白名单、再建立 AbstractDisplay，给点时间生效
    for (int i = 0; i < kWaitDefaultDisplayRetries; i++) {
        if (HasDefaultDisplay()) {
            HILOG_INFO(LOG_CORE, "默认屏已就绪（第 %{public}d 次探测）", i + 1);
            return true;
        }
        usleep(kWaitDefaultDisplayIntervalMs * 1000);
    }
    HILOG_ERROR(LOG_CORE, "虚拟屏已设为默认屏，但仍取不到默认屏");
    return false;
}

void DestroyVirtualDisplay()
{
    if (g_screenId == Rosen::SCREEN_ID_INVALID) {
        return;
    }
    // 注意：一旦该屏被 SetVirtualScreenAsDefault 设为默认屏，DMS 会拒绝销毁它
    //（AbstractScreenController::DestroyVirtualScreen: "Not destroy default virtual screen"，
    //  返回 DM_ERROR_INVALID_PARAM=130）。这里仍然尝试一次，失败只是打日志，
    // 下次启动会通过 FindOurVirtualScreen() + SetVirtualScreenSurface() 复用它。
    Rosen::DMError ret = Rosen::ScreenManager::GetInstance().DestroyVirtualScreen(g_screenId);
    HILOG_INFO(LOG_CORE, "销毁虚拟屏 %{public}" PRIu64 ", ret=%{public}d",
               g_screenId, static_cast<int32_t>(ret));
    g_screenId = Rosen::SCREEN_ID_INVALID;
}

} // namespace ohscrcpy
