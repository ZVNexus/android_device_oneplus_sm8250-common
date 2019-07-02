/*
 * Copyright (C) 2020 The LineageOS Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "FingerprintInscreenService"

#include "FingerprintInscreen.h"
#include <android-base/logging.h>
#include <hidl/HidlTransportSupport.h>
#include <fstream>
#include <linux/netlink.h>
#include <sys/socket.h>
#include <thread>

#define OP_ENABLE_FP_LONGPRESS 3
#define OP_DISABLE_FP_LONGPRESS 4
#define OP_RESUME_FP_ENROLL 8
#define OP_FINISH_FP_ENROLL 10

#define OP_DISPLAY_AOD_MODE 8
#define OP_DISPLAY_NOTIFY_PRESS 9
#define OP_DISPLAY_SET_DIM 10

// This is not a typo by me. It's by OnePlus.
#define HBM_ENABLE_PATH "/sys/class/drm/card0-DSI-1/op_friginer_print_hbm"
#define DIM_AMOUNT_PATH "/sys/class/drm/card0-DSI-1/dim_alpha"

#define GF_NETLINK_ROUTE 25
#define GF_NETLINK_TP_TOUCHDOWN 4
#define GF_NETLINK_TP_TOUCHUP 5

#define SIFP_NETLINK_ROUTE 30
#define SIFP_NETLINK_TP_TOUCHDOWN 7
#define SIFP_NETLINK_TP_TOUCHUP 8

#define MAX_NETLINK_PAYLOAD 1024

namespace vendor {
namespace pa {
namespace biometrics {
namespace fingerprint {
namespace inscreen {
namespace V1_0 {
namespace implementation {

/*
 * Write value to path and close file.
 */
template <typename T>
static void set(const std::string& path, const T& value) {
    std::ofstream file(path);
    file << value;
}

template <typename T>
static T get(const std::string& path, const T& def) {
    std::ifstream file(path);
    T result;

    file >> result;
    return file.fail() ? def : result;
}

FingerprintInscreen::FingerprintInscreen() {
    this->mVendorFpService = IVendorFingerprintExtensions::getService();
    this->mVendorDisplayService = IOneplusDisplay::getService();

    const auto listenToNetlinkMsgs = [this](int protocol, int msgDown, int msgUp) {
        auto sock_fd = socket(PF_NETLINK, SOCK_RAW, protocol);

        if (sock_fd < 0) {
            LOG(ERROR) << "Failed to open socket";
            return;
        }

        sockaddr_nl src_addr{};
        src_addr.nl_family = AF_NETLINK;
        src_addr.nl_pid = getpid();

        sockaddr_nl dest_addr{};
        dest_addr.nl_family = AF_NETLINK;
        dest_addr.nl_pid = 0;
        dest_addr.nl_groups = 0;

        if (bind(sock_fd, reinterpret_cast<sockaddr *>(&src_addr), sizeof(src_addr)) == -1) {
            LOG(ERROR) << "Failed to bind socket";
            return;
        }

        auto nlh = reinterpret_cast<nlmsghdr *>(malloc(NLMSG_SPACE(MAX_NETLINK_PAYLOAD)));
        memset(nlh, 0, NLMSG_SPACE(MAX_NETLINK_PAYLOAD));
        nlh->nlmsg_len = NLMSG_SPACE(MAX_NETLINK_PAYLOAD);
        nlh->nlmsg_pid = getpid();
        nlh->nlmsg_flags = 0;

        iovec iov{};
        iov.iov_base = nlh;
        iov.iov_len = nlh->nlmsg_len;

        msghdr msg{};
        msg.msg_name = &dest_addr;
        msg.msg_namelen = sizeof(dest_addr);
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;

        // Send message to kernel
        sendmsg(sock_fd, &msg, 0);

        while (true) {
            // Receive message from kernel
            recvmsg(sock_fd, &msg, 0);

            auto msgId = *reinterpret_cast<int8_t *>(NLMSG_DATA(nlh));

            if (msgId == msgDown) {
                Return<void> ret = mCallback->onFingerDown();
                if (!ret.isOk()) {
                    LOG(ERROR) << "FingerDown() error: " << ret.description();
                }
            } else if (msgId == msgUp) {
                Return<void> ret = mCallback->onFingerUp();
                if (!ret.isOk()) {
                    LOG(ERROR) << "FingerUp() error: " << ret.description();
                }
            }
        }
    };

    std::thread([=] {
        listenToNetlinkMsgs(GF_NETLINK_ROUTE, GF_NETLINK_TP_TOUCHDOWN, GF_NETLINK_TP_TOUCHUP);
    }).detach();

    std::thread([=] {
        listenToNetlinkMsgs(SIFP_NETLINK_ROUTE, SIFP_NETLINK_TP_TOUCHDOWN, SIFP_NETLINK_TP_TOUCHUP);
    }).detach();
}

