/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include "VirtualCamera.h"
#include "HalCamera.h"
#include "Enumerator.h"

#include <ui/GraphicBufferAllocator.h>
#include <ui/GraphicBufferMapper.h>

using ::android::hardware::automotive::evs::V1_0::DisplayState;


namespace android {
namespace automotive {
namespace evs {
namespace V1_1 {
namespace implementation {


VirtualCamera::VirtualCamera(const std::vector<sp<HalCamera>>& halCameras) :
    mStreamState(STOPPED) {
    for (auto&& cam : halCameras) {
        mHalCamera.try_emplace(cam->getId(), cam);
    }
}


VirtualCamera::~VirtualCamera() {
    shutdown();
}


void VirtualCamera::shutdown() {
    // In normal operation, the stream should already be stopped by the time we get here
    if (mStreamState == RUNNING) {
        // Note that if we hit this case, no terminating frame will be sent to the client,
        // but they're probably already dead anyway.
        ALOGW("Virtual camera being shutdown while stream is running");

        // Tell the frame delivery pipeline we don't want any more frames
        mStreamState = STOPPING;

        for (auto&& [key, hwCamera] : mHalCamera) {
            auto pHwCamera = hwCamera.promote();
            if (pHwCamera == nullptr) {
                ALOGW("Camera device %s is not alive.", key.c_str());
                continue;
            }

            if (mFramesHeld[key].size() > 0) {
                ALOGW("VirtualCamera destructing with frames in flight.");

                // Return to the underlying hardware camera any buffers the client was holding
                for (auto&& heldBuffer : mFramesHeld[key]) {
                    // Tell our parent that we're done with this buffer
                    pHwCamera->doneWithFrame(heldBuffer);
                }
                mFramesHeld[key].clear();
            }

            // Retire from a master client
            pHwCamera->unsetMaster(this);

            // Give the underlying hardware camera the heads up that it might be time to stop
            pHwCamera->clientStreamEnding(this);
        }

        // Join a capture thread
        if (mCaptureThread.joinable()) {
            mCaptureThread.join();
        }

        mFramesHeld.clear();
    }

    // Drop our reference to our associated hardware camera
    mHalCamera.clear();
}


std::vector<sp<HalCamera>> VirtualCamera::getHalCameras() {
    std::vector<sp<HalCamera>> cameras;
    for (auto&& [key, cam] : mHalCamera) {
        auto ptr = cam.promote();
        if (ptr != nullptr) {
            cameras.emplace_back(ptr);
        }
    }

    return cameras;
}


bool VirtualCamera::deliverFrame(const BufferDesc_1_1& bufDesc) {
    if (mStreamState == STOPPED) {
        // A stopped stream gets no frames
        ALOGE("A stopped stream should not get any frames");
        return false;
    } else if (mFramesHeld[bufDesc.deviceId].size() >= mFramesAllowed) {
        // Indicate that we declined to send the frame to the client because they're at quota
        ALOGI("Skipping new frame as we hold %zu of %u allowed.",
              mFramesHeld[bufDesc.deviceId].size(), mFramesAllowed);

        if (mStream_1_1 != nullptr) {
            // Report a frame drop to v1.1 client.
            EvsEventDesc event;
            event.deviceId = bufDesc.deviceId;
            event.aType = EvsEventType::FRAME_DROPPED;
            auto result = mStream_1_1->notify(event);
            if (!result.isOk()) {
                ALOGE("Error delivering end of stream event");
            }
        }

        return false;
    } else {
        // Keep a record of this frame so we can clean up if we have to in case of client death
        mFramesHeld[bufDesc.deviceId].emplace_back(bufDesc);

        // v1.0 client uses an old frame-delivery mechanism.
        if (mStream_1_1 == nullptr) {
            // Forward a frame to v1.0 client
            BufferDesc_1_0 frame_1_0 = {};
            const AHardwareBuffer_Desc* pDesc =
                reinterpret_cast<const AHardwareBuffer_Desc *>(&bufDesc.buffer.description);
            frame_1_0.width     = pDesc->width;
            frame_1_0.height    = pDesc->height;
            frame_1_0.format    = pDesc->format;
            frame_1_0.usage     = pDesc->usage;
            frame_1_0.stride    = pDesc->stride;
            frame_1_0.memHandle = bufDesc.buffer.nativeHandle;
            frame_1_0.pixelSize = bufDesc.pixelSize;
            frame_1_0.bufferId  = bufDesc.bufferId;

            mStream->deliverFrame(frame_1_0);
        }

        return true;
    }
}


bool VirtualCamera::notify(const EvsEventDesc& event) {
    switch(event.aType) {
        case EvsEventType::STREAM_STOPPED:
            if (mStreamState != STOPPING) {
                // Warn if we got an unexpected stream termination
                ALOGW("Stream unexpectedly stopped, current status 0x%X", mStreamState);
            }

            // Mark the stream as stopped.
            mStreamState = STOPPED;

            if (mStream_1_1 == nullptr) {
                // Send a null frame instead, for v1.0 client
                BufferDesc_1_0 nullBuff = {};
                auto result = mStream->deliverFrame(nullBuff);
                if (!result.isOk()) {
                    ALOGE("Error delivering end of stream marker");
                }
            }
            break;

        // v1.0 client will ignore all other events.
        case EvsEventType::PARAMETER_CHANGED:
            ALOGD("A camera parameter 0x%X is set to 0x%X", event.payload[0], event.payload[1]);
            break;

        case EvsEventType::MASTER_RELEASED:
            ALOGD("The master client has been released");
            break;

        default:
            ALOGE("Unknown event id 0x%X", event.aType);
            break;
    }

    if (mStream_1_1 != nullptr) {
        // Forward a received event to the v1.1 client
        auto result = mStream_1_1->notify(event);
        if (!result.isOk()) {
            ALOGE("Failed to forward an event");
            return false;
        }
    }

    return true;
}


// Methods from ::android::hardware::automotive::evs::V1_0::IEvsCamera follow.
Return<void> VirtualCamera::getCameraInfo(getCameraInfo_cb info_cb) {
    // Straight pass through to hardware layer
    auto halCamera = mHalCamera.begin()->second.promote();
    if (halCamera != nullptr) {
        return halCamera->getHwCamera()->getCameraInfo(info_cb);
    } else {
        CameraDesc nullCamera = {};
        info_cb(nullCamera.v1);
        return Void();
    }
}


Return<EvsResult> VirtualCamera::setMaxFramesInFlight(uint32_t bufferCount) {
    // How many buffers are we trying to add (or remove if negative)
    int bufferCountChange = bufferCount - mFramesAllowed;

    // Ask our parent for more buffers
    bool result = true;
    std::vector<sp<HalCamera>> changedCameras;
    for (auto&& [key, hwCamera] : mHalCamera) {
        auto pHwCam = hwCamera.promote();
        if (pHwCam == nullptr) {
            continue;
        }

        result = pHwCam->changeFramesInFlight(bufferCountChange);
        if (!result) {
            ALOGE("%s: Failed to change buffer count by %d to %d",
                  key.c_str(), bufferCountChange, bufferCount);
            break;
        }

        changedCameras.emplace_back(pHwCam);
    }

    // Update our notion of how many frames we're allowed
    mFramesAllowed = bufferCount;

    if (!result) {
        // Rollback changes because we failed to update all cameras
        for (auto&& hwCamera : changedCameras) {
            ALOGW("Rollback a change on %s", hwCamera->getId().c_str());
            hwCamera->changeFramesInFlight(-bufferCountChange);
        }

        // Restore the original buffer count
        mFramesAllowed -= bufferCountChange;
        return EvsResult::BUFFER_NOT_AVAILABLE;
    } else {
        return EvsResult::OK;
    }
}


Return<EvsResult> VirtualCamera::startVideoStream(const ::android::sp<IEvsCameraStream_1_0>& stream)  {
    // We only support a single stream at a time
    if (mStreamState != STOPPED) {
        ALOGE("ignoring startVideoStream call when a stream is already running.");
        return EvsResult::STREAM_ALREADY_RUNNING;
    }

    // Validate our held frame count is starting out at zero as we expect
    assert(mFramesHeld.size() == 0);

    // Record the user's callback for use when we have a frame ready
    mStream = stream;
    mStream_1_1 = IEvsCameraStream_1_1::castFrom(stream).withDefault(nullptr);
    if (mStream_1_1 == nullptr) {
        ALOGI("Start video stream for v1.0 client.");
    } else {
        ALOGI("Start video stream for v1.1 client.");
    }

    mStreamState = RUNNING;

    // Tell the underlying camera hardware that we want to stream
    auto iter = mHalCamera.begin();
    while (iter != mHalCamera.end()) {
        auto pHwCamera = iter->second.promote();
        if (pHwCamera == nullptr) {
            ALOGE("Failed to start a video stream on %s", iter->first.c_str());
            continue;
        }

        ALOGI("%s starts a video stream on %s", __FUNCTION__, iter->first.c_str());
        Return<EvsResult> result = pHwCamera->clientStreamStarting();
        if ((!result.isOk()) || (result != EvsResult::OK)) {
            // If we failed to start the underlying stream, then we're not actually running
            mStream = mStream_1_1 = nullptr;
            mStreamState = STOPPED;

            // Request to stop streams started by this client.
            auto rb = mHalCamera.begin();
            while (rb != iter) {
                auto ptr = rb->second.promote();
                if (ptr != nullptr) {
                    ptr->clientStreamEnding(this);
                }
                ++rb;
            }
            return EvsResult::UNDERLYING_SERVICE_ERROR;
        }
        ++iter;
    }

    // Start a thread that waits on the fence and forwards collected frames
    // to the v1.1 client.
    if (mStream_1_1 != nullptr) {
        mCaptureThread = std::thread([this]() {
            // TODO(b/145466570): With a proper camera hang handler, we may want
            // to reduce an amount of timeout.
            constexpr int kFrameTimeoutMs = 5000; // timeout in ms.
            int64_t lastFrameTimestamp = -1;
            while (mStreamState == RUNNING) {
                UniqueFence fence;
                unsigned count = 0;
                for (auto&& [key, hwCamera] : mHalCamera) {
                    auto pHwCamera = hwCamera.promote();
                    if (pHwCamera == nullptr) {
                        ALOGW("Invalid camera %s is ignored.", key.c_str());
                        continue;
                    }

                    UniqueFence another = pHwCamera->requestNewFrame(this, lastFrameTimestamp);
                    if (!another) {
                        ALOGW("%s returned an invalid fence.", key.c_str());
                        continue;
                    }

                    fence = UniqueFence::Merge("MergedFrameFence",
                                               fence,
                                               another);
                    ++count;
                }

                if (fence.Wait(kFrameTimeoutMs) < 0) {
                    // TODO(b/145466570): Replace this temporarily camera hang
                    // handler.
                    ALOGE("%p: Camera hangs? %s", this, strerror(errno));
                    break;
                } else if (mStreamState == RUNNING) {
                    // Fetch frames and forward to the client
                    if (mFramesHeld.size() > 0 && mStream_1_1 != nullptr) {
                        // Pass this buffer through to our client
                        hardware::hidl_vec<BufferDesc_1_1> frames;
                        frames.resize(count);
                        unsigned i = 0;
                        for (auto&& [key, hwCamera] : mHalCamera) {
                            auto pHwCamera = hwCamera.promote();
                            if (pHwCamera == nullptr) {
                                continue;
                            }

                            const auto frame = mFramesHeld[key].back();
                            if (frame.timestamp > lastFrameTimestamp) {
                                lastFrameTimestamp = frame.timestamp;
                            }
                            frames[i++] = frame;
                        }
                        mStream_1_1->deliverFrame_1_1(frames);
                    }
                }
            }
        });
    }

    // TODO(changyeon):
    // Detect and exit if we encounter a stalled stream or unresponsive driver?
    // Consider using a timer and watching for frame arrival?

    return EvsResult::OK;
}


Return<void> VirtualCamera::doneWithFrame(const BufferDesc_1_0& buffer) {
    if (buffer.memHandle == nullptr) {
        ALOGE("ignoring doneWithFrame called with invalid handle");
    } else {
        // Find this buffer in our "held" list
        auto& frameQueue = mFramesHeld.begin()->second;
        auto it = frameQueue.begin();
        while (it != frameQueue.end()) {
            if (it->bufferId == buffer.bufferId) {
                // found it!
                break;
            }
            ++it;
        }
        if (it == frameQueue.end()) {
            // We should always find the frame in our "held" list
            ALOGE("Ignoring doneWithFrame called with unrecognized frameID %d", buffer.bufferId);
        } else {
            // Take this frame out of our "held" list
            frameQueue.erase(it);

            // Tell our parent that we're done with this buffer
            auto pHwCamera = mHalCamera.begin()->second.promote();
            if (pHwCamera != nullptr) {
                pHwCamera->doneWithFrame(buffer);
            } else {
                ALOGW("Possible memory leak because a device %s is not valid.",
                      mHalCamera.begin()->first.c_str());
            }
        }
    }

    return Void();
}


Return<void> VirtualCamera::stopVideoStream()  {
    if (mStreamState == RUNNING) {
        // Tell the frame delivery pipeline we don't want any more frames
        mStreamState = STOPPING;

        // Deliver an empty frame to close out the frame stream
        if (mStream_1_1 != nullptr) {
            // v1.1 client waits for a stream stopped event
            EvsEventDesc event;
            event.aType = EvsEventType::STREAM_STOPPED;
            auto result = mStream_1_1->notify(event);
            if (!result.isOk()) {
                ALOGE("Error delivering end of stream event");
            }
        } else {
            // v1.0 client expects a null frame at the end of the stream
            BufferDesc_1_0 nullBuff = {};
            auto result = mStream->deliverFrame(nullBuff);
            if (!result.isOk()) {
                ALOGE("Error delivering end of stream marker");
            }
        }

        // Since we are single threaded, no frame can be delivered while this function is running,
        // so we can go directly to the STOPPED state here on the server.
        // Note, however, that there still might be frames already queued that client will see
        // after returning from the client side of this call.
        mStreamState = STOPPED;

        // Give the underlying hardware camera the heads up that it might be time to stop
        for (auto&& [key, hwCamera] : mHalCamera) {
            auto pHwCamera = hwCamera.promote();
            if (pHwCamera != nullptr) {
                pHwCamera->clientStreamEnding(this);
            }
        }

        // Join a thread
        if (mCaptureThread.joinable()) {
            mCaptureThread.join();
        }

    }

    return Void();
}


Return<int32_t> VirtualCamera::getExtendedInfo(uint32_t opaqueIdentifier)  {
    if (mHalCamera.size() > 1) {
        ALOGW("Logical camera device does not support %s", __FUNCTION__);
        return 0;
    }

    // Pass straight through to the hardware device
    auto pHwCamera = mHalCamera.begin()->second.promote();
    if (pHwCamera != nullptr) {
        return pHwCamera->getHwCamera()->getExtendedInfo(opaqueIdentifier);
    } else {
        ALOGW("%s is invalid.", mHalCamera.begin()->first.c_str());
        return 0;
    }
}


Return<EvsResult> VirtualCamera::setExtendedInfo(uint32_t opaqueIdentifier, int32_t opaqueValue)  {
    if (mHalCamera.size() > 1) {
        ALOGW("Logical camera device does not support %s", __FUNCTION__);
        return EvsResult::INVALID_ARG;
    }

    // Pass straight through to the hardware device
    auto pHwCamera = mHalCamera.begin()->second.promote();
    if (pHwCamera != nullptr) {
        return pHwCamera->getHwCamera()->setExtendedInfo(opaqueIdentifier, opaqueValue);
    } else {
        ALOGW("%s is invalid.", mHalCamera.begin()->first.c_str());
        return EvsResult::INVALID_ARG;
    }
}


// Methods from ::android::hardware::automotive::evs::V1_1::IEvsCamera follow.
Return<void> VirtualCamera::getCameraInfo_1_1(getCameraInfo_1_1_cb info_cb) {
    if (mHalCamera.size() > 1) {
        info_cb(*mDesc);
        return Void();
    }

    // Straight pass through to hardware layer
    auto pHwCamera = mHalCamera.begin()->second.promote();
    if (pHwCamera == nullptr) {
        // Return an empty list
        CameraDesc nullCamera = {};
        info_cb(nullCamera);
        return Void();
    }

    auto hwCamera_1_1 =
        IEvsCamera_1_1::castFrom(pHwCamera->getHwCamera()).withDefault(nullptr);
    if (hwCamera_1_1 != nullptr) {
        return hwCamera_1_1->getCameraInfo_1_1(info_cb);
    } else {
        // Return an empty list
        CameraDesc nullCamera = {};
        info_cb(nullCamera);
        return Void();
    }
}


Return<EvsResult> VirtualCamera::doneWithFrame_1_1(
    const hardware::hidl_vec<BufferDesc_1_1>& buffers) {

    for (auto&& buffer : buffers) {
        if (buffer.buffer.nativeHandle == nullptr) {
            ALOGW("ignoring doneWithFrame called with invalid handle");
        } else {
            // Find this buffer in our "held" list
            auto it = mFramesHeld[buffer.deviceId].begin();
            while (it != mFramesHeld[buffer.deviceId].end()) {
                if (it->bufferId == buffer.bufferId) {
                    // found it!
                    break;
                }
                ++it;
            }
            if (it == mFramesHeld[buffer.deviceId].end()) {
                // We should always find the frame in our "held" list
                ALOGE("Ignoring doneWithFrame called with unrecognized frameID %d",
                      buffer.bufferId);
            } else {
                // Take this frame out of our "held" list
                mFramesHeld[buffer.deviceId].erase(it);

                // Tell our parent that we're done with this buffer
                auto pHwCamera = mHalCamera[buffer.deviceId].promote();
                if (pHwCamera != nullptr) {
                    pHwCamera->doneWithFrame(buffer);
                } else {
                    ALOGW("Possible memory leak; %s is not valid.", buffer.deviceId.c_str());
                }
            }
        }
    }

    return EvsResult::OK;
}


Return<EvsResult> VirtualCamera::setMaster() {
    if (mHalCamera.size() > 1) {
        ALOGW("Logical camera device does not support %s.", __FUNCTION__);
        return EvsResult::INVALID_ARG;
    }

    auto pHwCamera = mHalCamera.begin()->second.promote();
    if (pHwCamera != nullptr) {
        return pHwCamera->setMaster(this);
    } else {
        ALOGW("Camera device %s is not alive.", mHalCamera.begin()->first.c_str());
        return EvsResult::INVALID_ARG;
    }
}


Return<EvsResult> VirtualCamera::forceMaster(const sp<IEvsDisplay>& display) {
    if (mHalCamera.size() > 1) {
        ALOGW("Logical camera device does not support %s.", __FUNCTION__);
        return EvsResult::INVALID_ARG;
    }

    if (display.get() == nullptr) {
        ALOGE("%s: Passed display is invalid", __FUNCTION__);
        return EvsResult::INVALID_ARG;
    }

    DisplayState state = display->getDisplayState();
    if (state == DisplayState::NOT_OPEN ||
        state == DisplayState::DEAD ||
        state >= DisplayState::NUM_STATES) {
        ALOGE("%s: Passed display is in invalid state", __FUNCTION__);
        return EvsResult::INVALID_ARG;
    }

    auto pHwCamera = mHalCamera.begin()->second.promote();
    if (pHwCamera != nullptr) {
        return pHwCamera->forceMaster(this);
    } else {
        ALOGW("Camera device %s is not alive.", mHalCamera.begin()->first.c_str());
        return EvsResult::INVALID_ARG;
    }
}


Return<EvsResult> VirtualCamera::unsetMaster() {
    if (mHalCamera.size() > 1) {
        ALOGW("Logical camera device does not support %s.", __FUNCTION__);
        return EvsResult::INVALID_ARG;
    }

    auto pHwCamera = mHalCamera.begin()->second.promote();
    if (pHwCamera != nullptr) {
        return pHwCamera->unsetMaster(this);
    } else {
        ALOGW("Camera device %s is not alive.", mHalCamera.begin()->first.c_str());
        return EvsResult::INVALID_ARG;
    }
}


Return<void> VirtualCamera::getParameterList(getParameterList_cb _hidl_cb) {
    if (mHalCamera.size() > 1) {
        ALOGW("Logical camera device does not support %s.", __FUNCTION__);

        // Return an empty list
        hardware::hidl_vec<CameraParam> emptyList;
        _hidl_cb(emptyList);
        return Void();
    }

    // Straight pass through to hardware layer
    auto pHwCamera = mHalCamera.begin()->second.promote();
    if (pHwCamera == nullptr) {
        ALOGW("Camera device %s is not alive.", mHalCamera.begin()->first.c_str());

        // Return an empty list
        hardware::hidl_vec<CameraParam> emptyList;
        _hidl_cb(emptyList);
        return Void();
    }

    auto hwCamera_1_1 =
        IEvsCamera_1_1::castFrom(pHwCamera->getHwCamera()).withDefault(nullptr);
    if (hwCamera_1_1 != nullptr) {
        return hwCamera_1_1->getParameterList(_hidl_cb);
    } else {
        ALOGW("Camera device %s does not support a parameter programming.",
              mHalCamera.begin()->first.c_str());

        // Return an empty list
        hardware::hidl_vec<CameraParam> emptyList;
        _hidl_cb(emptyList);
        return Void();
    }
}


Return<void> VirtualCamera::getIntParameterRange(CameraParam id,
                                                 getIntParameterRange_cb _hidl_cb) {
    if (mHalCamera.size() > 1) {
        ALOGW("Logical camera device does not support %s.", __FUNCTION__);

        // Return [0, 0, 0]
        _hidl_cb(0, 0, 0);
        return Void();
    }

    // Straight pass through to hardware layer
    auto pHwCamera = mHalCamera.begin()->second.promote();
    if (pHwCamera == nullptr) {
        ALOGW("Camera device %s is not alive.", mHalCamera.begin()->first.c_str());

        // Return [0, 0, 0]
        _hidl_cb(0, 0, 0);
        return Void();
    }

    auto hwCamera_1_1 =
        IEvsCamera_1_1::castFrom(pHwCamera->getHwCamera()).withDefault(nullptr);
    if (hwCamera_1_1 != nullptr) {
        return hwCamera_1_1->getIntParameterRange(id, _hidl_cb);
    } else {
        ALOGW("Camera device %s does not support a parameter programming.",
              mHalCamera.begin()->first.c_str());

        // Return [0, 0, 0]
        _hidl_cb(0, 0, 0);
        return Void();
    }
    return Void();
}


Return<void> VirtualCamera::setIntParameter(CameraParam id,
                                            int32_t value,
                                            setIntParameter_cb _hidl_cb) {
    hardware::hidl_vec<int32_t> values;
    EvsResult status = EvsResult::INVALID_ARG;
    if (mHalCamera.size() > 1) {
        ALOGW("Logical camera device does not support %s.", __FUNCTION__);
        _hidl_cb(status, values);
        return Void();
    }

    auto pHwCamera = mHalCamera.begin()->second.promote();
    if (pHwCamera == nullptr) {
        ALOGW("Camera device %s is not alive.", mHalCamera.begin()->first.c_str());
        _hidl_cb(status, values);
        return Void();
    }

    status = pHwCamera->setParameter(this, id, value);

    values.resize(1);
    values[0] = value;
    _hidl_cb(status, values);

    return Void();
}


Return<void> VirtualCamera::getIntParameter(CameraParam id,
                                            getIntParameter_cb _hidl_cb) {
    hardware::hidl_vec<int32_t> values;
    EvsResult status = EvsResult::INVALID_ARG;
    if (mHalCamera.size() > 1) {
        ALOGW("Logical camera device does not support %s.", __FUNCTION__);
        _hidl_cb(status, values);
        return Void();
    }

    auto pHwCamera = mHalCamera.begin()->second.promote();
    if (pHwCamera == nullptr) {
        ALOGW("Camera device %s is not alive.", mHalCamera.begin()->first.c_str());
        _hidl_cb(status, values);
        return Void();
    }

    int32_t value;
    status = pHwCamera->getParameter(id, value);

    values.resize(1);
    values[0] = value;
    _hidl_cb(status, values);

    return Void();
}


} // namespace implementation
} // namespace V1_1
} // namespace evs
} // namespace automotive
} // namespace android
