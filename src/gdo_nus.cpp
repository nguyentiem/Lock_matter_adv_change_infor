/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "gdo_nus.h"

#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include <platform/CHIPDeviceLayer.h>

using namespace ::chip;
using namespace ::chip::DeviceLayer;

LOG_MODULE_DECLARE(app, CONFIG_MATTER_LOG_LEVEL);

#define NUS_BT_NAME (CONFIG_BT_DEVICE_NAME "_NUS")

namespace
{
constexpr uint32_t kAdvertisingOptions = BT_LE_ADV_OPT_CONNECTABLE;
constexpr uint8_t kAdvertisingFlags = BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR;
constexpr uint8_t kBTUuid[] = { BT_UUID_NUS_VAL };
} /* namespace */

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = GDONUSService::Connected,
	.disconnected = GDONUSService::Disconnected,
	.security_changed = GDONUSService::SecurityChanged,
};
bt_conn_auth_cb GDONUSService::sConnAuthCallbacks = {
	.passkey_display = AuthPasskeyDisplay,
	.cancel = AuthCancel,
};
bt_conn_auth_info_cb GDONUSService::sConnAuthInfoCallbacks = {
	.pairing_complete = PairingComplete,
	.pairing_failed = PairingFailed,
};
bt_nus_cb GDONUSService::sNusCallbacks = {
	.received = RxCallback,
};

GDONUSService GDONUSService::sInstance;
uint8_t door[3]={0x66,0x66, 0x00}; 
static volatile uint8_t test = 0x00; 
bool GDONUSService::Init(uint8_t priority, uint16_t minInterval, uint16_t maxInterval)
{
	mAdvertisingItems[0] = BT_DATA(BT_DATA_FLAGS, &kAdvertisingFlags, sizeof(kAdvertisingFlags));
	mAdvertisingItems[1] = BT_DATA(BT_DATA_NAME_COMPLETE, "TIEM", 4);
    mAdvertisingItems[2] = BT_DATA(BT_DATA_SVC_DATA16, door, sizeof(door));
	mServiceItems[0] = BT_DATA(BT_DATA_UUID128_ALL, kBTUuid, sizeof(kBTUuid));

	mAdvertisingRequest.priority = priority;
	mAdvertisingRequest.options = kAdvertisingOptions;
	mAdvertisingRequest.minInterval = minInterval;
	mAdvertisingRequest.maxInterval = maxInterval;
	mAdvertisingRequest.advertisingData = Span<bt_data>(mAdvertisingItems);
	mAdvertisingRequest.scanResponseData = Span<bt_data>(mServiceItems);

	mAdvertisingRequest.onStarted = [](int rc) {
		if (rc == 0) {
			GetGDONUSService().mIsStarted = true;
			LOG_DBG("NUS BLE advertising started");
		} else {
			LOG_ERR("Failed to start NUS BLE advertising: %d", rc);
		}
	};
	mAdvertisingRequest.onStopped = []() {
		GetGDONUSService().mIsStarted = false;
		LOG_DBG("NUS BLE advertising stopped");
	};
if (bt_conn_auth_cb_register(&sConnAuthCallbacks) != 0)
		return false;
	if (bt_conn_auth_info_cb_register(&sConnAuthInfoCallbacks) != 0)
		return false;
	if (bt_nus_init(&sNusCallbacks) != 0)
		return false;
#if defined(CONFIG_BT_FIXED_PASSKEY)
	if (bt_passkey_set(CONFIG_CHIP_NUS_FIXED_PASSKEY) != 0)
		return false;
#endif

	return true;
}

bool GDONUSService::StartServer()
{
	// if (mIsStarted) {
	// 	LOG_WRN("NUS service was already started");
	// 	return false;
	// }
    LOG_INF("Start NUS again");
    door[2] = test++; 
    mAdvertisingItems[2] = BT_DATA(BT_DATA_SVC_DATA16, door, sizeof(door));
	mAdvertisingRequest.advertisingData = Span<bt_data>(mAdvertisingItems);
	mAdvertisingRequest.scanResponseData = Span<bt_data>(mServiceItems);
	
    
	PlatformMgr().LockChipStack();
	CHIP_ERROR ret = BLEAdvertisingArbiter::InsertRequest(mAdvertisingRequest);
	PlatformMgr().UnlockChipStack();

	if (CHIP_NO_ERROR != ret) {
		LOG_ERR("Could not start NUS service");
		return false;
	}

	return true;
}

