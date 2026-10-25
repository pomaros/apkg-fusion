# Press the category cycle, hold it, screenshot the popup, release in place.
import os, sys, time, subprocess
os.environ["AROS_QEMU_QMP"] = "/tmp/aros-onetest-qmp.sock"
os.environ["AROS_QEMU_MONITOR"] = "/tmp/aros-onetest-monitor.sock"
sys.path.insert(0, "/Users/tumash/Work/AROS/tools")
import vmctl
vm = vmctl.VM()
x, y = int(sys.argv[1]), int(sys.argv[2])
vm.move(x, y); time.sleep(0.3); vm.button(True); time.sleep(1.0)
subprocess.run("printf 'screendump /tmp/hold.ppm\\n' | nc -w 2 -U /tmp/aros-onetest-monitor.sock >/dev/null 2>&1", shell=True)
time.sleep(0.5)
vm.button(False)
subprocess.run("sips -s format jpeg /tmp/hold.ppm --out /tmp/hold.jpg >/dev/null 2>&1", shell=True)
print("held and shot")