Return<void> FingerprintInscreen::onStartEnroll() {
    LOG(INFO) << __func__;

    this->mVendorFpService->updateStatus(OP_DISABLE_FP_LONGPRESS);
    this->mVendorFpService->updateStatus(OP_RESUME_FP_ENROLL);

    return Void();
}

Return<void> FingerprintInscreen::onFinishEnroll() {
    LOG(INFO) << __func__;

    this->mVendorFpService->updateStatus(OP_FINISH_FP_ENROLL);

    return Void();
}

Return<void> FingerprintInscreen::onPress() {
    LOG(INFO) << __func__;

    this->mVendorDisplayService->setMode(OP_DISPLAY_AOD_MODE, 2);
    this->mVendorDisplayService->setMode(OP_DISPLAY_SET_DIM, 1);
    set(HBM_ENABLE_PATH, 1);
    this->mVendorDisplayService->setMode(OP_DISPLAY_NOTIFY_PRESS, 1);

    return Void();
}

Return<void> FingerprintInscreen::onRelease() {
    LOG(INFO) << __func__;

    this->mVendorDisplayService->setMode(OP_DISPLAY_AOD_MODE, 0);
    this->mVendorDisplayService->setMode(OP_DISPLAY_SET_DIM, 0);
    set(HBM_ENABLE_PATH, 0);
    this->mVendorDisplayService->setMode(OP_DISPLAY_NOTIFY_PRESS, 0);

    return Void();
}

Return<void> FingerprintInscreen::onShowFODView() {
    LOG(INFO) << __func__;

    return Void();
}

Return<void> FingerprintInscreen::onHideFODView() {
    LOG(INFO) << __func__;

    this->mVendorDisplayService->setMode(OP_DISPLAY_AOD_MODE, 0);
    this->mVendorDisplayService->setMode(OP_DISPLAY_SET_DIM, 0);
    set(HBM_ENABLE_PATH, 0);
    this->mVendorDisplayService->setMode(OP_DISPLAY_NOTIFY_PRESS, 0);

    return Void();
}

Return<bool> FingerprintInscreen::shouldHandleError(int32_t error) {
    return error != 8;
}

Return<void> FingerprintInscreen::setLongPressEnabled(bool enabled) {
    this->mVendorFpService->updateStatus(
            enabled ? OP_ENABLE_FP_LONGPRESS : OP_DISABLE_FP_LONGPRESS);

    return Void();
}

Return<int32_t> FingerprintInscreen::getDimAmount(int32_t) {
    LOG(INFO) << __func__;

    int dimAmount = get(DIM_AMOUNT_PATH, 0);
    LOG(INFO) << "dimAmount = " << dimAmount;

    return dimAmount;
}

Return<bool> FingerprintInscreen::shouldBoostBrightness() {
    return false;
}

Return<void> FingerprintInscreen::setCallback(const sp<IFingerprintInscreenCallback>& callback) {
    LOG(ERROR) << __func__;

    {
        std::lock_guard<std::mutex> _lock(mCallbackLock);
        mCallback = callback;
    }

    return Void();
}

}  // namespace implementation
}  // namespace V1_0
}  // namespace inscreen
}  // namespace fingerprint
}  // namespace biometrics
}  // namespace pa
}  // namespace vendor
