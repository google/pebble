/*
 * Copyright 2024 Google LLC
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

#include "bluetooth/responsiveness.h"
#include "bluetooth/gap_le_connect.h"
#include "hc_protocol/hc_protocol.h"
#include "hc_protocol/hc_endpoint_responsiveness.h"

#include <inttypes.h>

// Will be implemented by PBL-32986
bool bt_driver_le_connection_parameter_update(
    const BTDeviceInternal *addr, const BleConnectionParamsUpdateReq *req) {
  return hc_endpoint_responsiveness_request_update(addr, req);
}
