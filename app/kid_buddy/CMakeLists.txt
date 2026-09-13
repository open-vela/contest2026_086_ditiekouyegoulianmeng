# ##############################################################################
# app/kid_buddy/CMakeLists.txt
#
# Copyright (C) 2026 openvela Contest — team 086
#
# Licensed under the Apache License, Version 2.0 (the "License"); you may not
# use this file except in compliance with the License. You may obtain a copy of
# the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
# License for the specific language governing permissions and limitations under
# the License.
#
# ##############################################################################

if(CONFIG_KID_BUDDY_APP)
  nuttx_add_application(
    NAME kid_buddy
    PRIORITY ${CONFIG_KID_BUDDY_APP_PRIORITY}
    STACKSIZE ${CONFIG_KID_BUDDY_APP_STACKSIZE}
    MODULE ${CONFIG_KID_BUDDY_APP}
    SRCS kid_buddy.c)

  # ai_agent client headers (see Makefile for the make-based equivalent)
  target_include_directories(apps PRIVATE
    ${CMAKE_SOURCE_DIR}/packages/ai_agent/include
    ${CMAKE_SOURCE_DIR}/packages/ai_agent/src)
endif()
