/*
 * Copyright (C) 2024 Huawei Device Co., Ltd.
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
 * avplay — Command-line media player demo using the C++ inner API.
 *          Creates a real Rosen window so video frames are rendered on screen.
 *
 * Usage  : avplay <file>
 * Example: avplay demo.mp4
 */

#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

// C++ inner API
#include "media_errors.h"
#include "player.h"

// Window / Surface
#include "surface.h"
#include "ui/rs_surface_node.h"
#include "wm/window.h"
#include "wm/window_option.h"

namespace OHOS {
namespace Media {

/* --------------------------------------------------------------------------
 * AvPlayCallback
 * -------------------------------------------------------------------------- */
class AvPlayCallback : public PlayerCallback {
public:
    bool WaitForState(PlayerStates target, int timeoutSec = 15)
    {
        std::unique_lock<std::mutex> lk(mtx_);
        return cv_.wait_for(lk, std::chrono::seconds(timeoutSec), [&] {
            return state_ == target || hasError_ || completed_;
        });
    }

    PlayerStates GetState() const { std::lock_guard<std::mutex> lk(mtx_); return state_; }
    bool HasError()      const { std::lock_guard<std::mutex> lk(mtx_); return hasError_; }
    bool IsCompleted()   const { std::lock_guard<std::mutex> lk(mtx_); return completed_; }
    std::string GetErrorMsg() const { std::lock_guard<std::mutex> lk(mtx_); return errorMsg_; }

    void OnInfo(PlayerOnInfoType type, int32_t extra, const Format &infoBody) override
    {
        switch (type) {
            case INFO_TYPE_STATE_CHANGE: {
                // state is carried in extra
                auto newState = static_cast<PlayerStates>(extra);
                {
                    std::lock_guard<std::mutex> lk(mtx_);
                    state_ = newState;
                    printf("[avplay] State -> %d\n", extra);
                    if (newState == PLAYER_PLAYBACK_COMPLETE) {
                        completed_ = true;
                    }
                }
                cv_.notify_all();
                break;
            }
            case INFO_TYPE_EOS: {
                printf("[avplay] EOS.\n");
                { std::lock_guard<std::mutex> lk(mtx_); completed_ = true; }
                cv_.notify_all();
                break;
            }
            case INFO_TYPE_DURATION_UPDATE: {
                int32_t dur = 0;
                infoBody.GetIntValue(std::string(PlayerKeys::PLAYER_DURATION), dur);
                printf("[avplay] Duration: %d ms\n", dur);
                break;
            }
            case INFO_TYPE_RESOLUTION_CHANGE: {
                int32_t w = 0, h = 0;
                infoBody.GetIntValue(std::string(PlayerKeys::PLAYER_WIDTH), w);
                infoBody.GetIntValue(std::string(PlayerKeys::PLAYER_HEIGHT), h);
                printf("[avplay] Video: %dx%d\n", w, h);
                break;
            }
            case INFO_TYPE_BUFFERING_UPDATE: {
                int32_t bufType = 0, bufVal = 0;
                infoBody.GetIntValue(std::string(PlayerKeys::PLAYER_BUFFERING_TYPE), bufType);
                infoBody.GetIntValue(std::string(PlayerKeys::PLAYER_BUFFERING_VALUE), bufVal);
                if (bufType == static_cast<int32_t>(OHOS::Media::BUFFERING_PERCENT)) {
                    printf("[avplay] Buffering: %d%%\n", bufVal);
                } else if (bufType == static_cast<int32_t>(OHOS::Media::BUFFERING_START)) {
                    printf("[avplay] Buffering started.\n");
                } else if (bufType == static_cast<int32_t>(OHOS::Media::BUFFERING_END)) {
                    printf("[avplay] Buffering ended.\n");
                }
                break;
            }
            default:
                break;
        }
    }

    void OnError(int32_t errorCode, const std::string &errorMsg) override
    {
        fprintf(stderr, "[avplay] Error: code=%d  msg=%s\n", errorCode, errorMsg.c_str());
        { std::lock_guard<std::mutex> lk(mtx_); hasError_ = true; errorCode_ = errorCode; errorMsg_ = errorMsg; }
        cv_.notify_all();
    }

private:
    mutable std::mutex      mtx_;
    std::condition_variable cv_;
    PlayerStates state_     { PLAYER_IDLE };
    bool         completed_ { false };
    bool         hasError_  { false };
    int32_t      errorCode_ { 0 };
    std::string  errorMsg_;
};

} // namespace Media
} // namespace OHOS

/* --------------------------------------------------------------------------
 * Window helpers
 * -------------------------------------------------------------------------- */
static OHOS::sptr<OHOS::Rosen::Window> g_window;

static OHOS::sptr<OHOS::Surface> CreateWindowSurface(int32_t width, int32_t height)
{
    OHOS::sptr<OHOS::Rosen::WindowOption> option = new OHOS::Rosen::WindowOption();
    option->SetWindowRect({ 0, 0, static_cast<uint32_t>(width), static_cast<uint32_t>(height) });
    option->SetWindowType(OHOS::Rosen::WindowType::WINDOW_TYPE_APP_LAUNCHING);
    option->SetWindowMode(OHOS::Rosen::WindowMode::WINDOW_MODE_FULLSCREEN);

    g_window = OHOS::Rosen::Window::Create("avplay_window", option);
    if (g_window == nullptr || g_window->GetSurfaceNode() == nullptr) {
        fprintf(stderr, "[avplay] Window::Create failed.\n");
        return nullptr;
    }

    g_window->Show();
    auto surfaceNode = g_window->GetSurfaceNode();
    if (surfaceNode == nullptr) {
        fprintf(stderr, "[avplay] GetSurfaceNode failed.\n");
        return nullptr;
    }
    return surfaceNode->GetSurface();
}

