#!/usr/bin/env python3
"""RK3588 systemd 入口：复用 ttbox-core.service，不替代 C++ Core。"""
import signal,sys,time,json
from supervisor_supervisor import Supervisor
from supervisor_systemd_adapter import SystemdServiceAdapter,SystemdProcessAdapter
from runtime_controller import RuntimeController
from health_monitor import HealthMonitor
def build():
 svc=SystemdServiceAdapter(["sudo","-n","systemctl"])
 runtime=RuntimeController(SystemdProcessAdapter(svc,"ttbox-core"))
 health=HealthMonitor(runtime,svc,("ttbox-core",))
 return Supervisor(runtime,svc,health,"ttbox-core",())
def main():
 sup=build()
 def stop(sig,frame): sup.stop(); raise SystemExit(0)
 signal.signal(signal.SIGTERM,stop); signal.signal(signal.SIGINT,stop)
 sup.start(); st=sup.status(); print(json.dumps({"state":st.runtime.state,"pid":st.runtime.pid,"health":st.health.ok}),flush=True)
 while True:
  st=sup.status()
  if not st.health.ok and st.runtime.state in ("FAILED","STOPPED","STOPPING"): sup.recover()
  time.sleep(5)
if __name__=="__main__": main()
