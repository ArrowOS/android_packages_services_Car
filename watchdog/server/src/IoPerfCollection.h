/**
 * Copyright (c) 2020, The Android Open Source Project
 *
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

#ifndef WATCHDOG_SERVER_SRC_IOPERFCOLLECTION_H_
#define WATCHDOG_SERVER_SRC_IOPERFCOLLECTION_H_

#include <android-base/chrono_utils.h>
#include <android-base/result.h>
#include <cutils/multiuser.h>
#include <time.h>
#include <utils/Errors.h>
#include <utils/Mutex.h>

#include <string>
#include <vector>

namespace android {
namespace automotive {
namespace watchdog {

// TODO(b/148489461): Replace the below constants (except kCustomCollection* constants) with
// read-only persistent properties.
const int kTopNStatsPerCategory = 5;
const std::chrono::seconds kBoottimeCollectionInterval = 1s;
const std::chrono::seconds kPeriodicCollectionInterval = 10s;
// Number of periodic collection perf data snapshots to cache in memory.
const uint kPeriodicCollectionBufferSize = 180;

// Default values for the custom collection interval and max_duration.
const std::chrono::seconds kCustomCollectionInterval = 10s;
const std::chrono::seconds kCustomCollectionDuration = 30min;

// Performance data collected from the `/proc/uid_io/stats` file.
struct UIDIoPerfData {
    struct Stats {
        userid_t userId = 0;
        std::string packageName;
        uint64_t bytes = 0;
        double bytesPercent = 0.0;
        uint64_t fsync = 0;
        double fsyncPercent = 0.0;
    };
    Stats topNReads = {};
    Stats topNWrites = {};
};

// Performance data collected from the `/proc/stats` file.
struct SystemIoPerfData {
    uint64_t cpuIoWaitTime = 0;
    double cpuIoWaitPercent = 0.0;
    uint64_t ioBlockedProcessesCnt = 0;
    uint64_t ioBlockedProcessesPercent = 0;
};

// Performance data collected from the `/proc/[pid]/stat` and `/proc/[pid]/task/[tid]/stat` files.
struct ProcessIoPerfData {
    struct Stats {
        userid_t userId = 0;
        std::string packageName;
        uint64_t count = 0;
        uint64_t percent = 0;
    };
    Stats topNIoBlockedProcesses = {};
    Stats topNMajorPageFaults = {};
    uint64_t totalMajorPageFaults = 0;
    // Percentage of increase in the major page faults since last collection.
    double majorPageFaultsIncrease = 0.0;
};

struct IoPerfRecord {
    int64_t time;  // Collection time.
    UIDIoPerfData uidIoPerfData;
    SystemIoPerfData systemIoPerfData;
    ProcessIoPerfData processIoPerfData;
};

enum CollectionEvent {
    BOOT_TIME = 0,
    PERIODIC,
    CUSTOM,
    NONE,
};

static inline std::string toEventString(CollectionEvent event) {
    switch (event) {
        case CollectionEvent::BOOT_TIME:
            return "BOOT_TIME";
        case CollectionEvent::PERIODIC:
            return "PERIODIC";
        case CollectionEvent::CUSTOM:
            return "CUSTOM";
        case CollectionEvent::NONE:
            return "NONE";
        default:
            return "INVALID";
    }
}

// IoPerfCollection implements the I/O performance data collection module of the CarWatchDog
// service. It exposes APIs that the CarWatchDog main thread and binder service can call to start
// a collection, update the collection type, and generate collection dumps.
class IoPerfCollection {
  public:
    IoPerfCollection() : mCurrCollectionEvent(CollectionEvent::NONE) {
    }

    // Starts the boot-time collection on a separate thread and returns immediately. Must be called
    // only once. Otherwise, returns an error.
    android::base::Result<void> start();

    // Stops the boot-time collection thread, caches boot-time perf records, starts the periodic
    // collection on a separate thread, and returns immediately.
    android::base::Result<void> onBootFinished();

    // Generates a dump from the boot-time and periodic collection events.
    // Returns any error observed during the dump generation.
    status_t dump(int fd);

    // Starts a custom collection on a separate thread, stops the periodic collection (won't discard
    // the collected data), and returns immediately. Returns any error observed during this process.
    // The custom collection happens once every |interval| seconds. When the |maxDuration| is
    // reached, stops the collection, discards the collected data, and starts the periodic
    // collection. This is needed to ensure the custom collection doesn't run forever when
    // a subsequent |endCustomCollection| call is not received.
    status_t startCustomCollection(std::chrono::seconds interval = kCustomCollectionInterval,
                                   std::chrono::seconds maxDuration = kCustomCollectionDuration);

    // Stops the current custom collection thread, generates a dump, starts the periodic collection
    // on a separate thread, and returns immediately. Returns an error when there is no custom
    // collection running or when a dump couldn't be generated from the custom collection.
    status_t endCustomCollection(int fd);

  private:
    // Collects/stores the performance data for the current collection event.
    android::base::Result<void> collect();

    // Collects performance data from the `/proc/uid_io/stats` file.
    android::base::Result<void> collectUidIoPerfDataLocked();

    // Collects performance data from the `/proc/stats` file.
    android::base::Result<void> collectSystemIoPerfDataLocked();

    // Collects performance data from the `/proc/[pid]/stat` and
    // `/proc/[pid]/task/[tid]/stat` files.
    android::base::Result<void> collectProcessIoPerfDataLocked();

    // Protects |mBoottimeRecords|, |mPeriodicRecords|, |mCustomRecords|, and
    // |mCurrCollectionEvent|. Makes sure only one collection is running at any given time.
    Mutex mMutex;

    // Cache of the performance records collected during boot-time collection.
    std::vector<IoPerfRecord> mBoottimeRecords;

    // Cache of the performance records collected during periodic collection. Size of this cache is
    // limited by |kPeriodicCollectionBufferSize|.
    std::vector<IoPerfRecord> mPeriodicRecords;

    // Cache of the performance records collected during custom collection. This cache is cleared at
    // the end of every custom collection.
    std::vector<IoPerfRecord> mCustomRecords;

    // Tracks the current collection event. Updated on |start|, |onBootComplete|,
    // |startCustomCollection| and |endCustomCollection|.
    CollectionEvent mCurrCollectionEvent;
};

}  // namespace watchdog
}  // namespace automotive
}  // namespace android

#endif  //  WATCHDOG_SERVER_SRC_IOPERFCOLLECTION_H_
