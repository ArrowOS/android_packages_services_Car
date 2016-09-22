/*
 * Copyright (C) 2015 The Android Open Source Project
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

package android.car;

import android.os.Handler;
import android.os.IBinder;
import android.os.RemoteException;

import java.lang.ref.WeakReference;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;

/**
 * CarAppFocusManager allows applications to set and listen for the current application focus
 * like active navigation or active voice command. Usually only one instance of such application
 * should run in the system, and other app setting the flag for the matching app should
 * lead into other app to stop.
 */
public final class CarAppFocusManager implements CarManagerBase {
    /**
     * Listener to get notification for app getting information on application type status changes.
     */
    public interface OnAppFocusChangedListener {
        /**
         * Application focus has changed. Note that {@link CarAppFocusManager} instance
         * causing the change will not get this notification.
         * @param appType
         * @param active
         */
        void onAppFocusChanged(int appType, boolean active);
    }

    /**
     * Listener to get notification for app getting information on app type ownership loss.
     */
    public interface OnAppFocusOwnershipLostListener {
        /**
         * Lost ownership for the focus, which happens when other app has set the focus.
         * The app losing focus should stop the action associated with the focus.
         * For example, navigation app currently running active navigation should stop navigation
         * upon getting this for {@link CarAppFocusManager#APP_FOCUS_TYPE_NAVIGATION}.
         * @param appType
         */
        void onAppFocusOwnershipLost(int appType);
    }

    /**
     * Represents navigation focus.
     */
    public static final int APP_FOCUS_TYPE_NAVIGATION = 1;
    /**
     * Represents voice command focus.
     */
    public static final int APP_FOCUS_TYPE_VOICE_COMMAND = 2;
    /**
     * Update this after adding a new app type.
     * @hide
     */
    public static final int APP_FOCUS_MAX = 2;

    /**
     * A failed focus change request.
     */
    public static final int APP_FOCUS_REQUEST_FAILED = 0;
    /**
     * A successful focus change request.
     */
    public static final int APP_FOCUS_REQUEST_GRANTED = 1;

    private final IAppFocus mService;
    private final Handler mHandler;
    private final Map<OnAppFocusChangedListener, IAppFocusListenerImpl> mChangeBinders =
            new HashMap<>();
    private final Map<OnAppFocusOwnershipLostListener, IAppFocusOwnershipListenerImpl>
            mOwnershipBinders = new HashMap<>();

    /**
     * @hide
     */
    CarAppFocusManager(IBinder service, Handler handler) {
        mService = IAppFocus.Stub.asInterface(service);
        mHandler = handler;
    }

    /**
     * Register listener to monitor app focus change.
     * @param listener
     * @param appType Application type to get notification for.
     * @throws CarNotConnectedException if the connection to the car service has been lost.
     */
    public void addFocusListener(OnAppFocusChangedListener listener, int appType)
            throws CarNotConnectedException {
        if (listener == null) {
            throw new IllegalArgumentException("null listener");
        }
        IAppFocusListenerImpl binder;
        synchronized (this) {
            binder = mChangeBinders.get(listener);
            if (binder == null) {
                binder = new IAppFocusListenerImpl(this, listener);
                mChangeBinders.put(listener, binder);
            }
            binder.addAppType(appType);
        }
        try {
            mService.registerFocusListener(binder, appType);
        } catch (RemoteException e) {
            throw new CarNotConnectedException(e);
        }
    }

    /**
     * Unregister listener for application type and stop listening focus change events.
     * @param listener
     * @param appType
     * @throws CarNotConnectedException if the connection to the car service has been lost.
     */
    public void removeFocusListener(OnAppFocusChangedListener listener, int appType)
            throws CarNotConnectedException {
        IAppFocusListenerImpl binder;
        synchronized (this) {
            binder = mChangeBinders.get(listener);
            if (binder == null) {
                return;
            }
        }
        try {
            mService.unregisterFocusListener(binder, appType);
        } catch (RemoteException e) {
            throw new CarNotConnectedException(e);
        }
        synchronized (this) {
            binder.removeAppType(appType);
            if (!binder.hasAppTypes()) {
                mChangeBinders.remove(listener);
            }

        }
    }

    /**
     * Unregister listener and stop listening focus change events.
     * @param listener
     * @throws CarNotConnectedException if the connection to the car service has been lost.
     */
    public void removeFocusListener(OnAppFocusChangedListener listener)
            throws CarNotConnectedException {
        IAppFocusListenerImpl binder;
        synchronized (this) {
            binder = mChangeBinders.remove(listener);
            if (binder == null) {
                return;
            }
        }
        try {
            for (Integer appType : binder.getAppTypes()) {
                mService.unregisterFocusListener(binder, appType);
            }
        } catch (RemoteException e) {
            throw new CarNotConnectedException(e);
        }
    }

    /**
     * Returns application types currently active in the system.
     * @throws CarNotConnectedException if the connection to the car service has been lost.
     * @hide
     */
    public int[] getActiveAppTypes() throws CarNotConnectedException {
        try {
            return mService.getActiveAppTypes();
        } catch (RemoteException e) {
            throw new CarNotConnectedException(e);
        }
    }

    /**
     * Checks if listener is associated with active a focus
     * @param listener
     * @param appType
     * @throws CarNotConnectedException if the connection to the car service has been lost.
     */
    public boolean isOwningFocus(OnAppFocusOwnershipLostListener listener, int appType)
            throws CarNotConnectedException {
        IAppFocusOwnershipListenerImpl binder;
        synchronized (this) {
            binder = mOwnershipBinders.get(listener);
            if (binder == null) {
                return false;
            }
        }
        try {
            return mService.isOwningFocus(binder, appType);
        } catch (RemoteException e) {
            throw new CarNotConnectedException(e);
        }
    }