/* --------------------------------------------------------------------------
 * Helpers
 * -------------------------------------------------------------------------- */
static void PrintUsage(const char *prog)
{
    fprintf(stderr, "Usage  : %s <media_file> [width height]\n", prog);
    fprintf(stderr, "Example: %s demo.mp4\n", prog);
    fprintf(stderr, "         %s demo.mp4 1920 1080\n", prog);
}

static void PrintProgress(int32_t posMs, int32_t durMs)
{
    int posSec = posMs / 1000, durSec = durMs / 1000;
    printf("\r[avplay] %02d:%02d / %02d:%02d", posSec / 60, posSec % 60, durSec / 60, durSec % 60);
    fflush(stdout);
}

/* --------------------------------------------------------------------------
 * main
 * -------------------------------------------------------------------------- */
int main(int argc, char *argv[])
{
    if (argc < 2) { PrintUsage(argv[0]); return 1; }

    const char *filePath = argv[1];
    int32_t winW = (argc >= 4) ? atoi(argv[2]) : 1280;
    int32_t winH = (argc >= 4) ? atoi(argv[3]) : 720;

    // Open file
    int fd = open(filePath, O_RDONLY);
    if (fd < 0) { fprintf(stderr, "[avplay] Cannot open '%s': %s\n", filePath, strerror(errno)); return 1; }
    struct stat st {};
    fstat(fd, &st);
    int64_t fileSize = static_cast<int64_t>(st.st_size);
    printf("[avplay] Opening '%s' (%lld bytes)\n", filePath, static_cast<long long>(fileSize));

    // Create window surface
    auto surface = CreateWindowSurface(winW, winH);
    if (surface == nullptr) {
        fprintf(stderr, "[avplay] Failed to create window surface.\n");
        close(fd);
        return 1;
    }
    printf("[avplay] Window created (%dx%d).\n", winW, winH);

    // Create player
    auto player = OHOS::Media::PlayerFactory::CreatePlayer();
    if (player == nullptr) { fprintf(stderr, "[avplay] CreatePlayer failed.\n"); close(fd); return 1; }

    auto cb = std::make_shared<OHOS::Media::AvPlayCallback>();
    player->SetPlayerCallback(cb);

    // SetSource
    int32_t ret = player->SetSource(fd, 0, fileSize);
    if (ret != OHOS::Media::MSERR_OK) {
        fprintf(stderr, "[avplay] SetSource failed: %d\n", ret);
        player->Release();
        close(fd);
        return 1;
    }

    // SetVideoSurface BEFORE SetSource
    ret = player->SetVideoSurface(surface);
    if (ret != OHOS::Media::MSERR_OK) {
        fprintf(stderr, "[avplay] SetVideoSurface failed: %d\n", ret);
        player->Release();
        close(fd);
        return 1;
    }

    // PrepareAsync
    ret = player->PrepareAsync();
    if (ret != OHOS::Media::MSERR_OK) {
        fprintf(stderr, "[avplay] PrepareAsync failed: %d\n", ret);
        player->Release();
        close(fd);
        return 1;
    }

    if (!cb->WaitForState(OHOS::Media::PLAYER_PREPARED, 15)) {
        fprintf(stderr, "[avplay] Timed out waiting for PREPARED.\n");
        player->Release();
        close(fd);
        return 1;
    }
    if (cb->HasError()) {
        fprintf(stderr, "[avplay] Error during prepare: %s\n", cb->GetErrorMsg().c_str());
        player->Release();
        close(fd);
        return 1;
    }

    int32_t durationMs = 0;
    player->GetDuration(durationMs);
    int32_t videoW = player->GetVideoWidth();
    int32_t videoH = player->GetVideoHeight();
    printf("[avplay] Prepared. Duration: %d ms", durationMs);
    if (videoW > 0) { printf("  Video: %dx%d", videoW, videoH); }
    printf("\n");

    // Play
    ret = player->Play();
    if (ret != OHOS::Media::MSERR_OK) {
        fprintf(stderr, "[avplay] Play failed: %d\n", ret);
        player->Release();
        close(fd);
        return 1;
    }

    if (!cb->WaitForState(OHOS::Media::PLAYER_STARTED)) {
        fprintf(stderr, "[avplay] Timed out waiting for STARTED.\n");
        player->Release();
        close(fd);
        return 1;
    }
    printf("[avplay] Playing...\n");

    // Poll progress
    while (!cb->HasError() && !cb->IsCompleted()) {
        int32_t curPos = 0;
        player->GetCurrentTime(curPos);
        PrintProgress(curPos, durationMs);
        usleep(500 * 1000);
    }
    printf("\n");

    if (cb->HasError()) {
        fprintf(stderr, "[avplay] Playback error: %s\n", cb->GetErrorMsg().c_str());
    } else {
        printf("[avplay] Playback finished.\n");
    }

    // Cleanup
    player->Stop();
    player->Release();
    close(fd);
    if (g_window != nullptr) { g_window->Destroy(); }

    return cb->HasError() ? 1 : 0;
}