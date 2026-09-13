############################################################################
# vendor/allwinnertech/apps/kid_buddy/Makefile
#
# Licensed to the Apache Software Foundation (ASF) under one or more
# contributor license agreements.  See the NOTICE file distributed with
# this work for additional information regarding copyright ownership.  The
# ASF licenses this file to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance with the
# License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
# License for the specific language governing permissions and limitations
# under the License.
#
############################################################################
include $(APPDIR)/Make.defs
include $(APPDIR)/vendor/allwinnertech/Common.mk

MAINSRC = kid_buddy.c

PROGNAME = $(CONFIG_KID_BUDDY_APP_PROGNAME)
PRIORITY = $(CONFIG_KID_BUDDY_APP_PRIORITY)
STACKSIZE = $(CONFIG_KID_BUDDY_APP_STACKSIZE)
MODULE = $(CONFIG_KID_BUDDY_APP)

# Include ai_agent client headers
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/packages/ai_agent/include
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/packages/ai_agent/src

# Use common HAL includes
$(eval $(AW_ADD_HAL_INCLUDES))

include $(APPDIR)/Application.mk