void GDONUSService::StopServer()
{
	if (!mIsStarted)
		return;

	PlatformMgr().LockChipStack();
	BLEAdvertisingArbiter::CancelRequest(mAdvertisingRequest);
	PlatformMgr().UnlockChipStack();
}

void GDONUSService::RxCallback(bt_conn *conn, const uint8_t *const data, uint16_t len)
{
	if (bt_conn_get_security(GetGDONUSService().mBTConnection) < BT_SECURITY_L2) {
		LOG_ERR("Received NUS command, but security requirements are not met.");
		return;
	}

	LOG_DBG("NUS received: %d bytes", len);
	GetGDONUSService().DispatchCommand(reinterpret_cast<const char *>(data), len);
}

bool GDONUSService::RegisterCommand(const char *const name, size_t length, CommandCallback callback, void *context)
{
	if (!name)
		return false;
	if (mCommandsCounter > CONFIG_CHIP_NUS_MAX_COMMANDS)
		return false;
	if (length > CONFIG_CHIP_NUS_MAX_COMMAND_LEN)
		return false;

	Command newCommand{};
	memcpy(newCommand.command, name, length);
	newCommand.callback = callback;
	newCommand.context = context;
	mCommandsList[mCommandsCounter++] = newCommand;

	return true;
}

void GDONUSService::DispatchCommand(const char *const data, uint16_t len)
{
	for (const auto &c : mCommandsList) {
		if (strncmp(data, c.command, len) == 0 && len == strlen(c.command)) {
			if (c.callback) {
				PlatformMgr().LockChipStack();
				c.callback(c.context);
				PlatformMgr().UnlockChipStack();
			}
			return;
		}
	}
	LOG_ERR("NUS command unknown!");
}

bool GDONUSService::SendData(const char *const data, size_t length)
{
	if (!mIsStarted || !mBTConnection)
		return false;
	if (bt_conn_get_security(mBTConnection) < BT_SECURITY_L2)
		return false;
	if (bt_nus_send(mBTConnection, reinterpret_cast<const uint8_t *const>(data), length) != 0)
		return false;

	return true;
}

void GDONUSService::Connected(bt_conn *conn, uint8_t err)
{
	if (GetGDONUSService().mIsStarted) {
		if (err || !conn) {
			LOG_ERR("NUS Connection failed (err %u)", err);
			return;
		}

		GetGDONUSService().mBTConnection = conn;
		bt_conn_set_security(conn, BT_SECURITY_L3);
	}
}

void GDONUSService::Disconnected(bt_conn *conn, uint8_t reason)
{
	GetGDONUSService().mBTConnection = nullptr;
}

void GDONUSService::SecurityChanged(bt_conn *conn, bt_security_t level, enum bt_security_err err)
{
	if (!GetGDONUSService().mIsStarted)
		return;

	if (!err) {
		LOG_DBG("NUS BT Security changed: %s level %u", LogAddress(conn), level);
	} else {
		LOG_ERR("NUS BT Security failed: level %u err %d", level, err);
	}
}

void GDONUSService::AuthPasskeyDisplay(bt_conn *conn, unsigned int passkey)
{
	LOG_INF("PROVIDE THE FOLLOWING CODE IN YOUR MOBILE APP: %d", passkey);
}

void GDONUSService::AuthCancel(bt_conn *conn)
{
	LOG_INF("NUS BT Pairing cancelled: %s", LogAddress(conn));

	bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
}

void GDONUSService::PairingComplete(bt_conn *conn, bool bonded)
{
	if (!GetGDONUSService().mIsStarted)
		return;

	LOG_DBG("NUS BT Pairing completed: %s, bonded: %d", LogAddress(conn), bonded);
}

void GDONUSService::PairingFailed(bt_conn *conn, enum bt_security_err reason)
{
	if (!GetGDONUSService().mIsStarted)
		return;

	LOG_ERR("NUS BT Pairing failed to %s : reason %d", LogAddress(conn), static_cast<uint8_t>(reason));
}

char *GDONUSService::LogAddress(bt_conn *conn)
{
#if CONFIG_LOG
	static char addr[BT_ADDR_LE_STR_LEN];
	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	return addr;
#endif
	return NULL;
}
