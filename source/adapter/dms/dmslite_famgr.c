/*
 * Copyright (c) 2020 Huawei Device Co., Ltd.
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

#include "dmslite_famgr.h"

#include "dmslite.h"
#include "dmslite_log.h"
#include "dmslite_pack.h"
#include "dmslite_session.h"
#include "dmslite_utils.h"

#include "ability_manager.h"
#include "ability_service_interface.h"
#include "iproxy_client.h"
#include "iunknown.h"
#include "liteipc_adapter.h"
#include "ohos_errno.h"
#include "samgr_lite.h"
#include "securec.h"

#define INVALID_IPC_TOKEN 0
#define INVALID_IPC_HANDLE (-1)
#define DMS_VERSION_VALUE 200

static SvcIdentity g_serviceIdentity = {
    .handle = INVALID_IPC_HANDLE,
    .token = INVALID_IPC_TOKEN
};

static StartAbilityCallback g_onStartAbilityDone = NULL;

static int32_t AmsResultCallback(const IpcContext* context, void *ipcMsg, IpcIo *io, void *arg)
{
    /* Notice: must free ipcMsg, for we don't need ipcMsg here, just free it at first */
    FreeBuffer(context, ipcMsg);

    HILOGD("[AmsResultCallback called]");
    if (g_onStartAbilityDone == NULL) {
        return LITEIPC_EINVAL;
    }

    ElementName elementName;
    if (memset_s(&elementName, sizeof(ElementName), 0x00, sizeof(ElementName)) != EOK) {
        HILOGE("[elementName memset failed]");
        return LITEIPC_EINVAL;
    }

    /* the element is not used so far, and deserialize element first before we can get the errcode from io */
    if (!DeserializeElement(&elementName, io)) {
        return LITEIPC_EINVAL;
    }
    ClearElement(&elementName);

    int8_t errCode = DMS_EC_START_ABILITY_ASYNC_FAILURE;
    if (IpcIoPopInt32(io) == EC_SUCCESS) {
        /* this means that FA starts and shows on screen successfully */
        errCode = DMS_EC_START_ABILITY_ASYNC_SUCCESS;
    }
    g_onStartAbilityDone(errCode);
    return LITEIPC_OK;
}

static bool GetAmsInterface(struct AmsInterface **amsInterface)
{
    IUnknown *iUnknown = SAMGR_GetInstance()->GetFeatureApi(AMS_SERVICE, AMS_FEATURE);
    if (iUnknown == NULL) {
        HILOGE("[GetFeatureApi failed]");
        return false;
    }

    int32_t errCode = iUnknown->QueryInterface(iUnknown, DEFAULT_VERSION, (void**) amsInterface);
    if (errCode != EC_SUCCESS) {
        HILOGE("[QueryInterface failed]");
        return false;
    }
    return true;
}

static int32_t FillWant(Want *want, const char *bundleName, const char *abilityName)
{
    if (memset_s(want, sizeof(Want), 0x00, sizeof(Want)) != EOK) {
        HILOGE("[want memset failed]");
        return DMS_EC_FAILURE;
    }
    ElementName element;
    if (memset_s(&element, sizeof(ElementName), 0x00, sizeof(ElementName)) != EOK) {
        HILOGE("[elementName memset failed]");
        return DMS_EC_FAILURE;
    }

    if (!(SetElementBundleName(&element, bundleName)
        && SetElementAbilityName(&element, abilityName)
        && SetWantElement(want, element)
        && SetWantSvcIdentity(want, g_serviceIdentity))) {
        HILOGE("[Fill want failed]");
        ClearElement(&element);
        ClearWant(want);
        return DMS_EC_FAILURE;
    }
    ClearElement(&element);
    return DMS_EC_SUCCESS;
}

