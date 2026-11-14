'''
SPDX-License-Identifier: MIT
------------------------------------------------------------------------------
Copyright (c) 2026 Arm Limited
'''

'''Host endpoint for lightweight GPU Support shader dump progress messages.'''

import json
from typing import Any

from lglpy.comms.server import Message


class GPUShaderDumpService:
    '''Forward shader dump progress packets to an installer-owned reporter.'''

    def __init__(self, reporter: Any):
        self.reporter = reporter

    def get_service_name(self) -> str:
        return 'GPUShaderDump'

    def handle_message(self, message: Message) -> None:
        '''Handle pushed TX_ASYNC progress packets; no response is required.'''
        try:
            payload = json.loads(message.payload.decode('utf-8'))
        except (UnicodeDecodeError, json.JSONDecodeError):
            return

        if isinstance(payload, dict):
            self.reporter.handle_message(payload)
