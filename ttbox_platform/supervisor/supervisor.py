"""统一 Supervisor：服务编排、Runtime 接管和 crash recovery。"""
from __future__ import annotations

import logging
import time
from dataclasses import dataclass

from ..health.monitor import HealthMonitor, UnifiedHealth
from ..runtime.controller import RuntimeController, RuntimeSnapshot
from .systemd_adapter import ServiceAdapter, ServiceStatus

logger = logging.getLogger(__name__)


@dataclass(frozen=True)
class SupervisorStatus:
    runtime: RuntimeSnapshot
    services: dict[str, ServiceStatus]
    health: UnifiedHealth
    recovered: bool
    timestamp: float


class Supervisor:
    def __init__(
        self,
        runtime: RuntimeController,
        services: ServiceAdapter,
        health: HealthMonitor,
        core_service: str = 'ttbox-core',
        auxiliary: tuple[str, ...] = (),
    ) -> None:
        self.runtime = runtime
        self.services = services
        self.health = health
        self.core_service = core_service
        self.auxiliary = tuple(auxiliary)
        self._recovered = False
        self._started_aux: list[str] = []

    def start(self) -> SupervisorStatus:
        self._started_aux = []
        try:
            for name in self.auxiliary:
                self.services.start(name)
                self._started_aux.append(name)
            self.runtime.start()
            return self.status()
        except Exception:
            logger.exception("Supervisor.start() 失败，执行回滚")
            self._rollback_aux_started()
            try:
                self.runtime.stop()
            except Exception:
                logger.exception("回滚时 runtime.stop() 亦失败")
            raise

    def stop(self) -> SupervisorStatus:
        exc = None
        try:
            self.runtime.stop()
        except Exception as e:
            logger.exception("Supervisor.stop(): runtime.stop() 失败")
            exc = e
        for name in reversed(self.auxiliary):
            try:
                self.services.stop(name)
            except Exception:
                logger.exception("停止 auxiliary 服务失败: %s", name)
                exc = exc or RuntimeError(f"停止 {name} 失败")
        self._started_aux = []
        if exc:
            raise exc
        return self.status()

    def restart(self) -> SupervisorStatus:
        try:
            self.stop()
        except Exception:
            logger.exception("Supervisor.restart(): stop 阶段出错，仍尝试继续启动")
        return self.start()

    def recover(self) -> SupervisorStatus:
        self._recovered = False
        if self.runtime.state.value not in ('FAILED', 'RUNNING'):
            return self.status()
        try:
            self.runtime.stop()
        except Exception:
            logger.exception("recover(): runtime.stop() 失败，继续尝试 start")
        try:
            self.runtime.start()
        except Exception:
            logger.exception("recover(): runtime.start() 失败")
            raise
        self._recovered = True
        return self.status()

    def check_and_sync(self) -> None:
        service_states = {
            n: self.services.status(n)
            for n in (self.core_service, *self.auxiliary)
        }
        core = service_states[self.core_service]
        if self.runtime.state.value == 'RUNNING' and not core.active:
            reason = core.error or f'{self.core_service} is not active'
            logger.warning("同步 runtime 状态为 FAILED: %s", reason)
            self.runtime.mark_failed(reason)

    def status(self) -> SupervisorStatus:
        service_states = {
            n: self.services.status(n)
            for n in (self.core_service, *self.auxiliary)
        }
        return SupervisorStatus(
            runtime=self.runtime.status(),
            services=service_states,
            health=self.health.check(),
            recovered=self._recovered,
            timestamp=time.time(),
        )

    def _rollback_aux_started(self) -> None:
        for name in reversed(self._started_aux):
            try:
                self.services.stop(name)
            except Exception:
                logger.exception("回滚时停止 auxiliary 服务失败: %s", name)
        self._started_aux = []
