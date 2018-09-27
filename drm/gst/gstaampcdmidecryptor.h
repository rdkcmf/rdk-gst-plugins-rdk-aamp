/*
 * If not stated otherwise in this file or this component's license file the
 * following copyright and licenses apply:
 *
 * Copyright 2018 RDK Management
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
*/

#ifndef _GST_AAMPCDMIDECRYPTOR_H_
#define _GST_AAMPCDMIDECRYPTOR_H_


#include <gst/gst.h>
#include <gst/base/gstbasetransform.h>
#include "AampDRMSessionManager.h"
#include "priv_aamp.h"


G_BEGIN_DECLS

#define GST_TYPE_AAMP_CDMI_DECRYPTOR            (gst_aampcdmidecryptor_get_type())
#define GST_AAMP_CDMI_DECRYPTOR(obj)            (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_AAMP_CDMI_DECRYPTOR, GstAampCDMIDecryptor))
#define GST_AAMP_CDMI_DECRYPTOR_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST((klass), GST_TYPE_AAMP_CDMI_DECRYPTOR, GstAampCDMIDecryptorClass))
#define GST_IS_AAMP_CDMI_DECRYPTOR(obj)         (G_TYPE_CHECK_INSTANCE_TYPE((obj), GST_TYPE_AAMP_CDMI_DECRYPTOR))
#define GST_IS_AAMP_CDMI_DECRYPTOR_CLASS(obj)   (G_TYPE_CHECK_CLASS_TYPE((klass), GST_TYPE_AAMP_CDMI_DECRYPTOR))

typedef struct _GstAampCDMIDecryptor GstAampCDMIDecryptor;
typedef struct _GstAampCDMIDecryptorClass GstAampCDMIDecryptorClass;

/**
 * @struct _GstAampCDMIDecryptor
 * @brief GstElement structure override for CDMI decryptor
 */
struct _GstAampCDMIDecryptor
{
    GstBaseTransform                base_aampcdmidecryptor;
    class AampDRMSessionManager*    sessionManager;
    class AampDrmSession*           drmSession;
    class PrivateInstanceAAMP *     aamp;
    gboolean                        streamReceived;
    gboolean                        canWait;
    gboolean                        firstsegprocessed;
    MediaType                       streamtype;

    GMutex                          mutex;
    GCond                           condition;

    GstEvent*                       protectionEvent;
    const gchar*                    selectedProtection;
    gushort                         decryptFailCount;
    gboolean                        notifyDecryptError;
    //GstBuffer*                    initDataBuffer;
};

/**
 * @struct _GstAampCDMIDecryptorClass
 * @brief GstElementClass structure override for CDMI decryptor
 */
struct _GstAampCDMIDecryptorClass
{
    GstBaseTransformClass           base_aampcdmidecryptor_class;
};

/**
 * @brief Get type of CDMI decryptor
 * @retval Type of CDMI decryptor
 */
GType gst_aampcdmidecryptor_get_type (void);

G_END_DECLS

#endif
