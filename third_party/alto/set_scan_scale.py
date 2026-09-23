
# refer to https://github.com/MoatLab/SoarAlto/blob/main/run/bc-urand/set_scan_scale.py

from subprocess import Popen, PIPE
import subprocess
import time
import os
import ctypes
import time
import re
import sys

if len(sys.argv) < 2:
  print("Usage: set_scan_scale.py [filename]")
  exit(0)
perf_parameters_path = sys.argv[1]

def process_line(s):
  s = re.split(" |\t", s)
  return [x for x in s if len(x) > 0]

def read_perf_paramters():
  perf_file=perf_parameters_path
  p = Popen(['tail','-5',perf_file],shell=False, stderr=PIPE, stdout=PIPE)
  res,err = p.communicate()
  if err:
    print (err.decode())
  else:
    line=res.decode("utf-8")
    values = line.split('\n')

    cycles = process_line(values[0])[1].replace(",", "")
    cycles_stalls_l3 = process_line(values[1])[1].replace(",", "")
    outstanding_read = process_line(values[2])[1].replace(",", "")
    outstanding_cycles_read = process_line(values[3])[1].replace(",", "")
    demand_read = process_line(values[4])[1].replace(",", "")

    lst_cycles.append(cycles)
    lst_cycles_stalls_l3.append(cycles_stalls_l3)
    lst_outstanding_read.append(outstanding_read)
    lst_outstanding_cycles_read.append(outstanding_cycles_read)
    lst_demand_read.append(demand_read)

lst_cycles=[]
lst_cycles_stalls_l3=[]
lst_outstanding_read=[]
lst_outstanding_cycles_read=[]
lst_demand_read=[]
lst_time=[]

cnt=1

def check(val, history, load_lat):
  if len(history) < 2:
    return
  if load_lat <= 100:
    return
  if val <= 30:
    print("30")
    os.system('echo 0 > /proc/sys/kernel/numa_balancing_pte_scale')
  if val <= 40:
    print("40")
    os.system('echo 0 > /proc/sys/kernel/numa_balancing_pte_scale')
  elif val <= 50:
    print("50")
    os.system('echo 1 > /proc/sys/kernel/numa_balancing_pte_scale')
  elif val <= 60:
    print("60")
    os.system('echo 2 > /proc/sys/kernel/numa_balancing_pte_scale')
  elif val <= 80:
    print("80")
    os.system('echo 4 > /proc/sys/kernel/numa_balancing_pte_scale')
  elif val <= 100:
    print("100")
    os.system('echo 8 > /proc/sys/kernel/numa_balancing_pte_scale')
  else:
    print(">100")
    os.system('echo 16 > /proc/sys/kernel/numa_balancing_pte_scale')

history = [101]
avg_lat = 101
while True:
  print('online monitoring ....')
  read_perf_paramters()

  lst_time.append(cnt)
  load_lat = float(lst_outstanding_read[-1])/float(lst_demand_read[-1])
  mlp = float(lst_outstanding_read[-1])/float(lst_outstanding_cycles_read[-1])
  avg_lat = load_lat/mlp
  history.append(avg_lat)
  print(load_lat, mlp, int(avg_lat))
  check(avg_lat, history, load_lat)

  cnt+=1
  time.sleep(1)
