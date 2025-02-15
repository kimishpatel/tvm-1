# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.
"""Backend compiler related feature registration"""
from __future__ import absolute_import

import topi
from . import op as _reg


def _schedule_reduce(_, outs, target):
    """Generic schedule for reduce"""
    with target:
        return topi.generic.schedule_reduce(outs)


_reg.register_schedule("argmax", _schedule_reduce)
_reg.register_schedule("argmin", _schedule_reduce)
_reg.register_schedule("sum", _schedule_reduce)
_reg.register_schedule("all", _schedule_reduce)
_reg.register_schedule("any", _schedule_reduce)
_reg.register_schedule("max", _schedule_reduce)
_reg.register_schedule("min", _schedule_reduce)
_reg.register_schedule("prod", _schedule_reduce)
_reg.register_schedule("mean", _schedule_reduce)
_reg.register_schedule("variance", _schedule_reduce)
_reg.register_schedule("nn.cross_entropy", _schedule_reduce)
_reg.register_schedule("nn.cross_entropy_with_logits", _schedule_reduce)
