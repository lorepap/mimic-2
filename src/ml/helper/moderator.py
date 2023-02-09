from types import SimpleNamespace
import json
from helper import context
import traceback


class Moderator():

    def __init__(self, use_iperf: bool) -> None:
        self.started = False
        self.use_iperf = use_iperf

    def can_start(self) -> bool:
        return self.started or not self.use_iperf

    def start(self) -> None:
        self.started = True

    def stop(self) -> None:
        self.started = False