    /**
     * Requests application focus.
     * By requesting this, the application is becoming owner of the focus, and will get
     * {@link OnAppFocusOwnershipLostListener#onAppFocusOwnershipLost(int)}
     * if ownership is given to other app by calling this. Fore-ground app will have higher priority
     * and other app cannot set the same focus while owner is in fore-ground.
     * @param appType
     * @param ownershipListener
     * @return {@link #APP_FOCUS_REQUEST_FAILED} or {@link #APP_FOCUS_REQUEST_GRANTED}
     * @throws CarNotConnectedException if the connection to the car service has been lost.
     * @throws SecurityException If owner cannot be changed.
     */
    public int requestAppFocus(int appType, OnAppFocusOwnershipLostListener ownershipListener)
            throws SecurityException, CarNotConnectedException {
        if (ownershipListener == null) {
            throw new IllegalArgumentException("null listener");
        }
        IAppFocusOwnershipListenerImpl binder;
        synchronized (this) {
            binder = mOwnershipBinders.get(ownershipListener);
            if (binder == null) {
                binder = new IAppFocusOwnershipListenerImpl(this, ownershipListener);
                mOwnershipBinders.put(ownershipListener, binder);
            }
            binder.addAppType(appType);
        }
        try {
            return mService.requestAppFocus(binder, appType);
        } catch (RemoteException e) {
            throw new CarNotConnectedException(e);
        }
    }

    /**
     * Abandon the given focus, i.e. mark it as inactive. This also involves releasing ownership
     * for the focus.
     * @param ownershipListener
     * @param appType
     * @throws CarNotConnectedException if the connection to the car service has been lost.
     */
    public void abandonAppFocus(OnAppFocusOwnershipLostListener ownershipListener, int appType)
            throws CarNotConnectedException {
        if (ownershipListener == null) {
            throw new IllegalArgumentException("null listener");
        }
        IAppFocusOwnershipListenerImpl binder;
        synchronized (this) {
            binder = mOwnershipBinders.get(ownershipListener);
            if (binder == null) {
                return;
            }
        }
        try {
            mService.abandonAppFocus(binder, appType);
        } catch (RemoteException e) {
            throw new CarNotConnectedException(e);
        }
        synchronized (this) {
            binder.removeAppType(appType);
            if (!binder.hasAppTypes()) {
                mOwnershipBinders.remove(ownershipListener);
            }
        }
    }

    /**
     * Abandon all focuses, i.e. mark them as inactive. This also involves releasing ownership
     * for the focus.
     * @param ownershipListener
     * @throws CarNotConnectedException if the connection to the car service has been lost.
     */
    public void abandonAppFocus(OnAppFocusOwnershipLostListener ownershipListener)
            throws CarNotConnectedException {
        IAppFocusOwnershipListenerImpl binder;
        synchronized (this) {
            binder = mOwnershipBinders.remove(ownershipListener);
            if (binder == null) {
                return;
            }
        }
        try {
            for (Integer appType : binder.getAppTypes()) {
                mService.abandonAppFocus(binder, appType);
            }
        } catch (RemoteException e) {
            throw new CarNotConnectedException(e);
        }
    }

    /** @hide */
    @Override
    public void onCarDisconnected() {
        // nothing to do
    }

    private static class IAppFocusListenerImpl extends IAppFocusListener.Stub {

        private final WeakReference<CarAppFocusManager> mManager;
        private final WeakReference<OnAppFocusChangedListener> mListener;
        private final Set<Integer> mAppTypes = new HashSet<>();

        private IAppFocusListenerImpl(CarAppFocusManager manager, OnAppFocusChangedListener listener) {
            mManager = new WeakReference<>(manager);
            mListener = new WeakReference<>(listener);
        }

        public void addAppType(int appType) {
            mAppTypes.add(appType);
        }

        public void removeAppType(int appType) {
            mAppTypes.remove(appType);
        }

        public Set<Integer> getAppTypes() {
            return mAppTypes;
        }

        public boolean hasAppTypes() {
            return !mAppTypes.isEmpty();
        }

        @Override
        public void onAppFocusChanged(final int appType, final boolean active) {
            final CarAppFocusManager manager = mManager.get();
            final OnAppFocusChangedListener listener = mListener.get();
            if (manager == null || listener == null) {
                return;
            }
            manager.mHandler.post(new Runnable() {
                @Override
                public void run() {
                    listener.onAppFocusChanged(appType, active);
                }
            });
        }
    }

    private static class IAppFocusOwnershipListenerImpl extends IAppFocusOwnershipListener.Stub {

        private final WeakReference<CarAppFocusManager> mManager;
        private final WeakReference<OnAppFocusOwnershipLostListener> mListener;
        private final Set<Integer> mAppTypes = new HashSet<>();

        private IAppFocusOwnershipListenerImpl(CarAppFocusManager manager,
                OnAppFocusOwnershipLostListener listener) {
            mManager = new WeakReference<>(manager);
            mListener = new WeakReference<>(listener);
        }

        public void addAppType(int appType) {
            mAppTypes.add(appType);
        }

        public void removeAppType(int appType) {
            mAppTypes.remove(appType);
        }

        public Set<Integer> getAppTypes() {
            return mAppTypes;
        }

        public boolean hasAppTypes() {
            return !mAppTypes.isEmpty();
        }

        @Override
        public void onAppFocusOwnershipLost(final int appType) {
            final CarAppFocusManager manager = mManager.get();
            final OnAppFocusOwnershipLostListener listener = mListener.get();
            if (manager == null || listener == null) {
                return;
            }
            manager.mHandler.post(new Runnable() {
                @Override
                public void run() {
                    listener.onAppFocusOwnershipLost(appType);
                }
            });
        }
    }
}
