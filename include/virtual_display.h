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
 * @file virtual_display.h
 * @brief 无物理显示器时，为设备补一块「虚拟屏」并设为默认屏
 *
 * 背景（详见仓库内《无屏调试方案分析.md》）：
 *   1) OH 的屏幕采集在 ScreenCaptureServer::StartScreenCaptureInner() 一开头就要
 *          DisplayManager::GetInstance().GetDefaultDisplaySync()
 *      取不到就直接返回 331350528(0x13C00200)，而这一步在它自己创建 "screeen_capture"
 *      虚拟屏（用于镜像取画面）之前 —— 无屏设备上 DMS 里一块屏都没有，采集永远起不来。
 *   2) 更麻烦的是：即使补上了默认屏，AVScreenCapture 内部还要对默认屏做 MakeMirror，
 *      而 MakeMirror 拒绝虚拟屏作为镜像源：
 *          abstract_screen_controller.cpp: "screen is nullptr, or screenType is not real."
 *      所以「虚拟屏 + AVScreenCapture」这条路根本走不通。
 *
 * 因此本模块的用法是「虚拟屏直连编码器」：
 *      sptr<Surface> encSurface = capture->PrepareEncoderForExternalInput();
 *      CreateVirtualDisplayAsDefault(width, height, encSurface);
 *      capture->StartExternalInput();
 * 即把编码器的输入 Surface 作为虚拟屏的输出目标，由 RS 直接把该屏的合成结果渲染进
 * 编码器，完全绕开 AVScreenCapture。
 *
 * 三个必须遵守的约束（否则会静默失败/黑屏）：
 *   1) 屏名必须落在 WMS 的 VIRTUAL_SCREEN_NAME_SET 白名单里
 *      （screen_session_manager_client.cpp:33），否则
 *      ScreenSessionManagerClient::CheckIfNeedConnectScreen() 会拒绝接入，
 *      该屏永远不会有窗口、也就永远没有画面。
 *   2) 必须传入非空 Surface：rs_screen.cpp 里 producerSurface_ 为空会把该屏置为
 *      ScreenState::DISABLED，永不渲染。
 *   3) 必须设为默认屏：否则 WMS 的 GetDefaultScreenId() 返回无效值，新建窗口不会
 *      归属到该屏（scene_session_manager.cpp SetFocusedSessionDisplayIdIfNeeded）。
 */

#ifndef OHSCRCPY_VIRTUAL_DISPLAY_H
#define OHSCRCPY_VIRTUAL_DISPLAY_H

#include <cstdint>

#include "surface.h"

namespace ohscrcpy {

/**
 * 设备当前是否有【真实】屏幕。
 *
 * 不能简单地看「有没有默认屏」：本工具自己创建的虚拟屏也是默认屏，
 * 尤其是上次进程被强杀时留下的那块（SetVirtualScreenAsDefault 会把它从
 * screenAgentMap_ 里摘掉，导致创建进程死亡时 DMS 不再回收它）。
 * 若不区分，本次运行会误判为「有真实屏」而走 AVScreenCapture，
 * 然后因为 MakeMirror 拒绝虚拟屏作为镜像源而再次失败。
 *
 * 判定方式：枚举所有屏幕，只要存在屏幕名不是本工具虚拟屏名的屏幕，就算有真实屏。
 */
bool HasRealDisplay();

/**
 * 用给定 Surface 创建一块虚拟屏并设为默认屏，然后等待其生效。
 *
 * @param width         虚拟屏宽度
 * @param height        虚拟屏高度
 * @param targetSurface RS 渲染该屏内容的目标 Surface（传编码器的输入 Surface）
 * @param densityDpi    屏幕密度 DPI；VPR = densityDpi / 160。
 *                      这是清晰度的关键：VPR 1.5 意味着 UI 按 1280x720 逻辑分辨率
 *                      布局再放大 1.5 倍，文字/图标会发虚；想要 1:1 清晰就传 160。
 *                      传 0 表示不改动（沿用创建时的值）。
 * @return 成功且默认屏已就绪返回 true
 */
bool CreateVirtualDisplayAsDefault(int32_t width, int32_t height,
                                   OHOS::sptr<OHOS::Surface> targetSurface,
                                   uint32_t densityDpi);

/** 销毁本模块创建的虚拟屏（若创建过）。进程异常退出时由 DMS 兜底回收。 */
void DestroyVirtualDisplay();

/**
 * 查询当前默认屏的逻辑 displayId（取不到返回 -1）。
 *
 * 输入注入要用它来显式指定 targetDisplayId：不指定时 MMI 只在「默认显示组里
 * 取第一个屏」，一旦系统里同时存在多块屏（物理屏 + 虚拟屏、或遗留的旧虚拟屏），
 * 注入就可能落到另一块屏上，表现为「画面能看、点按却没反应」。
 */
int32_t QueryDefaultDisplayId();

} // namespace ohscrcpy

#endif // OHSCRCPY_VIRTUAL_DISPLAY_H