static int32_t StartAbilityFromRemoteInner(const char *bundleName, const char *abilityName)
{
    Want want; /* NOTICE: must call ClearWant if filling want sucessfully */
    if (FillWant(&want, bundleName, abilityName) != DMS_EC_SUCCESS) {
        return DMS_EC_FILL_WANT_FAILURE;
    }

    int32_t errCode;
    uid_t callerUid = getuid();
    if (callerUid == FOUNDATION_UID) {
        /* inner-process mode */
        struct AmsInterface *amsInterface = NULL;
        if (!GetAmsInterface(&amsInterface)) {
            HILOGE("[GetAmsInterface query null]");
            ClearWant(&want);
            return DMS_EC_GET_ABILITYMS_FAILURE;
        }
        errCode = amsInterface->StartAbility(&want);
    } else if (callerUid == SHELL_UID) {
        /* inter-process mode (mainly called in xts testsuit process started by shell) */
        errCode = StartAbility(&want);
    } else {
        errCode = EC_FAILURE;
    }
    ClearWant(&want);

    if (errCode != EC_SUCCESS) {
        HILOGE("[Call StartAbility failed errCode = %d]", errCode);
        return DMS_EC_START_ABILITY_SYNC_FAILURE;
    }
    /* this just means we send to the abilityms a request of starting FA successfully */
    return DMS_EC_START_ABILITY_SYNC_SUCCESS;
}

int32_t StartAbilityFromRemote(const char *bundleName, const char *abilityName,
    StartAbilityCallback onStartAbilityDone)
{
    if (bundleName == NULL || abilityName == NULL) {
        HILOGE("[Invalid parameters]");
        return DMS_EC_FAILURE;
    }

    if (g_serviceIdentity.token == INVALID_IPC_TOKEN) {
        /* register a callback for notification when abilityms starts ability successfully */
        IpcCbMode mode = ONCE;
        if (RegisterIpcCallback(AmsResultCallback, mode, IPC_WAIT_FOREVER,
            &g_serviceIdentity, NULL) != EC_SUCCESS) {
            HILOGE("[RegisterIpcCallback failed]");
            return DMS_EC_REGISTE_IPC_CALLBACK_FAILURE;
        }
    }
    if (g_onStartAbilityDone == NULL) {
        g_onStartAbilityDone = onStartAbilityDone;
    }

    return StartAbilityFromRemoteInner(bundleName, abilityName);
}

int32_t StartRemoteAbilityInner(Want *want, AbilityInfo *abilityInfo, CallerInfo *callerInfo,
        IDmsListener *callback)
{
    return EOK;
}

int32_t StartRemoteAbility(const Want *want)
{
    HILOGE("[StartRemoteAbility]");
    if (want == NULL || want->data == NULL || want->element == NULL) {
        return DMS_EC_INVALID_PARAMETER;
    }
    char *bundleName = (char *)want->data;
    BundleInfo bundleInfo;
    if (memset_s(&bundleInfo, sizeof(BundleInfo), 0x00, sizeof(BundleInfo)) != EOK) {
        HILOGE("[bundleInfo memset failed]");
        return DMS_EC_FAILURE;
    }
    GetBundleInfo(bundleName, 0, &bundleInfo);
#ifndef XTS_SUITE_TEST
    PreprareBuild();
#endif
    PACKET_MARSHALL_HELPER(Uint16, COMMAND_ID, DMS_MSG_CMD_START_FA);
    PACKET_MARSHALL_HELPER(String, CALLEE_BUNDLE_NAME, want->element->bundleName);
    PACKET_MARSHALL_HELPER(String, CALLEE_ABILITY_NAME, want->element->abilityName);
    if (bundleInfo.appId != NULL) {
        PACKET_MARSHALL_HELPER(String, CALLER_SIGNATURE, bundleInfo.appId);
    } else {
        PACKET_MARSHALL_HELPER(String, CALLER_SIGNATURE, "");
    }
    PACKET_MARSHALL_HELPER(Uint16, DMS_VERSION, DMS_VERSION_VALUE);
    HILOGE("[StartRemoteAbility len:%d]", GetPacketSize());
#ifndef XTS_SUITE_TEST
    return SendDmsMessage(GetPacketBufPtr(), GetPacketSize());
#else
    return EOK;
#endif
}