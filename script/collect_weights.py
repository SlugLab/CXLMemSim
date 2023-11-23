import os, subprocess
import time
workloads = ["mlc","ld","st","nt-ld","nt-st","ptr-chasing"]


def batch_run():
    os.system("../cmake-build-debug/CXLMemSim")
    
def run_command(size):
    start_time = time.time()
    cmd = ["../cmake-build-debug/CXLMemSim" ,"-s"]
    print(cmd)
    subprocess.run(cmd)
    end_time = time.time()
    return end_time - start_time
